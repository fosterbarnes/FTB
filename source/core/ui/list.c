#include <malloc.h>

#include <3ds.h>

#include "error.h"
#include "list.h"
#include "ui.h"
#include "../screen.h"
#include "../linkedlist.h"
#include "../../fbi/resources.h"

// Touch fling: velocities in px/ms, decay tuned to about 0.95 per 60 fps frame.
#define FRAME_MS (1000.0f / 60.0f)
#define FLING_DECAY_PER_MS 0.003f
#define FLING_START_VELOCITY 0.15f
#define FLING_STOP_VELOCITY 0.03f
#define FLING_MAX_VELOCITY 2.4f
#define SCROLL_BAR_MIN_HEIGHT 16.0f

// Grid layout: icon cells on the top screen. Per zoom level: icon edge, columns across the 390 px
// left of the scroll bar, row height. Icons scale by icon / GRID_NATIVE_ICON (CTR 48, TWL 32 native),
// and each cell leaves room for the selection ring.
#define GRID_NATIVE_ICON 48
#define GRID_SELECTION_BORDER 4

static const struct {
    u32 icon;
    u32 columns;
    u32 rowHeight;
} list_grid_sizes[LIST_GRID_SIZE_COUNT] = {
        {32, 9, 44},
        {48, 7, 64},
        {64, 5, 80},
        {80, 4, 100},
};

enum {
    LIST_TOUCH_NONE,
    LIST_TOUCH_LIST,
    LIST_TOUCH_SCROLL_BAR
};

typedef struct {
    void* data;
    linked_list items;
    u32 selectedIndex;
    list_item* selectedItem;
    u32 selectionScroll;
    u64 nextSelectionScrollResetTime;
    float scrollPos;
    u32 lastScrollTouchY;
    // Set only by a touch down this list saw, so stale held frames after a popped view are ignored.
    u8 touchMode;
    float scrollBarGrabOffset;
    float dragVelocity;
    float scrollVelocity;
    u64 lastScrollTime;
    u64 nextActionTime;
    list_layout layout;
    u32 gridSize;
    u32 (*itemIcon)(list_item* item);
    void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected);
} list_data;

// A row layout is a grid with one column of font-height rows, so scroll math works in rows.
static u32 list_columns(list_data* listData) {
    return listData->layout == LIST_LAYOUT_GRID ? list_grid_sizes[listData->gridSize].columns : 1;
}

static float list_row_height(list_data* listData) {
    return listData->layout == LIST_LAYOUT_GRID ? list_grid_sizes[listData->gridSize].rowHeight : screen_get_font_height(0.5f);
}

static u32 list_row_count(list_data* listData) {
    u32 columns = list_columns(listData);
    return (linked_list_size(&listData->items) + columns - 1) / columns;
}

// Every content rect is 200 px tall, so validating against the top or bottom rect is the same.
static void list_validate(list_data* listData, float by1, float by2) {
    u32 size = linked_list_size(&listData->items);

    if(size == 0 || listData->selectedIndex < 0) {
        listData->selectedIndex = 0;
        listData->selectedItem = NULL;
        listData->scrollPos = 0;
    }

    if(size > 0) {
        if(listData->selectedIndex > size - 1) {
            listData->selectedIndex = size - 1;
            listData->selectedItem = NULL;
            listData->scrollPos = 0;
        }

        float viewSize = by2 - by1;
        float rowHeight = list_row_height(listData);
        u32 columns = list_columns(listData);

        bool found = false;
        if(listData->selectedItem != NULL) {
            u32 oldIndex = listData->selectedIndex;

            int index = linked_list_index_of(&listData->items, listData->selectedItem);
            if(index != -1) {
                found = true;
                listData->selectedIndex = (u32) index;

                if(listData->selectedIndex != oldIndex) {
                    listData->scrollPos += (listData->selectedIndex / columns * rowHeight) - (oldIndex / columns * rowHeight);
                }
            }
        }

        if(!found) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        float itemY = listData->selectedIndex / columns * rowHeight;

        float minItemScrollPos = itemY - (viewSize - rowHeight);
        if(listData->scrollPos < minItemScrollPos) {
            listData->scrollPos = minItemScrollPos;
        }

        if(listData->scrollPos > itemY) {
            listData->scrollPos = itemY;
        }

        float maxScrollPos = (list_row_count(listData) * rowHeight) - viewSize;
        if(listData->scrollPos > maxScrollPos) {
            listData->scrollPos = maxScrollPos;
        }

        if(listData->scrollPos < 0) {
            listData->scrollPos = 0;
        }
    }
}

static float list_get_max_scroll_pos(list_data* listData, float viewSize) {
    float maxScrollPos = list_row_count(listData) * list_row_height(listData) - viewSize;
    return maxScrollPos > 0 ? maxScrollPos : 0;
}

// Scroll bar geometry shared by drawing and touch; false when the list fits and has no bar.
static bool list_get_scroll_bar(list_data* listData, float y1, float y2, float* barY, float* barHeight) {
    float viewSize = y2 - y1;
    float maxScrollPos = list_get_max_scroll_pos(listData, viewSize);
    if(maxScrollPos <= 0) {
        return false;
    }

    float height = viewSize * viewSize / (maxScrollPos + viewSize);
    if(height < SCROLL_BAR_MIN_HEIGHT) {
        height = SCROLL_BAR_MIN_HEIGHT;
    }

    *barHeight = height;
    *barY = y1 + listData->scrollPos / maxScrollPos * (viewSize - height);
    return true;
}

// Scroll-driven moves (drag, fling, scroll bar) keep the scroll position and pull the selection into
// the fully visible rows; list_validate would otherwise scroll back to the selection. True when the
// scroll hit either end.
static bool list_follow_scroll(list_data* listData, float viewSize) {
    float maxScrollPos = list_get_max_scroll_pos(listData, viewSize);
    bool clamped = true;
    if(listData->scrollPos < 0) {
        listData->scrollPos = 0;
    } else if(listData->scrollPos > maxScrollPos) {
        listData->scrollPos = maxScrollPos;
    } else {
        clamped = false;
    }

    // Rows, not items: a grid keeps the selected column.
    float rowHeight = list_row_height(listData);
    u32 first = (u32) (listData->scrollPos / rowHeight);
    if(first * rowHeight < listData->scrollPos) {
        first++;
    }

    u32 visible = (u32) ((listData->scrollPos + viewSize) / rowHeight);
    u32 last = visible > first ? visible - 1 : first;
    u32 rows = list_row_count(listData);
    if(last > rows - 1) {
        last = rows - 1;
    }

    u32 columns = list_columns(listData);
    u32 row = listData->selectedIndex / columns;
    if(row < first) {
        row = first;
    } else if(row > last) {
        row = last;
    }

    u32 index = row * columns + listData->selectedIndex % columns;
    u32 size = linked_list_size(&listData->items);
    listData->selectedIndex = index < size ? index : size - 1;

    return clamped;
}

// Places the scroll bar so the held point stays at the grab offset, then scrolls the list to match.
static void list_drag_scroll_bar(list_data* listData, float touchY, float y1, float y2) {
    float barY = 0;
    float barHeight = 0;
    if(list_get_scroll_bar(listData, y1, y2, &barY, &barHeight)) {
        float viewSize = y2 - y1;
        listData->scrollPos = (touchY - listData->scrollBarGrabOffset - y1) / (viewSize - barHeight)
                              * list_get_max_scroll_pos(listData, viewSize);
        list_follow_scroll(listData, viewSize);
    }
}

static void list_update_touch(list_data* listData, float bx2, float by1, float by2, bool* selectedTouched) {
    float viewSize = by2 - by1;
    u64 now = osGetTime();

    if(hidKeysDown() & KEY_TOUCH) {
        touchPosition pos;
        hidTouchRead(&pos);

        // A touch that catches a running fling only stops it.
        bool catching = listData->scrollVelocity != 0;
        listData->scrollVelocity = 0;
        listData->dragVelocity = 0;

        u32 scrollBarWidth = 0;
        screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

        // Top-screen layouts: the rows are not under the stylus, so the bottom screen only scrolls.
        bool rowsUnderTouch = listData->layout == LIST_LAYOUT_BOTTOM;

        float barY = 0;
        float barHeight = 0;
        if(rowsUnderTouch && list_get_scroll_bar(listData, by1, by2, &barY, &barHeight) && pos.px >= bx2 - 2 * scrollBarWidth) {
            listData->touchMode = LIST_TOUCH_SCROLL_BAR;
            // Grabbing the bar keeps its offset; touching the track centers the bar on the stylus.
            listData->scrollBarGrabOffset = pos.py >= barY && pos.py < barY + barHeight ? pos.py - barY : barHeight / 2;
            list_drag_scroll_bar(listData, pos.py, by1, by2);
        } else {
            listData->touchMode = LIST_TOUCH_LIST;

            if(rowsUnderTouch && !catching) {
                u32 index = (u32) ((listData->scrollPos + (pos.py - by1)) / screen_get_font_height(0.5f));
                if(listData->selectedIndex == index) {
                    // Activation usually opens another view; this touch must not drag the list later.
                    listData->touchMode = LIST_TOUCH_NONE;
                    *selectedTouched = true;
                } else {
                    listData->selectedIndex = index;
                }
            }
        }

        listData->lastScrollTouchY = pos.py;
        listData->lastScrollTime = now;
    } else if((hidKeysHeld() & KEY_TOUCH) && listData->touchMode != LIST_TOUCH_NONE) {
        touchPosition pos;
        hidTouchRead(&pos);

        if(listData->touchMode == LIST_TOUCH_SCROLL_BAR) {
            list_drag_scroll_bar(listData, pos.py, by1, by2);
        } else {
            float delta = (float) listData->lastScrollTouchY - pos.py;
            listData->scrollPos += delta;

            // Smoothed so a slow-down or noisy sample right before lift-off does not decide the fling.
            u64 dt = now - listData->lastScrollTime;
            if(dt > 0) {
                listData->dragVelocity = 0.6f * (delta / dt) + 0.4f * listData->dragVelocity;
            }

            list_follow_scroll(listData, viewSize);
        }

        listData->lastScrollTouchY = pos.py;
        listData->lastScrollTime = now;
    } else {
        float velocity = listData->dragVelocity;
        if((hidKeysUp() & KEY_TOUCH) && listData->touchMode == LIST_TOUCH_LIST
           && (velocity >= FLING_START_VELOCITY || velocity <= -FLING_START_VELOCITY)) {
            if(velocity > FLING_MAX_VELOCITY) {
                velocity = FLING_MAX_VELOCITY;
            } else if(velocity < -FLING_MAX_VELOCITY) {
                velocity = -FLING_MAX_VELOCITY;
            }

            listData->scrollVelocity = velocity;
            listData->lastScrollTime = now;
        } else if(listData->scrollVelocity != 0) {
            // Capped so a list resuming under a popped view does not jump.
            float dt = now - listData->lastScrollTime;
            if(dt > 2 * FRAME_MS) {
                dt = 2 * FRAME_MS;
            }

            listData->scrollPos += listData->scrollVelocity * dt;
            listData->scrollVelocity *= 1 - FLING_DECAY_PER_MS * dt;

            bool stopped = listData->scrollVelocity < FLING_STOP_VELOCITY && listData->scrollVelocity > -FLING_STOP_VELOCITY;
            if(list_follow_scroll(listData, viewSize) || stopped) {
                listData->scrollVelocity = 0;
            }

            listData->lastScrollTime = now;
        }

        listData->touchMode = LIST_TOUCH_NONE;
    }
}

// Moves the selection by step on a press or held repeat. A step clamps at the list ends; pressing
// again from the first or last item wraps around.
static void list_move(list_data* listData, u32 key, int step, u32 size) {
    bool pressed = (hidKeysDown() & key) != 0;
    if(!pressed && !((hidKeysHeld() & key) && osGetTime() >= listData->nextActionTime)) {
        return;
    }

    u32 index = listData->selectedIndex;
    if(step > 0) {
        u32 remaining = size - 1 - index;
        listData->selectedIndex = remaining == 0 ? 0 : index + (remaining < (u32) step ? remaining : (u32) step);
    } else {
        listData->selectedIndex = index == 0 ? size - 1 : index - (index < (u32) -step ? index : (u32) -step);
    }

    // A key press wins over a running fling; list_validate then scrolls to the new selection.
    if(pressed) {
        listData->scrollVelocity = 0;
    }

    listData->nextActionTime = osGetTime() + (pressed ? 500 : 100);
}

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;

    u32 size = linked_list_size(&listData->items);

    list_validate(listData, by1, by2);

    bool selectedTouched = false;
    if(size > 0) {
        bool scrolls = false;
        if(listData->selectedItem != NULL && listData->layout != LIST_LAYOUT_GRID) {
            float rowWidth = listData->layout == LIST_LAYOUT_TOP ? TOP_SCREEN_WIDTH : bx2 - bx1;

            float itemWidth;
            screen_get_string_size(&itemWidth, NULL, listData->selectedItem->name, 0.5f, 0.5f);
            if(itemWidth > rowWidth) {
                scrolls = true;

                if(listData->selectionScroll == 0 || listData->selectionScroll >= itemWidth - rowWidth) {
                    if(listData->nextSelectionScrollResetTime == 0) {
                        listData->nextSelectionScrollResetTime = osGetTime() + 2000;
                    } else if(osGetTime() >= listData->nextSelectionScrollResetTime) {
                        listData->selectionScroll = listData->selectionScroll == 0 ? 1 : 0;
                        listData->nextSelectionScrollResetTime = 0;
                    }
                } else {
                    listData->selectionScroll++;
                }
            }
        }

        if(!scrolls) {
            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        u32 lastSelectedIndex = listData->selectedIndex;

        if(listData->layout == LIST_LAYOUT_GRID) {
            int columns = (int) list_columns(listData);
            int pageRows = (int) ((by2 - by1) / list_row_height(listData));
            int page = columns * (pageRows > 0 ? pageRows : 1);

            list_move(listData, KEY_DOWN, columns, size);
            list_move(listData, KEY_UP, -columns, size);
            list_move(listData, KEY_RIGHT, 1, size);
            list_move(listData, KEY_LEFT, -1, size);
            list_move(listData, KEY_R, page, size);
        } else {
            list_move(listData, KEY_DOWN, 1, size);
            list_move(listData, KEY_UP, -1, size);
            list_move(listData, KEY_RIGHT, 13, size);
            list_move(listData, KEY_LEFT, -13, size);
        }

        list_update_touch(listData, bx2, by1, by2, &selectedTouched);

        if(listData->selectedIndex != lastSelectedIndex) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        list_validate(listData, by1, by2);
    }

    if(listData->update != NULL) {
        listData->update(view, listData->data, &listData->items, listData->selectedItem, selectedTouched);
    }
}

static void list_draw_scroll_bar(list_data* listData, float x2, float y1, float y2) {
    float scrollBarY = 0;
    float scrollBarHeight = 0;
    if(list_get_scroll_bar(listData, y1, y2, &scrollBarY, &scrollBarHeight)) {
        u32 scrollBarWidth = 0;
        screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

        screen_draw_texture(TEXTURE_SCROLL_BAR, x2 - scrollBarWidth, scrollBarY, scrollBarWidth, scrollBarHeight);
    }
}

static void list_draw_grid(list_data* listData, float x1, float y1, float x2, float y2) {
    u32 scrollBarWidth = 0;
    screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

    u32 iconSize = list_grid_sizes[listData->gridSize].icon;
    u32 columns = list_grid_sizes[listData->gridSize].columns;
    u32 rowHeight = list_grid_sizes[listData->gridSize].rowHeight;

    // Whole-pixel cells, centered in the space left of the scroll bar.
    float gridWidth = x2 - x1 - scrollBarWidth;
    float cellWidth = (float) (u32) (gridWidth / columns);
    float gridX = x1 + (float) (u32) ((gridWidth - cellWidth * columns) / 2);

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);

    u32 index = 0;
    while(linked_list_iter_has_next(&iter)) {
        float y = y1 - listData->scrollPos + index / columns * rowHeight;
        if(y > y2) {
            break;
        }

        list_item* item = linked_list_iter_next(&iter);

        if(y > y1 - rowHeight) {
            float x = gridX + index % columns * cellWidth;

            // Icons scale with the zoom level from their native size (CTR 48, TWL 32); a title without
            // one gets a gray tile.
            u32 icon = listData->itemIcon != NULL ? listData->itemIcon(item) : 0;
            u32 iconWidth = GRID_NATIVE_ICON;
            u32 iconHeight = GRID_NATIVE_ICON;
            if(icon != 0) {
                screen_get_texture_size(&iconWidth, &iconHeight, icon);
            }

            iconWidth = iconWidth * iconSize / GRID_NATIVE_ICON;
            iconHeight = iconHeight * iconSize / GRID_NATIVE_ICON;

            // Whole-pixel offsets keep the selection ring the same width on every side.
            float iconX = x + (u32) ((cellWidth - iconWidth) / 2);
            float iconY = y + (rowHeight - iconHeight) / 2;

            // Selection: a solid accent ring around the icon (the accent tile is the progress bar fill).
            if(item == listData->selectedItem) {
                screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, iconX - GRID_SELECTION_BORDER, iconY - GRID_SELECTION_BORDER,
                                    iconWidth + GRID_SELECTION_BORDER * 2, iconHeight + GRID_SELECTION_BORDER * 2);
            }

            screen_draw_texture(icon != 0 ? icon : TEXTURE_SCROLL_BAR, iconX, iconY, iconWidth, iconHeight);
        }

        index++;
    }

    list_draw_scroll_bar(listData, x2, y1, y2);
}

static void list_draw_rows(list_data* listData, float x1, float y1, float x2, float y2) {
    if(listData->layout == LIST_LAYOUT_GRID) {
        list_draw_grid(listData, x1, y1, x2, y2);
        return;
    }

    float fontHeight = screen_get_font_height(0.5f);
    float y = y1 - listData->scrollPos;

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        if(y > y2) {
            break;
        }

        list_item* item = linked_list_iter_next(&iter);

        if(y > y1 - fontHeight) {
            float x = x1 + 2;
            if(item == listData->selectedItem) {
                x -= listData->selectionScroll;
            }

            // Overlay goes behind the text so an opaque selection fill keeps the row readable.
            if(item == listData->selectedItem) {
                screen_draw_texture(TEXTURE_SELECTION_OVERLAY, x1, y, x2 - x1, fontHeight);
            }

            screen_draw_string(item->name, x, y, 0.5f, 0.5f, item->color, true);
        }

        y += fontHeight;
    }

    list_draw_scroll_bar(listData, x2, y1, y2);
}

// The info callback (list_display's drawTop) goes on whichever screen the rows are not on.
static void list_draw_info(ui_view* view, list_data* listData, float x1, float y1, float x2, float y2) {
    if(listData->drawTop != NULL) {
        list_validate(listData, y1, y2);

        listData->drawTop(view, listData->data, x1, y1, x2, y2, listData->selectedItem);
    }
}

static void list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->layout == LIST_LAYOUT_BOTTOM) {
        list_draw_info(view, listData, x1, y1, x2, y2);
    } else {
        list_validate(listData, y1, y2);
        list_draw_rows(listData, x1, y1, x2, y2);
    }
}

static void list_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->layout == LIST_LAYOUT_BOTTOM) {
        list_validate(listData, y1, y2);
        list_draw_rows(listData, x1, y1, x2, y2);
    } else {
        list_draw_info(view, listData, x1, y1, x2, y2);
    }
}

// Row heights differ per layout and grid size; list_validate scrolls back to the kept selection.
static void list_reset_scroll(list_data* listData) {
    listData->scrollPos = 0;
    listData->touchMode = LIST_TOUCH_NONE;
    listData->dragVelocity = 0;
    listData->scrollVelocity = 0;
    listData->selectionScroll = 0;
    listData->nextSelectionScrollResetTime = 0;
}

ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected)) {
    list_data* listData = (list_data*) calloc(1, sizeof(list_data));
    if(listData == NULL) {
        error_display(NULL, NULL, "Failed to allocate list data.");

        return NULL;
    }

    listData->data = data;
    linked_list_init(&listData->items);
    listData->selectedIndex = 0;
    listData->selectedItem = NULL;
    listData->selectionScroll = 0;
    listData->nextSelectionScrollResetTime = 0;
    listData->scrollPos = 0;
    listData->lastScrollTouchY = 0;
    listData->touchMode = LIST_TOUCH_NONE;
    listData->scrollBarGrabOffset = 0;
    listData->dragVelocity = 0;
    listData->scrollVelocity = 0;
    listData->lastScrollTime = 0;
    listData->layout = LIST_LAYOUT_BOTTOM;
    listData->gridSize = LIST_GRID_SIZE_DEFAULT;
    listData->itemIcon = NULL;
    listData->update = update;
    listData->drawTop = drawTop;

    ui_view* view = ui_create();
    view->name = name;
    view->info = info;
    view->data = listData;
    view->update = list_update;
    view->drawTop = list_draw_top;
    view->drawBottom = list_draw_bottom;
    ui_push(view);

    return view;
}

void list_set_layout(ui_view* view, list_layout layout, u32 (*itemIcon)(list_item* item)) {
    list_data* listData = (list_data*) view->data;

    listData->layout = layout;
    listData->itemIcon = itemIcon;
    list_reset_scroll(listData);
}

void list_set_grid_size(ui_view* view, u32 size) {
    list_data* listData = (list_data*) view->data;

    listData->gridSize = size < LIST_GRID_SIZE_COUNT ? size : LIST_GRID_SIZE_DEFAULT;
    list_reset_scroll(listData);
}

void list_destroy(ui_view* view) {
    if(view != NULL) {
        linked_list_destroy(&((list_data*) view->data)->items);

        free(view->data);
        ui_destroy(view);
    }
}