#pragma once

// FTB settings, kept in sdmc:/ftb/settings.cfg as key=value lines.
typedef struct ftb_settings_s {
    bool titleCache; // Titles reads and writes sdmc:/ftb/cache/
    bool clock24h; // top bar clock: 24-hour instead of 12-hour
    bool dateDayFirst; // top bar date: DD/MM/YY instead of MM/DD/YY
    bool systemChanges; // safety.h: system title, ticket, save, and NAND changes allowed
    bool webAutoAccept; // Web Transfer starts uploads without asking (boot.firm, luma, Nintendo 3DS still ask)
    u8 webApproveDevices; // web_approve: when Web Transfer asks before a device's first request

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

// settings.cfg stores the value; 0 and 1 kept their meaning from when webApproveDevices was a bool.
typedef enum web_approve_e {
    WEB_APPROVE_NONE, // anyone on the network
    WEB_APPROVE_SESSION, // approvals last until Web Transfer closes
    WEB_APPROVE_REMEMBER, // approved browsers are remembered in PATHS_WEB_DEVICES
    WEB_APPROVE_COUNT
} web_approve;

typedef enum title_sort_e {
    TITLE_SORT_NAME,
    TITLE_SORT_ID,
    TITLE_SORT_COUNT
} title_sort;

const ftb_settings* settings_get();
void settings_load();
// Replaces the settings and saves them; false when saving failed (the new values still apply).
bool settings_set(const ftb_settings* next);
