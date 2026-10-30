#include <stdio.h>

#include <3ds.h>

#include "resources.h"
#include "safety.h"
#include "settings.h"
#include "../core/core.h"

bool safety_is_system_title(u64 titleId) {
    u16 category = (u16) (titleId >> 32);
    return (category & 0x8000) != 0 ? (category & 0x1) != 0 : (category & 0x10) != 0;
}

bool safety_is_native_firm(u64 titleId) {
    // 0004013800000002 (Old 3DS) and 0004013820000002 (New 3DS).
    return (titleId >> 32) == 0x00040138 && (titleId & 0x0FFFFFFF) == 0x2;
}

bool safety_is_system_ext_save_data(u64 extSaveDataId) {
    // HOME Menu for JPN, USA, EUR, CHN, KOR, TWN (3dbrew Extdata, Title list): each is the unique ID
    // of that region's HOME Menu title (0004003000008202 = 0x82).
    static const u64 ids[] = {0x82, 0x8F, 0x98, 0xA1, 0xA9, 0xB1};

    for(u32 i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if(extSaveDataId == ids[i]) {
            return true;
        }
    }

    return false;
}

bool safety_allowed() {
    return settings_get()->systemChanges;
}

bool safety_check(const char* what) {
    if(safety_allowed()) {
        return true;
    }

    // The prompt keeps the text pointer; only the UI thread shows this notice.
    static char message[128];
    snprintf(message, sizeof(message), "%s is locked.\nTurn on Settings > System changes\nto allow it.", what);
    prompt_display_notify("Locked", message, COLOR_TEXT, NULL, NULL, NULL);
    return false;
}

Result safety_check_title(u64 titleId) {
    return safety_is_system_title(titleId) && !safety_allowed() ? R_APP_SYSTEM_LOCKED : 0;
}

Result safety_confirm_install(u64 titleId, void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2)) {
    if(!safety_is_system_title(titleId)) {
        return 0;
    }

    if(!safety_allowed()) {
        return R_APP_SYSTEM_LOCKED;
    }

    const char* text = safety_is_native_firm(titleId)
                       ? "This CIA is NATIVE_FIRM. Installing it\n"
                         "also writes the console's firmware.\n"
                         "A bad firmware can stop the console\n"
                         "from booting.\n"
                         "\n"
                         "Install it?"
                       : "This is a system title. Installing it\n"
                         "deletes the current version first; if\n"
                         "the install fails, the console may not\n"
                         "boot.\n"
                         "\n"
                         "Install it?";

    return prompt_wait_yes_no("Warning", text, COLOR_WARNING, data, drawTop) ? 0 : R_APP_SKIPPED;
}
