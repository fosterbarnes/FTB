#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "settings.h"
#include "task/uitask.h"
#include "../core/core.h"

#define SETTINGS_PATH "sdmc:/ftb/settings.cfg"
#define SETTINGS_TEXT_MARGIN 10

static ftb_settings settings = {.titleCache = true};

// On/off settings stored as key=0|1. The file is user-editable: anything else keeps the default.
static const struct {
    const char* key;
    bool* value;
} settings_bools[] = {
        {"titleCache", &settings.titleCache},
};

#define SETTINGS_BOOL_COUNT (sizeof(settings_bools) / sizeof(settings_bools[0]))

const ftb_settings* settings_get() {
    return &settings;
}

void settings_load() {
    FILE* fd = fopen(SETTINGS_PATH, "r");
    if(fd == NULL) {
        return;
    }

    char line[128];
    while(fgets(line, sizeof(line), fd) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';

        char* value = strchr(line, '=');
        if(value == NULL || (strcmp(value + 1, "0") != 0 && strcmp(value + 1, "1") != 0)) {
            continue;
        }

        *value++ = '\0';

        for(u32 i = 0; i < SETTINGS_BOOL_COUNT; i++) {
            if(strcmp(line, settings_bools[i].key) == 0) {
                *settings_bools[i].value = value[0] == '1';
            }
        }
    }

    fclose(fd);
}

static bool settings_save() {
    mkdir("sdmc:/ftb", 0777);

    FILE* fd = fopen(SETTINGS_PATH, "w");
    if(fd == NULL) {
        return false;
    }

    bool written = true;
    for(u32 i = 0; i < SETTINGS_BOOL_COUNT && written; i++) {
        written = fprintf(fd, "%s=%d\n", settings_bools[i].key, *settings_bools[i].value ? 1 : 0) > 0;
    }

    return fclose(fd) == 0 && written;
}

typedef struct settings_entry_s {
    void (*action)();
    const char* description; // top screen text while the item is selected
} settings_entry;

static void settings_toggle_title_cache() {
    settings.titleCache = !settings.titleCache;

    // The new value still applies for this session.
    if(!settings_save()) {
        prompt_display_notify("Failure", "Saving " SETTINGS_PATH " failed.", COLOR_TEXT, NULL, NULL, NULL);
    }
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

static list_item title_cache_item = {"", COLOR_TEXT, &title_cache_entry};
static list_item clear_title_cache_item = {"Clear title cache", COLOR_TEXT, &clear_title_cache_entry};

static void settings_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    const char* text = ((settings_entry*) selected->data)->description;
    float wrapWidth = x2 - x1 - SETTINGS_TEXT_MARGIN * 2;

    float width = 0;
    float height = 0;
    screen_get_string_size_wrap(&width, &height, text, 0.5f, 0.5f, wrapWidth);

    float x = x1 + (x2 - x1 - width) / 2;
    float y = y1 + (y2 - y1 - height) / 2;
    screen_draw_string_wrap(text, x, y, 0.5f, 0.5f, COLOR_TEXT, true, x + wrapWidth);
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
    }

    snprintf(title_cache_item.name, LIST_ITEM_NAME_MAX, "Title cache: %s", settings.titleCache ? "On" : "Off");
    title_cache_item.color = settings.titleCache ? COLOR_ENABLED : COLOR_DISABLED;
}

void settings_open() {
    list_display("Settings", "A: Select, B: Return", NULL, settings_update, settings_draw_top);
}
