#pragma once

// Folders the CIA Installer scans (subfolders included), kept in sdmc:/ftb/ciaFolders.cfg as one
// absolute SD path per line. Stored paths always end in '/'.
#define CIA_FOLDERS_MAX 8

void ciafolders_load();
u32 ciafolders_count();
const char* ciafolders_get(u32 index);
// Bumped on every change, so a view can tell when to rescan.
u32 ciafolders_version();
// NULL when added, else why not. A failed save still adds the folder for this session.
const char* ciafolders_add(const char* path);
// False when saving failed (the folder is still removed for this session).
bool ciafolders_remove(const char* path);
