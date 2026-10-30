#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "contentinstall.h"
#include "../../core/core.h"

#define CONTENT_MAGIC_3DSX 0x58534433
#define CONTENT_MAGIC_SMDH 0x48444D53

#define CONTENT_COPY_SIZE (64 * 1024)

static bool content_part_path(char* out, const char* path) {
    return snprintf(out, FILE_PATH_MAX, "%s.part", path) < FILE_PATH_MAX;
}

static Result content_delete_file(FS_Archive archive, const char* path) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_DeleteFile(archive, *fsPath);
    fs_free_path_utf8(fsPath);

    return res;
}

// Every folder above path, from the root down.
static Result content_ensure_parents(FS_Archive archive, const char* path) {
    char dir[FILE_PATH_MAX];
    for(const char* slash = strchr(path + 1, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
        size_t len = (size_t) (slash - path) + 1;
        if(len >= sizeof(dir)) {
            return R_APP_OUT_OF_RANGE;
        }

        memcpy(dir, path, len);
        dir[len] = '\0';

        Result res = fs_ensure_dir(archive, dir);
        if(R_FAILED(res)) {
            return res;
        }
    }

    return 0;
}

static Result content_open_file(const char* path, u32 flags, u32* handle) {
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    Result res = FSUSER_OpenFileDirectly(handle, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, flags, 0);
    fs_free_path_utf8(fsPath);

    return res;
}

static Result content_copy_handles(Handle src, Handle dst) {
    u64 size = 0;
    Result res = 0;
    if(R_FAILED(res = FSFILE_GetSize(src, &size))) {
        return res;
    }

    u8* buffer = (u8*) malloc(CONTENT_COPY_SIZE);
    if(buffer == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(R_FAILED(res = FSFILE_SetSize(dst, size))) {
        free(buffer);
        return res;
    }

    for(u64 offset = 0; offset < size; offset += CONTENT_COPY_SIZE) {
        u32 length = size - offset < CONTENT_COPY_SIZE ? (u32) (size - offset) : CONTENT_COPY_SIZE;

        u32 bytesRead = 0;
        if(R_FAILED(res = FSFILE_Read(src, &bytesRead, offset, buffer, length))) {
            break;
        }

        u32 bytesWritten = 0;
        if(bytesRead != length || (R_SUCCEEDED(res = FSFILE_Write(dst, &bytesWritten, offset, buffer, length, 0)) && bytesWritten != length)) {
            res = R_APP_BAD_DATA;
        }

        if(R_FAILED(res)) {
            break;
        }
    }

    free(buffer);
    return res;
}

// Copies the part file over path in place: path may be the running 3DSX, which romfs keeps open, so it
// cannot be deleted or renamed over. Downloading finishes before this copy starts; an SD error during
// the copy can still damage the destination, so the whole part file is kept on failure.
static Result content_copy_over(const char* partPath, const char* path) {
    Handle src = 0;
    Result res = content_open_file(partPath, FS_OPEN_READ, &src);
    if(R_FAILED(res)) {
        return res;
    }

    Handle dst = 0;
    if(R_SUCCEEDED(res = content_open_file(path, FS_OPEN_WRITE | FS_OPEN_CREATE, &dst))) {
        res = content_copy_handles(src, dst);

        Result closeRes = FSFILE_Close(dst);
        if(R_SUCCEEDED(res)) {
            res = closeRes;
        }
    }

    FSFILE_Close(src);
    return res;
}

Result content_install_begin(content_install* content, const char* fileName, const char* dest3dsx, void* block, u32 blockSize, void* data,
                             void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), u32* handle) {
    content->type = CONTENT_CIA;
    content->path[0] = '\0';
    content->part = false;
    content->copyOver = false;

    // The magic checks below read 4 bytes.
    if(blockSize < sizeof(u32)) {
        return R_APP_BAD_DATA;
    }

    if(*(u16*) block == 0x2020) {
        return cia_install_begin(&content->cia, block, blockSize, data, drawTop, handle);
    }

    if(*(u16*) block == 0x0100) {
        content->type = CONTENT_TICKET;
        return ticket_install_begin(&content->ticket, block, blockSize, handle);
    }

    u32 magic = *(u32*) block;
    if(magic != CONTENT_MAGIC_3DSX && magic != CONTENT_MAGIC_SMDH) {
        return R_APP_BAD_DATA;
    }

    content->type = CONTENT_FILE;

    char dir[FILE_PATH_MAX];
    if(dest3dsx != NULL && !string_is_empty(dest3dsx)) {
        string_get_parent_path(dir, dest3dsx, FILE_PATH_MAX);
        string_copy(content->path, dest3dsx, FILE_PATH_MAX);
    } else {
        // A URL's name can hold characters FAT does not allow (fs_make_3dsx_path's rule).
        char baseName[FILE_NAME_MAX];
        string_get_file_name(baseName, fileName, FILE_NAME_MAX);
        char name[FILE_NAME_MAX];
        string_escape_file_name(name, baseName, FILE_NAME_MAX);

        snprintf(dir, FILE_PATH_MAX, "/3ds/%s/", name);
        if(snprintf(content->path, FILE_PATH_MAX, "/3ds/%s/%s.%s", name, name, magic == CONTENT_MAGIC_3DSX ? "3dsx" : "smdh") >= FILE_PATH_MAX) {
            return R_APP_OUT_OF_RANGE; // a cut path would write to the wrong file
        }
    }

    char partPath[FILE_PATH_MAX];
    if(!content_part_path(partPath, content->path)) {
        return R_APP_OUT_OF_RANGE;
    }

    content->part = true;
    content->copyOver = true;

    FS_Archive sdmcArchive = 0;
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_SUCCEEDED(res)) {
        if(R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, "/3ds/")) && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, dir))) {
            content_delete_file(sdmcArchive, partPath); // an earlier cut download's part file
            res = content_open_file(partPath, FS_OPEN_WRITE | FS_OPEN_CREATE, handle);
        }

        FSUSER_CloseArchive(sdmcArchive);
    }

    return res;
}

Result content_save_begin(content_install* content, const char* path, bool replace, u32* handle) {
    content->type = CONTENT_FILE;
    content->part = true;
    content->copyOver = false;
    string_copy(content->path, path, FILE_PATH_MAX);

    char partPath[FILE_PATH_MAX];
    if(!content_part_path(partPath, path)) {
        return R_APP_OUT_OF_RANGE;
    }

    FS_Archive sdmcArchive = 0;
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(res)) {
        return res;
    }

    if(!replace && fs_file_exists(sdmcArchive, path)) {
        res = R_APP_SKIPPED;
    } else if(R_SUCCEEDED(res = content_ensure_parents(sdmcArchive, path))) {
        // A part file left by an earlier cut transfer would keep its old tail.
        content_delete_file(sdmcArchive, partPath);
        res = content_open_file(partPath, FS_OPEN_WRITE | FS_OPEN_CREATE, handle);
    }

    FSUSER_CloseArchive(sdmcArchive);
    return res;
}

Result content_end(content_install* content, data_op_data* op, bool succeeded, u32 handle) {
    if(content->type == CONTENT_CIA) {
        return cia_install_end(&content->cia, op, succeeded, handle);
    }

    if(content->type == CONTENT_TICKET) {
        return ticket_install_end(&content->ticket, succeeded, handle);
    }

    Result res = FSFILE_Close(handle);

    char partPath[FILE_PATH_MAX];
    const char* written = content->path;
    if(content->part) {
        // Fit was checked when it opened; checking again keeps -Wformat-truncation quiet.
        if(!content_part_path(partPath, content->path)) {
            return R_SUCCEEDED(res) ? R_APP_OUT_OF_RANGE : res;
        }

        written = partPath;
    }

    FS_Archive sdmcArchive = 0;
    Result archiveRes = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_FAILED(archiveRes)) {
        return R_SUCCEEDED(res) ? archiveRes : res;
    }

    if(!succeeded || R_FAILED(res)) {
        content_delete_file(sdmcArchive, written);
    } else if(content->copyOver) {
        // A failed copy keeps the part file: it may hold the only whole copy left.
        if(R_SUCCEEDED(res = content_copy_over(partPath, content->path))) {
            content_delete_file(sdmcArchive, partPath);
        }
    } else if(content->part) {
        // Without replace the file did not exist when the save began, so this deletes nothing then.
        content_delete_file(sdmcArchive, content->path);

        // A failed rename keeps the part file: with replace it holds the only copy left.
        res = R_APP_OUT_OF_MEMORY;

        FS_Path* fromPath = fs_make_path_utf8(partPath);
        if(fromPath != NULL) {
            FS_Path* toPath = fs_make_path_utf8(content->path);
            if(toPath != NULL) {
                res = FSUSER_RenameFile(sdmcArchive, *fromPath, sdmcArchive, *toPath);
                fs_free_path_utf8(toPath);
            }

            fs_free_path_utf8(fromPath);
        }
    }

    FSUSER_CloseArchive(sdmcArchive);
    return res;
}
