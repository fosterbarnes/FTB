#include <string.h>

#include <3ds.h>

#include "stringutil.h"

bool string_is_empty(const char* str) {
    if(strlen(str) == 0) {
        return true;
    }

    const char* curr = str;
    while(*curr) {
        if(*curr != ' ') {
            return false;
        }

        curr++;
    }

    return true;
}

void string_copy(char* dst, const char* src, size_t size) {
    if(size > 0) {
        strncpy(dst, src, size - 1);
        dst[size - 1] = '\0';
    }
}

void string_get_file_name(char* out, const char* file, u32 size) {
    const char* end = file + strlen(file);
    const char* curr = file - 1;
    while((curr = strchr(curr + 1, '.')) != NULL) {
        end = curr;
    }

    u32 terminatorPos = end - file < size - 1 ? end - file : size - 1;
    strncpy(out, file, terminatorPos);
    out[terminatorPos] = '\0';
}

void string_escape_file_name(char* out, const char* file, size_t size) {
    if(size == 0) {
        return;
    }

    // Characters FAT file names cannot hold become '_'; the result is always terminated.
    size_t i = 0;
    for(; i < size - 1 && file[i] != '\0'; i++) {
        unsigned char c = (unsigned char) file[i];
        out[i] = c < 0x20 || c == 0x7F || strchr("<>:\"/\\|?*", c) != NULL ? '_' : (char) c;
    }

    out[i] = '\0';
}

void string_get_path_file(char* out, const char* path, u32 size) {
    const char* start = NULL;
    const char* end = NULL;
    const char* curr = path - 1;
    while((curr = strchr(curr + 1, '/')) != NULL) {
        start = end != NULL ? end : path;
        end = curr;
    }

    if(end != path + strlen(path) - 1) {
        start = end;
        end = path + strlen(path);
    }

    if(end - start == 0) {
        strncpy(out, "/", size);
    } else {
        u32 terminatorPos = end - start - 1 < size - 1 ? end - start - 1 : size - 1;
        strncpy(out, start + 1, terminatorPos);
        out[terminatorPos] = '\0';
    }
}

void string_get_parent_path(char* out, const char* path, u32 size) {
    size_t pathLen = strlen(path);

    const char* start = NULL;
    const char* end = NULL;
    const char* curr = path - 1;
    while((curr = strchr(curr + 1, '/')) != NULL && (start == NULL || curr != path + pathLen - 1)) {
        start = end != NULL ? end : path;
        end = curr;
    }

    u32 terminatorPos = end - path + 1 < size - 1 ? end - path + 1 : size - 1;
    strncpy(out, path, terminatorPos);
    out[terminatorPos] = '\0';
}