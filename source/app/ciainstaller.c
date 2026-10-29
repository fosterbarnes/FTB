#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "ciafolders.h"
#include "folderpicker.h"
#include "resources.h"
#include "screens.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

// SD root folder names offered on first use. The SD card ignores case, so at most one folder
// matches each name.
static const char* const ciainstaller_detect_names[] = {"cia", ".cia", "cias"};
#define CIA_DETECT_MAX (sizeof(ciainstaller_detect_names) / sizeof(ciainstaller_detect_names[0]))
#define CIA_DETECT_CHUNK 64

typedef struct {
    populate_cias_data populateData;
    bool populated;
    u32 foldersVersion;

    FS_Archive archive;
} ciainstaller_data;

typedef struct {
    linked_list* items;
    list_item* selected;
} ciainstaller_action_data;

static list_item install_cia = {"Install CIA", COLOR_TEXT, action_install_cia};
static list_item install_and_delete_cia = {"Install and delete CIA", COLOR_TEXT, action_install_cia_delete};
static list_item delete_cia = {"Delete", COLOR_TEXT, action_delete_file};
static list_item install_all_cias = {"Install all CIAs", COLOR_TEXT, action_install_cias_all};
static list_item install_and_delete_all_cias = {"Install and delete all CIAs", COLOR_TEXT, action_install_cias_all_delete};

static bool ciainstaller_is_all_row(list_item* item) {
    return (((file_info*) item->data)->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
}

static void ciainstaller_draw_text(const char* text, float x1, float y1, float x2, float y2) {
    float width = 0;
    float height = 0;
    screen_get_string_size(&width, &height, text, 0.5f, 0.5f);
    screen_draw_string(text, x1 + (x2 - x1 - width) / 2, y1 + (y2 - y1 - height) / 2, 0.5f, 0.5f, COLOR_TEXT, true);
}

// The all row: count and total size of the CIA rows in items.
static void ciainstaller_draw_all_info(linked_list* items, float x1, float y1, float x2, float y2) {
    u32 count = 0;
    u64 size = 0;

    linked_list_iter iter;
    linked_list_iterate(items, &iter);
    while(linked_list_iter_has_next(&iter)) {
        file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;
        if(!(info->attributes & FS_ATTRIBUTE_DIRECTORY)) {
            count++;
            size += info->size;
        }
    }

    char text[64];
    if(count == 0) {
        snprintf(text, sizeof(text), "No CIAs");
    } else {
        snprintf(text, sizeof(text), "%lu CIA%s\n%.2f %s", count, count == 1 ? "" : "s", ui_get_display_size(size), ui_get_display_size_units(size));
    }

    ciainstaller_draw_text(text, x1, y1, x2, y2);
}

static void ciainstaller_action_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    ciainstaller_action_data* actionData = (ciainstaller_action_data*) data;

    if(ciainstaller_is_all_row(actionData->selected)) {
        ciainstaller_draw_all_info(actionData->items, x1, y1, x2, y2);
    } else {
        task_draw_file_info(view, actionData->selected->data, x1, y1, x2, y2);
    }
}

static void ciainstaller_action_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    ciainstaller_action_data* actionData = (ciainstaller_action_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        free(data);
        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        void (*action)(linked_list*, list_item*) = (void (*)(linked_list*, list_item*)) selected->data;

        ui_pop();
        list_destroy(view);

        action(actionData->items, actionData->selected);

        free(data);
        return;
    }

    if(linked_list_size(items) == 0) {
        if(ciainstaller_is_all_row(actionData->selected)) {
            linked_list_add(items, &install_all_cias);
            linked_list_add(items, &install_and_delete_all_cias);
        } else {
            if(((file_info*) actionData->selected->data)->isCia) {
                linked_list_add(items, &install_cia);
                linked_list_add(items, &install_and_delete_cia);
            }

            linked_list_add(items, &delete_cia);
        }
    }
}

static void ciainstaller_action_open(linked_list* items, list_item* selected) {
    ciainstaller_action_data* data = (ciainstaller_action_data*) calloc(1, sizeof(ciainstaller_action_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate CIA action data.");

        return;
    }

    data->items = items;
    data->selected = selected;

    list_display(ciainstaller_is_all_row(selected) ? "All CIAs" : "CIA Action", "A: Select, B: Return", data, ciainstaller_action_update, ciainstaller_action_draw_top);
}

static void ciainstaller_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    ciainstaller_data* installerData = (ciainstaller_data*) data;
    populate_cias_data* populateData = &installerData->populateData;
    bool finished = task_is_finished(&populateData->state);

    if(selected != NULL && selected->data != NULL) {
        if(ciainstaller_is_all_row(selected)) {
            ciainstaller_draw_all_info(populateData->items, x1, y1, x2, y2);
        } else {
            task_draw_file_info(view, selected->data, x1, y1, x2, y2);
        }
    } else if(finished) {
        ciainstaller_draw_text(ciafolders_count() == 0 ? "No CIA folders." : "No CIAs found.", x1, y1, x2, y2);
    }

    if(!finished) {
        // Snapshot: the populate thread keeps writing these.
        u32 total = populateData->metaTotal;
        u32 loaded = populateData->metaLoaded;

        char text[64];
        if(total == 0) {
            snprintf(text, sizeof(text), "Scanning CIA folders");
        } else {
            snprintf(text, sizeof(text), "Reading CIAs %lu / %lu", loaded < total ? loaded : total, total);
        }

        float width = 0;
        float height = 0;
        screen_get_string_size(&width, &height, text, 0.5f, 0.5f);
        screen_draw_string(text, x1 + (x2 - x1 - width) / 2, y2 - height - 4, 0.5f, 0.5f, COLOR_TEXT, true);
    }
}

static void ciainstaller_repopulate(ciainstaller_data* installerData, linked_list* items) {
    task_cancel_and_wait(&installerData->populateData.state);

    populate_cias_data* populateData = &installerData->populateData;
    populateData->items = items;
    populateData->archive = installerData->archive;

    populateData->folderCount = ciafolders_count();
    for(u32 i = 0; i < populateData->folderCount; i++) {
        string_copy(populateData->folders[i], ciafolders_get(i), FILE_PATH_MAX);
    }

    Result res = task_populate_cias(populateData);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to initiate CIA list population.");
    }

    installerData->populated = true;
    installerData->foldersVersion = ciafolders_version();
}

static void ciainstaller_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    ciainstaller_data* installerData = (ciainstaller_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();

        task_cancel_and_wait(&installerData->populateData.state);
        fs_close_archive(installerData->archive);
        free(installerData);

        task_clear_files(items);
        list_destroy(view);

        return;
    }

    if(hidKeysDown() & KEY_SELECT) {
        ciafolders_open();
        return;
    }

    // Actions wait for the scan: install and delete remove rows that the meta pass walks.
    if(task_is_finished(&installerData->populateData.state)) {
        if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
            if(!ciainstaller_is_all_row(selected) || linked_list_size(items) > 1) {
                ciainstaller_action_open(items, selected);
                return;
            }
        }

        if((hidKeysDown() & KEY_Y) && linked_list_size(items) > 1) {
            ciainstaller_action_open(items, (list_item*) linked_list_get(items, 0));
            return;
        }
    }

    if(!installerData->populated || (hidKeysDown() & KEY_X) || installerData->foldersVersion != ciafolders_version()) {
        ciainstaller_repopulate(installerData, items);
    }

    task_state* state = &installerData->populateData.state;
    if(task_is_finished(state) && R_FAILED(state->result)) {
        error_display_res(NULL, NULL, state->result, "Failed to populate CIA list.");

        state->result = 0;
    }
}

static void ciainstaller_open_list() {
    ciainstaller_data* data = (ciainstaller_data*) calloc(1, sizeof(ciainstaller_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate CIA installer data.");

        return;
    }

    data->populateData.state.finished = true;

    Result res = fs_open_archive(&data->archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to open the SD card.");

        free(data);
        return;
    }

    list_display("CIA Installer", "A: Select, B: Return, X: Refresh, Y: All CIAs, Select: Folders", data, ciainstaller_update, ciainstaller_draw_top);
}

// First use: no folders configured yet.

static char detected[CIA_DETECT_MAX][FILE_PATH_MAX];
static u32 detectedCount;
static char setupMessage[FILE_PATH_MAX + 48];

static list_item detected_items[CIA_DETECT_MAX];
static list_item use_all_item = {"Use all", COLOR_TEXT, NULL};
static list_item choose_other_item = {"Choose another folder", COLOR_TEXT, NULL};

// Adds the folders, then opens the list (with a notice on top when one was not added).
static void ciainstaller_use_folders(const char (*paths)[FILE_PATH_MAX], u32 count) {
    const char* failure = NULL;
    for(u32 i = 0; i < count; i++) {
        const char* message = ciafolders_add(paths[i]);
        if(failure == NULL) {
            failure = message;
        }
    }

    ciainstaller_open_list();

    if(failure != NULL) {
        prompt_display_notify("Failure", failure, COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void ciainstaller_picked(void* data, const char* path) {
    char paths[1][FILE_PATH_MAX];
    string_copy(paths[0], path, FILE_PATH_MAX);

    ciainstaller_use_folders((const char (*)[FILE_PATH_MAX]) paths, 1);
}

static void ciainstaller_pick_folder() {
    folderpicker_open("Choose CIA Folder", NULL, ciainstaller_picked);
}

// None found: Yes picks a folder, No returns to the main menu.
static void ciainstaller_setup_none_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        ciainstaller_pick_folder();
    }
}

// One found: No picks another folder instead.
static void ciainstaller_setup_single_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        ciainstaller_use_folders((const char (*)[FILE_PATH_MAX]) detected, 1);
    } else {
        ciainstaller_pick_folder();
    }
}

static void ciainstaller_choose_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        ui_pop();
        list_destroy(view);

        if(selected == &use_all_item) {
            ciainstaller_use_folders((const char (*)[FILE_PATH_MAX]) detected, detectedCount);
        } else if(selected == &choose_other_item) {
            ciainstaller_pick_folder();
        } else {
            ciainstaller_use_folders((const char (*)[FILE_PATH_MAX]) selected->data, 1);
        }

        return;
    }

    if(linked_list_size(items) == 0) {
        for(u32 i = 0; i < detectedCount; i++) {
            linked_list_add(items, &detected_items[i]);
        }

        linked_list_add(items, &use_all_item);
        linked_list_add(items, &choose_other_item);
    }
}

static void ciainstaller_choose_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    ciainstaller_draw_text("Choose the folder the CIA Installer scans.\nMore can be added later in Settings.", x1, y1, x2, y2);
}

// Finds the SD root folders named like ciainstaller_detect_names, as "/<name>/" in on-disk case.
static Result ciainstaller_detect() {
    detectedCount = 0;

    FS_Archive archive = 0;
    Result res = fs_open_archive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        return res;
    }

    FS_DirectoryEntry* entries = (FS_DirectoryEntry*) calloc(CIA_DETECT_CHUNK, sizeof(FS_DirectoryEntry));
    Handle dirHandle = 0;
    if(entries == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(R_SUCCEEDED(res = FSUSER_OpenDirectory(&dirHandle, archive, fsMakePath(PATH_ASCII, "/")))) {
        u32 count = 0;
        while(R_SUCCEEDED(res = FSDIR_Read(dirHandle, &count, CIA_DETECT_CHUNK, entries)) && count > 0) {
            for(u32 i = 0; i < count && detectedCount < CIA_DETECT_MAX; i++) {
                if(!(entries[i].attributes & FS_ATTRIBUTE_DIRECTORY)) {
                    continue;
                }

                char name[FILE_NAME_MAX] = {'\0'};
                utf16_to_utf8((uint8_t*) name, entries[i].name, FILE_NAME_MAX - 1);

                for(u32 n = 0; n < CIA_DETECT_MAX; n++) {
                    if(strcasecmp(name, ciainstaller_detect_names[n]) == 0) {
                        snprintf(detected[detectedCount++], FILE_PATH_MAX, "/%s/", name);
                        break;
                    }
                }
            }
        }

        FSDIR_Close(dirHandle);
    }

    free(entries);
    fs_close_archive(archive);

    return res;
}

static void ciainstaller_setup() {
    Result res = ciainstaller_detect();
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to look for CIA folders.");
        return;
    }

    if(detectedCount == 0) {
        prompt_display_yes_no("Confirmation", "No CIA folder found on the SD card.\nChoose one?", COLOR_TEXT, NULL, NULL, ciainstaller_setup_none_onresponse);
    } else if(detectedCount == 1) {
        snprintf(setupMessage, sizeof(setupMessage), "Use %s as your CIA folder?\nNo: choose another folder.", detected[0]);
        prompt_display_yes_no("Confirmation", setupMessage, COLOR_TEXT, NULL, NULL, ciainstaller_setup_single_onresponse);
    } else {
        for(u32 i = 0; i < detectedCount; i++) {
            string_copy(detected_items[i].name, detected[i], LIST_ITEM_NAME_MAX);
            detected_items[i].color = COLOR_DIRECTORY;
            detected_items[i].data = detected[i];
        }

        list_display("Choose CIA Folder", "A: Select, B: Return", NULL, ciainstaller_choose_update, ciainstaller_choose_draw_top);
    }
}

void ciainstaller_open() {
    if(ciafolders_count() > 0) {
        ciainstaller_open_list();
    } else {
        ciainstaller_setup();
    }
}
