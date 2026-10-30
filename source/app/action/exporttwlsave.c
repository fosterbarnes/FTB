#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    title_info* title;

    data_op_data exportInfo;

    // The export is written to partPath and renamed over path only once it is whole.
    char path[FILE_PATH_MAX];
    char partPath[FILE_PATH_MAX];
} export_twl_save_data;

static Result action_export_twl_save_delete_file(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_DeleteFile(archive, *fsPath);
    fs_free_path_utf8(fsPath);

    return res;
}

static void action_export_twl_save_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    task_draw_title_info(view, ((export_twl_save_data*) data)->title, x1, y1, x2, y2);
}

static Result action_export_twl_save_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result action_export_twl_save_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result action_export_twl_save_open_src(void* data, u32 index, u32* handle) {
    return spi_init_card();
}

static Result action_export_twl_save_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return spi_deinit_card();
}

static Result action_export_twl_save_get_src_size(void* data, u32 handle, u64* size) {
    Result res = 0;

    u32 saveSize = 0;
    if(R_SUCCEEDED(res = spi_get_save_size(&saveSize))) {
        *size = saveSize;
    }

    return res;
}

static Result action_export_twl_save_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return spi_read_save(bytesRead, buffer, (u32) offset, size);
}

static Result action_export_twl_save_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    export_twl_save_data* exportData = (export_twl_save_data*) data;

    Result res = 0;

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, "/fbi/")) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, "/fbi/save/"))) {
            char gameName[0x10] = {'\0'};
            string_escape_file_name(gameName, exportData->title->productCode, sizeof(gameName));

            snprintf(exportData->path, sizeof(exportData->path), "/fbi/save/%s.sav", gameName);
            snprintf(exportData->partPath, sizeof(exportData->partPath), "/fbi/save/%s.sav.part", gameName);

            FS_Path* fsPath = fs_make_path_utf8(exportData->partPath);
            if(fsPath != NULL) {
                // A part file left by an earlier cut export would otherwise keep its tail.
                if(R_SUCCEEDED(res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))
                   && R_FAILED(res = FSFILE_SetSize(*handle, size))) {
                    FSFILE_Close(*handle);
                    *handle = 0; // dataop closes a nonzero handle again

                    action_export_twl_save_delete_file(sdmcArchive, exportData->partPath);
                }

                fs_free_path_utf8(fsPath);
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        FSUSER_CloseArchive(sdmcArchive);
    }

    return res;
}

static Result action_export_twl_save_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    export_twl_save_data* exportData = (export_twl_save_data*) data;

    Result res = FSFILE_Close(handle);

    FS_Archive sdmcArchive = 0;
    Result archiveRes = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(archiveRes)) {
        return R_SUCCEEDED(res) ? archiveRes : res;
    }

    if(!succeeded || R_FAILED(res)) {
        action_export_twl_save_delete_file(sdmcArchive, exportData->partPath);
    } else {
        // The previous export goes only now; a failed rename keeps the part file, the only copy left.
        action_export_twl_save_delete_file(sdmcArchive, exportData->path);

        res = R_APP_OUT_OF_MEMORY;

        FS_Path* fromPath = fs_make_path_utf8(exportData->partPath);
        if(fromPath != NULL) {
            FS_Path* toPath = fs_make_path_utf8(exportData->path);
            if(toPath != NULL) {
                res = FSUSER_RenameFile(sdmcArchive, *fromPath, sdmcArchive, *toPath);
                fs_free_path_utf8(toPath);
            }

            fs_free_path_utf8(fromPath);
        }
    }

    FSUSER_CloseArchive(sdmcArchive);
    return res;
}

static Result action_export_twl_save_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result action_export_twl_save_suspend(void* data, u32 index) {
    return 0;
}

static Result action_export_twl_save_restore(void* data, u32 index) {
    return 0;
}

static bool action_export_twl_save_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(((export_twl_save_data*) data)->title, task_draw_title_info, res, "Failed to export save.");
    return true;
}

static void action_export_twl_save_update(ui_view* view, void* data, float* progress, char* text) {
    export_twl_save_data* exportData = (export_twl_save_data*) data;

    if(task_is_finished(&exportData->exportInfo.state)) {
        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(exportData->exportInfo.state.result)) {
            prompt_display_notify("Success", "Save exported.", COLOR_TEXT, exportData->title, task_draw_title_info, NULL);
        }

        free(data);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        task_cancel(&exportData->exportInfo.state);
    }
}

static void action_export_twl_save_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        export_twl_save_data* exportData = (export_twl_save_data*) data;

        Result res = task_data_op(&exportData->exportInfo);
        if(R_SUCCEEDED(res)) {
            ui_view* progressView = info_display("Exporting Save", "Press B to cancel.", true, data, action_export_twl_save_update, action_export_twl_save_draw_top);
            info_set_data_op(progressView, &exportData->exportInfo);
        } else {
            error_display_res(exportData->title, task_draw_title_info, res, "Failed to initiate save export.");
            free(data);
        }
    } else {
        free(data);
    }
}

void action_export_twl_save(linked_list* items, list_item* selected) {
    export_twl_save_data* data = (export_twl_save_data*) calloc(1, sizeof(export_twl_save_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate export TWL save data.");

        return;
    }

    data->title = (title_info*) selected->data;

    data->exportInfo.data = data;

    data->exportInfo.op = DATAOP_COPY;

    data->exportInfo.bufferSize = 128 * 1024;
    data->exportInfo.copyEmpty = true;

    data->exportInfo.total = 1;

    data->exportInfo.isSrcDirectory = action_export_twl_save_is_src_directory;
    data->exportInfo.makeDstDirectory = action_export_twl_save_make_dst_directory;

    data->exportInfo.openSrc = action_export_twl_save_open_src;
    data->exportInfo.closeSrc = action_export_twl_save_close_src;
    data->exportInfo.getSrcSize = action_export_twl_save_get_src_size;
    data->exportInfo.readSrc = action_export_twl_save_read_src;

    data->exportInfo.openDst = action_export_twl_save_open_dst;
    data->exportInfo.closeDst = action_export_twl_save_close_dst;
    data->exportInfo.writeDst = action_export_twl_save_write_dst;

    data->exportInfo.suspend = action_export_twl_save_suspend;
    data->exportInfo.restore = action_export_twl_save_restore;

    data->exportInfo.error = action_export_twl_save_error;

    data->exportInfo.state.finished = true;

    prompt_display_yes_no("Confirmation", "Export the save of the selected title?", COLOR_TEXT, data, action_export_twl_save_draw_top, action_export_twl_save_onresponse);
}