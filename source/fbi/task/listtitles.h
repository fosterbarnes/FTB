#pragma once

#include "titlecategory.h"

typedef struct linked_list_s linked_list;
typedef struct list_item_s list_item;

typedef struct title_info_s {
    FS_MediaType mediaType;
    u64 titleId;
    char productCode[0x10];
    u16 version;
    u64 installedSize;
    bool twl;
    bool hasMeta;
    title_category category;
    meta_info meta;
} title_info;

typedef enum title_cache_mode_e {
    TITLE_CACHE_OFF,    // FBI behavior: no read, no write
    TITLE_CACHE_ON,     // show cached titles, load the rest, write when changed
    TITLE_CACHE_REBUILD // ignore sdmc:/ftb/cache/, load every title, rewrite it
} title_cache_mode;

typedef enum title_load_stage_e {
    TITLE_STAGE_LISTING, // asking AM which titles exist; total not known yet
    TITLE_STAGE_CACHE,   // showing cached titles
    TITLE_STAGE_LOADING, // loading titles the cache did not cover
    TITLE_STAGE_SAVING   // writing a cache file
} title_load_stage;

typedef struct populate_titles_data_s {
    linked_list* items;

    void* userData;
    bool (*filter)(void* data, u64 titleId, FS_MediaType mediaType);
    // Runs after a title loads, so a hidden title still counts toward a complete cache pass.
    bool (*visible)(void* data, title_category category);
    int (*compare)(void* data, const void* p1, const void* p2);
    title_cache_mode cacheMode;

    // Progress, written only by the populate thread.
    volatile title_load_stage stage;
    volatile u32 total;
    volatile u32 loaded;

    volatile bool finished;
    Result result;
    Handle cancelEvent;
} populate_titles_data;

void task_free_title(list_item* item);
void task_clear_titles(linked_list* items);
Result task_populate_titles(populate_titles_data* data);
// Deletes the sdmc:/ftb/cache/ title files. Call only while no title population runs.
bool task_clear_title_cache();