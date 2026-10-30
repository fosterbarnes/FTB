#pragma once

#include "ciainstall.h"
#include "ticketinstall.h"
#include "../../core/fs.h"

// Destinations for streamed items, shared by URL installs (installurl.c) and Web Transfer
// (webtransfer/webserver.c): install by content, or save the bytes to an SD file. Worker thread only.
// Items are written with FSFILE_Write on the returned handle.

typedef enum {
    CONTENT_CIA,
    CONTENT_TICKET,
    CONTENT_FILE
} content_type;

typedef struct {
    content_type type;
    cia_install cia;
    ticket_install ticket;
    char path[FILE_PATH_MAX]; // CONTENT_FILE: the file being written
    bool part; // CONTENT_FILE: written to "<path>.part" and moved into place on success
} content_install;

// Picks the destination from the item's first block: CIA and ticket install into AM; 3DSX and SMDH are
// saved to dest3dsx, or to /3ds/<name>/<name>.<ext> named after fileName when dest3dsx is empty.
// Anything else is R_APP_BAD_DATA; R_APP_SKIPPED when a prompt is declined.
Result content_install_begin(content_install* content, const char* fileName, const char* dest3dsx, void* block, u32 blockSize, void* data,
                             void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), u32* handle);
// Saves the bytes as they are to the SD path (missing folders are made) through "<path>.part", so a
// cut transfer never leaves a short file under the real name. R_APP_SKIPPED when path exists and
// replace is false.
Result content_save_begin(content_install* content, const char* path, bool replace, u32* handle);
// Finishes what a begin started, or undoes it when succeeded is false.
Result content_end(content_install* content, data_op_data* op, bool succeeded, u32 handle);
