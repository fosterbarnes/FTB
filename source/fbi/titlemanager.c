#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "task/uitask.h"
#include "../core/core.h"

// Title Manager: FTB's title list. Layout, sort, and filters live in settings (titleLayout etc.)
// and are changed from Options. FBI's original screen stays in titles.c as Titles (FBI Legacy).

#define TITLEMANAGER_INFO "A: Select, B: Return, X: Refresh, Select: Options"
#define TITLEMANAGER_INFO_GRID "A: Select, B: Return, X: Refresh, L/R: Page, Select: Options"

typedef struct {
    populate_titles_data populateData;
    ui_view* view;

    bool populated;
    bool sortDirty; // the sort changed: resort once population has finished
    u64 populateStart; // osGetTime() when the current population began
} titlemanager_data;

static const list_layout titlemanager_layouts[TITLE_LAYOUT_COUNT] = {LIST_LAYOUT_GRID, LIST_LAYOUT_TOP, LIST_LAYOUT_BOTTOM};
static const char* const titlemanager_layout_names[TITLE_LAYOUT_COUNT] = {"Grid", "List top", "List bottom"};
static const char* const titlemanager_sort_names[TITLE_SORT_COUNT] = {"Name", "Title ID", "Size", "Publisher"};

typedef enum {
    OPTION_LAYOUT,
    OPTION_SORT,
    OPTION_ORDER,
    OPTION_GROUP,
    OPTION_SHOW_CARD,
    OPTION_SHOW_SD,
    OPTION_SHOW_NAND,
    OPTION_COUNT
} titlemanager_option;

static list_item titlemanager_option_items[OPTION_COUNT];

static u32 titlemanager_item_icon(list_item* item) {
    title_info* info = (title_info*) item->data;
    return info != NULL && info->hasMeta ? info->meta.texture : 0;
}

static void titlemanager_apply_layout(titlemanager_data* data) {
    u8 layout = settings_get()->titleLayout;

    data->view->info = layout == TITLE_LAYOUT_GRID ? TITLEMANAGER_INFO_GRID : TITLEMANAGER_INFO;
    list_set_layout(data->view, titlemanager_layouts[layout], titlemanager_item_icon);
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

static int titlemanager_compare(void* data, const void* p1, const void* p2) {
    const ftb_settings* settings = settings_get();

    const list_item* item1 = (const list_item*) p1;
    const list_item* item2 = (const list_item*) p2;

    const title_info* title1 = (const title_info*) item1->data;
    const title_info* title2 = (const title_info*) item2->data;

    if(settings->titleGroupLocation) {
        int location = titlemanager_location_rank(title1) - titlemanager_location_rank(title2);
        if(location != 0) {
            return location;
        }
    }

    int result = 0;
    if(settings->titleSort == TITLE_SORT_ID) {
        result = titlemanager_compare_u64(title1->titleId, title2->titleId);
    } else if(settings->titleSort == TITLE_SORT_SIZE) {
        result = titlemanager_compare_u64(title1->installedSize, title2->installedSize);
    } else {
        // Titles without the text sort last in either order.
        const char* text1 = titlemanager_sort_text(item1, settings->titleSort);
        const char* text2 = titlemanager_sort_text(item2, settings->titleSort);
        if((text1 == NULL) != (text2 == NULL)) {
            return text1 == NULL ? 1 : -1;
        }

        result = text1 != NULL ? strcasecmp(text1, text2) : 0;
    }

    if(settings->titleSortDesc) {
        result = -result;
    }

    // Ties fall back to name, then title ID, so the order never depends on load order.
    if(result == 0) {
        result = strcasecmp(item1->name, item2->name);
    }

    return result != 0 ? result : titlemanager_compare_u64(title1->titleId, title2->titleId);
}

static bool titlemanager_filter(void* data, u64 titleId, FS_MediaType mediaType) {
    const ftb_settings* settings = settings_get();

    if(mediaType == MEDIATYPE_GAME_CARD) {
        return settings->titleShowCard;
    } else if(mediaType == MEDIATYPE_SD) {
        return settings->titleShowSD;
    } else {
        return settings->titleShowNAND;
    }
}

static void titlemanager_option_set(titlemanager_option option, const char* name, const char* value, u32 color) {
    snprintf(titlemanager_option_items[option].name, LIST_ITEM_NAME_MAX, "%s: %s", name, value);
    titlemanager_option_items[option].color = color;
}

static void titlemanager_option_set_bool(titlemanager_option option, const char* name, bool value) {
    titlemanager_option_set(option, name, value ? "On" : "Off", value ? COLOR_ENABLED : COLOR_DISABLED);
}

static void titlemanager_options_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        ftb_settings next = *settings_get();
        titlemanager_option option = (titlemanager_option) (selected - titlemanager_option_items);

        switch(option) {
            case OPTION_LAYOUT:
                next.titleLayout = (u8) ((next.titleLayout + 1) % TITLE_LAYOUT_COUNT);
                break;
            case OPTION_SORT:
                next.titleSort = (u8) ((next.titleSort + 1) % TITLE_SORT_COUNT);
                tmData->sortDirty = true;
                break;
            case OPTION_ORDER:
                next.titleSortDesc = !next.titleSortDesc;
                tmData->sortDirty = true;
                break;
            case OPTION_GROUP:
                next.titleGroupLocation = !next.titleGroupLocation;
                tmData->sortDirty = true;
                break;
            case OPTION_SHOW_CARD:
                next.titleShowCard = !next.titleShowCard;
                tmData->populated = false;
                break;
            case OPTION_SHOW_SD:
                next.titleShowSD = !next.titleShowSD;
                tmData->populated = false;
                break;
            case OPTION_SHOW_NAND:
                next.titleShowNAND = !next.titleShowNAND;
                tmData->populated = false;
                break;
            default:
                break;
        }

        // The new values still apply for this session.
        bool saved = settings_set(&next);
        if(option == OPTION_LAYOUT) {
            titlemanager_apply_layout(tmData);
        }

        if(!saved) {
            prompt_display_notify("Failure", "Saving settings failed.", COLOR_TEXT, NULL, NULL, NULL);
        }
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < OPTION_COUNT; i++) {
            linked_list_add(items, &titlemanager_option_items[i]);
        }
    }

    const ftb_settings* settings = settings_get();
    titlemanager_option_set(OPTION_LAYOUT, "Layout", titlemanager_layout_names[settings->titleLayout], COLOR_TEXT);
    titlemanager_option_set(OPTION_SORT, "Sort", titlemanager_sort_names[settings->titleSort], COLOR_TEXT);
    titlemanager_option_set(OPTION_ORDER, "Order", settings->titleSortDesc ? "Descending" : "Ascending", COLOR_TEXT);
    titlemanager_option_set_bool(OPTION_GROUP, "Group by location", settings->titleGroupLocation);
    titlemanager_option_set_bool(OPTION_SHOW_CARD, "Show game card", settings->titleShowCard);
    titlemanager_option_set_bool(OPTION_SHOW_SD, "Show SD", settings->titleShowSD);
    titlemanager_option_set_bool(OPTION_SHOW_NAND, "Show NAND", settings->titleShowNAND);
}

static void titlemanager_draw_info(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(selected != NULL && selected->data != NULL) {
        task_draw_title_info(view, selected->data, x1, y1, x2, y2);
    }

    task_draw_title_progress(&tmData->populateData, tmData->populateStart, x1, x2, y2);
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

    if(!tmData->populated || (hidKeysDown() & KEY_X)) {
        titlemanager_wait_populate(tmData);

        tmData->populateData.items = items;
        title_cache_mode cacheMode = TITLE_CACHE_OFF;
        if(settings_get()->titleCache) {
            cacheMode = (hidKeysDown() & KEY_X) ? TITLE_CACHE_REBUILD : TITLE_CACHE_ON;
        }

        tmData->populateData.cacheMode = cacheMode;
        tmData->populateStart = osGetTime();
        tmData->sortDirty = false; // a new population inserts in the current order
        Result res = task_populate_titles(&tmData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate title list population.");
        }

        tmData->populated = true;
    }

    // The populate thread inserts while it runs, so only sort once it is done.
    if(tmData->populateData.finished && tmData->sortDirty) {
        linked_list_sort(items, NULL, titlemanager_compare);
        tmData->sortDirty = false;
    }

    if(tmData->populateData.finished && R_FAILED(tmData->populateData.result)) {
        error_display_res(NULL, NULL, tmData->populateData.result, "Failed to populate title list.");

        tmData->populateData.result = 0;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
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
    data->populateData.filter = titlemanager_filter;
    data->populateData.compare = titlemanager_compare;

    data->populateData.finished = true;

    data->view = list_display("Title Manager", TITLEMANAGER_INFO, data, titlemanager_update, titlemanager_draw_info);
    if(data->view == NULL) {
        free(data);
        return;
    }

    titlemanager_apply_layout(data);
}
