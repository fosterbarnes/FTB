#pragma once

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
    bool titleGroupLocation; // game card, SD, NAND, DSiWare blocks before the sort key
    bool titleShowCard;
    bool titleShowSD;
    bool titleShowNAND;
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
    TITLE_SORT_COUNT
} title_sort;

const ftb_settings* settings_get();
void settings_load();
// Replaces the settings and saves them; false when saving failed (the new values still apply).
bool settings_set(const ftb_settings* next);
