#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "action.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    linked_list* items;

    list_item* targetItem;
    file_info* target;

    linked_list contents;

    data_op_data pasteInfo;
} paste_contents_data;

static void action_paste_contents_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    u32 curr = pasteData->pasteInfo.processed;
    if(curr < pasteData->pasteInfo.total) {
        task_draw_file_info(view, ((list_item*) linked_list_get(&pasteData->contents, curr))->data, x1, y1, x2, y2);
    } else {
        task_draw_file_info(view, pasteData->target, x1, y1, x2, y2);
    }
}

// R_APP_OUT_OF_RANGE when the path does not fit: a cut path could replace or delete another file.
static Result action_paste_contents_get_dst_path(paste_contents_data* data, u32 index, char* dstPath) {
    char baseSrcPath[FILE_PATH_MAX];
    if(clipboard_is_contents_only()) {
        string_copy(baseSrcPath, clipboard_get_path(), FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseSrcPath, clipboard_get_path(), FILE_PATH_MAX);
    }

    char baseDstPath[FILE_PATH_MAX];
    if(data->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
        string_copy(baseDstPath, data->target->path, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseDstPath, data->target->path, FILE_PATH_MAX);
    }

    const char* relativePath = ((file_info*) ((list_item*) linked_list_get(&data->contents, index))->data)->path + strlen(baseSrcPath);
    return snprintf(dstPath, FILE_PATH_MAX, "%s%s", baseDstPath, relativePath) < FILE_PATH_MAX ? 0 : R_APP_OUT_OF_RANGE;
}

// A file is copied to partPath and renamed over dstPath only once it is whole.
static Result action_paste_contents_get_part_path(char* partPath, const char* dstPath) {
    return snprintf(partPath, FILE_PATH_MAX, "%s.part", dstPath) < FILE_PATH_MAX ? 0 : R_APP_OUT_OF_RANGE;
}

static Result action_paste_contents_delete_file(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_DeleteFile(archive, *fsPath);
    fs_free_path_utf8(fsPath);

    return res;
}

static Result action_paste_contents_is_src_directory(void* data, u32 index, bool* isDirectory) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    *isDirectory = (bool) (((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & FS_ATTRIBUTE_DIRECTORY);
    return 0;
}

static Result action_paste_contents_make_dst_directory(void* data, u32 index) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    u32 attributes = ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes;

    char dstPath[FILE_PATH_MAX];
    if(R_FAILED(res = action_paste_contents_get_dst_path(pasteData, index, dstPath))) {
        return res;
    }

    FS_Path* fsPath = fs_make_path_utf8(dstPath);
    if(fsPath != NULL) {
        Handle dirHandle = 0;
        if(R_SUCCEEDED(FSUSER_OpenDirectory(&dirHandle, pasteData->target->archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        } else if(R_SUCCEEDED(res = FSUSER_CreateDirectory(pasteData->target->archive, *fsPath, attributes))) {
            char parentPath[FILE_PATH_MAX];
            string_get_parent_path(parentPath, dstPath, FILE_PATH_MAX);

            char baseDstPath[FILE_PATH_MAX];
            if(pasteData->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
                string_copy(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
            } else {
                string_get_parent_path(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
            }

            if(strncmp(parentPath, baseDstPath, FILE_PATH_MAX) == 0) {
                list_item* dstItem = NULL;
                if(R_SUCCEEDED(res) && R_SUCCEEDED(task_create_file_item(&dstItem, pasteData->target->archive, dstPath, attributes, true))) {
                    linked_list_add(pasteData->items, dstItem);
                }
            }
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_open_src(void* data, u32 index, u32* handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->path);
    if(fsPath != NULL) {
        res = FSUSER_OpenFile(handle, clipboard_get_archive(), *fsPath, FS_OPEN_READ, 0);

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return FSFILE_Close(handle);
}

static Result action_paste_contents_get_src_size(void* data, u32 handle, u64* size) {
    return FSFILE_GetSize(handle, size);
}

static Result action_paste_contents_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result action_paste_contents_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    Result res = 0;

    char dstPath[FILE_PATH_MAX];
    char partPath[FILE_PATH_MAX];
    if(R_FAILED(res = action_paste_contents_get_dst_path(pasteData, index, dstPath)) || R_FAILED(res = action_paste_contents_get_part_path(partPath, dstPath))) {
        return res;
    }

    FS_Path* fsPath = fs_make_path_utf8(partPath);
    if(fsPath != NULL) {
        // A part file left by an earlier cut paste would make CreateFile fail.
        FSUSER_DeleteFile(pasteData->target->archive, *fsPath);

        // dataop never closes a failed open, so a part file that cannot be opened goes here.
        if(R_SUCCEEDED(res = FSUSER_CreateFile(pasteData->target->archive, *fsPath, ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & ~FS_ATTRIBUTE_READ_ONLY, size))
           && R_FAILED(res = FSUSER_OpenFile(handle, pasteData->target->archive, *fsPath, FS_OPEN_WRITE, 0))) {
            FSUSER_DeleteFile(pasteData->target->archive, *fsPath);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

static Result action_paste_contents_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    paste_contents_data* pasteData = (paste_contents_data*) data;
    FS_Archive archive = pasteData->target->archive;

    Result res = FSFILE_Close(handle);

    // open_dst built the same paths, so they fit here too.
    char dstPath[FILE_PATH_MAX];
    char partPath[FILE_PATH_MAX];
    Result pathRes = 0;
    if(R_FAILED(pathRes = action_paste_contents_get_dst_path(pasteData, index, dstPath)) || R_FAILED(pathRes = action_paste_contents_get_part_path(partPath, dstPath))) {
        return R_SUCCEEDED(res) ? pathRes : res;
    }

    // A cancelled or failed copy leaves a truncated part file; remove it so it never passes for a
    // complete one (a cut-off CIA, for example). The old destination stays untouched.
    if(!succeeded || R_FAILED(res)) {
        action_paste_contents_delete_file(archive, partPath);
        return res;
    }

    // The old file goes only now; a failed rename keeps the part file, the only whole copy left.
    if(R_SUCCEEDED(action_paste_contents_delete_file(archive, dstPath))) {
        linked_list_iter iter;
        linked_list_iterate(pasteData->items, &iter);

        while(linked_list_iter_has_next(&iter)) {
            list_item* item = (list_item*) linked_list_iter_next(&iter);
            file_info* currInfo = (file_info*) item->data;

            if(strncmp(currInfo->path, dstPath, FILE_PATH_MAX) == 0) {
                linked_list_iter_remove(&iter);
                task_free_file(item);
            }
        }
    }

    res = R_APP_OUT_OF_MEMORY;

    FS_Path* fromPath = fs_make_path_utf8(partPath);
    if(fromPath != NULL) {
        FS_Path* toPath = fs_make_path_utf8(dstPath);
        if(toPath != NULL) {
            res = FSUSER_RenameFile(archive, *fromPath, archive, *toPath);
            fs_free_path_utf8(toPath);
        }

        fs_free_path_utf8(fromPath);
    }

    if(R_FAILED(res)) {
        return res;
    }

    char parentPath[FILE_PATH_MAX];
    string_get_parent_path(parentPath, dstPath, FILE_PATH_MAX);

    char baseDstPath[FILE_PATH_MAX];
    if(pasteData->target->attributes & FS_ATTRIBUTE_DIRECTORY) {
        string_copy(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseDstPath, pasteData->target->path, FILE_PATH_MAX);
    }

    if(strncmp(parentPath, baseDstPath, FILE_PATH_MAX) == 0) {
        list_item* dstItem = NULL;
        if(R_SUCCEEDED(task_create_file_item(&dstItem, archive, dstPath, ((file_info*) ((list_item*) linked_list_get(&pasteData->contents, index))->data)->attributes & ~FS_ATTRIBUTE_READ_ONLY, true))) {
            linked_list_add(pasteData->items, dstItem);
        }
    }

    return res;
}

static Result action_paste_contents_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result action_paste_contents_suspend(void* data, u32 index) {
    return 0;
}

static Result action_paste_contents_restore(void* data, u32 index) {
    return 0;
}

static bool action_paste_contents_error(void* data, u32 index, Result res, ui_view** errorView) {
    *errorView = error_display_res(data, action_paste_contents_draw_top, res, "Failed to paste content.");
    return true;
}

static void action_paste_contents_free_data(paste_contents_data* data) {
    task_clear_files(&data->contents);
    linked_list_destroy(&data->contents);

    if(data->targetItem != NULL) {
        task_free_file(data->targetItem);
        data->targetItem = NULL;
        data->target = NULL;
    }

    free(data);
}

static void action_paste_contents_update(ui_view* view, void* data, float* progress, char* text) {
    paste_contents_data* pasteData = (paste_contents_data*) data;

    if(task_is_finished(&pasteData->pasteInfo.state)) {
        FSUSER_ControlArchive(pasteData->target->archive, ARCHIVE_ACTION_COMMIT_SAVE_DATA, NULL, 0, NULL, 0);

        linked_list_sort(pasteData->items, NULL, task_compare_files);

        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(pasteData->pasteInfo.state.result)) {
            prompt_display_notify("Success", "Contents pasted.", COLOR_TEXT, NULL, NULL, NULL);
        }

        action_paste_contents_free_data(pasteData);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        task_cancel(&pasteData->pasteInfo.state);
    }
}

static void action_paste_contents_onresponse(ui_view* view, void* data, u32 response) {
    paste_contents_data* pasteData = (paste_contents_data*) data;
    if(response == PROMPT_YES) {
        Result res = task_data_op(&pasteData->pasteInfo);
        if(R_SUCCEEDED(res)) {
            ui_view* progressView = info_display("Pasting Contents", "Press B to cancel.", true, data, action_paste_contents_update, action_paste_contents_draw_top);
            info_set_data_op(progressView, &pasteData->pasteInfo);
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate paste operation.");

            action_paste_contents_free_data(pasteData);
        }
    } else {
        action_paste_contents_free_data(pasteData);
    }
}

typedef struct {
    paste_contents_data* pasteData;

    populate_files_data popData;
} paste_contents_loading_data;

static void action_paste_contents_loading_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    action_paste_contents_draw_top(view, ((paste_contents_loading_data*) data)->pasteData, x1, y1, x2, y2);
}

static void action_paste_contents_loading_update(ui_view* view, void* data, float* progress, char* text)  {
    paste_contents_loading_data* loadingData = (paste_contents_loading_data*) data;

    if(task_is_finished(&loadingData->popData.state)) {
        ui_pop();
        info_destroy(view);

        // B during the listing returns success with a partial list; never offer to paste it.
        if(loadingData->popData.state.cancelled) {
            action_paste_contents_free_data(loadingData->pasteData);
        } else if(R_SUCCEEDED(loadingData->popData.state.result) && loadingData->popData.skipped > 0) {
            // The listing left out entries whose paths do not fit; a paste would silently miss them.
            prompt_display_notify("Failure", "Some paths are too long to copy.", COLOR_TEXT, NULL, NULL, NULL);

            action_paste_contents_free_data(loadingData->pasteData);
        } else if(R_SUCCEEDED(loadingData->popData.state.result)) {
            loadingData->pasteData->pasteInfo.total = linked_list_size(&loadingData->pasteData->contents);
            loadingData->pasteData->pasteInfo.processed = loadingData->pasteData->pasteInfo.total;

            prompt_display_yes_no("Confirmation", "Paste clipboard contents to the current directory?", COLOR_TEXT, loadingData->pasteData, action_paste_contents_draw_top, action_paste_contents_onresponse);
        } else {
            error_display_res(NULL, NULL, loadingData->popData.state.result, "Failed to populate clipboard content list.");

            action_paste_contents_free_data(loadingData->pasteData);
        }

        free(loadingData);
        return;
    }

    if(hidKeysDown() & KEY_B) {
        task_cancel(&loadingData->popData.state);
    }

    snprintf(text, PROGRESS_TEXT_MAX, "Fetching clipboard content list...");
}

// Pasting into the clipboard's own folder makes every destination its source, which close_dst deletes
// and replaces; a folder pasted into itself copies into the tree being read. Archive handles differ
// per browser session, so only paths are compared.
static const char* action_paste_contents_check_target(const file_info* target) {
    const char* srcPath = clipboard_get_path();

    char baseSrcPath[FILE_PATH_MAX];
    if(clipboard_is_contents_only()) {
        string_copy(baseSrcPath, srcPath, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseSrcPath, srcPath, FILE_PATH_MAX);
    }

    char baseDstPath[FILE_PATH_MAX];
    if(target->attributes & FS_ATTRIBUTE_DIRECTORY) {
        string_copy(baseDstPath, target->path, FILE_PATH_MAX);
    } else {
        string_get_parent_path(baseDstPath, target->path, FILE_PATH_MAX);
    }

    if(strcasecmp(baseDstPath, baseSrcPath) == 0) {
        return "Cannot paste into the folder it was copied from.";
    }

    size_t srcLen = strlen(srcPath);
    if(srcLen > 0 && srcPath[srcLen - 1] == '/' && strncasecmp(baseDstPath, srcPath, srcLen) == 0) {
        return "Cannot paste a folder into itself.";
    }

    return NULL;
}

void action_paste_contents(linked_list* items, list_item* selected) {
    if(!clipboard_has_contents()) {
        prompt_display_notify("Failure", "Clipboard empty.", COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    const char* refusal = action_paste_contents_check_target((file_info*) selected->data);
    if(refusal != NULL) {
        prompt_display_notify("Failure", refusal, COLOR_TEXT, NULL, NULL, NULL);
        return;
    }

    paste_contents_data* data = (paste_contents_data*) calloc(1, sizeof(paste_contents_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate paste contents data.");

        return;
    }

    data->items = items;

    file_info* targetInfo = (file_info*) selected->data;
    Result targetCreateRes = task_create_file_item(&data->targetItem, targetInfo->archive, targetInfo->path, targetInfo->attributes, false);
    if(R_FAILED(targetCreateRes)) {
        error_display_res(NULL, NULL, targetCreateRes, "Failed to create target file item.");

        action_paste_contents_free_data(data);
        return;
    }

    data->target = (file_info*) data->targetItem->data;

    data->pasteInfo.data = data;

    data->pasteInfo.op = DATAOP_COPY;

    data->pasteInfo.bufferSize = 256 * 1024;
    data->pasteInfo.copyEmpty = true;

    data->pasteInfo.isSrcDirectory = action_paste_contents_is_src_directory;
    data->pasteInfo.makeDstDirectory = action_paste_contents_make_dst_directory;

    data->pasteInfo.openSrc = action_paste_contents_open_src;
    data->pasteInfo.closeSrc = action_paste_contents_close_src;
    data->pasteInfo.getSrcSize = action_paste_contents_get_src_size;
    data->pasteInfo.readSrc = action_paste_contents_read_src;

    data->pasteInfo.openDst = action_paste_contents_open_dst;
    data->pasteInfo.closeDst = action_paste_contents_close_dst;
    data->pasteInfo.writeDst = action_paste_contents_write_dst;

    data->pasteInfo.suspend = action_paste_contents_suspend;
    data->pasteInfo.restore = action_paste_contents_restore;

    data->pasteInfo.error = action_paste_contents_error;

    data->pasteInfo.state.finished = true;

    linked_list_init(&data->contents);

    paste_contents_loading_data* loadingData = (paste_contents_loading_data*) calloc(1, sizeof(paste_contents_loading_data));
    if(loadingData == NULL) {
        error_display(NULL, NULL, "Failed to allocate loading data.");

        action_paste_contents_free_data(data);
        return;
    }

    loadingData->pasteData = data;

    loadingData->popData.items = &data->contents;
    loadingData->popData.archive = clipboard_get_archive();
    string_copy(loadingData->popData.path, clipboard_get_path(), FILE_PATH_MAX);
    loadingData->popData.recursive = true;
    loadingData->popData.includeBase = !clipboard_is_contents_only() || !fs_is_dir(clipboard_get_archive(), clipboard_get_path());
    loadingData->popData.meta = false;
    loadingData->popData.filter = NULL;
    loadingData->popData.filterData = NULL;

    Result listRes = task_populate_files(&loadingData->popData);
    if(R_FAILED(listRes)) {
        error_display_res(NULL, NULL, listRes, "Failed to initiate clipboard content list population.");

        free(loadingData);
        action_paste_contents_free_data(data);
        return;
    }

    info_display("Loading", "Press B to cancel.", false, loadingData, action_paste_contents_loading_update, action_paste_contents_loading_draw_top);
}