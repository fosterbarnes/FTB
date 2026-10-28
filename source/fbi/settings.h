#pragma once

// FTB settings, kept in sdmc:/ftb/settings.cfg as key=value lines.
typedef struct ftb_settings_s {
    bool titleCache; // Titles reads and writes sdmc:/ftb/cache/
} ftb_settings;

const ftb_settings* settings_get();
void settings_load();
