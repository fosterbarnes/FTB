#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "../paths.h"
#include "../resources.h"
#include "../screens.h"
#include "../task/uitask.h"
#include "../../core/core.h"

static Result dumpnand_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result dumpnand_make_dst_directory(void* data, u32 index) {
    return 0;
}

static Result dumpnand_open_src(void* data, u32 index, u32* handle) {
    return FSUSER_OpenFileDirectly(handle, ARCHIVE_NAND_W_FS, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_UTF16, u"/"), FS_OPEN_READ, 0);
}

static Result dumpnand_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result dumpnand_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result dumpnand_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

// The dump being written, so a failed or cancelled one can be deleted. One dump runs at a time.
static char dumpnand_path[FILE_PATH_MAX];

static Result dumpnand_open_dst(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle) {
    // A full SD card would leave a cut-off image behind after a long dump.
    FS_ArchiveResource resource = {0};
    Result res = FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD);
    if(R_FAILED(res)) {
        return res;
    }

    if((u64) resource.freeClusters * resource.clusterSize < size) {
        return R_APP_NOT_ENOUGH_SPACE;
    }

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, PATHS_FS_DATA_DIR)) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, PATHS_FS_NAND_DIR))) {
            time_t t = time(NULL);
            struct tm* timeInfo = localtime(&t);

            strftime(dumpnand_path, sizeof(dumpnand_path), PATHS_FS_NAND_DIR "NAND_%m-%d-%Y_%H-%M-%S.bin", timeInfo);

            FS_Path* fsPath = fs_make_path_utf8(dumpnand_path);
            if(fsPath != NULL) {
                res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_WRITE | FS_OPEN_CREATE, 0);

                fs_free_path_utf8(fsPath);
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        FSUSER_CloseArchive(sdmcArchive);
    }

    return res;
}

static Result dumpnand_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    Result res = FSFILE_Close(handle);

    FS_Archive sdmcArchive = 0;
    if(!succeeded && R_SUCCEEDED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path* fsPath = fs_make_path_utf8(dumpnand_path);
        if(fsPath != NULL) {
            FSUSER_DeleteFile(sdmcArchive, *fsPath);
            fs_free_path_utf8(fsPath);
        }

        FSUSER_CloseArchive(sdmcArchive);
    }

    return res;
}

static Result dumpnand_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result dumpnand_suspend(void* data, u32 index) {
    return 0;
}

static Result dumpnand_restore(void* data, u32 index) {
    return 0;
}

static bool dumpnand_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(NULL, NULL, res, "Failed to dump NAND.");
    return true;
}

static void dumpnand_update(ui_view* view, void* data, float* progress, char* text) {
    data_op_data* dumpData = (data_op_data*) data;

    if(task_is_finished(&dumpData->state)) {
        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(dumpData->state.result)) {
            prompt_display_notify("Success", "NAND dumped.", COLOR_TEXT, NULL, NULL, NULL);
        }

        free(dumpData);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        task_cancel(&dumpData->state);
    }
}

static void dumpnand_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES) {
        data_op_data* dumpData = (data_op_data*) data;

        Result res = task_data_op(dumpData);
        if(R_SUCCEEDED(res)) {
            ui_view* progressView = info_display("Dumping NAND", "Press B to cancel.", true, data, dumpnand_update, NULL);
            info_set_data_op(progressView, dumpData);
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate NAND dump.");
            free(data);
        }
    } else {
        free(data);
    }
}

void dumpnand_open() {
    data_op_data* data = (data_op_data*) calloc(1, sizeof(data_op_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate dump NAND data.");

        return;
    }

    data->data = data;

    data->op = DATAOP_COPY;

    data->bufferSize = 256 * 1024;
    data->copyEmpty = true;

    data->total = 1;

    data->isSrcDirectory = dumpnand_is_src_directory;
    data->makeDstDirectory = dumpnand_make_dst_directory;

    data->openSrc = dumpnand_open_src;
    data->closeSrc = dumpnand_close_src;
    data->getSrcSize = dumpnand_get_src_size;
    data->readSrc = dumpnand_read_src;

    data->openDst = dumpnand_open_dst;
    data->closeDst = dumpnand_close_dst;
    data->writeDst = dumpnand_write_dst;

    data->suspend = dumpnand_suspend;
    data->restore = dumpnand_restore;

    data->error = dumpnand_error;

    data->state.finished = true;

    prompt_display_yes_no("Confirmation", "Dump raw NAND image to the SD card?", COLOR_TEXT, data, NULL, dumpnand_onresponse);
}