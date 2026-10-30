#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "paths.h"
#include "resources.h"
#include "../core/core.h"

static bool resources_load_texture_from(u32 id, const char* dir, const char* name) {
    char path[FILE_PATH_MAX];
    snprintf(path, sizeof(path), "%s%s", dir, name);

    FILE* fd = fopen(path, "rb");
    if(fd == NULL) {
        return false;
    }

    bool loaded = screen_load_texture_file(id, fd, true);

    fclose(fd);
    return loaded;
}

// A missing or corrupt theme PNG falls back to the built-in art instead of panicking on every launch.
static void resources_load_texture(u32 id, const char* name) {
    if(!resources_load_texture_from(id, PATHS_THEME_DIR, name) && !resources_load_texture_from(id, "romfs:/", name)) {
        error_panic("Failed to load texture \"%s\".\n", name);
    }
}

// textcolor.cfg keys (matched case-insensitively). Names are the theme contract: never rename.
static const struct {
    const char* key;
    u32 colorId;
} resources_colors[] = {
    {"text", COLOR_TEXT},
    {"nand", COLOR_NAND},
    {"sd", COLOR_SD},
    {"gamecard", COLOR_GAME_CARD},
    {"dstitle", COLOR_DS_TITLE},
    {"file", COLOR_FILE},
    {"directory", COLOR_DIRECTORY},
    {"enabled", COLOR_ENABLED},
    {"disabled", COLOR_DISABLED},
    {"ticketinuse", COLOR_TICKET_IN_USE},
    {"ticketnotinuse", COLOR_TICKET_NOT_IN_USE},
    {"ftperror", COLOR_FTP_ERROR},
    {"ftpcommand", COLOR_FTP_COMMAND},
    {"ftpresponse", COLOR_FTP_RESPONSE},
    {"textdim", COLOR_TEXT_DIM},
    {"warning", COLOR_WARNING},
};

// romfs/ file names, overridable from the theme folder. Names are the theme contract: never rename.
static const struct {
    u32 id;
    const char* file;
} resources_textures[] = {
    {TEXTURE_BOTTOM_SCREEN_BG, "bottom_screen_bg.png"},
    {TEXTURE_BOTTOM_SCREEN_TOP_BAR, "bottom_screen_top_bar.png"},
    {TEXTURE_BOTTOM_SCREEN_TOP_BAR_SHADOW, "bottom_screen_top_bar_shadow.png"},
    {TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR, "bottom_screen_bottom_bar.png"},
    {TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR_SHADOW, "bottom_screen_bottom_bar_shadow.png"},
    {TEXTURE_TOP_SCREEN_BG, "top_screen_bg.png"},
    {TEXTURE_TOP_SCREEN_TOP_BAR, "top_screen_top_bar.png"},
    {TEXTURE_TOP_SCREEN_TOP_BAR_SHADOW, "top_screen_top_bar_shadow.png"},
    {TEXTURE_TOP_SCREEN_BOTTOM_BAR, "top_screen_bottom_bar.png"},
    {TEXTURE_TOP_SCREEN_BOTTOM_BAR_SHADOW, "top_screen_bottom_bar_shadow.png"},
    {TEXTURE_LOGO, "logo.png"},
    {TEXTURE_SELECTION_OVERLAY, "selection_overlay.png"},
    {TEXTURE_SCROLL_BAR, "scroll_bar.png"},
    {TEXTURE_BUTTON, "button.png"},
    {TEXTURE_PROGRESS_BAR_BG, "progress_bar_bg.png"},
    {TEXTURE_PROGRESS_BAR_CONTENT, "progress_bar_content.png"},
    {TEXTURE_META_INFO_BOX, "meta_info_box.png"},
    {TEXTURE_META_INFO_BOX_SHADOW, "meta_info_box_shadow.png"},
    {TEXTURE_BATTERY_CHARGING, "battery_charging.png"},
    {TEXTURE_BATTERY_0, "battery0.png"},
    {TEXTURE_BATTERY_1, "battery1.png"},
    {TEXTURE_BATTERY_2, "battery2.png"},
    {TEXTURE_BATTERY_3, "battery3.png"},
    {TEXTURE_BATTERY_4, "battery4.png"},
    {TEXTURE_BATTERY_5, "battery5.png"},
    {TEXTURE_WIFI_DISCONNECTED, "wifi_disconnected.png"},
    {TEXTURE_WIFI_0, "wifi0.png"},
    {TEXTURE_WIFI_1, "wifi1.png"},
    {TEXTURE_WIFI_2, "wifi2.png"},
    {TEXTURE_WIFI_3, "wifi3.png"},
    {TEXTURE_TOUCH_BUTTON, "touch_button.png"},
    {TEXTURE_TOUCH_BUTTON_HELD, "touch_button_held.png"},
    {TEXTURE_ICON_SORT, "icon_sort.png"},
    {TEXTURE_ICON_ZOOM_IN, "icon_zoom_in.png"},
    {TEXTURE_ICON_ZOOM_OUT, "icon_zoom_out.png"},
    {TEXTURE_ICON_LAYOUT_GRID, "icon_layout_grid.png"},
    {TEXTURE_ICON_LAYOUT_TOP, "icon_layout_top.png"},
    {TEXTURE_ICON_LAYOUT_BOTTOM, "icon_layout_bottom.png"},
    {TEXTURE_ICON_LAYOUT_ICON_LIST, "icon_layout_icon_list.png"},
    {TEXTURE_ICON_LAYOUT_GRID_BOTTOM, "icon_layout_grid_bottom.png"},
    {TEXTURE_ICON_LAYOUT_ICON_LIST_BOTTOM, "icon_layout_icon_list_bottom.png"},
};

#define RESOURCES_COUNT(table) (sizeof(table) / sizeof(table[0]))

// Reads one textcolor.cfg over colors (indexed by color ID) and marks the keys it has in colorSet.
// False when the file cannot be opened.
static bool resources_read_colors(const char* path, u32* colors, bool* colorSet) {
    FILE* fd = fopen(path, "rb");
    if(fd == NULL) {
        return false;
    }

    char line[128];
    while(fgets(line, sizeof(line), fd) != NULL) {
        char key[64];
        u32 color = 0;

        // Lines without "key=hex" (blank lines, comments) are skipped.
        if(sscanf(line, "%63[^=]=%lx", key, &color) != 2) {
            continue;
        }

        for(u32 i = 0; i < RESOURCES_COUNT(resources_colors); i++) {
            if(strcasecmp(key, resources_colors[i].key) == 0) {
                u32 id = resources_colors[i].colorId;

                colors[id] = color;
                colorSet[id] = true;
                break;
            }
        }
    }

    fclose(fd);
    return true;
}

void resources_load() {
    u32 colors[COLOR_WARNING + 1] = {0};
    bool builtInSet[COLOR_WARNING + 1] = {false};
    if(!resources_read_colors("romfs:/textcolor.cfg", colors, builtInSet)) {
        error_panic("Failed to open text color config: %s\n", strerror(errno));
        return;
    }

    // A theme textcolor.cfg layers over the built-in colors, so a key it lacks never stays transparent.
    bool colorSet[COLOR_WARNING + 1] = {false};
    if(resources_read_colors(PATHS_THEME_DIR "textcolor.cfg", colors, colorSet)) {
        // Themes made before the FTP screen have no ftp* keys, and themes made before the icon list have
        // no textdim key; those use the text color. Without a warning key, warnings use ftperror, then
        // text.
        if(!colorSet[COLOR_WARNING]) {
            colors[COLOR_WARNING] = colorSet[COLOR_FTP_ERROR] ? colors[COLOR_FTP_ERROR] : colors[COLOR_TEXT];
        }

        for(u32 id = COLOR_FTP_ERROR; id <= COLOR_TEXT_DIM; id++) {
            if(!colorSet[id]) {
                colors[id] = colors[COLOR_TEXT];
            }
        }
    }

    for(u32 id = 0; id <= COLOR_WARNING; id++) {
        screen_set_color(id, colors[id]);
    }

    for(u32 i = 0; i < RESOURCES_COUNT(resources_textures); i++) {
        resources_load_texture(resources_textures[i].id, resources_textures[i].file);
    }
}