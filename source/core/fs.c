#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "error.h"
#include "fs.h"
#include "linkedlist.h"
#include "stringutil.h"

// Entries per FSDIR_Read call in fs_read_dir.
#define FS_DIR_CHUNK 64

bool fs_is_dir(FS_Archive archive, const char* path) {
    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        Handle dirHandle = 0;
        if(R_SUCCEEDED(res = FSUSER_OpenDirectory(&dirHandle, archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return R_SUCCEEDED(res);
}

bool fs_file_exists(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return false;
    }

    Handle file = 0;
    bool exists = R_SUCCEEDED(FSUSER_OpenFile(&file, archive, *fsPath, FS_OPEN_READ, 0));
    if(exists) {
        FSFILE_Close(file);
    }

    fs_free_path_utf8(fsPath);
    return exists;
}

Result fs_ensure_dir(FS_Archive archive, const char* path) {
    Result res = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        // A file in the way fails the create; it is the user's data, so it stays.
        Handle dirHandle = 0;
        if(R_SUCCEEDED(FSUSER_OpenDirectory(&dirHandle, archive, *fsPath))) {
            FSDIR_Close(dirHandle);
        } else {
            res = FSUSER_CreateDirectory(archive, *fsPath, 0);
        }

        fs_free_path_utf8(fsPath);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

Result fs_read_dir(FS_Archive archive, const char* path, FS_DirectoryEntry** entries, u32* count) {
    *entries = NULL;
    *count = 0;

    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Handle dirHandle = 0;
    Result res = FSUSER_OpenDirectory(&dirHandle, archive, *fsPath);
    fs_free_path_utf8(fsPath);
    if(R_FAILED(res)) {
        return res;
    }

    FS_DirectoryEntry* list = NULL;
    u32 total = 0;
    u32 capacity = 0;
    while(true) {
        if(total + FS_DIR_CHUNK > capacity) {
            u32 grownCapacity = capacity == 0 ? FS_DIR_CHUNK : capacity * 2;
            FS_DirectoryEntry* grown = (FS_DirectoryEntry*) realloc(list, grownCapacity * sizeof(FS_DirectoryEntry));
            if(grown == NULL) {
                res = R_APP_OUT_OF_MEMORY;
                break;
            }

            list = grown;
            capacity = grownCapacity;
        }

        u32 read = 0;
        if(R_FAILED(res = FSDIR_Read(dirHandle, &read, FS_DIR_CHUNK, &list[total])) || read == 0) {
            break;
        }

        total += read;
    }

    FSDIR_Close(dirHandle);

    if(R_FAILED(res) || total == 0) {
        free(list);
        return res;
    }

    *entries = list;
    *count = total;
    return 0;
}

bool fs_write_file_safe(const char* path, bool (*write)(FILE* fd, void* data), void* data) {
    char tempPath[FILE_PATH_MAX];
    if(snprintf(tempPath, sizeof(tempPath), "%s.tmp", path) >= (int) sizeof(tempPath)) {
        return false;
    }

    FILE* fd = fopen(tempPath, "wb");
    if(fd == NULL) {
        return false;
    }

    bool written = write(fd, data);
    written = fclose(fd) == 0 && written;

    // FS rename fails when the target exists, so the old file goes just before it.
    if(!written || (remove(path) != 0 && errno != ENOENT) || rename(tempPath, path) != 0) {
        remove(tempPath);
        return false;
    }

    return true;
}

FS_Path fs_make_path_binary(const void* data, u32 size) {
    FS_Path path = {PATH_BINARY, size, data};
    return path;
}

FS_Path* fs_make_path_utf8(const char* path) {
    size_t len = strlen(path);

    u16* utf16 = (u16*) calloc(len + 1, sizeof(u16));
    if(utf16 == NULL) {
        return NULL;
    }

    ssize_t utf16Len = utf8_to_utf16(utf16, (const uint8_t*) path, len);
    if(utf16Len < 0) {
        free(utf16);
        return NULL;
    }

    FS_Path* fsPath = (FS_Path*) calloc(1, sizeof(FS_Path));
    if(fsPath == NULL) {
        free(utf16);
        return NULL;
    }

    fsPath->type = PATH_UTF16;
    fsPath->size = (utf16Len + 1) * sizeof(u16);
    fsPath->data = utf16;

    return fsPath;
}

void fs_free_path_utf8(FS_Path* path) {
    free((void*) path->data);
    free(path);
}

typedef struct {
    FS_Archive archive;
    u32 refs;
} archive_ref;

static linked_list opened_archives;

Result fs_open_archive(FS_Archive* archive, FS_ArchiveID id, FS_Path path) {
    if(archive == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    Result res = 0;

    FS_Archive arch = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&arch, id, path))) {
        if(R_SUCCEEDED(res = fs_ref_archive(arch))) {
            *archive = arch;
        } else {
            FSUSER_CloseArchive(arch);
        }
    }

    return res;
}

Result fs_ref_archive(FS_Archive archive) {
    linked_list_iter iter;
    linked_list_iterate(&opened_archives, &iter);

    while(linked_list_iter_has_next(&iter)) {
        archive_ref* ref = (archive_ref*) linked_list_iter_next(&iter);
        if(ref->archive == archive) {
            ref->refs++;
            return 0;
        }
    }

    Result res = 0;

    archive_ref* ref = (archive_ref*) calloc(1, sizeof(archive_ref));
    if(ref != NULL) {
        ref->archive = archive;
        ref->refs = 1;

        linked_list_add(&opened_archives, ref);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    return res;
}

Result fs_close_archive(FS_Archive archive) {
    linked_list_iter iter;
    linked_list_iterate(&opened_archives, &iter);

    while(linked_list_iter_has_next(&iter)) {
        archive_ref* ref = (archive_ref*) linked_list_iter_next(&iter);
        if(ref->archive == archive) {
            ref->refs--;

            if(ref->refs == 0) {
                linked_list_iter_remove(&iter);
                free(ref);
            } else {
                return 0;
            }
        }
    }

    return FSUSER_CloseArchive(archive);
}

static char path_3dsx[FILE_PATH_MAX] = "";

const char* fs_get_3dsx_path() {
    if(strlen(path_3dsx) == 0) {
        return NULL;
    }

    return path_3dsx;
}

void fs_set_3dsx_path(const char* path) {
    if(strlen(path) >= 5 && strncmp(path, "sdmc:", 5) == 0) {
        string_copy(path_3dsx, path + 5, FILE_PATH_MAX);
    } else {
        string_copy(path_3dsx, path, FILE_PATH_MAX);
    }
}

int fs_make_3dsx_path(char* out, const char* name, size_t size) {
    char filename[FILE_NAME_MAX];
    string_escape_file_name(filename, name, sizeof(filename));

    return snprintf(out, size, "/3ds/%s/%s.3dsx", filename, filename);
}

int fs_make_smdh_path(char* out, const char* name, size_t size) {
    char filename[FILE_NAME_MAX];
    string_escape_file_name(filename, name, sizeof(filename));

    return snprintf(out, size, "/3ds/%s/%s.smdh", filename, filename);
}

FS_MediaType fs_get_title_destination(u64 titleId) {
    u16 platform = (u16) ((titleId >> 48) & 0xFFFF);
    u16 category = (u16) ((titleId >> 32) & 0xFFFF);
    u8 variation = (u8) (titleId & 0xFF);

    //     DSiWare                3DS                    DSiWare, System, DLP         Application           System Title
    return platform == 0x0003 || (platform == 0x0004 && ((category & 0x8011) != 0 || (category == 0x0000 && variation == 0x02))) ? MEDIATYPE_NAND : MEDIATYPE_SD;
}

const char* fs_media_type_name(FS_MediaType mediaType) {
    switch(mediaType) {
        case MEDIATYPE_NAND:
            return "NAND";
        case MEDIATYPE_SD:
            return "SD";
        default:
            return "Game Card";
    }
}

bool fs_filter_cias(void* data, const char* name, u32 attributes) {
    if(data != NULL) {
        fs_filter_data* filterData = (fs_filter_data*) data;
        if(filterData->parentFilter != NULL && !filterData->parentFilter(filterData->parentFilterData, name, attributes)) {
            return false;
        }
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    size_t len = strlen(name);
    return len >= 4 && strncasecmp(name + len - 4, ".cia", 4) == 0;
}

bool fs_filter_tickets(void* data, const char* name, u32 attributes) {
    if(data != NULL) {
        fs_filter_data* filterData = (fs_filter_data*) data;
        if(filterData->parentFilter != NULL && !filterData->parentFilter(filterData->parentFilterData, name, attributes)) {
            return false;
        }
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return false;
    }

    size_t len = strlen(name);
    return (len >= 4 && strncasecmp(name + len - 4, ".tik", 4) == 0) || (len >= 5 && strncasecmp(name + len - 5, ".cetk", 5) == 0);
}