#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "language.h"
#include "smdh.h"
#include "../stringutil.h"

#define SMDH_NUM_REGIONS 7
#define SMDH_ALL_REGIONS 0x7F

static const char* smdh_region_strings[SMDH_NUM_REGIONS] = {
        "Japan",
        "North America",
        "Europe",
        "Australia",
        "China",
        "Korea",
        "Taiwan"
};

void smdh_region_to_string(char* out, u32 region, size_t size) {
    if(out == NULL) {
        return;
    }

    if(region == 0) {
        snprintf(out, size, "Unknown");
    } else if((region & SMDH_ALL_REGIONS) == SMDH_ALL_REGIONS) {
        snprintf(out, size, "Region Free");
    } else {
        size_t pos = 0;
        string_append(out, size, &pos, ""); // terminated even when no known bit is set

        for(u32 i = 0; i < SMDH_NUM_REGIONS; i++) {
            if(region & (1 << i)) {
                if(pos > 0) {
                    string_append(out, size, &pos, ", ");
                }

                string_append(out, size, &pos, smdh_region_strings[i]);
            }
        }
    }
}

bool smdh_is_valid(const SMDH* smdh) {
    return memcmp(smdh->magic, "SMDH", sizeof(smdh->magic)) == 0;
}

SMDH_title* smdh_select_title(SMDH* smdh) {
    return &smdh->titles[language_pick(smdh->titles[language_system()].shortDescription)];
}