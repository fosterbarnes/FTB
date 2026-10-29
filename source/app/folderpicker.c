#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "folderpicker.h"
#include "resources.h"
#include "task/uitask.h"
#include "../core/core.h"

typedef struct {
    populate_files_data populateData;
    bool populated;

    FS_Archive archive;
    char currDir[FILE_PATH_MAX];

    void* data;
    void (*onPicked)(void* data, const char* path);
} folderpicker_data;

static bool folderpicker_filter(void* data, const char* name, u32 attributes) {
    return (attributes & FS_ATTRIBUTE_DIRECTORY) && !(attributes & FS_ATTRIBUTE_HIDDEN);
}

static void folderpicker_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL || selected->data == NULL) {
        return;
    }

    ui_draw_centered_text(((file_info*) selected->data)->path, x1, y1, x2, y2);
}

static void folderpicker_navigate_up(folderpicker_data* pickerData) {
    char parentDir[FILE_PATH_MAX];
    string_get_parent_path(parentDir, pickerData->currDir, FILE_PATH_MAX);
    string_copy(pickerData->currDir, parentDir, FILE_PATH_MAX);

    pickerData->populated = false;
}

static void folderpicker_close(ui_view* view, folderpicker_data* pickerData, linked_list* items) {
    task_cancel_and_wait(&pickerData->populateData.state);

    ui_pop();

    if(pickerData->archive != 0) {
        fs_close_archive(pickerData->archive);
    }

    free(pickerData);

    task_clear_files(items);
    list_destroy(view);
}

static void folderpicker_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    folderpicker_data* pickerData = (folderpicker_data*) data;

    // The picker has no actions, so a folder can only be missing when it is about to be listed.
    while(!pickerData->populated && strcmp(pickerData->currDir, "/") != 0 && !fs_is_dir(pickerData->archive, pickerData->currDir)) {
        folderpicker_navigate_up(pickerData);
    }

    if(hidKeysDown() & KEY_B) {
        if(strcmp(pickerData->currDir, "/") == 0) {
            folderpicker_close(view, pickerData, items);
            return;
        }

        folderpicker_navigate_up(pickerData);
    }

    bool pick = (hidKeysDown() & KEY_Y) != 0;
    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        if(((file_info*) selected->data)->isBase) {
            pick = true;
        } else {
            string_copy(pickerData->currDir, ((file_info*) selected->data)->path, FILE_PATH_MAX);
            pickerData->populated = false;
        }
    }

    if(pick) {
        char path[FILE_PATH_MAX];
        string_copy(path, pickerData->currDir, FILE_PATH_MAX);

        void* pickedData = pickerData->data;
        void (*onPicked)(void* data, const char* path) = pickerData->onPicked;

        folderpicker_close(view, pickerData, items);
        onPicked(pickedData, path);
        return;
    }

    if(!pickerData->populated) {
        task_cancel_and_wait(&pickerData->populateData.state);

        pickerData->populateData.items = items;
        pickerData->populateData.archive = pickerData->archive;
        string_copy(pickerData->populateData.path, pickerData->currDir, FILE_PATH_MAX);

        Result res = task_populate_files(&pickerData->populateData);
        if(R_FAILED(res)) {
            error_display_res(NULL, NULL, res, "Failed to initiate folder list population.");
        }

        pickerData->populated = true;
    }

    if(task_is_finished(&pickerData->populateData.state) && R_FAILED(pickerData->populateData.state.result)) {
        error_display_res(NULL, NULL, pickerData->populateData.state.result, "Failed to populate folder list.");

        pickerData->populateData.state.result = 0;
    }
}

void folderpicker_open(const char* title, void* data, void (*onPicked)(void* data, const char* path)) {
    folderpicker_data* pickerData = (folderpicker_data*) calloc(1, sizeof(folderpicker_data));
    if(pickerData == NULL) {
        error_display(NULL, NULL, "Failed to allocate folder picker data.");

        return;
    }

    pickerData->populateData.includeBase = true;
    pickerData->populateData.filter = folderpicker_filter;
    pickerData->populateData.state.finished = true;

    snprintf(pickerData->currDir, FILE_PATH_MAX, "/");

    pickerData->data = data;
    pickerData->onPicked = onPicked;

    Result res = fs_open_archive(&pickerData->archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to open the SD card.");

        free(pickerData);
        return;
    }

    list_display(title, "A: Open, B: Back, Y: Use this folder", pickerData, folderpicker_update, folderpicker_draw_top);
}
