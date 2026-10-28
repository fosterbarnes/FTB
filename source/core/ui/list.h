#pragma once

#define LIST_ITEM_NAME_MAX 512

typedef struct linked_list_s linked_list;
typedef struct ui_view_s ui_view;

typedef struct list_item_s {
    char name[LIST_ITEM_NAME_MAX];
    u32 color;
    void* data;
} list_item;

// Where the rows go. BOTTOM is the default; TOP and GRID put the rows on the top screen and the
// drawTop callback (item info) on the bottom screen, where touch only scrolls.
typedef enum list_layout_e {
    LIST_LAYOUT_BOTTOM,
    LIST_LAYOUT_TOP,
    LIST_LAYOUT_GRID
} list_layout;

// Grid zoom levels, smallest first; the default draws 48 px icons 1:1.
#define LIST_GRID_SIZE_COUNT 4
#define LIST_GRID_SIZE_DEFAULT 1

ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected));
// itemIcon returns an item's texture id, or 0 for none; only GRID uses it.
void list_set_layout(ui_view* view, list_layout layout, u32 (*itemIcon)(list_item* item));
// size: 0 .. LIST_GRID_SIZE_COUNT - 1.
void list_set_grid_size(ui_view* view, u32 size);
void list_destroy(ui_view* view);