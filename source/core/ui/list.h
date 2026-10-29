#pragma once

#define LIST_ITEM_NAME_MAX 512

typedef struct linked_list_s linked_list;
typedef struct ui_view_s ui_view;

typedef struct list_item_s {
    char name[LIST_ITEM_NAME_MAX];
    u32 color;
    void* data;
    // A blank row that is never selected: keys and scrolling step past it, taps ignore it. Text row layouts (BOTTOM, TOP) only.
    bool spacer;
} list_item;

// Where the rows go. BOTTOM is the default; TOP, GRID, and ICON_LIST put the rows on the top screen
// and the drawTop callback (item info) on the bottom screen, where touch only scrolls. ICON_LIST rows
// show the item's icon, its name, and detail lines.
typedef enum list_layout_e {
    LIST_LAYOUT_BOTTOM,
    LIST_LAYOUT_TOP,
    LIST_LAYOUT_GRID,
    LIST_LAYOUT_ICON_LIST
} list_layout;

// Zoom levels of GRID and ICON_LIST, smallest first; the default draws 48 px icons 1:1.
#define LIST_ZOOM_COUNT 2
#define LIST_ZOOM_DEFAULT 1

ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected));
// itemIcon returns an item's texture id, or 0 for none; GRID and ICON_LIST use it. itemDetail writes
// an item's detail lines, separated by '\n'; ICON_LIST draws as many as its zoom level has room for.
// Either may be NULL.
void list_set_layout(ui_view* view, list_layout layout, u32 (*itemIcon)(list_item* item),
                     void (*itemDetail)(list_item* item, char* out, size_t size));
// L and R page back and forward by a screen of rows, in every layout. Off by default (FBI's lists).
void list_set_paging(ui_view* view, bool paging);
// zoom: 0 .. LIST_ZOOM_COUNT - 1. Applies to GRID and ICON_LIST.
void list_set_zoom(ui_view* view, u32 zoom);
// Section headers: label writes an item's section name, and a header starts wherever it differs from
// the previous item's. Sections follow the items' order and rebuild on their own when the items change.
// NULL (the default) draws no headers. Setting the same callback again does nothing.
void list_set_sections(ui_view* view, void (*label)(void* data, list_item* item, char* out, size_t size));
// While loading, the rows show a loading animation, input is ignored, and the update and drawTop
// callbacks get no selection. Both directions start again at the first item.
void list_set_loading(ui_view* view, bool loading);
void list_destroy(ui_view* view);