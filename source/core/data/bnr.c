#include <3ds.h>

#include "bnr.h"
#include "language.h"

u16* bnr_select_title(BNR* bnr) {
    return bnr->titles[language_pick(bnr->titles[language_system()])];
}
