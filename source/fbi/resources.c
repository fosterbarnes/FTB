#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "../core/core.h"

static FILE* resources_open_file(const char* path) {
    char realPath[FILE_PATH_MAX];
    snprintf(realPath, sizeof(realPath), "sdmc:/ftb/theme/%s", path);

    FILE* fd = fopen(realPath, "rb");

    if(fd != NULL) {
        return fd;
    } else {
        snprintf(realPath, sizeof(realPath), "romfs:/%s", path);

        return fopen(realPath, "rb");
    }
}

static void resources_load_texture(u32 id, const char* name) {
    FILE* fd = resources_open_file(name);
    if(fd == NULL) {
        error_panic("Failed to open texture \"%s\": %s\n", name, strerror(errno));
        return;
    }

    screen_load_texture_file(id, fd, true);

    fclose(fd);
}

void resources_load() {
    FILE* fd = resources_open_file("textcolor.cfg");
    if(fd == NULL) {
        error_panic("Failed to open text color config: %s\n", strerror(errno));
        return;
    }

    // Themes made before the FTP screen have no ftp* keys; those log lines use the text color.
    u32 textColor = 0xFFFFFFFF;
    bool ftpColorSet[3] = {false, false, false};

    char line[128];
    while(fgets(line, sizeof(line), fd) != NULL) {
        char key[64];
        u32 color = 0;

        sscanf(line, "%63[^=]=%lx", key, &color);

        if(strcasecmp(key, "text") == 0) {
            screen_set_color(COLOR_TEXT, color);
            textColor = color;
        } else if(strcasecmp(key, "nand") == 0) {
            screen_set_color(COLOR_NAND, color);
        } else if(strcasecmp(key, "sd") == 0) {
            screen_set_color(COLOR_SD, color);
        } else if(strcasecmp(key, "gamecard") == 0) {
            screen_set_color(COLOR_GAME_CARD, color);
        } else if(strcasecmp(key, "dstitle") == 0) {
            screen_set_color(COLOR_DS_TITLE, color);
        } else if(strcasecmp(key, "file") == 0) {
            screen_set_color(COLOR_FILE, color);
        } else if(strcasecmp(key, "directory") == 0) {
            screen_set_color(COLOR_DIRECTORY, color);
        } else if(strcasecmp(key, "enabled") == 0) {
            screen_set_color(COLOR_ENABLED, color);
        } else if(strcasecmp(key, "disabled") == 0) {
            screen_set_color(COLOR_DISABLED, color);
        } else if(strcasecmp(key, "ticketinuse") == 0) {
            screen_set_color(COLOR_TICKET_IN_USE, color);
        } else if(strcasecmp(key, "ticketnotinuse") == 0) {
            screen_set_color(COLOR_TICKET_NOT_IN_USE, color);
        } else if(strcasecmp(key, "ftperror") == 0) {
            screen_set_color(COLOR_FTP_ERROR, color);
            ftpColorSet[0] = true;
        } else if(strcasecmp(key, "ftpcommand") == 0) {
            screen_set_color(COLOR_FTP_COMMAND, color);
            ftpColorSet[1] = true;
        } else if(strcasecmp(key, "ftpresponse") == 0) {
            screen_set_color(COLOR_FTP_RESPONSE, color);
            ftpColorSet[2] = true;
        }
    }

    fclose(fd);

    for(u32 i = 0; i < 3; i++) {
        if(!ftpColorSet[i]) {
            screen_set_color(COLOR_FTP_ERROR + i, textColor);
        }
    }

    resources_load_texture(TEXTURE_BOTTOM_SCREEN_BG, "bottom_screen_bg.png");
    resources_load_texture(TEXTURE_BOTTOM_SCREEN_TOP_BAR, "bottom_screen_top_bar.png");
    resources_load_texture(TEXTURE_BOTTOM_SCREEN_TOP_BAR_SHADOW, "bottom_screen_top_bar_shadow.png");
    resources_load_texture(TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR, "bottom_screen_bottom_bar.png");
    resources_load_texture(TEXTURE_BOTTOM_SCREEN_BOTTOM_BAR_SHADOW, "bottom_screen_bottom_bar_shadow.png");
    resources_load_texture(TEXTURE_TOP_SCREEN_BG, "top_screen_bg.png");
    resources_load_texture(TEXTURE_TOP_SCREEN_TOP_BAR, "top_screen_top_bar.png");
    resources_load_texture(TEXTURE_TOP_SCREEN_TOP_BAR_SHADOW, "top_screen_top_bar_shadow.png");
    resources_load_texture(TEXTURE_TOP_SCREEN_BOTTOM_BAR, "top_screen_bottom_bar.png");
    resources_load_texture(TEXTURE_TOP_SCREEN_BOTTOM_BAR_SHADOW, "top_screen_bottom_bar_shadow.png");
    resources_load_texture(TEXTURE_LOGO, "logo.png");
    resources_load_texture(TEXTURE_SELECTION_OVERLAY, "selection_overlay.png");
    resources_load_texture(TEXTURE_SCROLL_BAR, "scroll_bar.png");
    resources_load_texture(TEXTURE_BUTTON, "button.png");
    resources_load_texture(TEXTURE_PROGRESS_BAR_BG, "progress_bar_bg.png");
    resources_load_texture(TEXTURE_PROGRESS_BAR_CONTENT, "progress_bar_content.png");
    resources_load_texture(TEXTURE_META_INFO_BOX, "meta_info_box.png");
    resources_load_texture(TEXTURE_META_INFO_BOX_SHADOW, "meta_info_box_shadow.png");
    resources_load_texture(TEXTURE_BATTERY_CHARGING, "battery_charging.png");
    resources_load_texture(TEXTURE_BATTERY_0, "battery0.png");
    resources_load_texture(TEXTURE_BATTERY_1, "battery1.png");
    resources_load_texture(TEXTURE_BATTERY_2, "battery2.png");
    resources_load_texture(TEXTURE_BATTERY_3, "battery3.png");
    resources_load_texture(TEXTURE_BATTERY_4, "battery4.png");
    resources_load_texture(TEXTURE_BATTERY_5, "battery5.png");
    resources_load_texture(TEXTURE_WIFI_DISCONNECTED, "wifi_disconnected.png");
    resources_load_texture(TEXTURE_WIFI_0, "wifi0.png");
    resources_load_texture(TEXTURE_WIFI_1, "wifi1.png");
    resources_load_texture(TEXTURE_WIFI_2, "wifi2.png");
    resources_load_texture(TEXTURE_WIFI_3, "wifi3.png");
    resources_load_texture(TEXTURE_TOUCH_BUTTON, "touch_button.png");
    resources_load_texture(TEXTURE_TOUCH_BUTTON_HELD, "touch_button_held.png");
    resources_load_texture(TEXTURE_ICON_SORT, "icon_sort.png");
    resources_load_texture(TEXTURE_ICON_ZOOM_IN, "icon_zoom_in.png");
    resources_load_texture(TEXTURE_ICON_ZOOM_OUT, "icon_zoom_out.png");
    resources_load_texture(TEXTURE_ICON_LAYOUT_GRID, "icon_layout_grid.png");
    resources_load_texture(TEXTURE_ICON_LAYOUT_TOP, "icon_layout_top.png");
    resources_load_texture(TEXTURE_ICON_LAYOUT_BOTTOM, "icon_layout_bottom.png");
}