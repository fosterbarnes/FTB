#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "ciafolders.h"
#include "resources.h"
#include "screens.h"
#include "settings.h"
#include "../core/core.h"

static void mainmenu_legacy_open();

static list_item cia_installer = {.name = "CIA Installer", .color = COLOR_TEXT, .data = ciainstaller_open};
static list_item titles = {.name = "Title Manager", .color = COLOR_TEXT, .data = titlemanager_open};
static list_item ftp_server = {.name = "FTP Server", .color = COLOR_TEXT, .data = ftp_open};
static list_item receive = {.name = "Receive", .color = COLOR_TEXT, .data = receive_open};
// Same screen as FBI Legacy > SD until the SD File Browser gets its own features.
static list_item file_browser = {.name = "SD File Browser", .color = COLOR_TEXT, .data = files_open_sd};
static list_item remote_install = {.name = "Remote Install", .color = COLOR_TEXT, .data = remoteinstall_open};
// Sets FBI Legacy, Update, and Settings apart from the main tools.
static list_item spacer = {.color = COLOR_TEXT, .spacer = true};
static list_item fbi_legacy = {.name = "FBI Legacy", .color = COLOR_TEXT, .data = mainmenu_legacy_open};
static list_item update = {.name = "Update", .color = COLOR_TEXT, .data = update_open};
static list_item settings = {.name = "Settings", .color = COLOR_TEXT, .data = settings_open};

// FBI Legacy submenu.
static list_item sd = {.name = "SD", .color = COLOR_TEXT, .data = files_open_sd};
static list_item titles_legacy = {.name = "Titles", .color = COLOR_TEXT, .data = titles_legacy_open};
static list_item ctr_nand = {.name = "CTR NAND", .color = COLOR_TEXT, .data = files_open_ctr_nand};
static list_item twl_nand = {.name = "TWL NAND", .color = COLOR_TEXT, .data = files_open_twl_nand};
static list_item twl_photo = {.name = "TWL Photo", .color = COLOR_TEXT, .data = files_open_twl_photo};
static list_item twl_sound = {.name = "TWL Sound", .color = COLOR_TEXT, .data = files_open_twl_sound};
static list_item dump_nand = {.name = "Dump NAND", .color = COLOR_TEXT, .data = dumpnand_open};
static list_item pending_titles = {.name = "Pending Titles", .color = COLOR_TEXT, .data = pendingtitles_open};
static list_item tickets = {.name = "Tickets", .color = COLOR_TEXT, .data = tickets_open};
static list_item ext_save_data = {.name = "Ext Save Data", .color = COLOR_TEXT, .data = extsavedata_open};
static list_item system_save_data = {.name = "System Save Data", .color = COLOR_TEXT, .data = systemsavedata_open};

static void mainmenu_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    u32 logoWidth;
    u32 logoHeight;
    screen_get_texture_size(&logoWidth, &logoHeight, TEXTURE_LOGO);

    screen_draw_texture(TEXTURE_LOGO, x1 + (x2 - x1 - logoWidth) / 2, y1 + (y2 - y1 - logoHeight) / 2, logoWidth, logoHeight);
}

static void mainmenu_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_START) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || hidKeysDown() & KEY_A) && selected->data != NULL) {
        ((void(*)()) selected->data)();
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &cia_installer);
        linked_list_add(items, &titles);
        linked_list_add(items, &file_browser);
        linked_list_add(items, &ftp_server);
        linked_list_add(items, &receive);
        linked_list_add(items, &remote_install);
        linked_list_add(items, &spacer);
        linked_list_add(items, &fbi_legacy);
        linked_list_add(items, &update);
        linked_list_add(items, &settings);
    }
}

static void mainmenu_legacy_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || hidKeysDown() & KEY_A) && selected->data != NULL) {
        ((void(*)()) selected->data)();
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &sd);
        linked_list_add(items, &titles_legacy);
        linked_list_add(items, &ctr_nand);
        linked_list_add(items, &twl_nand);
        linked_list_add(items, &twl_photo);
        linked_list_add(items, &twl_sound);
        linked_list_add(items, &dump_nand);
        linked_list_add(items, &pending_titles);
        linked_list_add(items, &tickets);
        linked_list_add(items, &ext_save_data);
        linked_list_add(items, &system_save_data);
    }
}

static void mainmenu_legacy_open() {
    list_display("FBI Legacy", "A: Select, B: Return", NULL, mainmenu_legacy_update, mainmenu_draw_top);
}

void mainmenu_open() {
    resources_load();
    settings_load();
    ciafolders_load();

    list_display("Main Menu", "A: Select, START: Exit", NULL, mainmenu_update, mainmenu_draw_top);
}
