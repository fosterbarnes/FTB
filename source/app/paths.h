#pragma once

#include <sys/stat.h>

// Everything FTB keeps on the SD card. "sdmc:" paths are for stdio; the others are for FS calls on the
// SD archive. ftpd keeps its own FTPDCONFIG ("/ftb/ftpd.cfg", source/ftpd/platform.h).
#define PATHS_DATA_DIR "sdmc:/ftb"
#define PATHS_SETTINGS PATHS_DATA_DIR "/settings.cfg"
#define PATHS_CIA_FOLDERS PATHS_DATA_DIR "/ciaFolders.cfg"
#define PATHS_TITLE_CACHE_DIR PATHS_DATA_DIR "/cache"
#define PATHS_THEME_DIR PATHS_DATA_DIR "/theme/"
#define PATHS_FS_DATA_DIR "/ftb/"
#define PATHS_FS_LAST_URLS PATHS_FS_DATA_DIR "lasturls"
#define PATHS_FS_NAND_DIR PATHS_FS_DATA_DIR "nand/"

// Creates sdmc:/ftb when it is missing (stdio writers call this first).
static inline void paths_ensure_data_dir() {
    mkdir(PATHS_DATA_DIR, 0777);
}
