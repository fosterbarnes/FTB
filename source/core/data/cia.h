#pragma once

typedef struct SMDH_s SMDH;

typedef enum cia_seed_e {
    CIA_SEED_UNKNOWN,
    CIA_SEED_NO,
    CIA_SEED_YES
} cia_seed;

Result cia_get_title_id(u64* titleId, u8* cia, size_t size);
cia_seed cia_get_seed_state(u8* cia, size_t size);
Result cia_file_get_smdh(SMDH* smdh, Handle handle);