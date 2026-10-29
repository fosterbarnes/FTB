#pragma once

// FTB settings, kept in sdmc:/ftb/settings.cfg as key=value lines.
typedef struct ftb_settings_s {
    bool titleCache; // Titles reads and writes sdmc:/ftb/cache/
    bool clock24h; // top bar clock: 24-hour instead of 12-hour
    bool dateDayFirst; // top bar date: DD/MM/YY instead of MM/DD/YY
    bool systemChanges; // safety.h: system title, ticket, save, and NAND changes allowed

    // Title Manager view, changed from its Options menu and touch buttons.
    u8 titleStyle; // title_style
    bool titleBottom; // rows on the bottom screen instead of the top
    u8 titleGridSize; // grid zoom, 0 .. LIST_GRID_ZOOM_COUNT - 1
    u8 titleIconListZoom; // icon list zoom, 0 .. LIST_ICON_LIST_ZOOM_COUNT - 1
    u8 titleSort; // title_sort
    bool titleSortDesc;
    bool titleHeaders; // section headers over the list
    bool titleHideSystem; // safety_is_system_title titles left out of the list
} ftb_settings;

// settings.cfg stores the value: append new styles only.
typedef enum title_style_e {
    TITLE_STYLE_GRID,
    TITLE_STYLE_LIST,
    TITLE_STYLE_ICON_LIST,
    TITLE_STYLE_COUNT
} title_style;

typedef enum title_sort_e {
    TITLE_SORT_NAME,
    TITLE_SORT_ID,
    TITLE_SORT_COUNT
} title_sort;

const ftb_settings* settings_get();
void settings_load();
// Replaces the settings and saves them; false when saving failed (the new values still apply).
bool settings_set(const ftb_settings* next);
