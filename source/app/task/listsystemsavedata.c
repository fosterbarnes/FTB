#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>

#include <3ds.h>

#include "listsystemsavedata.h"
#include "../resources.h"
#include "../../core/core.h"

#define SYSTEM_SAVE_DATA_IDS_START 512

static int task_populate_system_save_data_compare_ids(const void* e1, const void* e2) {
    u32 id1 = *(u32*) e1;
    u32 id2 = *(u32*) e2;

    return id1 > id2 ? 1 : id1 < id2 ? -1 : 0;
}

// FS reports only how many IDs it wrote, so a full buffer may mean more: grow it and ask again.
static Result task_populate_system_save_data_ids(u32** outIds, u32* outCount) {
    for(u32 capacity = SYSTEM_SAVE_DATA_IDS_START; ; capacity *= 2) {
        u32* ids = (u32*) calloc(capacity, sizeof(u32));
        if(ids == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }

        u32 count = 0;
        Result res = FSUSER_EnumerateSystemSaveData(&count, capacity * sizeof(u32), ids);
        if(R_SUCCEEDED(res) && count < capacity) {
            *outIds = ids;
            *outCount = count;
            return 0;
        }

        free(ids);

        if(R_FAILED(res)) {
            return res;
        }
    }
}

static Result task_populate_system_save_data_thread(void* arg) {
    populate_system_save_data_data* data = (populate_system_save_data_data*) arg;

    Result res = 0;

    u32 systemSaveDataCount = 0;
    u32* systemSaveDataIds = NULL;
    if(R_SUCCEEDED(res = task_populate_system_save_data_ids(&systemSaveDataIds, &systemSaveDataCount))) {
        qsort(systemSaveDataIds, systemSaveDataCount, sizeof(u32), task_populate_system_save_data_compare_ids);

        for(u32 i = 0; i < systemSaveDataCount && R_SUCCEEDED(res); i++) {
            if(task_is_cancelled(&data->state)) {
                break;
            }

            list_item* item = (list_item*) calloc(1, sizeof(list_item));
            if(item != NULL) {
                system_save_data_info* systemSaveDataInfo = (system_save_data_info*) calloc(1, sizeof(system_save_data_info));
                if(systemSaveDataInfo != NULL) {
                    systemSaveDataInfo->systemSaveDataId = systemSaveDataIds[i];

                    snprintf(item->name, LIST_ITEM_NAME_MAX, "%08lX", systemSaveDataIds[i]);
                    item->color = COLOR_TEXT;
                    item->data = systemSaveDataInfo;

                    if(!linked_list_add(data->items, item)) {
                        task_free_system_save_data(item);

                        res = R_APP_OUT_OF_MEMORY;
                    }
                } else {
                    free(item);

                    res = R_APP_OUT_OF_MEMORY;
                }
            } else {
                res = R_APP_OUT_OF_MEMORY;
            }
        }

        free(systemSaveDataIds);
    }

    return res;
}

void task_free_system_save_data(list_item* item) {
    if(item == NULL) {
        return;
    }

    if(item->data != NULL) {
        free(item->data);
    }

    free(item);
}

void task_clear_system_save_data(linked_list* items) {
    if(items == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);

        linked_list_iter_remove(&iter);
        task_free_system_save_data(item);
    }
}

Result task_populate_system_save_data(populate_system_save_data_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_system_save_data(data->items);

    return task_start(&data->state, task_populate_system_save_data_thread, data, 0x19, 1);
}