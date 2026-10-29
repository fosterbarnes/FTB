#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "task/uitask.h"
#include "../core/core.h"

// Title Manager: FTB's title list. Layout, zoom, sort, grouping, headers, and category filters live
// in settings (titleLayout etc.) and change from Options, X (sort), or the bottom-right touch buttons
// (sort, layout, grid zoom). L and R page. FBI's original screen stays in titles.c as FBI Legacy > Titles.

#define TITLEMANAGER_INFO "A: Select, B: Return, X: Sort, L/R: Page, Select: Options"

// Touch buttons, bottom-right of the bottom screen in every layout:
//         [Zoom]
//   [Sort][Layout]
// Zoom shows in the grid only. The right margin clears List bottom's scroll bar.
#define TITLEMANAGER_BUTTON_GAP 3
#define TITLEMANAGER_BUTTON_RIGHT 14
#define TITLEMANAGER_BUTTON_BOTTOM 4

enum {
    BUTTON_SORT,
    BUTTON_LAYOUT,
    BUTTON_ZOOM,
    BUTTON_COUNT
};

// Sort keys titlemanager_compare reads. The populate thread inserts with them, so they are copied
// from settings only while no population runs; a mid-load change waits for the resort.
typedef struct {
    u8 sort;
    bool desc;
    u8 group;
} titlemanager_order;

typedef struct {
    populate_titles_data populateData;
    titlemanager_order order;
    ui_view* view;
    ui_button buttons[BUTTON_COUNT];

    bool populated;
    bool rebuild; // the next population rebuilds the title cache (Options > Rebuild list)
    bool sortDirty; // the sort changed: resort once population has finished
    bool loading; // a cached load: the list shows a loading animation until population finishes
    u64 populateStart; // osGetTime() when the current population began
    char name[64]; // view name: current sort and grouping
} titlemanager_data;

static const list_layout titlemanager_layouts[TITLE_LAYOUT_COUNT] = {LIST_LAYOUT_GRID, LIST_LAYOUT_TOP, LIST_LAYOUT_BOTTOM};
static const char* const titlemanager_layout_names[TITLE_LAYOUT_COUNT] = {"Grid", "List top", "List bottom"};
// The Layout button shows the current layout.
static const u32 titlemanager_layout_icons[TITLE_LAYOUT_COUNT] = {TEXTURE_ICON_LAYOUT_GRID, TEXTURE_ICON_LAYOUT_TOP, TEXTURE_ICON_LAYOUT_BOTTOM};
static const char* const titlemanager_sort_names[TITLE_SORT_COUNT] = {"Name", "Title ID", "Size", "Publisher", "Category", "Region", "Game"};
static const char* const titlemanager_group_names[TITLE_GROUP_COUNT] = {"Off", "Location", "Category"};
static const char* const titlemanager_category_names[TITLE_CATEGORY_COUNT] = {"3DS", "DS", "VC/Emulator", "Homebrew", "System", "Other"};

// Section headers for Title ID sort: the title ID high word, with its usual name.
static const struct {
    u32 type;
    const char* name;
} titlemanager_type_names[] = {
        {0x00040000, "Applications"},
        {0x00040001, "Download Play"},
        {0x00040002, "Demos"},
        {0x0004000E, "Updates"},
        {0x0004008C, "DLC"},
        {0x00040010, "System applications"},
        {0x0004001B, "System data"},
        {0x00040030, "System applets"},
        {0x0004009B, "Shared data"},
        {0x000400DB, "System data"},
        {0x00040130, "System modules"},
        {0x00040138, "System firmware"},
        {0x00048004, "DSiWare"},
        {0x00048005, "DSi system"},
        {0x0004800F, "DSi data"},
};

typedef enum {
    OPTION_LAYOUT,
    OPTION_SORT,
    OPTION_ORDER,
    OPTION_GROUP,
    OPTION_HEADERS,
    OPTION_SHOW,
    OPTION_REBUILD,
    OPTION_COUNT
} titlemanager_option;

static list_item titlemanager_option_items[OPTION_COUNT];
static list_item titlemanager_show_items[TITLE_CATEGORY_COUNT];

static u32 titlemanager_item_icon(list_item* item) {
    title_info* info = (title_info*) item->data;
    return info != NULL && info->hasMeta ? info->meta.texture : 0;
}

// Icons follow the settings: the Layout icon shows the current layout, Zoom what a tap does.
static void titlemanager_update_buttons(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();

    u32 barHeight = 0;
    screen_get_texture_size(NULL, &barHeight, TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR);

    float right = BOTTOM_SCREEN_WIDTH - TITLEMANAGER_BUTTON_RIGHT - UI_BUTTON_SIZE;
    float bottom = BOTTOM_SCREEN_HEIGHT - barHeight - TITLEMANAGER_BUTTON_BOTTOM - UI_BUTTON_SIZE;
    float step = UI_BUTTON_SIZE + TITLEMANAGER_BUTTON_GAP;
    u32 zoomIcon = settings->titleGridSize == 0 ? TEXTURE_ICON_ZOOM_IN : TEXTURE_ICON_ZOOM_OUT;

    data->buttons[BUTTON_SORT] = (ui_button) {TEXTURE_ICON_SORT, right - step, bottom};
    data->buttons[BUTTON_LAYOUT] = (ui_button) {titlemanager_layout_icons[settings->titleLayout], right, bottom};
    data->buttons[BUTTON_ZOOM] = (ui_button) {zoomIcon, right, bottom - step};

    data->view->buttons = data->buttons;
    data->view->buttonCount = settings->titleLayout == TITLE_LAYOUT_GRID ? BUTTON_COUNT : BUTTON_ZOOM;
}

static void titlemanager_apply_layout(titlemanager_data* data) {
    list_set_layout(data->view, titlemanager_layouts[settings_get()->titleLayout], titlemanager_item_icon);
    list_set_grid_size(data->view, settings_get()->titleGridSize);
    titlemanager_update_buttons(data);
}

// The view name (bottom screen's top bar) shows the active order, so X, Sort, and Options changes are visible.
static void titlemanager_update_name(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();
    static const char* const groups[TITLE_GROUP_COUNT] = {"Ungrouped", "By location", "By category"};

    snprintf(data->name, sizeof(data->name), "%s, %s, %s", titlemanager_sort_names[settings->titleSort],
             settings->titleSortDesc ? "Desc" : "Asc", groups[settings->titleGroup]);
}

static void titlemanager_save(const ftb_settings* next) {
    // The new values still apply for this session.
    if(!settings_set(next)) {
        prompt_display_notify("Failure", "Saving settings failed.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

// The Layout button and the Layout row in Options: Grid -> List top -> List bottom.
static void titlemanager_cycle_layout(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleLayout = (u8) ((next.titleLayout + 1) % TITLE_LAYOUT_COUNT);

    titlemanager_save(&next);
    titlemanager_apply_layout(data);
}

// The Zoom button (grid only): next zoom level, back to the smallest after the largest.
static void titlemanager_cycle_grid_size(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleGridSize = (u8) ((next.titleGridSize + 1) % LIST_GRID_SIZE_COUNT);

    titlemanager_save(&next);
    list_set_grid_size(data->view, next.titleGridSize);
    titlemanager_update_buttons(data);
}

// X, the Sort button, and the Sort row: next sort key. Sorting and grouping only reorder, so no reload.
static void titlemanager_cycle_sort(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleSort = (u8) ((next.titleSort + 1) % TITLE_SORT_COUNT);

    titlemanager_save(&next);
    data->sortDirty = true;
}

// The Group row: Off -> Location -> Category.
static void titlemanager_cycle_group(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleGroup = (u8) ((next.titleGroup + 1) % TITLE_GROUP_COUNT);

    titlemanager_save(&next);
    data->sortDirty = true;
}

// Game card, SD, NAND, then DSiWare: the order FBI lists them in.
static int titlemanager_location_rank(const title_info* info) {
    int media = info->mediaType == MEDIATYPE_GAME_CARD ? 0 : info->mediaType == MEDIATYPE_SD ? 1 : 2;
    return media * 2 + (info->twl ? 1 : 0);
}

static int titlemanager_compare_u64(u64 value1, u64 value2) {
    return value1 > value2 ? 1 : value1 < value2 ? -1 : 0;
}

// Text for the Name and Publisher sorts, or NULL when the title has none.
static const char* titlemanager_sort_text(const list_item* item, u8 sort) {
    const title_info* info = (const title_info*) item->data;
    if(!info->hasMeta) {
        return NULL;
    }

    if(sort == TITLE_SORT_PUBLISHER) {
        return string_is_empty(info->meta.publisher) ? NULL : info->meta.publisher;
    }

    return string_is_empty(info->meta.shortDescription) ? NULL : item->name;
}

static void titlemanager_load_order(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();
    data->order = (titlemanager_order) {settings->titleSort, settings->titleSortDesc, settings->titleGroup};
}

static const char* titlemanager_size_section(u64 size) {
    const u64 mb = 1024 * 1024;
    return size < 10 * mb ? "Under 10 MB" : size < 100 * mb ? "10 - 100 MB" : size < 1024 * mb ? "100 MB - 1 GB" : "Over 1 GB";
}

// Section header text for an item. Reads the order the list was sorted with, so headers always match
// the order on screen: the group when grouped, else the sort key.
static void titlemanager_section(void* data, list_item* item, char* out, size_t size) {
    const titlemanager_order* order = &((titlemanager_data*) data)->order;
    const title_info* info = (const title_info*) item->data;

    if(order->group == TITLE_GROUP_LOCATION) {
        // Rank order; equal neighbors merge, so a DS game card is still one "Game card" section.
        static const char* const locations[] = {"Game card", "Game card", "SD", "SD", "NAND", "DSiWare"};
        snprintf(out, size, "%s", locations[titlemanager_location_rank(info)]);
    } else if(order->group == TITLE_GROUP_CATEGORY || order->sort == TITLE_SORT_CATEGORY) {
        snprintf(out, size, "%s", titlemanager_category_names[info->category]);
    } else if(order->sort == TITLE_SORT_ID) {
        u32 type = (u32) (info->titleId >> 32);
        snprintf(out, size, "%08lX", type);
        for(u32 i = 0; i < sizeof(titlemanager_type_names) / sizeof(titlemanager_type_names[0]); i++) {
            if(titlemanager_type_names[i].type == type) {
                snprintf(out, size, "%08lX %s", type, titlemanager_type_names[i].name);
                break;
            }
        }
    } else if(order->sort == TITLE_SORT_SIZE) {
        snprintf(out, size, "%s", titlemanager_size_section(info->installedSize));
    } else if(order->sort == TITLE_SORT_REGION) {
        if(info->hasMeta) {
            smdh_region_to_string(out, info->meta.region, size);
        } else {
            snprintf(out, size, "Unknown");
        }
    } else {
        // Name and Publisher, from the same text the sort compares.
        const char* text = titlemanager_sort_text(item, order->sort);
        if(order->sort == TITLE_SORT_PUBLISHER) {
            snprintf(out, size, "%s", text != NULL ? text : "Unknown publisher");
        } else if(text == NULL) {
            snprintf(out, size, "Unnamed");
        } else {
            // strcasecmp order: digits and symbols before A, non-ASCII (e.g. Japanese) after Z.
            int first = tolower((unsigned char) text[0]);
            if(first < 'a') {
                snprintf(out, size, "#");
            } else if(first > 'z') {
                snprintf(out, size, "Other");
            } else {
                snprintf(out, size, "%c", toupper(first));
            }
        }
    }
}

static int titlemanager_compare(void* data, const void* p1, const void* p2) {
    const titlemanager_order* order = &((titlemanager_data*) data)->order;

    const list_item* item1 = (const list_item*) p1;
    const list_item* item2 = (const list_item*) p2;

    const title_info* title1 = (const title_info*) item1->data;
    const title_info* title2 = (const title_info*) item2->data;

    int group = 0;
    if(order->group == TITLE_GROUP_LOCATION) {
        group = titlemanager_location_rank(title1) - titlemanager_location_rank(title2);
    } else if(order->group == TITLE_GROUP_CATEGORY) {
        group = (int) title1->category - (int) title2->category;
    }

    if(group != 0) {
        return group;
    }

    int result = 0;
    if(order->sort == TITLE_SORT_ID) {
        result = titlemanager_compare_u64(title1->titleId, title2->titleId);
    } else if(order->sort == TITLE_SORT_SIZE) {
        result = titlemanager_compare_u64(title1->installedSize, title2->installedSize);
    } else if(order->sort == TITLE_SORT_CATEGORY) {
        result = (int) title1->category - (int) title2->category;
    } else if(order->sort == TITLE_SORT_GAME) {
        result = titlemanager_compare_u64((u32) title1->titleId, (u32) title2->titleId);
    } else if(order->sort == TITLE_SORT_REGION) {
        // Titles without an SMDH sort last in either order.
        if(title1->hasMeta != title2->hasMeta) {
            return title1->hasMeta ? -1 : 1;
        }

        result = title1->hasMeta ? titlemanager_compare_u64(title1->meta.region, title2->meta.region) : 0;
    } else {
        // Titles without the text sort last in either order.
        const char* text1 = titlemanager_sort_text(item1, order->sort);
        const char* text2 = titlemanager_sort_text(item2, order->sort);
        if((text1 == NULL) != (text2 == NULL)) {
            return text1 == NULL ? 1 : -1;
        }

        result = text1 != NULL ? strcasecmp(text1, text2) : 0;
    }

    if(order->desc) {
        result = -result;
    }

    // Ties fall back to name, then title ID, so the order never depends on load order.
    if(result == 0) {
        result = strcasecmp(item1->name, item2->name);
    }

    return result != 0 ? result : titlemanager_compare_u64(title1->titleId, title2->titleId);
}

static bool titlemanager_visible(void* data, title_category category) {
    return settings_get()->titleShowCategory[category];
}

static void titlemanager_item_set(list_item* item, const char* name, const char* value, u32 color) {
    snprintf(item->name, LIST_ITEM_NAME_MAX, "%s: %s", name, value);
    item->color = color;
}

static void titlemanager_item_set_bool(list_item* item, const char* name, bool value) {
    titlemanager_item_set(item, name, value ? "On" : "Off", value ? COLOR_ENABLED : COLOR_DISABLED);
}

// Options > Show: one On/Off row per category. Hidden titles are dropped while loading, so a change
// reloads the list (from the cache) once Options closes.
static void titlemanager_show_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        u32 category = (u32) (selected - titlemanager_show_items);

        ftb_settings next = *settings_get();
        next.titleShowCategory[category] = !next.titleShowCategory[category];

        titlemanager_save(&next);
        tmData->populated = false;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < TITLE_CATEGORY_COUNT; i++) {
            linked_list_add(items, &titlemanager_show_items[i]);
        }
    }

    for(u32 i = 0; i < TITLE_CATEGORY_COUNT; i++) {
        titlemanager_item_set_bool(&titlemanager_show_items[i], titlemanager_category_names[i], settings_get()->titleShowCategory[i]);
    }
}

static void titlemanager_options_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    titlemanager_option option = selected != NULL ? (titlemanager_option) (selected - titlemanager_option_items) : OPTION_COUNT;
    bool activated = option != OPTION_COUNT && (selectedTouched || (hidKeysDown() & KEY_A));
    if(activated && option == OPTION_REBUILD) {
        // Back to the list so the reload shows.
        tmData->rebuild = true;
        tmData->populated = false;

        ui_pop();
        list_destroy(view);

        return;
    } else if(activated && option == OPTION_LAYOUT) {
        titlemanager_cycle_layout(tmData);
    } else if(activated && option == OPTION_SORT) {
        titlemanager_cycle_sort(tmData);
    } else if(activated && option == OPTION_GROUP) {
        titlemanager_cycle_group(tmData);
    } else if(activated && option == OPTION_SHOW) {
        list_display("Show", "A: Toggle, B: Return", tmData, titlemanager_show_update, NULL);
    } else if(activated) {
        ftb_settings next = *settings_get();

        // Headers need no resort: titlemanager_update hands the list its section callback.
        if(option == OPTION_ORDER) {
            next.titleSortDesc = !next.titleSortDesc;
            tmData->sortDirty = true;
        } else if(option == OPTION_HEADERS) {
            next.titleHeaders = !next.titleHeaders;
        }

        titlemanager_save(&next);
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < OPTION_COUNT; i++) {
            linked_list_add(items, &titlemanager_option_items[i]);
        }
    }

    const ftb_settings* settings = settings_get();

    u32 shown = 0;
    for(u32 i = 0; i < TITLE_CATEGORY_COUNT; i++) {
        shown += settings->titleShowCategory[i] ? 1 : 0;
    }

    char show[16];
    if(shown == TITLE_CATEGORY_COUNT) {
        snprintf(show, sizeof(show), "All");
    } else {
        snprintf(show, sizeof(show), "%lu of %d", shown, TITLE_CATEGORY_COUNT);
    }

    list_item* options = titlemanager_option_items;
    titlemanager_item_set(&options[OPTION_LAYOUT], "Layout", titlemanager_layout_names[settings->titleLayout], COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_SORT], "Sort", titlemanager_sort_names[settings->titleSort], COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_ORDER], "Order", settings->titleSortDesc ? "Descending" : "Ascending", COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_GROUP], "Group", titlemanager_group_names[settings->titleGroup], COLOR_TEXT);
    titlemanager_item_set_bool(&options[OPTION_HEADERS], "Headers", settings->titleHeaders);
    titlemanager_item_set(&options[OPTION_SHOW], "Show", show, COLOR_TEXT);
    snprintf(options[OPTION_REBUILD].name, LIST_ITEM_NAME_MAX, "Rebuild list");
    options[OPTION_REBUILD].color = COLOR_TEXT;
}

static void titlemanager_draw_info(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(selected != NULL && selected->data != NULL) {
        task_draw_title_info(view, selected->data, x1, y1, x2, y2);
    }

    // On the bottom screen (Grid, List top) the progress bar ends left of the touch buttons.
    float progressX2 = x2;
    if(settings_get()->titleLayout != TITLE_LAYOUT_LIST_BOTTOM) {
        progressX2 = tmData->buttons[BUTTON_SORT].x - TITLEMANAGER_BUTTON_GAP;
    }

    task_draw_title_progress(&tmData->populateData, tmData->populateStart, x1, progressX2, y2);
}

static void titlemanager_wait_populate(titlemanager_data* tmData) {
    if(!tmData->populateData.finished) {
        svcSignalEvent(tmData->populateData.cancelEvent);
        while(!tmData->populateData.finished) {
            svcSleepThread(1000000);
        }
    }
}

static void titlemanager_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(hidKeysDown() & KEY_B) {
        titlemanager_wait_populate(tmData);

        ui_pop();

        task_clear_titles(items);
        list_destroy(view);

        free(tmData);
        return;
    }

    if(hidKeysDown() & KEY_SELECT) {
        list_display("Options", "A: Change, B: Return", tmData, titlemanager_options_update, NULL);
        return;
    }

    if(view->buttonActivated == BUTTON_LAYOUT) {
        titlemanager_cycle_layout(tmData);
        return;
    }

    if(view->buttonActivated == BUTTON_ZOOM) {
        titlemanager_cycle_grid_size(tmData);
        return;
    }

    if((hidKeysDown() & KEY_X) || view->buttonActivated == BUTTON_SORT) {
        titlemanager_cycle_sort(tmData);
    }

    titlemanager_update_name(tmData);

    if(!tmData->populated) {
        titlemanager_wait_populate(tmData);

        tmData->populateData.items = items;
        title_cache_mode cacheMode = TITLE_CACHE_OFF;
        if(settings_get()->titleCache) {
            cacheMode = tmData->rebuild ? TITLE_CACHE_REBUILD : TITLE_CACHE_ON;
        }

        tmData->rebuild = false;
        tmData->populateData.cacheMode = cacheMode;
        tmData->populateStart = osGetTime();
        // Cached titles arrive in a burst, and inserting them sorted would drag the selection away
        // from the top, so the list stays hidden until the load is done. Uncached loads are slow
        // enough that showing titles as they arrive stays more useful.
        tmData->loading = cacheMode == TITLE_CACHE_ON;
        // A hidden list takes titles unsorted and sorts once at the end; inserting each one in order
        // costs a walk of the list per title. A shown list inserts in the current order.
        tmData->populateData.compare = tmData->loading ? NULL : titlemanager_compare;
        tmData->sortDirty = tmData->loading;
        titlemanager_load_order(tmData);
        list_set_loading(view, tmData->loading);
        Result res = task_populate_titles(&tmData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate title list population.");
        }

        tmData->populated = true;
    }

    // The populate thread inserts while it runs, so only sort once it is done.
    if(tmData->populateData.finished && tmData->sortDirty) {
        titlemanager_load_order(tmData);
        linked_list_sort(items, tmData, titlemanager_compare);
        tmData->sortDirty = false;
    }

    // Game sort has no useful sections (one per game), so it only gets headers when grouped.
    const titlemanager_order* order = &tmData->order;
    bool headers = settings_get()->titleHeaders && (order->group != TITLE_GROUP_OFF || order->sort != TITLE_SORT_GAME);
    list_set_sections(view, headers ? titlemanager_section : NULL);

    // Reveal the finished list at its first title.
    if(tmData->populateData.finished && tmData->loading) {
        tmData->loading = false;
        list_set_loading(view, false);
    }

    if(tmData->populateData.finished && R_FAILED(tmData->populateData.result)) {
        error_display_res(NULL, NULL, tmData->populateData.result, "Failed to populate title list.");

        tmData->populateData.result = 0;
    }

    // Actions such as Delete Title remove items, which the populate thread may be walking until it
    // finishes, so titles open once the list is complete.
    if(selected != NULL && selected->data != NULL && tmData->populateData.finished
       && (selectedTouched || (hidKeysDown() & KEY_A))) {
        titles_action_open(items, selected);
        return;
    }
}

void titlemanager_open() {
    titlemanager_data* data = (titlemanager_data*) calloc(1, sizeof(titlemanager_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate title manager data.");

        return;
    }

    data->populateData.userData = data;
    data->populateData.visible = titlemanager_visible;

    data->populateData.finished = true;

    titlemanager_update_name(data);
    data->view = list_display(data->name, TITLEMANAGER_INFO, data, titlemanager_update, titlemanager_draw_info);
    if(data->view == NULL) {
        free(data);
        return;
    }

    list_set_paging(data->view, true);
    titlemanager_apply_layout(data);
}
