#pragma once

// CIA install steps shared by SD installs (installcias.c) and URL installs (installurl.c), so both
// pass the same System changes lock, New 3DS prompt, and finishing steps. Worker thread only.

typedef struct data_op_data_s data_op_data;
typedef struct ui_view_s ui_view;

typedef struct {
    u64 titleId;
    // Seeded titles (or unreadable CIAs) need the seed lookup, which can mean a server request.
    bool seedLookup;
    // Set by the caller: ask (red) before reinstalling a title that is already installed. Off is
    // FBI's behavior; Web Transfer links set it because their page cannot check them first.
    bool askInstalled;
    // Set by the caller: install New 3DS titles on an Old 3DS without asking (trusted Web Transfer
    // senders).
    bool allowNew3ds;
} cia_install;

// From the CIA's first block: reads the title ID, checks the lock (red confirm for system titles),
// asks before a New 3DS title on an Old 3DS (and before a reinstall with askInstalled), removes the
// old title, and starts the AM install. R_APP_SKIPPED when the user declines.
Result cia_install_begin(cia_install* install, void* block, u32 blockSize, void* data,
                         void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), u32* handle);
// Finishes the install cia_install_begin started (seed import, NATIVE_FIRM install) or cancels it.
Result cia_install_end(const cia_install* install, data_op_data* op, bool succeeded, u32 handle);
