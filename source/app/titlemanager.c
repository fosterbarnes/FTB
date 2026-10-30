#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "safety.h"
#include "screens.h"
#include "settings.h"
#include "task/uitask.h"
#include "../core/core.h"

// Title Manager: FTB's title list. Style, screen, zoom, sort, headers, and hidden system titles live
// in settings (titleStyle etc.) and change from Options or the bottom-left touch buttons (sort, style,
// zoom in Grid and Icon list). L and R jump sections. FBI's original screen stays in titles.c as FBI
// Legacy > Titles.

#define TITLEMANAGER_INFO "A: Select, B: Return, L/R: Section, Select: Options"

// Touch buttons, bottom-left of the bottom screen in every layout (clear of the scroll bar):
//   [Zoom]
//   [Layout][Sort]
// Zoom shows in Grid and Icon list only.
#define TITLEMANAGER_BUTTON_GAP 3
#define TITLEMANAGER_BUTTON_LEFT 4
#define TITLEMANAGER_BUTTON_BOTTOM 4
// Rows on the bottom screen sit under the buttons, so the rows under them show through.
#define TITLEMANAGER_BUTTON_LIST_ALPHA 0xC0

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
    bool hideSystem; // titleHideSystem when the current population began (read by its thread)
    char sortText[32]; // current sort and order
    char name[64]; // view name: the sort text, or with rows on top the selected title's section
} titlemanager_data;

static const list_style titlemanager_styles[TITLE_STYLE_COUNT] = {LIST_STYLE_GRID, LIST_STYLE_TEXT, LIST_STYLE_ICON_LIST};
static const char* const titlemanager_style_names[TITLE_STYLE_COUNT] = {"Grid", "List", "Icon list"};
// The Layout button shows the current style on the current screen: [style][rows on bottom].
static const u32 titlemanager_style_icons[TITLE_STYLE_COUNT][2] = {
        {TEXTURE_ICON_LAYOUT_GRID, TEXTURE_ICON_LAYOUT_GRID_BOTTOM},
        {TEXTURE_ICON_LAYOUT_TOP, TEXTURE_ICON_LAYOUT_BOTTOM},
        {TEXTURE_ICON_LAYOUT_ICON_LIST, TEXTURE_ICON_LAYOUT_ICON_LIST_BOTTOM},
};
static const char* const titlemanager_sort_names[TITLE_SORT_COUNT] = {"A-Z", "Title ID"};

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
    OPTION_SCREEN,
    OPTION_SORT,
    OPTION_ORDER,
    OPTION_HEADERS,
    OPTION_HIDE_SYSTEM,
    OPTION_REBUILD,
    OPTION_COUNT
} titlemanager_option;

static list_item titlemanager_option_items[OPTION_COUNT];

static u32 titlemanager_item_icon(list_item* item) {
    title_info* info = (title_info*) item->data;
    return info != NULL && info->hasMeta ? info->meta.texture : 0;
}

// Icon list detail lines: publisher, then media, size, and version.
static void titlemanager_item_detail(list_item* item, char* out, size_t size) {
    const title_info* info = (const title_info*) item->data;
    if(info == NULL) {
        out[0] = '\0';
        return;
    }

    if(!info->hasMeta) {
        snprintf(out, size, "Title ID %016llX", info->titleId);
        return;
    }

    char version[16];
    ui_format_version(version, sizeof(version), info->titleId, info->version);

    snprintf(out, size, "%s\n%s, %.2f %s, v%s",
             string_is_empty(info->meta.publisher) ? "Unknown publisher" : info->meta.publisher,
             fs_media_type_name(info->mediaType),
             ui_get_display_size(info->installedSize), ui_get_display_size_units(info->installedSize),
             version);
}

static void titlemanager_section(void* data, list_item* item, char* out, size_t size);

// Grid and Icon list zoom separately (shared by both screens); List has no zoom and uses the grid's.
static u8* titlemanager_zoom(ftb_settings* settings) {
    return settings->titleStyle == TITLE_STYLE_ICON_LIST ? &settings->titleIconListZoom : &settings->titleGridSize;
}

static u8 titlemanager_current_zoom() {
    ftb_settings settings = *settings_get();
    return *titlemanager_zoom(&settings);
}

// Icons follow the settings: the Layout icon shows the current style and screen, Zoom what a tap does
// (zoom out only at the largest level, where the next tap wraps to the smallest).
static void titlemanager_update_buttons(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();

    u32 barHeight = 0;
    screen_get_texture_size(NULL, &barHeight, TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR);

    float left = TITLEMANAGER_BUTTON_LEFT;
    float bottom = BOTTOM_SCREEN_HEIGHT - barHeight - TITLEMANAGER_BUTTON_BOTTOM - UI_BUTTON_SIZE;
    float step = UI_BUTTON_SIZE + TITLEMANAGER_BUTTON_GAP;
    bool largest = titlemanager_current_zoom() + 1u >= list_zoom_count(titlemanager_styles[settings->titleStyle]);
    u32 zoomIcon = largest ? TEXTURE_ICON_ZOOM_OUT : TEXTURE_ICON_ZOOM_IN;
    bool zooms = settings->titleStyle != TITLE_STYLE_LIST;

    data->buttons[BUTTON_SORT] = (ui_button) {TEXTURE_ICON_SORT, left + step, bottom};
    data->buttons[BUTTON_LAYOUT] = (ui_button) {titlemanager_style_icons[settings->titleStyle][settings->titleBottom], left, bottom};
    data->buttons[BUTTON_ZOOM] = (ui_button) {zoomIcon, left, bottom - step};

    data->view->buttons = data->buttons;
    data->view->buttonAlpha = settings->titleBottom ? TITLEMANAGER_BUTTON_LIST_ALPHA : 0xFF;
    data->view->buttonCount = zooms ? BUTTON_COUNT : BUTTON_ZOOM;
}

// The top bars. Rows on the bottom: the sort, centered on the bottom screen's bar. Rows on top: the
// sort pinned left on the top screen's bar, and the selected title's section pinned left on the bottom
// screen's (empty while nothing is selected). Runs every update, since the selection moves.
static void titlemanager_update_name(titlemanager_data* data, list_item* selected) {
    const ftb_settings* settings = settings_get();
    snprintf(data->sortText, sizeof(data->sortText), "%s, %s", titlemanager_sort_names[settings->titleSort],
             settings->titleSortDesc ? "Desc" : "Asc");

    if(settings->titleBottom) {
        string_copy(data->name, data->sortText, sizeof(data->name));
        data->view->topBarText = NULL;
        data->view->nameLeft = false;
    } else {
        data->name[0] = '\0';
        if(selected != NULL && selected->data != NULL) {
            titlemanager_section(data, selected, data->name, sizeof(data->name));
        }

        data->view->topBarText = data->sortText;
        data->view->nameLeft = true;
    }
}

static void titlemanager_apply_layout(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();
    list_set_layout(data->view, titlemanager_styles[settings->titleStyle], !settings->titleBottom, titlemanager_item_icon, titlemanager_item_detail);
    list_set_zoom(data->view, titlemanager_current_zoom());
    titlemanager_update_buttons(data);
    titlemanager_update_name(data, NULL);
}

static void titlemanager_save(const ftb_settings* next) {
    // The new values still apply for this session.
    if(!settings_set(next)) {
        prompt_display_notify("Failure", "Saving settings failed.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

// The Layout button and the Layout row in Options: Grid -> List -> Icon list, on the same screen.
static void titlemanager_cycle_layout(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleStyle = (u8) ((next.titleStyle + 1) % TITLE_STYLE_COUNT);

    titlemanager_save(&next);
    titlemanager_apply_layout(data);
}

// The Screen row in Options: rows on the top or the bottom screen, same style.
static void titlemanager_toggle_screen(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleBottom = !next.titleBottom;

    titlemanager_save(&next);
    titlemanager_apply_layout(data);
}

// The Zoom button (Grid and Icon list): the style's next zoom level, back to the smallest after the
// largest.
static void titlemanager_cycle_zoom(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    u8* zoom = titlemanager_zoom(&next);
    *zoom = (u8) ((*zoom + 1) % list_zoom_count(titlemanager_styles[next.titleStyle]));

    titlemanager_save(&next);
    list_set_zoom(data->view, *zoom);
    titlemanager_update_buttons(data);
}

// The Sort button and the Sort row: next sort key. Sorting only reorders, so no reload.
static void titlemanager_cycle_sort(titlemanager_data* data) {
    ftb_settings next = *settings_get();
    next.titleSort = (u8) ((next.titleSort + 1) % TITLE_SORT_COUNT);

    titlemanager_save(&next);
    data->sortDirty = true;
}

static int titlemanager_compare_u64(u64 value1, u64 value2) {
    return value1 > value2 ? 1 : value1 < value2 ? -1 : 0;
}

// The Name sort's text, or NULL when the title has no name.
static const char* titlemanager_sort_text(const list_item* item) {
    const title_info* info = (const title_info*) item->data;
    return info->hasMeta && !string_is_empty(info->meta.shortDescription) ? item->name : NULL;
}

static void titlemanager_load_order(titlemanager_data* data) {
    const ftb_settings* settings = settings_get();
    data->order = (titlemanager_order) {settings->titleSort, settings->titleSortDesc};
}

// Section header text for an item. Reads the order the list was sorted with, so headers always match
// the order on screen: the title type for Title ID, else the name's first letter.
static void titlemanager_section(void* data, list_item* item, char* out, size_t size) {
    const titlemanager_order* order = &((titlemanager_data*) data)->order;
    const title_info* info = (const title_info*) item->data;

    if(order->sort == TITLE_SORT_ID) {
        u32 type = (u32) (info->titleId >> 32);
        snprintf(out, size, "%08lX", type);
        for(u32 i = 0; i < sizeof(titlemanager_type_names) / sizeof(titlemanager_type_names[0]); i++) {
            if(titlemanager_type_names[i].type == type) {
                snprintf(out, size, "%08lX %s", type, titlemanager_type_names[i].name);
                break;
            }
        }

        return;
    }

    const char* text = titlemanager_sort_text(item);
    if(text == NULL) {
        snprintf(out, size, "Unnamed");
        return;
    }

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

static int titlemanager_compare(void* data, const void* p1, const void* p2) {
    const titlemanager_order* order = &((titlemanager_data*) data)->order;

    const list_item* item1 = (const list_item*) p1;
    const list_item* item2 = (const list_item*) p2;

    const title_info* title1 = (const title_info*) item1->data;
    const title_info* title2 = (const title_info*) item2->data;

    int result = 0;
    if(order->sort == TITLE_SORT_ID) {
        result = titlemanager_compare_u64(title1->titleId, title2->titleId);
    } else {
        // Unnamed titles sort last in either order.
        const char* text1 = titlemanager_sort_text(item1);
        const char* text2 = titlemanager_sort_text(item2);
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

// Options > System titles: Hidden. Runs on the populate thread, which only reads hideSystem.
static bool titlemanager_hide(void* data, u64 titleId) {
    return ((titlemanager_data*) data)->hideSystem && safety_is_system_title(titleId);
}

static void titlemanager_item_set(list_item* item, const char* name, const char* value, u32 color) {
    snprintf(item->name, LIST_ITEM_NAME_MAX, "%s: %s", name, value);
    item->color = color;
}

static void titlemanager_item_set_bool(list_item* item, const char* name, bool value) {
    titlemanager_item_set(item, name, value ? "On" : "Off", value ? COLOR_ENABLED : COLOR_DISABLED);
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
    } else if(activated && option == OPTION_SCREEN) {
        titlemanager_toggle_screen(tmData);
    } else if(activated && option == OPTION_SORT) {
        titlemanager_cycle_sort(tmData);
    } else if(activated) {
        ftb_settings next = *settings_get();

        // Headers need no resort: titlemanager_update hands the list its section settings.
        if(option == OPTION_ORDER) {
            next.titleSortDesc = !next.titleSortDesc;
            tmData->sortDirty = true;
        } else if(option == OPTION_HEADERS) {
            next.titleHeaders = !next.titleHeaders;
        } else if(option == OPTION_HIDE_SYSTEM) {
            // Reload when back on the list; hidden titles stay cached, so cache hits make it quick.
            next.titleHideSystem = !next.titleHideSystem;
            tmData->populated = false;
        }

        titlemanager_save(&next);
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < OPTION_COUNT; i++) {
            linked_list_add(items, &titlemanager_option_items[i]);
        }
    }

    const ftb_settings* settings = settings_get();
    list_item* options = titlemanager_option_items;
    titlemanager_item_set(&options[OPTION_LAYOUT], "Layout", titlemanager_style_names[settings->titleStyle], COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_SCREEN], "Screen", settings->titleBottom ? "Bottom" : "Top", COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_SORT], "Sort", titlemanager_sort_names[settings->titleSort], COLOR_TEXT);
    titlemanager_item_set(&options[OPTION_ORDER], "Order", settings->titleSortDesc ? "Descending" : "Ascending", COLOR_TEXT);
    titlemanager_item_set_bool(&options[OPTION_HEADERS], "Headers", settings->titleHeaders);
    titlemanager_item_set(&options[OPTION_HIDE_SYSTEM], "System titles", settings->titleHideSystem ? "Hidden" : "Shown", COLOR_TEXT);
    snprintf(options[OPTION_REBUILD].name, LIST_ITEM_NAME_MAX, "Rebuild list");
    options[OPTION_REBUILD].color = COLOR_TEXT;
}

static void titlemanager_draw_info(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(selected != NULL && selected->data != NULL) {
        task_draw_title_info(view, selected->data, x1, y1, x2, y2);
    }

    // Info on the bottom screen (rows on top): the progress bar starts right of the touch buttons.
    float progressX1 = x1;
    if(!settings_get()->titleBottom) {
        progressX1 = tmData->buttons[BUTTON_SORT].x + UI_BUTTON_SIZE + TITLEMANAGER_BUTTON_GAP;
    }

    task_draw_title_progress(&tmData->populateData, tmData->populateStart, progressX1, x2, y2);
}

static void titlemanager_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    titlemanager_data* tmData = (titlemanager_data*) data;

    if(hidKeysDown() & KEY_B) {
        task_cancel_and_wait(&tmData->populateData.state);

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
        titlemanager_cycle_zoom(tmData);
        return;
    }

    if(view->buttonActivated == BUTTON_SORT) {
        titlemanager_cycle_sort(tmData);
    }

    titlemanager_update_name(tmData, selected);

    if(!tmData->populated) {
        task_cancel_and_wait(&tmData->populateData.state);

        tmData->populateData.items = items;
        tmData->hideSystem = settings_get()->titleHideSystem;
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
        selected = NULL;
        selectedTouched = false;
        Result res = task_populate_titles(&tmData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate title list population.");
        }

        tmData->populated = true;
    }

    bool finished = task_is_finished(&tmData->populateData.state);

    // The populate thread inserts while it runs, so only sort once it is done.
    if(finished && tmData->sortDirty) {
        titlemanager_load_order(tmData);
        linked_list_sort(items, tmData, titlemanager_compare);
        tmData->sortDirty = false;
    }

    // Sections always exist for L/R; Headers only decides whether they are drawn.
    list_set_sections(view, titlemanager_section, settings_get()->titleHeaders);

    // Reveal the finished list at its first title.
    if(finished && tmData->loading) {
        tmData->loading = false;
        list_set_loading(view, false);
    }

    if(finished && R_FAILED(tmData->populateData.state.result)) {
        error_display_res(NULL, NULL, tmData->populateData.state.result, "Failed to populate title list.");

        tmData->populateData.state.result = 0;
    }

    // Actions such as Delete Title remove items, which the populate thread may be walking until it
    // finishes, so titles open once the list is complete.
    if(selected != NULL && selected->data != NULL && finished
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
    data->populateData.hide = titlemanager_hide;

    data->populateData.state.finished = true;

    // apply_layout fills the name.
    data->view = list_display(data->name, TITLEMANAGER_INFO, data, titlemanager_update, titlemanager_draw_info);
    if(data->view == NULL) {
        free(data);
        return;
    }

    list_set_section_keys(data->view, true);
    titlemanager_apply_layout(data);
}
