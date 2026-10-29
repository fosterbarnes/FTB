#pragma once

// Ticket install steps shared by SD installs (installtickets.c) and URL installs (installurl.c).
// The title's current tickets are deleted before the new one installs (FBI order), so they are
// exported first and installed again when the new ticket does not finish. Worker thread only.

typedef struct {
    u8* data;
    u32 size;
} ticket_copy;

typedef struct {
    u64 titleId;
    ticket_copy* copies;
    u32 count;
} ticket_install;

// From the ticket's first block: reads the title ID, checks the System changes lock, backs up and
// deletes the title's tickets, and starts the AM install. A failed start restores the backup.
Result ticket_install_begin(ticket_install* install, void* block, u32 blockSize, u32* handle);
// Finishes or aborts the install ticket_install_begin started; restores the backup unless the new
// ticket finished. Frees the backup either way.
Result ticket_install_end(ticket_install* install, bool succeeded, u32 handle);
