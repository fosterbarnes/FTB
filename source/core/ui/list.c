#include <malloc.h>
#include <string.h>

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

// Loading: centered dots in place of the rows, shown after a delay so a fast load never flashes them.
#define LOADING_DELAY_MS 200
#define LOADING_DOT 8

// Grid layout: icon cells on the top screen. Per zoom level: icon edge, columns across the 390 px
// left of the scroll bar, row height. Icons scale by icon / GRID_NATIVE_ICON (CTR 48, TWL 32 native),
// and each cell leaves room for the selection ring.
#define GRID_NATIVE_ICON 48
#define GRID_SELECTION_BORDER 4

// Section header: a text line, then 1 px gap, 1 px divider, 2 px gap. Same height in every layout.
#define SECTION_HEADER_PAD 4
#define SECTION_LABEL_MAX 64

static const struct {
    u32 icon;
    u32 columns;
    u32 rowHeight;
} list_grid_sizes[LIST_GRID_SIZE_COUNT] = {
        {32, 9, 44},
        {48, 7, 64},
};

enum {
    LIST_TOUCH_NONE,
    LIST_TOUCH_LIST,
    LIST_TOUCH_SCROLL_BAR
};

// Items [start, next section's start) under one header. rowsBefore counts the item rows above it.
typedef struct {
    u32 start;
    u32 rowsBefore;
} list_section;

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
    bool loading;
    u64 loadingStart;
    // Sections. Without headers the list is one section with header height 0, which is the plain
    // layout, so every position goes through the same section math.
    void (*sectionLabel)(void* data, list_item* item, char* out, size_t size);
    list_section* sections; // &single or sectionBuffer
    u32 sectionCount;
    list_section single;
    list_section* sectionBuffer;
    u32 sectionCapacity;
    float headerHeight;
    bool sectionsDirty;
    bool sectionsFailed; // allocation failed: this list stays without headers
    bool sectionsReported; // the failure was shown (from list_update, not mid-draw)
    unsigned int sectionVersion;
    u32 sectionColumns;
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

// Rebuilds the sections when the items, the column count, or the label callback changed. Runs at the
// start of every validate, so update and draw never read sections older than the items they walk. The
// populate thread may still append meanwhile; appended items fall into the last section until the next
// rebuild.
static void list_update_sections(list_data* listData) {
    u32 columns = list_columns(listData);
    unsigned int version = listData->items.version;
    if(!listData->sectionsDirty && listData->sectionVersion == version && listData->sectionColumns == columns) {
        return;
    }

    listData->sectionsDirty = false;
    listData->sectionVersion = version;
    listData->sectionColumns = columns;
    listData->sections = &listData->single;
    listData->sectionCount = 1;
    listData->headerHeight = 0;

    // A loading list is hidden and its order is not final yet.
    u32 size = linked_list_size(&listData->items);
    if(listData->sectionLabel == NULL || listData->loading || listData->sectionsFailed || size == 0) {
        return;
    }

    if(listData->sectionCapacity < size) {
        list_section* buffer = (list_section*) realloc(listData->sectionBuffer, size * sizeof(list_section));
        if(buffer == NULL) {
            listData->sectionsFailed = true;
            return;
        }

        listData->sectionBuffer = buffer;
        listData->sectionCapacity = size;
    }

    list_section* sections = listData->sectionBuffer;
    char labels[2][SECTION_LABEL_MAX];
    u32 count = 0;
    u32 rows = 0;
    u32 index = 0;

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);

    // Bounded by the size read above: items the populate thread appends meanwhile wait for the next rebuild.
    while(index < size && linked_list_iter_has_next(&iter)) {
        char* label = labels[index % 2];
        listData->sectionLabel(listData->data, linked_list_iter_next(&iter), label, SECTION_LABEL_MAX);

        if(index == 0 || strcmp(label, labels[(index + 1) % 2]) != 0) {
            if(count > 0) {
                rows += (index - sections[count - 1].start + columns - 1) / columns;
            }

            sections[count++] = (list_section) {index, rows};
        }

        index++;
    }

    listData->sections = sections;
    listData->sectionCount = count;
    listData->headerHeight = screen_get_font_height(0.5f) + SECTION_HEADER_PAD;
}

// The last section starting at or before index.
static u32 list_section_of_item(list_data* listData, u32 index) {
    u32 low = 0;
    u32 high = listData->sectionCount - 1;
    while(low < high) {
        u32 mid = (low + high + 1) / 2;
        if(listData->sections[mid].start <= index) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    return low;
}

// The section holding item row row. Every section has at least one row, so rowsBefore only increases.
static u32 list_section_of_row(list_data* listData, u32 row) {
    u32 low = 0;
    u32 high = listData->sectionCount - 1;
    while(low < high) {
        u32 mid = (low + high + 1) / 2;
        if(listData->sections[mid].rowsBefore <= row) {
            low = mid;
        } else {
            high = mid - 1;
        }
    }

    return low;
}

static u32 list_section_end(list_data* listData, u32 section) {
    return section + 1 < listData->sectionCount ? listData->sections[section + 1].start : linked_list_size(&listData->items);
}

// Item rows only; headers are not rows.
static u32 list_row_count(list_data* listData) {
    u32 size = linked_list_size(&listData->items);
    if(size == 0) {
        return 0;
    }

    u32 columns = list_columns(listData);
    const list_section* last = &listData->sections[listData->sectionCount - 1];
    return last->rowsBefore + (size - last->start + columns - 1) / columns;
}

static u32 list_item_row(list_data* listData, u32 index) {
    const list_section* section = &listData->sections[list_section_of_item(listData, index)];
    return section->rowsBefore + (index - section->start) / list_columns(listData);
}

static u32 list_item_column(list_data* listData, u32 index) {
    const list_section* section = &listData->sections[list_section_of_item(listData, index)];
    return (index - section->start) % list_columns(listData);
}

// Content y of a row's top: the rows above it plus the headers above it, its own section's included.
static float list_row_y(list_data* listData, u32 row) {
    return row * list_row_height(listData) + (list_section_of_row(listData, row) + 1) * listData->headerHeight;
}

static bool list_row_starts_section(list_data* listData, u32 row) {
    return listData->sections[list_section_of_row(listData, row)].rowsBefore == row;
}

static float list_content_height(list_data* listData) {
    u32 rows = list_row_count(listData);
    return rows > 0 ? rows * list_row_height(listData) + listData->sectionCount * listData->headerHeight : 0;
}

// Number of rows whose top is at or above content y.
static u32 list_rows_at_or_above(list_data* listData, float y) {
    u32 low = 0;
    u32 high = list_row_count(listData);
    while(low < high) {
        u32 mid = (low + high) / 2;
        if(list_row_y(listData, mid) <= y) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }

    return low;
}

// A row's item at column, clamped to the row's section: a section's last row may be short.
static u32 list_row_item(list_data* listData, u32 row, u32 column) {
    u32 section = list_section_of_row(listData, row);
    u32 index = listData->sections[section].start + (row - listData->sections[section].rowsBefore) * list_columns(listData) + column;
    u32 end = list_section_end(listData, section);
    return index < end ? index : end - 1;
}

// The row covering content y; false in a header or past the last row.
static bool list_row_at_y(list_data* listData, float y, u32* row) {
    u32 above = list_rows_at_or_above(listData, y);
    if(above == 0 || y >= list_row_y(listData, above - 1) + list_row_height(listData)) {
        return false;
    }

    *row = above - 1;
    return true;
}

// Every content rect is 200 px tall, so validating against the top or bottom rect is the same.
static void list_validate(list_data* listData, float by1, float by2) {
    list_update_sections(listData);

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

        bool found = false;
        if(listData->selectedItem != NULL) {
            u32 oldIndex = listData->selectedIndex;

            int index = linked_list_index_of(&listData->items, listData->selectedItem);
            if(index != -1) {
                found = true;
                listData->selectedIndex = (u32) index;

                if(listData->selectedIndex != oldIndex) {
                    listData->scrollPos += list_row_y(listData, list_item_row(listData, listData->selectedIndex))
                                           - list_row_y(listData, list_item_row(listData, oldIndex));
                }
            }
        }

        if(!found) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        u32 row = list_item_row(listData, listData->selectedIndex);
        float itemY = list_row_y(listData, row);

        float minItemScrollPos = itemY - (viewSize - rowHeight);
        if(listData->scrollPos < minItemScrollPos) {
            listData->scrollPos = minItemScrollPos;
        }

        // A section's first row brings its header into view too.
        float itemTop = list_row_starts_section(listData, row) ? itemY - listData->headerHeight : itemY;
        if(listData->scrollPos > itemTop) {
            listData->scrollPos = itemTop;
        }

        float maxScrollPos = list_content_height(listData) - viewSize;
        if(listData->scrollPos > maxScrollPos) {
            listData->scrollPos = maxScrollPos;
        }

        if(listData->scrollPos < 0) {
            listData->scrollPos = 0;
        }
    }
}

static float list_get_max_scroll_pos(list_data* listData, float viewSize) {
    float maxScrollPos = list_content_height(listData) - viewSize;
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

    u32 rows = list_row_count(listData);
    if(rows == 0) {
        return clamped;
    }

    // Rows, not items: a grid keeps the selected column. first is the first row whose top is in view,
    // last the last row whose bottom is.
    float rowHeight = list_row_height(listData);
    u32 above = list_rows_at_or_above(listData, listData->scrollPos);
    u32 first = above > 0 && list_row_y(listData, above - 1) == listData->scrollPos ? above - 1 : above;
    u32 visible = list_rows_at_or_above(listData, listData->scrollPos + viewSize - rowHeight);
    u32 last = visible > first ? visible - 1 : first;
    if(first > rows - 1) {
        first = rows - 1;
    }

    if(last > rows - 1) {
        last = rows - 1;
    }

    u32 row = list_item_row(listData, listData->selectedIndex);
    if(row < first) {
        row = first;
    } else if(row > last) {
        row = last;
    }

    listData->selectedIndex = list_row_item(listData, row, list_item_column(listData, listData->selectedIndex));

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

            // Only a row under the stylus counts: a tap on the top or bottom bar, a section header, or
            // below the last row used to select (and on a second tap open) a row that was not touched.
            u32 index = UINT32_MAX;
            u32 row = 0;
            if(rowsUnderTouch && !catching && pos.py >= by1 && pos.py < by2
               && list_row_at_y(listData, listData->scrollPos + (pos.py - by1), &row)) {
                index = list_row_item(listData, row, 0);
            }

            if(index < linked_list_size(&listData->items)) {
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

// True on a press or held repeat of key.
static bool list_key_fires(list_data* listData, u32 key) {
    bool pressed = (hidKeysDown() & key) != 0;
    if(!pressed && !((hidKeysHeld() & key) && osGetTime() >= listData->nextActionTime)) {
        return false;
    }

    // A key press wins over a running fling; list_validate then scrolls to the new selection.
    if(pressed) {
        listData->scrollVelocity = 0;
    }

    listData->nextActionTime = osGetTime() + (pressed ? 500 : 100);
    return true;
}

// Moves the selection by step items. A step clamps at the list ends; pressing again from the first or
// last item wraps around.
static void list_step_items(list_data* listData, int step, u32 size) {
    u32 index = listData->selectedIndex;
    if(step > 0) {
        u32 remaining = size - 1 - index;
        listData->selectedIndex = remaining == 0 ? 0 : index + (remaining < (u32) step ? remaining : (u32) step);
    } else {
        listData->selectedIndex = index == 0 ? size - 1 : index - (index < (u32) -step ? index : (u32) -step);
    }
}

// Moves the selection by step grid rows in the same column, clamped on a section's short last row.
// Ends clamp and wrap like list_step_items.
static void list_step_rows(list_data* listData, int step, u32 size) {
    u32 index = listData->selectedIndex;
    u32 row = list_item_row(listData, index);
    u32 column = list_item_column(listData, index);
    if(step > 0) {
        if(row + (u32) step >= list_row_count(listData)) {
            listData->selectedIndex = index == size - 1 ? 0 : size - 1;
        } else {
            listData->selectedIndex = list_row_item(listData, row + (u32) step, column);
        }
    } else {
        if(row < (u32) -step) {
            listData->selectedIndex = index == 0 ? size - 1 : 0;
        } else {
            listData->selectedIndex = list_row_item(listData, row - (u32) -step, column);
        }
    }
}

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;

    u32 size = linked_list_size(&listData->items);

    list_validate(listData, by1, by2);

    if(listData->sectionsFailed && !listData->sectionsReported) {
        listData->sectionsReported = true;
        error_display(NULL, NULL, "Failed to allocate list sections.");
    }

    if(listData->loading) {
        if(listData->update != NULL) {
            listData->update(view, listData->data, &listData->items, NULL, false);
        }

        return;
    }

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

        // Grid: up, down, and R move by rows (sections can end in a short row); left and right by items.
        if(listData->layout == LIST_LAYOUT_GRID) {
            int pageRows = (int) ((by2 - by1) / list_row_height(listData));

            if(list_key_fires(listData, KEY_DOWN)) {
                list_step_rows(listData, 1, size);
            }

            if(list_key_fires(listData, KEY_UP)) {
                list_step_rows(listData, -1, size);
            }

            if(list_key_fires(listData, KEY_RIGHT)) {
                list_step_items(listData, 1, size);
            }

            if(list_key_fires(listData, KEY_LEFT)) {
                list_step_items(listData, -1, size);
            }

            if(list_key_fires(listData, KEY_R)) {
                list_step_rows(listData, pageRows > 0 ? pageRows : 1, size);
            }
        } else {
            if(list_key_fires(listData, KEY_DOWN)) {
                list_step_items(listData, 1, size);
            }

            if(list_key_fires(listData, KEY_UP)) {
                list_step_items(listData, -1, size);
            }

            if(list_key_fires(listData, KEY_RIGHT)) {
                list_step_items(listData, 13, size);
            }

            if(list_key_fires(listData, KEY_LEFT)) {
                list_step_items(listData, -13, size);
            }
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

// The header above item (its section's first), whose top is at y: label, then a divider across width.
// Only labels on screen are formatted.
static void list_draw_header(list_data* listData, list_item* item, float x, float y, float width, float y1, float y2) {
    if(y + listData->headerHeight <= y1 || y >= y2) {
        return;
    }

    char label[SECTION_LABEL_MAX];
    listData->sectionLabel(listData->data, item, label, sizeof(label));

    screen_draw_string(label, x + 2, y, 0.5f, 0.5f, COLOR_TEXT, true);
    screen_draw_texture(TEXTURE_SCROLL_BAR, x, y + listData->headerHeight - SECTION_HEADER_PAD + 1, width, 1);
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

    // Walks items in order; the section advances at each section's first item.
    u32 section = 0;
    u32 index = 0;
    while(linked_list_iter_has_next(&iter)) {
        if(section + 1 < listData->sectionCount && listData->sections[section + 1].start == index) {
            section++;
        }

        const list_section* current = &listData->sections[section];
        u32 offset = index - current->start;
        float y = y1 - listData->scrollPos + (current->rowsBefore + offset / columns) * rowHeight + (section + 1) * listData->headerHeight;
        if(y - listData->headerHeight > y2) {
            break;
        }

        list_item* item = linked_list_iter_next(&iter);

        if(offset == 0 && listData->headerHeight > 0) {
            list_draw_header(listData, item, gridX, y - listData->headerHeight, cellWidth * columns, y1, y2);
        }

        if(y > y1 - rowHeight && y <= y2) {
            float x = gridX + offset % columns * cellWidth;

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
    if(listData->loading) {
        if(osGetTime() - listData->loadingStart >= LOADING_DELAY_MS) {
            float width = ui_get_loading_dots_width(LOADING_DOT);
            ui_draw_loading_dots(x1 + (x2 - x1 - width) / 2, y1 + (y2 - y1 - LOADING_DOT) / 2, LOADING_DOT);
        }

        return;
    }

    if(listData->layout == LIST_LAYOUT_GRID) {
        list_draw_grid(listData, x1, y1, x2, y2);
        return;
    }

    float fontHeight = screen_get_font_height(0.5f);
    float y = y1 - listData->scrollPos;

    u32 scrollBarWidth = 0;
    screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

    linked_list_iter iter;
    linked_list_iterate(&listData->items, &iter);

    // One item per row; each section's header comes before its first item.
    u32 section = 0;
    u32 index = 0;
    while(linked_list_iter_has_next(&iter)) {
        if(y > y2) {
            break;
        }

        list_item* item = linked_list_iter_next(&iter);

        if(section + 1 < listData->sectionCount && listData->sections[section + 1].start == index) {
            section++;
        }

        if(listData->headerHeight > 0 && listData->sections[section].start == index) {
            list_draw_header(listData, item, x1, y, x2 - x1 - scrollBarWidth, y1, y2);
            y += listData->headerHeight;
        }

        index++;

        if(y > y1 - fontHeight && y <= y2) {
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

        listData->drawTop(view, listData->data, x1, y1, x2, y2, listData->loading ? NULL : listData->selectedItem);
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
    listData->sections = &listData->single;
    listData->sectionCount = 1;
    listData->sectionsDirty = true;
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

void list_set_loading(ui_view* view, bool loading) {
    list_data* listData = (list_data*) view->data;

    listData->loading = loading;
    listData->loadingStart = osGetTime();
    listData->sectionsDirty = true;

    listData->selectedIndex = 0;
    listData->selectedItem = NULL;
    list_reset_scroll(listData);
}

void list_set_sections(ui_view* view, void (*label)(void* data, list_item* item, char* out, size_t size)) {
    list_data* listData = (list_data*) view->data;

    if(listData->sectionLabel != label) {
        listData->sectionLabel = label;
        listData->sectionsDirty = true;
    }
}

void list_destroy(ui_view* view) {
    if(view != NULL) {
        list_data* listData = (list_data*) view->data;
        linked_list_destroy(&listData->items);
        free(listData->sectionBuffer);

        free(view->data);
        ui_destroy(view);
    }
}