#include <stdio.h>

#include <3ds.h>

#include "resources.h"
#include "screens.h"
#include "screenshot.h"
#include "../core/core.h"

#define ABOUT_MARGIN 10
#define ABOUT_LOGO_GAP 12
#define ABOUT_TITLE_SCALE 0.75f
#define ABOUT_SECRET_HOLD_MS 800

// Credits from README.md "Credit" and LICENSE.txt. Item data is the detail prompt text, kept to
// short lines because prompts do not wrap.
static list_item about_items[] = {
        {.name = "FTB: fosterbarnes", .color = COLOR_TEXT, .data =
         "Dark-themed fork of FBI 2.6.1.\n"
         "Adds a new Title Manager, CIA\n"
         "Installer, FTP Server, 3DS Web Server,\n"
         "themes, and in-app updates.\n"
         "Logo by fosterbarnes.\n"
         "github.com/fosterbarnes/FTB"},
        {.name = "FBI: Steveice10", .color = COLOR_TEXT, .data =
         "The 3DS title manager FTB is forked from.\n"
         "Copyright (C) 2015-2019 Steveice10, MIT\n"
         "github.com/Steveice10/FBI"},
        {.name = "FTP server: ftpd", .color = COLOR_TEXT, .data =
         "Michael Theall, Dave Murphy, TuxSH\n"
         "GPL v3 or later\n"
         "github.com/mtheall/ftpd"},
        {.name = "SPI protocol: TuxSH", .color = COLOR_TEXT, .data =
         "Save chip access for DS cards.\n"
         "github.com/TuxSH/TWLSaveTool"},
        {.name = "Original banner", .color = COLOR_TEXT, .data =
         "Original FBI banner by\n"
         "OctopusRift, Apache Thunder, PabloMK7"},
        {.name = "Original logo: PabloMK7", .color = COLOR_TEXT, .data =
         "Original FBI logo by PabloMK7"},
        {.name = "Libraries", .color = COLOR_TEXT, .data =
         "libctru, citro3d: zlib license\n"
         "curl: curl license\n"
         "mbedTLS: Apache 2.0\n"
         "jansson: MIT\n"
         "zlib: zlib license\n"
         "quirc: ISC (Daniel Beer)\n"
         "QR Code generator: MIT (Project Nayuki)\n"
         "stb_image: public domain"},
        {.name = "License", .color = COLOR_TEXT, .data =
         "FBI and FTB source: MIT\n"
         "ftpd is GPL v3 or later, so FTB\n"
         "binaries are GPL v3 or later.\n"
         "Full text: LICENSE.txt"},
};

#define ABOUT_ITEM_COUNT (sizeof(about_items) / sizeof(about_items[0]))

static char about_text[FILE_PATH_MAX + 256];

// Install location is fixed for the session, so it is read once instead of every frame.
static void about_build_text() {
    char install[FILE_PATH_MAX + 32];
    const char* build = "CIA";

    if(envIsHomebrew()) {
        build = "3dsx";

        // fs_set_3dsx_path strips "sdmc:", so a leading '/' is an SD path.
        const char* path = fs_get_3dsx_path();
        if(path == NULL) {
            snprintf(install, sizeof(install), "unknown");
        } else {
            snprintf(install, sizeof(install), "%s%s", path[0] == '/' ? "sdmc:" : "", path);
        }
    } else {
        u64 titleId = 0;
        FS_MediaType mediaType = MEDIATYPE_SD;
        if(R_FAILED(APT_GetAppletInfo((NS_APPID) envGetAptAppId(), &titleId, (u8*) &mediaType, NULL, NULL, NULL))) {
            snprintf(install, sizeof(install), "unknown");
        } else {
            const char* media = "SD";
            if(mediaType == MEDIATYPE_GAME_CARD) {
                build = "3DS";
                media = "Game card";
            } else if(mediaType == MEDIATYPE_NAND) {
                media = "NAND";
            }

            snprintf(install, sizeof(install), "%s title %016llX", media, titleId);
        }
    }

    snprintf(about_text, sizeof(about_text),
             "v%d.%d.%d\n"
             "github.com/fosterbarnes/FTB\n"
             "\n"
             "Build: %s\n"
             "Install: %s\n"
             "Data: sdmc:/ftb/\n"
             "\n"
             "made with <3 by fosterbarnes",
             VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO, build, install);
}

// ciaGoober About layout: logo left at 1:1, title and info column right, centered as a group.
static void about_draw_panel(float x1, float y1, float x2, float y2) {
    u32 logoWidth;
    u32 logoHeight;
    screen_get_texture_size(&logoWidth, &logoHeight, TEXTURE_LOGO);

    float wrapWidth = x2 - x1 - ABOUT_MARGIN * 2 - logoWidth - ABOUT_LOGO_GAP;

    float titleWidth;
    float titleHeight;
    screen_get_string_size(&titleWidth, &titleHeight, "FTB", ABOUT_TITLE_SCALE, ABOUT_TITLE_SCALE);

    float textWidth;
    float textHeight;
    screen_get_string_size_wrap(&textWidth, &textHeight, about_text, 0.5f, 0.5f, wrapWidth);

    float columnWidth = textWidth > titleWidth ? textWidth : titleWidth;
    float columnHeight = titleHeight + 4 + textHeight;

    float logoX = x1 + (x2 - x1 - logoWidth - ABOUT_LOGO_GAP - columnWidth) / 2;
    screen_draw_texture(TEXTURE_LOGO, logoX, y1 + (y2 - y1 - logoHeight) / 2, logoWidth, logoHeight);

    float textX = logoX + logoWidth + ABOUT_LOGO_GAP;
    float textY = y1 + (y2 - y1 - columnHeight) / 2;
    screen_draw_string("FTB", textX, textY, ABOUT_TITLE_SCALE, ABOUT_TITLE_SCALE, COLOR_TEXT, false);
    screen_draw_string_wrap(about_text, textX, textY + titleHeight + 4, 0.5f, 0.5f, COLOR_TEXT, false, textX + wrapWidth);
}

static void about_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    about_draw_panel(x1, y1, x2, y2);
}

static void about_prompt_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    about_draw_panel(x1, y1, x2, y2);
}

// When the current START hold began on this screen; 0 = no hold being timed.
static u64 about_start_held_since = 0;

static void about_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    u32 down = hidKeysDown();
    if(down & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(down & KEY_START) {
        about_start_held_since = osGetTime();
    } else if(!(hidKeysHeld() & KEY_START)) {
        about_start_held_since = 0;
    }

    if(about_start_held_since != 0 && osGetTime() - about_start_held_since >= ABOUT_SECRET_HOLD_MS) {
        about_start_held_since = 0;
        screenshot_mode_toggle();

        if(screenshot_mode_on()) {
            prompt_display_notify("Screenshot mode", "On until FTB closes.\nSTART saves a screenshot to\nsdmc:/ftb/screenshots/.", COLOR_TEXT, NULL, about_prompt_draw_top, NULL);
        } else {
            prompt_display_notify("Screenshot mode", "Off. START exits FTB again.", COLOR_TEXT, NULL, about_prompt_draw_top, NULL);
        }

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        prompt_display_notify(selected->name, (const char*) selected->data, COLOR_TEXT, NULL, about_prompt_draw_top, NULL);
        return;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < ABOUT_ITEM_COUNT; i++) {
            linked_list_add(items, &about_items[i]);
        }
    }
}

void about_open() {
    about_start_held_since = 0;
    about_build_text();
    list_display("About", "A: Details, B: Return", NULL, about_update, about_draw_top);
}
