#pragma once

// Settings > System changes (off by default) guards everything that can delete or replace system
// titles, tickets, or saves, or write system storage. Every new action of that kind checks here.

typedef struct ui_view_s ui_view;

// 3dbrew Title list: the category (title ID bits 32-47) marks system titles with 0x10, or with 0x1
// for TWL titles (category 0x8000).
bool safety_is_system_title(u64 titleId);
// NATIVE_FIRM for Old and New 3DS; installing it also writes the FIRM partitions.
bool safety_is_native_firm(u64 titleId);

bool safety_allowed();
// UI thread: true while unlocked; otherwise shows "<what> is locked." and returns false.
bool safety_check(const char* what);
// Worker threads, before changing titleId's title or ticket: R_APP_SYSTEM_LOCKED for a system
// title while locked, else 0.
Result safety_check_title(u64 titleId);
// Worker threads, before a CIA install replaces titleId: safety_check_title, then a red
// confirmation for system titles. 0 to go ahead, R_APP_SKIPPED when declined.
Result safety_confirm_install(u64 titleId, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2));
