#pragma once

// FTB settings, kept in sdmc:/ftb/settings.cfg as key=value lines.
typedef struct ftb_settings_s {
    bool titleCache; // Titles reads and writes sdmc:/ftb/cache/
    bool clock24h; // top bar clock: 24-hour instead of 12-hour
    bool dateDayFirst; // top bar date: DD/MM/YY instead of MM/DD/YY
} ftb_settings;

const ftb_settings* settings_get();
void settings_load();
