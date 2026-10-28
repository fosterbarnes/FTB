#include <string.h>

#include <3ds.h>

#include "cia.h"
#include "smdh.h"
#include "tmd.h"
#include "../error.h"

Result cia_get_title_id(u64* titleId, u8* cia, size_t size) {
    if(cia == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    if(size < 0x10) {
        return R_APP_BAD_DATA;
    }

    // u64 like cia_get_seed_state: header sizes from a bad download can wrap a u32 sum back into the
    // buffer and pass the bounds check.
    u64 headerSize = ((u64) *(u32*) &cia[0x00] + 0x3F) & ~0x3F;
    u64 certSize = ((u64) *(u32*) &cia[0x08] + 0x3F) & ~0x3F;
    u64 ticketSize = ((u64) *(u32*) &cia[0x0C] + 0x3F) & ~0x3F;
    u64 offset = headerSize + certSize + ticketSize;

    if(offset >= size) {
        return R_APP_BAD_DATA;
    }

    return tmd_get_title_id(titleId, &cia[offset], size - offset);
}

// Whether a title needs a seed, read from the CIA's first bytes: content 0's NCCH header has bit 0x20
// in flags[7] when it uses seed crypto (3dbrew NCCH). Missing content 0, content encrypted with the
// title key, or a header past the end of the buffer give CIA_SEED_UNKNOWN.
cia_seed cia_get_seed_state(u8* cia, size_t size) {
    if(cia == NULL || size < 0x2020 || !(cia[0x20] & 0x80)) {
        return CIA_SEED_UNKNOWN;
    }

    u64 headerSize = ((u64) *(u32*) &cia[0x00] + 0x3F) & ~0x3F;
    u64 certSize = ((u64) *(u32*) &cia[0x08] + 0x3F) & ~0x3F;
    u64 ticketSize = ((u64) *(u32*) &cia[0x0C] + 0x3F) & ~0x3F;
    u64 tmdSize = ((u64) *(u32*) &cia[0x10] + 0x3F) & ~0x3F;

    u64 tmdOffset = headerSize + certSize + ticketSize;
    if(tmdOffset >= size) {
        return CIA_SEED_UNKNOWN;
    }

    u16 contentType = 0;
    if(R_FAILED(tmd_get_content_type(&contentType, &cia[tmdOffset], size - tmdOffset, 0)) || (contentType & 0x1)) {
        return CIA_SEED_UNKNOWN;
    }

    u64 ncchOffset = tmdOffset + tmdSize;
    if(ncchOffset + 0x190 > size) {
        return CIA_SEED_UNKNOWN;
    }

    u8* ncch = &cia[ncchOffset];
    if(memcmp(&ncch[0x100], "NCCH", 4) != 0) {
        return CIA_SEED_UNKNOWN;
    }

    return (ncch[0x18F] & 0x20) ? CIA_SEED_YES : CIA_SEED_NO;
}

Result cia_file_get_smdh(SMDH* smdh, Handle handle) {
    Result res = 0;

    if(smdh != NULL) {
        u32 bytesRead = 0;

        u32 header[8];
        if(R_SUCCEEDED(res = FSFILE_Read(handle, &bytesRead, 0, header, sizeof(header))) && bytesRead == sizeof(header)) {
            u32 headerSize = (header[0] + 0x3F) & ~0x3F;
            u32 certSize = (header[2] + 0x3F) & ~0x3F;
            u32 ticketSize = (header[3] + 0x3F) & ~0x3F;
            u32 tmdSize = (header[4] + 0x3F) & ~0x3F;
            u32 metaSize = (header[5] + 0x3F) & ~0x3F;
            u64 contentSize = ((header[6] | ((u64) header[7] << 32)) + 0x3F) & ~0x3F;

            if(metaSize >= 0x3AC0) {
                res = FSFILE_Read(handle, &bytesRead, headerSize + certSize + ticketSize + tmdSize + contentSize + 0x400, smdh, sizeof(SMDH));
            } else {
                res = R_APP_BAD_DATA;
            }
        }
    } else {
        res = R_APP_INVALID_ARGUMENT;
    }

    return res;
}