#pragma once

// Title Manager categories, in display order. Derived from cached title data, never stored.
typedef enum title_category_e {
    TITLE_CATEGORY_3DS,
    TITLE_CATEGORY_DS, // DS and DSi titles, DSiWare, NDS forwarders
    TITLE_CATEGORY_VC, // Virtual Console and GBA forwarders
    TITLE_CATEGORY_HOMEBREW,
    TITLE_CATEGORY_SYSTEM,
    TITLE_CATEGORY_OTHER, // updates, DLC, demos
    TITLE_CATEGORY_COUNT
} title_category;

title_category title_category_get(u64 titleId, bool twl, const char* productCode, const char* publisher, bool hasMeta);
