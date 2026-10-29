#pragma once

bool string_is_empty(const char* str);
void string_copy(char* dst, const char* src, size_t size);
// Appends text at *pos and advances *pos; stops at the end of out, which stays terminated.
void string_append(char* out, size_t size, size_t* pos, const char* text);

void string_get_file_name(char* out, const char* file, u32 size);
void string_escape_file_name(char* out, const char* file, size_t size);
void string_get_path_file(char* out, const char* path, u32 size);
void string_get_parent_path(char* out, const char* path, u32 size);