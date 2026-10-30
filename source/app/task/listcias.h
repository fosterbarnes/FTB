#pragma once

#include "../ciafolders.h"
#include "../../core/task/task.h"

// Name of the pinned first row, which stands for every CIA in the list.
#define CIA_ALL_NAME "<all CIAs>"

typedef struct populate_cias_data_s {
    linked_list* items;
    FS_Archive archive;

    char folders[CIA_FOLDERS_MAX][FILE_PATH_MAX];
    u32 folderCount;

    // Meta loading progress, written by the populate thread after the rows are listed.
    volatile u32 metaLoaded;
    volatile u32 metaTotal;

    task_state state;
} populate_cias_data;

// Lists the .cia files in every folder and its subfolders, sorted by name, after the pinned
// CIA_ALL_NAME row (a directory item, none when no CIAs), then loads their meta. A folder that
// cannot be read is left out and the first such error is the result; the other rows stay.
Result task_populate_cias(populate_cias_data* data);
