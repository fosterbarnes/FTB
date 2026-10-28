#include <string.h>

#include <3ds.h>

#include "titlecategory.h"

// Rules checked against a real 346-title library (retail, eShop VC, forwarders, homebrew, system).
// Product codes are CTR-X-YYYY: X is P (card) or N (download), YYYY the game code.

// Retail apps use unique IDs below 0x8000; SNES VC sits at 0xF70xx.
static bool title_category_retail_uid(u32 uid) {
    return uid < 0x8000 || (uid >= 0xF7000 && uid < 0xF8000);
}

static bool title_category_code_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

// CTR-P-ABCD, CTR-N-ABCD, KTR-P-ABCD, or KTR-N-ABCD.
static bool title_category_retail_code(const char* code) {
    if(strlen(code) != 10 || (strncmp(code, "CTR-", 4) != 0 && strncmp(code, "KTR-", 4) != 0)
       || (code[4] != 'P' && code[4] != 'N') || code[5] != '-') {
        return false;
    }

    for(u32 i = 6; i < 10; i++) {
        if(!title_category_code_char(code[i])) {
            return false;
        }
    }

    return true;
}

// eShop VC game codes: T NES, Q GBC, R GB, P GBA (ambassador); KTR-N-U SNES. R and P are unverified.
static bool title_category_vc_code(const char* code) {
    return title_category_retail_code(code)
           && ((strncmp(code, "CTR-N-", 6) == 0 && strchr("TQRP", code[6]) != NULL) || strncmp(code, "KTR-N-U", 7) == 0);
}

title_category title_category_get(u64 titleId, bool twl, const char* productCode, const char* publisher, bool hasMeta) {
    u32 category = (u32) (titleId >> 32) & 0xFFFF;
    u32 uid = (u32) (titleId >> 8) & 0xFFFFF;

    // DSi system titles are 00048005 and 0004800F; DSiWare is 00048004.
    if(twl) {
        return (category & 0x1) != 0 ? TITLE_CATEGORY_SYSTEM : TITLE_CATEGORY_DS;
    }

    // System apps, applets, modules, and data. First: HOME Menu and Friend List use CTR-P-CTAP.
    if((category & 0x10) != 0) {
        return TITLE_CATEGORY_SYSTEM;
    }

    // Updates (0004000E), DLC (0004008C), demos (00040002).
    if(category != 0) {
        return TITLE_CATEGORY_OTHER;
    }

    // Forwarders: CTR-H- codes in the developer unique ID range. The GBA ones name their publisher.
    if(strncmp(productCode, "CTR-H-", 6) == 0 && uid >= 0xFF400 && uid < 0xFF800) {
        return strcmp(publisher, "Game Boy Advance") == 0 ? TITLE_CATEGORY_VC : TITLE_CATEGORY_DS;
    }

    bool retailUid = title_category_retail_uid(uid);
    if(retailUid && title_category_vc_code(productCode)) {
        return TITLE_CATEGORY_VC;
    }

    // An empty code with an SMDH is a failed read, not evidence of homebrew.
    if(!hasMeta || !retailUid || (productCode[0] != '\0' && !title_category_retail_code(productCode))) {
        return TITLE_CATEGORY_HOMEBREW;
    }

    return TITLE_CATEGORY_3DS;
}
