#pragma once

#define LIST_ITEM_NAME_MAX 512

typedef struct linked_list_s linked_list;
typedef struct ui_view_s ui_view;

typedef struct list_item_s {
    char name[LIST_ITEM_NAME_MAX];
    u32 color;
    void* data;
    // A blank row that is never selected: keys and scrolling step past it, taps ignore it. TEXT rows only.
    bool spacer;
} list_item;

// How rows look: TEXT (FBI's text rows, the default), ICON_LIST (the item's icon, its name, and detail
// lines), GRID (icon cells).
typedef enum list_style_e {
    LIST_STYLE_TEXT,
    LIST_STYLE_ICON_LIST,
    LIST_STYLE_GRID
} list_style;

// Zoom levels per style, smallest first; each default draws 48 px icons 1:1. TEXT has one level.
#define LIST_GRID_ZOOM_COUNT 2
#define LIST_GRID_ZOOM_DEFAULT 1
#define LIST_ICON_LIST_ZOOM_COUNT 3
#define LIST_ICON_LIST_ZOOM_DEFAULT 2

ui_view* list_display(const char* name, const char* info, void* data, void (*update)(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched),
                                                                      void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected));
// Rows go on the bottom screen by default (TEXT there is FBI's list). rowsOnTop puts them on the top
// screen and the drawTop callback (item info) on the bottom, where touch only scrolls. itemIcon returns
// an item's texture id, or 0 for none; GRID and ICON_LIST use it. itemDetail writes an item's detail
// lines, separated by '\n'; ICON_LIST draws as many as its zoom level has room for. Either may be NULL.
void list_set_layout(ui_view* view, list_style style, bool rowsOnTop, u32 (*itemIcon)(list_item* item),
                     void (*itemDetail)(list_item* item, char* out, size_t size));
u32 list_zoom_count(list_style style);
// zoom: 0 .. list_zoom_count(style) - 1 for the current style; anything else picks its default.
void list_set_zoom(ui_view* view, u32 zoom);
// Sections: label writes an item's section name, and a section starts wherever it differs from the
// previous item's. Sections follow the items' order and rebuild on their own when the items change.
// headers draws a header over each section; without them the rows lay out as with no sections, and
// the sections only serve the section keys. NULL (the default) is one section. Setting the same values
// again does nothing.
void list_set_sections(ui_view* view, void (*label)(void* data, list_item* item, char* out, size_t size), bool headers);
// R: the next section's first item (stays put on the last section). L: the current section's first
// item, or the previous section's when already there (stays put on the first item). With one section R
// goes to the last item and L to the first. Off by default (FBI's lists).
void list_set_section_keys(ui_view* view, bool sectionKeys);
// While loading, the rows show a loading animation, input is ignored, and the update and drawTop
// callbacks get no selection. Both directions start again at the first item.
void list_set_loading(ui_view* view, bool loading);
void list_destroy(ui_view* view);