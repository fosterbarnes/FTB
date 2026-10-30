#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "uitask.h"
#include "../resources.h"
#include "../../core/core.h"

// A folder inside another configured folder is already covered by that folder's walk.
static bool task_populate_cias_nested(populate_cias_data* data, u32 index) {
    const char* path = data->folders[index];
    size_t pathLen = strlen(path);

    for(u32 i = 0; i < data->folderCount; i++) {
        size_t len = strlen(data->folders[i]);
        if(i != index && pathLen > len && strncasecmp(path, data->folders[i], len) == 0) {
            return true;
        }
    }

    return false;
}

static int task_populate_cias_compare(void* userData, const void* p1, const void* p2) {
    const file_info* f1 = (const file_info*) ((const list_item*) p1)->data;
    const file_info* f2 = (const file_info*) ((const list_item*) p2)->data;

    int diff = strcasecmp(f1->name, f2->name);
    return diff != 0 ? diff : strcasecmp(f1->path, f2->path);
}

// Adds dir's CIAs to found and its subfolders to dirs (heap path strings ending in '/').
static Result task_populate_cias_read_dir(populate_cias_data* data, const char* dir, linked_list* dirs, linked_list* found, bool* quit) {
    FS_DirectoryEntry* entries = NULL;
    u32 count = 0;
    Result res = fs_read_dir(data->archive, dir, &entries, &count);

    for(u32 i = 0; i < count && R_SUCCEEDED(res); i++) {
        if(task_is_cancelled(&data->state)) {
            *quit = true;
            break;
        }

        u32 attributes = entries[i].attributes;
        bool isDir = (attributes & FS_ATTRIBUTE_DIRECTORY) != 0;

        // Up to 3 bytes per UTF-16 unit, so long names are never cut.
        char name[sizeof(entries[i].name) / sizeof(entries[i].name[0]) * 3 + 1] = {'\0'};
        utf16_to_utf8((uint8_t*) name, entries[i].name, sizeof(name) - 1);

        if((attributes & FS_ATTRIBUTE_HIDDEN) || (!isDir && !fs_filter_cias(NULL, name, attributes))) {
            continue;
        }

        char path[FILE_PATH_MAX];
        if(snprintf(path, sizeof(path), "%s%s%s", dir, name, isDir ? "/" : "") >= (int) sizeof(path)) {
            continue;
        }

        if(isDir) {
            char* sub = strdup(path);
            if(sub == NULL || !linked_list_add(dirs, sub)) {
                free(sub);
                res = R_APP_OUT_OF_MEMORY;
            }
        } else {
            list_item* item = NULL;
            if(R_SUCCEEDED(res = task_create_entry_item(&item, data->archive, path, attributes))) {
                ((file_info*) item->data)->size = entries[i].fileSize;

                if(!linked_list_add(found, item)) {
                    task_free_file(item);
                    res = R_APP_OUT_OF_MEMORY;
                }
            }
        }
    }

    free(entries);
    return res;
}

static Result task_populate_cias_walk(populate_cias_data* data, const char* root, linked_list* found, bool* quit) {
    linked_list dirs;
    linked_list_init(&dirs);

    Result res = 0;

    char* first = strdup(root);
    if(first == NULL || !linked_list_add(&dirs, first)) {
        free(first);
        res = R_APP_OUT_OF_MEMORY;
    }

    while(R_SUCCEEDED(res) && !*quit && linked_list_size(&dirs) > 0) {
        u32 tail = linked_list_size(&dirs) - 1;
        char* dir = (char*) linked_list_get(&dirs, tail);
        linked_list_remove_at(&dirs, tail);

        res = task_populate_cias_read_dir(data, dir, &dirs, found, quit);

        // An unreadable subfolder is skipped; the configured folder itself and out of memory still fail.
        if(R_FAILED(res) && res != R_APP_OUT_OF_MEMORY && strcmp(dir, root) != 0) {
            res = 0;
        }

        free(dir);
    }

    linked_list_iter iter;
    linked_list_iterate(&dirs, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
        linked_list_iter_remove(&iter);
    }

    linked_list_destroy(&dirs);

    return res;
}

static Result task_populate_cias_add_all_row(populate_cias_data* data, const char* folder) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    file_info* info = (file_info*) calloc(1, sizeof(file_info));
    if(item == NULL || info == NULL) {
        free(item);
        free(info);

        return R_APP_OUT_OF_MEMORY;
    }

    // A directory item, so row actions that match rows by path never hit it.
    info->archive = data->archive;
    string_copy(info->name, CIA_ALL_NAME, FILE_NAME_MAX);
    string_copy(info->path, folder, FILE_PATH_MAX);
    info->attributes = FS_ATTRIBUTE_DIRECTORY;

    string_copy(item->name, CIA_ALL_NAME, LIST_ITEM_NAME_MAX);
    item->color = COLOR_DIRECTORY;
    item->data = info;

    if(!linked_list_add(data->items, item)) {
        task_free_file(item);
        return R_APP_OUT_OF_MEMORY;
    }

    return 0;
}

static Result task_populate_cias_thread(void* arg) {
    populate_cias_data* data = (populate_cias_data*) arg;

    Result res = 0;
    Result firstError = 0;
    bool quit = false;

    // Rows are collected and sorted privately, so the shown list only ever grows at the end.
    linked_list found;
    linked_list_init(&found);

    const char* firstFolder = NULL;
    for(u32 i = 0; i < data->folderCount && R_SUCCEEDED(res) && !quit; i++) {
        if(task_populate_cias_nested(data, i) || !fs_is_dir(data->archive, data->folders[i])) {
            continue;
        }

        if(firstFolder == NULL) {
            firstFolder = data->folders[i];
        }

        // An unreadable folder is left out and its error returned, but the other folders still list.
        if(R_FAILED(res = task_populate_cias_walk(data, data->folders[i], &found, &quit)) && res != R_APP_OUT_OF_MEMORY) {
            firstError = R_SUCCEEDED(firstError) ? res : firstError;
            res = 0;
        }
    }

    if(R_SUCCEEDED(res) && !quit && linked_list_size(&found) > 0) {
        linked_list_sort(&found, NULL, task_populate_cias_compare);
        res = task_populate_cias_add_all_row(data, firstFolder);
    }

    linked_list_iter iter;
    linked_list_iterate(&found, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        linked_list_iter_remove(&iter);

        if(R_SUCCEEDED(res) && !quit) {
            if(linked_list_add(data->items, item)) {
                continue;
            }

            res = R_APP_OUT_OF_MEMORY;
        }

        task_free_file(item);
    }

    linked_list_destroy(&found);

    if(R_SUCCEEDED(res) && !quit) {
        data->metaTotal = linked_list_size(data->items) > 0 ? linked_list_size(data->items) - 1 : 0;

        linked_list_iterate(data->items, &iter);
        while(linked_list_iter_has_next(&iter) && !task_is_cancelled(&data->state)) {
            file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;
            if(info->attributes & FS_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            task_load_file_meta(info);
            data->metaLoaded++;
        }
    }

    return R_SUCCEEDED(res) ? firstError : res;
}

Result task_populate_cias(populate_cias_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_files(data->items);

    data->metaLoaded = 0;
    data->metaTotal = 0;

    return task_start(&data->state, task_populate_cias_thread, data, 0x19, 1);
}
