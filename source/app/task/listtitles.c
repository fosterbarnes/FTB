#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <3ds.h>

#include "uitask.h"
#include "listtitles.h"
#include "../paths.h"
#include "../resources.h"
#include "../../core/core.h"

// Title list cache: one file per pass under TITLE_CACHE_DIR, records sorted by title ID. The live AM
// title list decides which titles exist; a cached record is reused only while its title ID, version,
// and installed size still match, so installs, deletes, and updates never show stale entries.
#define TITLE_CACHE_DIR PATHS_TITLE_CACHE_DIR
#define TITLE_CACHE_SD_PATH TITLE_CACHE_DIR "/sdTitles.bin"
#define TITLE_CACHE_NAND_PATH TITLE_CACHE_DIR "/nandTitles.bin"
#define TITLE_CACHE_TWL_PATH TITLE_CACHE_DIR "/twlTitles.bin"
#define TITLE_CACHE_MAGIC 0x54425446 // "FTBT"
#define TITLE_CACHE_FORMAT 1
#define TITLE_CACHE_BUFFER_SIZE 0x10000

#define TITLE_STATE_INFO 0x1   // AM version and size known: the title can be cached
#define TITLE_STATE_LOADED 0x2 // record filled and list item added

typedef struct title_cache_header_s {
    u32 magic;
    u16 format;
    u16 recordSize;
    u8 language;
    u8 reserved[3];
} title_cache_header;

typedef struct title_record_s {
    u64 titleId;
    u64 installedSize;
    u32 region;
    u16 version;
    u8 hasMeta;
    u8 twl;
    char productCode[0x10];
    char name[0x100];
    char shortDescription[0x100];
    char longDescription[0x200];
    char publisher[0x100];
    u8 icon[0x1200]; // CTR: 48x48 RGB565 tiled SMDH icon; TWL: 32x32 RGBA5551 untiled
} title_record;

typedef struct title_pass_s {
    FS_MediaType mediaType;
    bool twl;
    const char* path; // NULL: never cached (game card)

    bool twlCard;
    u32 count;
    u64* ids;
    AM_TitleEntry* infos;
    u8* states;
    title_record* records;

    bool complete; // every title of the pass was loaded: no filter rejects, cancel, or error
    bool changed;  // the cache file differs from what this pass loaded
} title_pass;

static Result task_populate_titles_add_record(populate_titles_data* data, FS_MediaType mediaType, const title_record* record) {
    if(data->hide != NULL && data->hide(data->userData, record->titleId)) {
        data->loaded++;
        return 0;
    }

    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    title_info* titleInfo = (title_info*) calloc(1, sizeof(title_info));
    if(titleInfo == NULL) {
        free(item);

        return R_APP_OUT_OF_MEMORY;
    }

    titleInfo->mediaType = mediaType;
    titleInfo->titleId = record->titleId;
    memcpy(titleInfo->productCode, record->productCode, sizeof(titleInfo->productCode));
    titleInfo->version = record->version;
    titleInfo->installedSize = record->installedSize;
    titleInfo->twl = record->twl;
    titleInfo->hasMeta = record->hasMeta;

    if(record->hasMeta) {
        string_copy(item->name, record->name, sizeof(item->name));
        string_copy(titleInfo->meta.shortDescription, record->shortDescription, sizeof(titleInfo->meta.shortDescription));
        string_copy(titleInfo->meta.longDescription, record->longDescription, sizeof(titleInfo->meta.longDescription));
        string_copy(titleInfo->meta.publisher, record->publisher, sizeof(titleInfo->meta.publisher));
        titleInfo->meta.region = record->region;
        titleInfo->meta.texture = screen_allocate_free_texture();

        // Linear filtering smooths the Title Manager's zoomed grid; 1:1 draws are unchanged.
        if(record->twl) {
            screen_load_texture_untiled(titleInfo->meta.texture, (void*) record->icon, 32 * 32 * 2, 32, 32, GPU_RGBA5551, true);
        } else {
            screen_load_texture_tiled(titleInfo->meta.texture, (void*) record->icon, 48 * 48 * 2, 48, 48, GPU_RGB565, true);
        }
    }

    if(string_is_empty(item->name)) {
        snprintf(item->name, LIST_ITEM_NAME_MAX, "%016llX", record->titleId);
    }

    if(record->twl) {
        item->color = COLOR_DS_TITLE;
    } else if(mediaType == MEDIATYPE_NAND) {
        item->color = COLOR_NAND;
    } else if(mediaType == MEDIATYPE_SD) {
        item->color = COLOR_SD;
    } else if(mediaType == MEDIATYPE_GAME_CARD) {
        item->color = COLOR_GAME_CARD;
    }

    item->data = titleInfo;

    if(!linked_list_add_sorted(data->items, item, data->userData, data->compare)) {
        task_free_title(item);

        return R_APP_OUT_OF_MEMORY;
    }

    data->loaded++;
    return 0;
}

// info: the batched AM entry, or NULL to query AM for this title alone.
static Result task_populate_titles_load_ctr(FS_MediaType mediaType, u64 titleId, const AM_TitleEntry* info, title_record* record) {
    Result res = 0;

    AM_TitleEntry entry;
    if(info == NULL) {
        if(R_FAILED(res = AM_GetTitleInfo(mediaType, 1, &titleId, &entry))) {
            return res;
        }

        info = &entry;
    }

    memset(record, 0, sizeof(*record));
    record->titleId = titleId;
    AM_GetTitleProductCode(mediaType, titleId, record->productCode);
    record->version = info->version;
    record->installedSize = info->size;

    static const u32 filePath[5] = {0x00000000, 0x00000000, 0x00000002, 0x6E6F6369, 0x00000000};
    u32 archivePath[4] = {(u32) (titleId & 0xFFFFFFFF), (u32) ((titleId >> 32) & 0xFFFFFFFF), mediaType, 0x00000000};

    Handle fileHandle;
    if(R_SUCCEEDED(FSUSER_OpenFileDirectly(&fileHandle, ARCHIVE_SAVEDATA_AND_CONTENT,
                                           fs_make_path_binary(archivePath, sizeof(archivePath)),
                                           fs_make_path_binary(filePath, sizeof(filePath)), FS_OPEN_READ, 0))) {
        SMDH* smdh = (SMDH*) calloc(1, sizeof(SMDH));
        if(smdh != NULL) {
            u32 bytesRead = 0;
            if(R_SUCCEEDED(FSFILE_Read(fileHandle, &bytesRead, 0, smdh, sizeof(SMDH))) && bytesRead == sizeof(SMDH)) {
                if(smdh_is_valid(smdh)) {
                    record->hasMeta = true;

                    SMDH_title* smdhTitle = smdh_select_title(smdh);

                    utf16_to_utf8((uint8_t*) record->name, smdhTitle->shortDescription, sizeof(record->name) - 1);

                    utf16_to_utf8((uint8_t*) record->shortDescription, smdhTitle->shortDescription, sizeof(record->shortDescription) - 1);
                    utf16_to_utf8((uint8_t*) record->longDescription, smdhTitle->longDescription, sizeof(record->longDescription) - 1);
                    utf16_to_utf8((uint8_t*) record->publisher, smdhTitle->publisher, sizeof(record->publisher) - 1);
                    record->region = smdh->region;
                    memcpy(record->icon, smdh->largeIcon, sizeof(smdh->largeIcon));
                }
            }

            free(smdh);
        }

        FSFILE_Close(fileHandle);
    }

    return res;
}

// info: the batched AM entry, or NULL to query AM and fall back to the ROM header (DS game card).
static Result task_populate_titles_load_twl(FS_MediaType mediaType, u64 titleId, const AM_TitleEntry* info, title_record* record) {
    Result res = 0;

    u64 realTitleId = 0;
    char productCode[0x10] = {'\0'};
    u16 version = 0;
    u64 installedSize = 0;

    u8 header[0x3B4] = {0};
    Result headerRes = FSUSER_GetLegacyRomHeader(mediaType, titleId, header);

    AM_TitleEntry entry;
    if(info != NULL || R_SUCCEEDED(res = AM_GetTitleInfo(mediaType, 1, &titleId, &entry))) {
        if(info == NULL) {
            info = &entry;
        }

        realTitleId = titleId;
        AM_GetTitleProductCode(mediaType, titleId, productCode);
        version = info->version;
        installedSize = info->size;
    } else if(R_SUCCEEDED(res = headerRes)) {
        memcpy(&realTitleId, &header[0x230], sizeof(realTitleId));
        memcpy(productCode, header, 0xC);
        version = header[0x01E];

        u32 size = 0;
        if((header[0x012] & 0x2) != 0) {
            memcpy(&size, &header[0x210], sizeof(size));
        } else {
            memcpy(&size, &header[0x080], sizeof(size));
        }

        installedSize = size;
    }

    if(R_FAILED(res)) {
        return res;
    }

    memset(record, 0, sizeof(*record));
    record->titleId = realTitleId;
    string_copy(record->productCode, productCode, sizeof(record->productCode));
    record->version = version;
    record->installedSize = installedSize;
    record->twl = true;

    BNR* bnr = (BNR*) calloc(1, sizeof(BNR));
    if(bnr != NULL) {
        if(R_SUCCEEDED(FSUSER_GetLegacyBannerData(mediaType, titleId, (u8*) bnr))) {
            record->hasMeta = true;

            char title[0x100] = {'\0'};
            utf16_to_utf8((uint8_t*) title, bnr_select_title(bnr), sizeof(title) - 1);

            if(strchr(title, '\n') == NULL) {
                string_copy(record->name, title, sizeof(record->name));
                string_copy(record->shortDescription, title, sizeof(record->shortDescription));
            } else {
                char* destinations[] = {record->shortDescription, record->longDescription, record->publisher};
                u32 destinationLens[] = {sizeof(record->shortDescription), sizeof(record->longDescription), sizeof(record->publisher)};
                int currDest = 0;

                char* last = title;
                char* curr = NULL;

                while(currDest < 3 && (curr = strchr(last, '\n')) != NULL) {
                    u32 copyLen = curr - last + 1;
                    if(copyLen > destinationLens[currDest]) {
                        copyLen = destinationLens[currDest];
                    }

                    string_copy(destinations[currDest++], last, copyLen);

                    last = curr + 1;
                    *curr = ' ';
                }

                string_copy(record->name, title, last - title);
                if(currDest < 3) {
                    string_copy(destinations[currDest], last, destinationLens[currDest]);
                }
            }

            for(u32 x = 0; x < 32; x++) {
                for(u32 y = 0; y < 32; y++) {
                    u32 srcPos = (((y >> 3) * 4 + (x >> 3)) * 8 + (y & 7)) * 4 + ((x & 7) >> 1);
                    u32 srcShift = (x & 1) * 4;
                    u16 srcPx = bnr->mainIconPalette[(bnr->mainIconBitmap[srcPos] >> srcShift) & 0xF];

                    u8 r = (u8) (srcPx & 0x1F);
                    u8 g = (u8) ((srcPx >> 5) & 0x1F);
                    u8 b = (u8) ((srcPx >> 10) & 0x1F);

                    u16 reversedPx = (u16) ((r << 11) | (g << 6) | (b << 1) | 1);

                    u32 dstPos = (y * 32 + x) * 2;
                    record->icon[dstPos + 0] = (u8) (reversedPx & 0xFF);
                    record->icon[dstPos + 1] = (u8) ((reversedPx >> 8) & 0xFF);
                }
            }

            if(R_SUCCEEDED(headerRes)) {
                memcpy(&record->region, &header[0x1B0], sizeof(record->region));
            }
        }

        free(bnr);
    }

    return 0;
}

static int task_populate_titles_compare_ids(const void* e1, const void* e2) {
    u64 id1 = *(u64*) e1;
    u64 id2 = *(u64*) e2;

    return id1 > id2 ? 1 : id1 < id2 ? -1 : 0;
}

static void task_populate_titles_free_pass(title_pass* pass) {
    free(pass->ids);
    free(pass->infos);
    free(pass->states);
    free(pass->records);
}

// Lists the pass's title IDs (ascending, filtered) and their AM version and size.
static Result task_populate_titles_prepare(populate_titles_data* data, title_pass* pass) {
    pass->complete = true;
    pass->changed = true;

    if(pass->mediaType == MEDIATYPE_GAME_CARD) {
        bool inserted = false;
        FS_CardType type = CARD_CTR;
        if(R_FAILED(FSUSER_CardSlotIsInserted(&inserted)) || !inserted || R_FAILED(FSUSER_GetCardType(&type))) {
            return 0;
        }

        pass->twlCard = type != CARD_CTR;
    }

    Result res = 0;

    u32 total = 1;
    if(!pass->twlCard && R_FAILED(res = AM_GetTitleCount(pass->mediaType, &total))) {
        return res;
    }

    if(total == 0) {
        return 0;
    }

    if((pass->ids = (u64*) calloc(total, sizeof(u64))) == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    // A DS game card has no AM entry: its one title (ID 0) is read from the ROM header.
    if(!pass->twlCard) {
        if(R_FAILED(res = AM_GetTitleList(&total, pass->mediaType, total, pass->ids))) {
            return res;
        }

        qsort(pass->ids, total, sizeof(u64), task_populate_titles_compare_ids);
    }

    for(u32 i = 0; i < total; i++) {
        bool dsiWare = ((pass->ids[i] >> 32) & 0x8000) != 0;
        if(dsiWare != pass->twl) {
            continue;
        }

        if(data->filter != NULL && !data->filter(data->userData, pass->ids[i], pass->mediaType)) {
            pass->complete = false;
            continue;
        }

        pass->ids[pass->count++] = pass->ids[i];
    }

    if(pass->count == 0) {
        return 0;
    }

    pass->infos = (AM_TitleEntry*) calloc(pass->count, sizeof(AM_TitleEntry));
    pass->states = (u8*) calloc(pass->count, sizeof(u8));
    pass->records = (title_record*) calloc(pass->count, sizeof(title_record));
    if(pass->infos == NULL || pass->states == NULL || pass->records == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(pass->twlCard) {
        return 0;
    }

    if(R_SUCCEEDED(AM_GetTitleInfo(pass->mediaType, pass->count, pass->ids, pass->infos))) {
        memset(pass->states, TITLE_STATE_INFO, pass->count);
    } else {
        // One bad entry fails the whole batch: ask per title so the rest still cache.
        for(u32 i = 0; i < pass->count; i++) {
            if(R_SUCCEEDED(AM_GetTitleInfo(pass->mediaType, 1, &pass->ids[i], &pass->infos[i]))) {
                pass->states[i] = TITLE_STATE_INFO;
            }
        }
    }

    return 0;
}

static bool task_populate_titles_read_record(FILE* fd, title_record* record) {
    if(fread(record, sizeof(title_record), 1, fd) != 1) {
        return false;
    }

    // The file is user-editable: terminate every string and normalize the flags.
    record->productCode[sizeof(record->productCode) - 1] = '\0';
    record->name[sizeof(record->name) - 1] = '\0';
    record->shortDescription[sizeof(record->shortDescription) - 1] = '\0';
    record->longDescription[sizeof(record->longDescription) - 1] = '\0';
    record->publisher[sizeof(record->publisher) - 1] = '\0';
    record->hasMeta = record->hasMeta != 0;
    record->twl = record->twl != 0;
    return true;
}

// Returns the cache file positioned at its first record, or NULL when it is missing or invalid.
static FILE* task_populate_titles_open_cache(const char* path, u8 language) {
    FILE* fd = fopen(path, "rb");
    if(fd == NULL) {
        return NULL;
    }

    title_cache_header header;
    long size = 0;
    if(setvbuf(fd, NULL, _IOFBF, TITLE_CACHE_BUFFER_SIZE) == 0
       && fseek(fd, 0, SEEK_END) == 0
       && (size = ftell(fd)) >= (long) sizeof(header)
       && (size - sizeof(header)) % sizeof(title_record) == 0
       && fseek(fd, 0, SEEK_SET) == 0
       && fread(&header, sizeof(header), 1, fd) == 1
       && header.magic == TITLE_CACHE_MAGIC
       && header.format == TITLE_CACHE_FORMAT
       && (size_t) header.recordSize == sizeof(title_record)
       && header.language == language) {
        return fd;
    }

    fclose(fd);
    return NULL;
}

// Phase 1: adds every title whose cached record still matches AM. Both sides are sorted by title ID,
// so one forward merge finds hits, misses, and stale records.
static Result task_populate_titles_add_cached(populate_titles_data* data, title_pass* pass, u8 language) {
    if(pass->path == NULL || data->cacheMode != TITLE_CACHE_ON) {
        return 0;
    }

    FILE* fd = task_populate_titles_open_cache(pass->path, language);
    if(fd == NULL) {
        return 0;
    }

    title_record* old = (title_record*) calloc(1, sizeof(title_record));
    if(old == NULL) {
        fclose(fd);
        return R_APP_OUT_OF_MEMORY;
    }

    pass->changed = false;

    Result res = 0;
    bool have = task_populate_titles_read_record(fd, old);
    for(u32 i = 0; i < pass->count && have && R_SUCCEEDED(res); i++) {
        if(task_is_cancelled(&data->state)) {
            pass->complete = false;
            break;
        }

        while(have && old->titleId < pass->ids[i]) {
            if(task_is_cancelled(&data->state)) {
                pass->complete = false;
                have = false;
                break;
            }

            pass->changed = true;
            u64 previousId = old->titleId;
            have = task_populate_titles_read_record(fd, old);
            if(have && old->titleId <= previousId) {
                have = false;
            }
        }

        if(!have || old->titleId != pass->ids[i]) {
            continue;
        }

        if((pass->states[i] & TITLE_STATE_INFO) && old->twl == pass->twl
           && old->version == pass->infos[i].version && old->installedSize == pass->infos[i].size) {
            pass->records[i] = *old;
            pass->states[i] |= TITLE_STATE_LOADED;
            res = task_populate_titles_add_record(data, pass->mediaType, &pass->records[i]);
        }

        u64 previousId = old->titleId;
        have = task_populate_titles_read_record(fd, old);
        if(have && old->titleId <= previousId) {
            pass->changed = true;
            have = false;
        }
    }

    if(have) {
        pass->changed = true;
    }

    free(old);
    fclose(fd);
    return res;
}

// Phase 2: loads every title the cache did not cover.
static Result task_populate_titles_add_missing(populate_titles_data* data, title_pass* pass) {
    Result res = 0;
    for(u32 i = 0; i < pass->count && R_SUCCEEDED(res); i++) {
        if(pass->states[i] & TITLE_STATE_LOADED) {
            continue;
        }

        if(task_is_cancelled(&data->state)) {
            pass->complete = false;
            break;
        }

        const AM_TitleEntry* info = (pass->states[i] & TITLE_STATE_INFO) ? &pass->infos[i] : NULL;
        title_record* record = &pass->records[i];
        res = (pass->twl || pass->twlCard) ? task_populate_titles_load_twl(pass->mediaType, pass->ids[i], info, record)
                                           : task_populate_titles_load_ctr(pass->mediaType, pass->ids[i], info, record);
        if(R_SUCCEEDED(res)) {
            pass->states[i] |= TITLE_STATE_LOADED;
            pass->changed |= info != NULL;
            res = task_populate_titles_add_record(data, pass->mediaType, record);
        }
    }

    if(R_FAILED(res)) {
        pass->complete = false;
    }

    return res;
}

typedef struct {
    const title_pass* pass;
    u8 language;
} title_cache_write_data;

static bool task_populate_titles_write_records(FILE* fd, void* arg) {
    const title_cache_write_data* data = (const title_cache_write_data*) arg;
    const title_pass* pass = data->pass;

    title_cache_header header = {TITLE_CACHE_MAGIC, TITLE_CACHE_FORMAT, sizeof(title_record), data->language, {0}};
    bool written = setvbuf(fd, NULL, _IOFBF, TITLE_CACHE_BUFFER_SIZE) == 0 && fwrite(&header, sizeof(header), 1, fd) == 1;
    for(u32 i = 0; i < pass->count && written; i++) {
        if((pass->states[i] & (TITLE_STATE_INFO | TITLE_STATE_LOADED)) == (TITLE_STATE_INFO | TITLE_STATE_LOADED)) {
            written = fwrite(&pass->records[i], sizeof(title_record), 1, fd) == 1;
        }
    }

    return written;
}

// Writes the pass's cacheable records. A failed write only costs speed: the list is already shown
// and the next open tries again, so it is not reported.
static void task_populate_titles_write_cache(const title_pass* pass, u8 language) {
    paths_ensure_data_dir();
    mkdir(TITLE_CACHE_DIR, 0777);

    title_cache_write_data data = {pass, language};
    fs_write_file_safe(pass->path, task_populate_titles_write_records, &data);
}

static Result task_populate_titles_thread(void* arg) {
    populate_titles_data* data = (populate_titles_data*) arg;

    title_pass passes[] = {
            {.mediaType = MEDIATYPE_GAME_CARD},
            {.mediaType = MEDIATYPE_SD, .path = TITLE_CACHE_SD_PATH},
            {.mediaType = MEDIATYPE_NAND, .path = TITLE_CACHE_NAND_PATH},
            {.mediaType = MEDIATYPE_NAND, .twl = true, .path = TITLE_CACHE_TWL_PATH},
    };
    const u32 passCount = sizeof(passes) / sizeof(passes[0]);

    // Names come from the SMDH/banner entry for the system language, so it keys the cache.
    u8 language = language_system();

    // List every pass first so the progress total is known before any title shows. A failing pass
    // stops the passes after it, but the ones before it still finish.
    Result res = 0;
    u32 ready = 0;
    while(ready < passCount && R_SUCCEEDED(res = task_populate_titles_prepare(data, &passes[ready]))) {
        ready++;
    }

    u32 total = 0;
    for(u32 i = 0; i < ready; i++) {
        total += passes[i].count;
    }

    data->total = total;

    // Show every cached title first, then load the new and changed ones.
    if(data->cacheMode == TITLE_CACHE_ON) {
        data->stage = TITLE_STAGE_CACHE;
    }

    for(u32 i = 0; i < ready; i++) {
        Result cacheRes = task_populate_titles_add_cached(data, &passes[i], language);
        if(R_FAILED(cacheRes)) {
            res = cacheRes;
            ready = i;
            break;
        }
    }

    for(u32 i = 0; i < ready; i++) {
        data->stage = TITLE_STAGE_LOADING;

        Result passRes = task_populate_titles_add_missing(data, &passes[i]);
        if(R_FAILED(passRes)) {
            res = passRes;
            break;
        }

        if(data->cacheMode != TITLE_CACHE_OFF && passes[i].path != NULL && passes[i].complete && passes[i].changed) {
            data->stage = TITLE_STAGE_SAVING;
            task_populate_titles_write_cache(&passes[i], language);
        }
    }

    for(u32 i = 0; i < passCount; i++) {
        task_populate_titles_free_pass(&passes[i]);
    }

    return res;
}

void task_free_title(list_item* item) {
    if(item == NULL) {
        return;
    }

    if(item->data != NULL) {
        title_info* titleInfo = (title_info*) item->data;
        if(titleInfo->hasMeta) {
            screen_unload_texture(titleInfo->meta.texture);
        }

        free(item->data);
    }

    free(item);
}

void task_clear_titles(linked_list* items) {
    if(items == NULL) {
        return;
    }

    linked_list_iter iter;
    linked_list_iterate(items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);

        linked_list_iter_remove(&iter);
        task_free_title(item);
    }
}

Result task_populate_titles(populate_titles_data* data) {
    if(data == NULL || data->items == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_clear_titles(data->items);

    data->stage = TITLE_STAGE_LISTING;
    data->total = 0;
    data->loaded = 0;

    return task_start(&data->state, task_populate_titles_thread, data, 0x19, 1);
}

static bool task_clear_title_cache_file(const char* path) {
    return remove(path) == 0 || errno == ENOENT;
}

bool task_clear_title_cache() {
    static const char* const paths[] = {TITLE_CACHE_SD_PATH, TITLE_CACHE_NAND_PATH, TITLE_CACHE_TWL_PATH};

    bool cleared = true;
    for(u32 i = 0; i < sizeof(paths) / sizeof(paths[0]); i++) {
        char tempPath[64];
        snprintf(tempPath, sizeof(tempPath), "%s.tmp", paths[i]);

        cleared = task_clear_title_cache_file(paths[i]) && cleared;
        cleared = task_clear_title_cache_file(tempPath) && cleared;
    }

    return cleared;
}
