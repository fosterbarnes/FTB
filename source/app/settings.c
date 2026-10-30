#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "paths.h"
#include "resources.h"
#include "screens.h"
#include "settings.h"
#include "task/uitask.h"
#include "webtransfer/webserver.h"
#include "../core/core.h"

#define SETTINGS_PATH PATHS_SETTINGS

static ftb_settings settings = {
        .titleCache = true,
        .titleStyle = TITLE_STYLE_GRID,
        .titleGridSize = LIST_GRID_ZOOM_DEFAULT,
        .titleIconListZoom = LIST_ICON_LIST_ZOOM_DEFAULT,
        .titleSort = TITLE_SORT_NAME,
        .titleHeaders = true,
        .webApproveDevices = WEB_APPROVE_REMEMBER,
};

// On/off settings stored as key=0|1. The file is user-editable: anything else keeps the default.
static const struct {
    const char* key;
    bool* value;
} settings_bools[] = {
        {"titleCache", &settings.titleCache},
        {"clock24h", &settings.clock24h},
        {"dateDayFirst", &settings.dateDayFirst},
        {"titleSortDesc", &settings.titleSortDesc},
        {"titleHeaders", &settings.titleHeaders},
        {"systemChanges", &settings.systemChanges},
        {"titleBottom", &settings.titleBottom},
        {"titleHideSystem", &settings.titleHideSystem},
        {"webAutoAccept", &settings.webAutoAccept},
};

// Choice settings stored as key=N, N below count; anything else keeps the default.
static const struct {
    const char* key;
    u8* value;
    u8 count;
} settings_choices[] = {
        // New keys, not new meanings: old titleLayout (4 layouts) and titleIconListSize (2 levels)
        // values would read as the wrong style and zoom, so they are dropped on the next save.
        {"titleStyle", &settings.titleStyle, TITLE_STYLE_COUNT},
        {"titleGridSize", &settings.titleGridSize, LIST_GRID_ZOOM_COUNT},
        {"titleIconListZoom", &settings.titleIconListZoom, LIST_ICON_LIST_ZOOM_COUNT},
        {"titleSort", &settings.titleSort, TITLE_SORT_COUNT},
        {"webApproveDevices", &settings.webApproveDevices, WEB_APPROVE_COUNT},
};

#define SETTINGS_BOOL_COUNT (sizeof(settings_bools) / sizeof(settings_bools[0]))
#define SETTINGS_CHOICE_COUNT (sizeof(settings_choices) / sizeof(settings_choices[0]))

const ftb_settings* settings_get() {
    return &settings;
}

void settings_load() {
    FILE* fd = fs_open_file_safe(SETTINGS_PATH, "r");
    if(fd == NULL) {
        return;
    }

    char line[128];
    while(fgets(line, sizeof(line), fd) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        // Values are one digit.
        char* value = strchr(line, '=');
        if(value == NULL || value[1] < '0' || value[1] > '9' || value[2] != '\0') {
            continue;
        }

        *value++ = '\0';
        u8 number = (u8) (value[0] - '0');

        for(u32 i = 0; i < SETTINGS_BOOL_COUNT; i++) {
            if(number <= 1 && strcmp(line, settings_bools[i].key) == 0) {
                *settings_bools[i].value = number == 1;
            }
        }

        for(u32 i = 0; i < SETTINGS_CHOICE_COUNT; i++) {
            if(number < settings_choices[i].count && strcmp(line, settings_choices[i].key) == 0) {
                *settings_choices[i].value = number;
            }
        }
    }

    fclose(fd);
}

static bool settings_write(FILE* fd, void* data) {
    bool written = true;
    for(u32 i = 0; i < SETTINGS_BOOL_COUNT && written; i++) {
        written = fprintf(fd, "%s=%d\n", settings_bools[i].key, *settings_bools[i].value ? 1 : 0) > 0;
    }

    for(u32 i = 0; i < SETTINGS_CHOICE_COUNT && written; i++) {
        written = fprintf(fd, "%s=%d\n", settings_choices[i].key, *settings_choices[i].value) > 0;
    }

    return written;
}

static bool settings_save() {
    paths_ensure_data_dir();
    return fs_write_file_safe(SETTINGS_PATH, settings_write, NULL);
}

bool settings_set(const ftb_settings* next) {
    settings = *next;
    return settings_save();
}

typedef struct settings_entry_s {
    void (*action)();
    const char* description; // top screen text while the item is selected
} settings_entry;

// After a change from this screen; the new value still applies for this session when saving fails.
static void settings_save_changed() {
    if(!settings_save()) {
        prompt_display_notify("Failure", "Saving " SETTINGS_PATH " failed.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void settings_toggle(bool* value) {
    *value = !*value;
    settings_save_changed();
}

static void settings_toggle_title_cache() {
    settings_toggle(&settings.titleCache);
}

static void settings_toggle_clock24h() {
    settings_toggle(&settings.clock24h);
}

static void settings_toggle_date_day_first() {
    settings_toggle(&settings.dateDayFirst);
}

static void settings_toggle_web_auto_accept() {
    settings_toggle(&settings.webAutoAccept);
}

static const char* settings_web_approve_names[WEB_APPROVE_COUNT] = {"Allow all", "Ask each session", "Ask once"};

static void settings_cycle_web_approve_devices() {
    settings.webApproveDevices = (u8) ((settings.webApproveDevices + 1) % WEB_APPROVE_COUNT);
    settings_save_changed();
}

static void settings_forget_web_devices_onresponse(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    if(webserver_forget_devices()) {
        prompt_display_notify("Success", "3DS Web Server devices forgotten.", COLOR_TEXT, NULL, NULL, NULL);
    } else {
        prompt_display_notify("Failure", "Failed to delete " PATHS_WEB_DEVICES ".", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void settings_forget_web_devices() {
    prompt_display_yes_no("Confirmation", "Forget every remembered\n3DS Web Server device?", COLOR_TEXT, NULL, NULL, settings_forget_web_devices_onresponse);
}

static void settings_system_changes_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        settings_toggle(&settings.systemChanges);
    }
}

// Turning the lock off needs no confirmation.
static void settings_toggle_system_changes() {
    if(settings.systemChanges) {
        settings_toggle(&settings.systemChanges);
        return;
    }

    prompt_display_yes_no("Warning",
                          "Allows deleting and installing system\n"
                          "titles, tickets, and saves, and writing\n"
                          "to NAND. A mistake can stop the console\n"
                          "from booting. Back up NAND with\n"
                          "GodMode9 first.\n"
                          "\n"
                          "Allow system changes?",
                          COLOR_WARNING, NULL, NULL, settings_system_changes_onresponse);
}

static void settings_clear_title_cache_onresponse(ui_view* view, void* data, u32 response) {
    if(response != PROMPT_YES) {
        return;
    }

    if(task_clear_title_cache()) {
        prompt_display_notify("Success", "Title cache cleared.", COLOR_TEXT, NULL, NULL, NULL);
    } else {
        prompt_display_notify("Failure", "Failed to clear the title cache.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void settings_clear_title_cache() {
    prompt_display_yes_no("Confirmation", "Clear the title cache?", COLOR_TEXT, NULL, NULL, settings_clear_title_cache_onresponse);
}

static settings_entry title_cache_entry = {
        settings_toggle_title_cache,
        "Keeps the Title Manager list in sdmc:/ftb/cache/ so it opens faster.\n"
        "Off: every title loads from the system each time, like FBI."
};

static settings_entry clear_title_cache_entry = {
        settings_clear_title_cache,
        "Deletes the saved Title Manager list in sdmc:/ftb/cache/.\n"
        "Title Manager reloads everything the next time it opens."
};

static settings_entry clock24h_entry = {
        settings_toggle_clock24h,
        "Clock in the top bar: 12-hour (11:16 PM) or 24-hour (23:16)."
};

static settings_entry date_day_first_entry = {
        settings_toggle_date_day_first,
        "Date in the top bar: month first (10/27/26) or day first (27/10/26)."
};

static settings_entry cia_folders_entry = {
        ciafolders_open,
        "Folders the CIA Installer scans, subfolders included.\n"
        "Stored in sdmc:/ftb/ciaFolders.cfg."
};

static settings_entry web_approve_devices_entry = {
        settings_cycle_web_approve_devices,
        "Who can browse, download, and send while 3DS Web Server is open.\n"
        "Ask once: the 3DS asks once for each phone or PC browser and remembers it.\n"
        "Ask each session: approvals last until the screen closes.\n"
        "Allow all: anyone on your network, without asking."
};

static settings_entry forget_web_devices_entry = {
        settings_forget_web_devices,
        "Deletes the browsers 3DS Web Server remembers (" PATHS_WEB_DEVICES ").\n"
        "Each one asks again on its next visit."
};

static settings_entry web_auto_accept_entry = {
        settings_toggle_web_auto_accept,
        "Off: 3DS Web Server asks before each batch of links or files.\n"
        "On: uploads start without a prompt. Writes to boot.firm, luma, and Nintendo 3DS still ask."
};

static settings_entry system_changes_entry = {
        settings_toggle_system_changes,
        "Off: system titles, tickets, and save data cannot be deleted or replaced, and system storage "
        "(NAND, system saves, shared ext data) opens read-only.\n"
        "On: works like FBI. Back up NAND with GodMode9 first."
};

static settings_entry about_entry = {
        about_open,
        "Build type, install location, data folder, credits, and licenses."
};

static list_item title_cache_item = {.color = COLOR_TEXT, .data = &title_cache_entry};
static list_item clear_title_cache_item = {.name = "Clear title cache", .color = COLOR_TEXT, .data = &clear_title_cache_entry};
static list_item clock24h_item = {.color = COLOR_TEXT, .data = &clock24h_entry};
static list_item date_day_first_item = {.color = COLOR_TEXT, .data = &date_day_first_entry};
static list_item cia_folders_item = {.name = "CIA folders", .color = COLOR_TEXT, .data = &cia_folders_entry};
static list_item web_approve_devices_item = {.color = COLOR_TEXT, .data = &web_approve_devices_entry};
static list_item forget_web_devices_item = {.name = "Forget 3DS Web Server devices", .color = COLOR_TEXT, .data = &forget_web_devices_entry};
static list_item web_auto_accept_item = {.color = COLOR_TEXT, .data = &web_auto_accept_entry};
static list_item system_changes_item = {.color = COLOR_WARNING, .data = &system_changes_entry};
static list_item about_item = {.name = "About", .color = COLOR_TEXT, .data = &about_entry};

static void settings_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    ui_draw_centered_text(((settings_entry*) selected->data)->description, x1, y1, x2, y2);
}

static void settings_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        ((settings_entry*) selected->data)->action();
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &title_cache_item);
        linked_list_add(items, &clear_title_cache_item);
        linked_list_add(items, &clock24h_item);
        linked_list_add(items, &date_day_first_item);
        linked_list_add(items, &cia_folders_item);
        linked_list_add(items, &web_approve_devices_item);
        linked_list_add(items, &forget_web_devices_item);
        linked_list_add(items, &web_auto_accept_item);
        linked_list_add(items, &system_changes_item);
        linked_list_add(items, &about_item);
    }

    snprintf(title_cache_item.name, LIST_ITEM_NAME_MAX, "Title cache: %s", settings.titleCache ? "On" : "Off");
    title_cache_item.color = settings.titleCache ? COLOR_ENABLED : COLOR_DISABLED;
    snprintf(clock24h_item.name, LIST_ITEM_NAME_MAX, "Time format: %s", settings.clock24h ? "24-hour" : "12-hour");
    snprintf(date_day_first_item.name, LIST_ITEM_NAME_MAX, "Date format: %s", settings.dateDayFirst ? "DD/MM/YY" : "MM/DD/YY");
    snprintf(web_approve_devices_item.name, LIST_ITEM_NAME_MAX, "3DS Web Server devices: %s", settings_web_approve_names[settings.webApproveDevices]);
    snprintf(web_auto_accept_item.name, LIST_ITEM_NAME_MAX, "Accept uploads without asking: %s", settings.webAutoAccept ? "On" : "Off");
    web_auto_accept_item.color = settings.webAutoAccept ? COLOR_ENABLED : COLOR_DISABLED;
    snprintf(system_changes_item.name, LIST_ITEM_NAME_MAX, "System changes: %s", settings.systemChanges ? "On" : "Off");
}

void settings_open() {
    list_display("Settings", "A: Select, B: Return", NULL, settings_update, settings_draw_top);
}
