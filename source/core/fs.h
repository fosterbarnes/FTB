#pragma once

#include <stdio.h>

#define FILE_NAME_MAX 256
#define FILE_PATH_MAX 512

typedef struct fs_filter_data_s {
    bool (*parentFilter)(void* data, const char* name, u32 attributes);
    void* parentFilterData;
} fs_filter_data;

bool fs_is_dir(FS_Archive archive, const char* path);
// A file (not a directory) at path that opens for reading.
bool fs_file_exists(FS_Archive archive, const char* path);
// Creates the directory when it is missing. A file at that path is an error, never deleted.
Result fs_ensure_dir(FS_Archive archive, const char* path);
// Every entry of a directory, unsorted, however many there are (read in chunks). *entries is
// malloc'd for the caller to free, NULL when the read fails or the directory is empty.
Result fs_read_dir(FS_Archive archive, const char* path, FS_DirectoryEntry** entries, u32* count);
// Replaces a stdio file through "<path>.tmp", so a crash or power loss mid-write never leaves a
// cut-off file: write fills fd and returns false on failure; the old file is replaced only after a
// complete write. False when anything failed (the old file then stays, except when the final rename
// fails: the new copy then stays as "<path>.tmp").
bool fs_write_file_safe(const char* path, bool (*write)(FILE* fd, void* data), void* data);
// fopen for files written by fs_write_file_safe: when path is missing, a leftover "<path>.tmp" takes
// its place first.
FILE* fs_open_file_safe(const char* path, const char* mode);

FS_Path fs_make_path_binary(const void* data, u32 size);
FS_Path* fs_make_path_utf8(const char* path);
void fs_free_path_utf8(FS_Path* path);

Result fs_open_archive(FS_Archive* archive, FS_ArchiveID id, FS_Path path);
Result fs_ref_archive(FS_Archive archive);
Result fs_close_archive(FS_Archive archive);

const char* fs_get_3dsx_path();
void fs_set_3dsx_path(const char* path);

int fs_make_3dsx_path(char* out, const char* name, size_t size);
int fs_make_smdh_path(char* out, const char* name, size_t size);

FS_MediaType fs_get_title_destination(u64 titleId);
// "NAND", "SD", or "Game Card".
const char* fs_media_type_name(FS_MediaType mediaType);

bool fs_filter_cias(void* data, const char* name, u32 attributes);
bool fs_filter_tickets(void* data, const char* name, u32 attributes);