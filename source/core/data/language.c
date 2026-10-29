#include <3ds.h>

#include "language.h"
#include "../stringutil.h"

// CFG_Region order: JPN, USA, EUR, AUS, CHN, KOR, TWN.
static const u8 language_region_default[] = {
        CFG_LANGUAGE_JP,
        CFG_LANGUAGE_EN,
        CFG_LANGUAGE_EN,
        CFG_LANGUAGE_EN,
        CFG_LANGUAGE_ZH,
        CFG_LANGUAGE_KO,
        CFG_LANGUAGE_ZH
};

#define LANGUAGE_REGION_COUNT (sizeof(language_region_default) / sizeof(language_region_default[0]))

static u8 language_system_value;
static u8 language_region_value;
static volatile bool language_loaded;

// List workers call this at the same time; every caller reads the same CFG values, so a second load
// only repeats the same writes.
static void language_load() {
    if(language_loaded) {
        return;
    }

    // u8 locals: CFGU writes one byte, so reading into an enum left its other bytes unset.
    u8 language = CFG_LANGUAGE_JP;
    if(R_FAILED(CFGU_GetSystemLanguage(&language)) || language > CFG_LANGUAGE_TW) {
        language = CFG_LANGUAGE_JP;
    }

    u8 region = CFG_REGION_JPN;
    if(R_FAILED(CFGU_SecureInfoGetRegion(&region)) || region >= LANGUAGE_REGION_COUNT) {
        region = CFG_REGION_JPN;
    }

    language_system_value = language;
    language_region_value = language_region_default[region];

    __dmb();
    language_loaded = true;
}

u8 language_system() {
    language_load();
    return language_system_value;
}

u8 language_pick(const u16* systemText) {
    language_load();

    char text[0x100] = {'\0'};
    utf16_to_utf8((uint8_t*) text, systemText, sizeof(text) - 1);

    return string_is_empty(text) ? language_region_value : language_system_value;
}
