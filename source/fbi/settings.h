#pragma once

#include "task/titlecategory.h"

// FTB settings, kept in sdmc:/ftb/settings.cfg as key=value lines.
typedef struct ftb_settings_s {
    bool titleCache; // Titles reads and writes sdmc:/ftb/cache/
    bool clock24h; // top bar clock: 24-hour instead of 12-hour
    bool dateDayFirst; // top bar date: DD/MM/YY instead of MM/DD/YY

    // Title Manager view, changed from its Options menu.
    u8 titleLayout; // title_layout
    u8 titleGridSize; // grid zoom, 0 .. LIST_GRID_SIZE_COUNT - 1
    u8 titleSort; // title_sort
    bool titleSortDesc;
    u8 titleGroup; // title_group: blocks before the sort key
    bool titleHeaders; // section headers over the list
    bool titleShowCategory[TITLE_CATEGORY_COUNT];
} ftb_settings;

typedef enum title_layout_e {
    TITLE_LAYOUT_GRID,
    TITLE_LAYOUT_LIST_TOP,
    TITLE_LAYOUT_LIST_BOTTOM,
    TITLE_LAYOUT_COUNT
} title_layout;

typedef enum title_sort_e {
    TITLE_SORT_NAME,
    TITLE_SORT_ID,
    TITLE_SORT_SIZE,
    TITLE_SORT_PUBLISHER,
    TITLE_SORT_CATEGORY, // title_category; was Type, so a saved Type sort loads as Category
    TITLE_SORT_REGION, // SMDH region lockout mask
    TITLE_SORT_GAME, // title ID low word, so an app sits next to its update and DLC
    TITLE_SORT_COUNT
} title_sort;

typedef enum title_group_e {
    TITLE_GROUP_OFF,
    TITLE_GROUP_LOCATION, // game card, SD, NAND, DSiWare
    TITLE_GROUP_CATEGORY,
    TITLE_GROUP_COUNT
} title_group;

const ftb_settings* settings_get();
void settings_load();
// Replaces the settings and saves them; false when saving failed (the new values still apply).
bool settings_set(const ftb_settings* next);
