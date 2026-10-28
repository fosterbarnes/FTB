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
    void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected);
} list_data;

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
        float fontHeight = screen_get_font_height(0.5f);

        bool found = false;
        if(listData->selectedItem != NULL) {
            u32 oldIndex = listData->selectedIndex;

            int index = linked_list_index_of(&listData->items, listData->selectedItem);
            if(index != -1) {
                found = true;
                listData->selectedIndex = (u32) index;

                if(listData->selectedIndex != oldIndex) {
                    listData->scrollPos += (listData->selectedIndex * fontHeight) - (oldIndex * fontHeight);
                }
            }
        }

        if(!found) {
            listData->selectedItem = linked_list_get(&listData->items, listData->selectedIndex);

            listData->selectionScroll = 0;
            listData->nextSelectionScrollResetTime = 0;
        }

        float itemY = listData->selectedIndex * fontHeight;

        float minItemScrollPos = itemY - (viewSize - fontHeight);
        if(listData->scrollPos < minItemScrollPos) {
            listData->scrollPos = minItemScrollPos;
        }

        if(listData->scrollPos > itemY) {
            listData->scrollPos = itemY;
        }

        float maxScrollPos = (size * fontHeight) - viewSize;
        if(listData->scrollPos > maxScrollPos) {
            listData->scrollPos = maxScrollPos;
        }

        if(listData->scrollPos < 0) {
            listData->scrollPos = 0;
        }
    }
}

static float list_get_max_scroll_pos(list_data* listData, float viewSize) {
    float maxScrollPos = linked_list_size(&listData->items) * screen_get_font_height(0.5f) - viewSize;
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

    float fontHeight = screen_get_font_height(0.5f);
    u32 first = (u32) (listData->scrollPos / fontHeight);
    if(first * fontHeight < listData->scrollPos) {
        first++;
    }

    u32 visible = (u32) ((listData->scrollPos + viewSize) / fontHeight);
    u32 last = visible > first ? visible - 1 : first;
    u32 size = linked_list_size(&listData->items);
    if(last > size - 1) {
        last = size - 1;
    }

    if(listData->selectedIndex < first) {
        listData->selectedIndex = first;
    } else if(listData->selectedIndex > last) {
        listData->selectedIndex = last;
    }

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

        float barY = 0;
        float barHeight = 0;
        if(list_get_scroll_bar(listData, by1, by2, &barY, &barHeight) && pos.px >= bx2 - 2 * scrollBarWidth) {
            listData->touchMode = LIST_TOUCH_SCROLL_BAR;
            // Grabbing the bar keeps its offset; touching the track centers the bar on the stylus.
            listData->scrollBarGrabOffset = pos.py >= barY && pos.py < barY + barHeight ? pos.py - barY : barHeight / 2;
            list_drag_scroll_bar(listData, pos.py, by1, by2);
        } else {
            listData->touchMode = LIST_TOUCH_LIST;

            if(!catching) {
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

static void list_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    list_data* listData = (list_data*) data;

    u32 size = linked_list_size(&listData->items);

    list_validate(listData, by1, by2);

    bool selectedTouched = false;
    if(size > 0) {
        bool scrolls = false;
        if(listData->selectedItem != NULL) {
            float itemWidth;
            screen_get_string_size(&itemWidth, NULL, listData->selectedItem->name, 0.5f, 0.5f);
            if(itemWidth > bx2 - bx1) {
                scrolls = true;

                if(listData->selectionScroll == 0 || listData->selectionScroll >= itemWidth - (bx2 - bx1)) {
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

        if((hidKeysDown() & KEY_DOWN) || ((hidKeysHeld() & KEY_DOWN) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex < size - 1) {
                listData->selectedIndex++;
            } else {
                listData->selectedIndex = 0;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_DOWN) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_UP) || ((hidKeysHeld() & KEY_UP) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex > 0) {
                listData->selectedIndex--;
            } else {
                listData->selectedIndex = size - 1;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_UP) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_RIGHT) || ((hidKeysHeld() & KEY_RIGHT) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex < size - 1) {
                u32 remaining = size - 1 - listData->selectedIndex;
                listData->selectedIndex += remaining < 13 ? remaining : 13;
            } else {
                listData->selectedIndex = 0;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_RIGHT) ? 500 : 100);
        }

        if((hidKeysDown() & KEY_LEFT) || ((hidKeysHeld() & KEY_LEFT) && osGetTime() >= listData->nextActionTime)) {
            if(listData->selectedIndex > 0) {
                u32 remaining = listData->selectedIndex;
                listData->selectedIndex -= remaining < 13 ? remaining : 13;
            } else {
                listData->selectedIndex = size - 1;
            }

            listData->nextActionTime = osGetTime() + ((hidKeysDown() & KEY_LEFT) ? 500 : 100);
        }

        // D-pad wins over a running fling; list_validate then scrolls to the new selection.
        if(hidKeysDown() & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            listData->scrollVelocity = 0;
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

static void list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    if(listData->drawTop != NULL) {
        list_validate(listData, y1, y2);

        listData->drawTop(view, listData->data, x1, y1, x2, y2, listData->selectedItem);
    }
}

static void list_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    list_data* listData = (list_data*) data;

    list_validate(listData, y1, y2);

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
                u32 selectionOverlayWidth = 0;
                screen_get_texture_size(&selectionOverlayWidth, NULL, TEXTURE_SELECTION_OVERLAY);
                screen_draw_texture(TEXTURE_SELECTION_OVERLAY, (x1 + x2 - selectionOverlayWidth) / 2, y, selectionOverlayWidth, fontHeight);
            }

            screen_draw_string(item->name, x, y, 0.5f, 0.5f, item->color, true);
        }

        y += fontHeight;
    }

    float scrollBarY = 0;
    float scrollBarHeight = 0;
    if(list_get_scroll_bar(listData, y1, y2, &scrollBarY, &scrollBarHeight)) {
        u32 scrollBarWidth = 0;
        screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

        screen_draw_texture(TEXTURE_SCROLL_BAR, x2 - scrollBarWidth, scrollBarY, scrollBarWidth, scrollBarHeight);
    }
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

void list_destroy(ui_view* view) {
    if(view != NULL) {
        linked_list_destroy(&((list_data*) view->data)->items);

        free(view->data);
        ui_destroy(view);
    }
}