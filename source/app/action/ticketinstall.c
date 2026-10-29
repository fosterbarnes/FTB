#include <malloc.h>

#include <3ds.h>

#include "ticketinstall.h"
#include "../safety.h"
#include "../../core/core.h"

// Most tickets are 0x350 bytes; large DLC content indexes need more, which AM reports back.
#define TICKET_EXPORT_SIZE 0x1000

static void ticket_install_free(ticket_install* install) {
    for(u32 i = 0; i < install->count; i++) {
        free(install->copies[i].data);
    }

    free(install->copies);
    install->copies = NULL;
    install->count = 0;
}

static Result ticket_install_export(ticket_copy* copy, u64 titleId, u64 ticketId) {
    u32 size = TICKET_EXPORT_SIZE;

    // A second try with the size AM reported covers tickets bigger than the first buffer.
    for(u32 attempt = 0; attempt < 2; attempt++) {
        u8* data = (u8*) malloc(size);
        if(data == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }

        u32 actualSize = 0;
        Result res = AMNET_ExportLicenseTicket(&actualSize, data, size, titleId, ticketId);
        if(R_SUCCEEDED(res) && actualSize > 0 && actualSize <= size) {
            copy->data = data;
            copy->size = actualSize;
            return 0;
        }

        free(data);

        if(R_FAILED(res)) {
            return res;
        }

        if(actualSize == 0) {
            break;
        }

        size = actualSize;
    }

    return R_APP_BAD_DATA;
}

// The AM ticket list is the proven way to see whether the title has a ticket at all.
static Result ticket_install_has_ticket(bool* installed, u64 titleId) {
    *installed = false;

    u32 count = 0;
    Result res = 0;
    if(R_FAILED(res = AM_GetTicketCount(&count)) || count == 0) {
        return res;
    }

    u64* titleIds = (u64*) calloc(count, sizeof(u64));
    if(titleIds == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    if(R_SUCCEEDED(res = AM_GetTicketList(&count, count, 0, titleIds))) {
        for(u32 i = 0; i < count && !*installed; i++) {
            *installed = titleIds[i] == titleId;
        }
    }

    free(titleIds);
    return res;
}

static Result ticket_install_backup(ticket_install* install) {
    bool installed = false;
    Result res = 0;
    if(R_FAILED(res = ticket_install_has_ticket(&installed, install->titleId)) || !installed) {
        return res;
    }

    // An installed ticket that cannot be exported is not deleted: the install fails instead.
    u32 count = 0;
    if(R_FAILED(res = AMNET_GetTitleNumTicketIds(&count, install->titleId))) {
        return res;
    }

    if(count == 0) {
        return R_APP_BAD_DATA;
    }

    u64* ticketIds = (u64*) calloc(count, sizeof(u64));
    install->copies = (ticket_copy*) calloc(count, sizeof(ticket_copy));
    if(ticketIds == NULL || install->copies == NULL) {
        res = R_APP_OUT_OF_MEMORY;
    } else if(R_SUCCEEDED(res = AMNET_GetTitleTicketIds(&count, ticketIds, count, install->titleId, false))) {
        for(u32 i = 0; i < count && R_SUCCEEDED(res); i++) {
            if(R_SUCCEEDED(res = ticket_install_export(&install->copies[install->count], install->titleId, ticketIds[i]))) {
                install->count++;
            }
        }

        if(R_SUCCEEDED(res) && install->count == 0) {
            res = R_APP_BAD_DATA;
        }
    }

    free(ticketIds);

    if(R_FAILED(res)) {
        ticket_install_free(install);
    }

    return res;
}

// Best effort: the failed install's own result is what the user sees.
static void ticket_install_restore(ticket_install* install) {
    for(u32 i = 0; i < install->count; i++) {
        ticket_copy* copy = &install->copies[i];

        Handle handle = 0;
        if(R_FAILED(AMNET_InstallTicketBegin(&handle))) {
            continue;
        }

        u32 bytesWritten = 0;
        if(R_SUCCEEDED(FSFILE_Write(handle, &bytesWritten, 0, copy->data, copy->size, 0)) && bytesWritten == copy->size) {
            AMNET_InstallTicketFinish(handle);
        } else {
            AMNET_InstallTicketAbort(handle);
        }
    }
}

Result ticket_install_begin(ticket_install* install, void* block, u32 blockSize, u32* handle) {
    install->titleId = 0;
    install->copies = NULL;
    install->count = 0;

    Result res = 0;
    if(R_FAILED(res = ticket_get_title_id(&install->titleId, (u8*) block, blockSize))
       || R_FAILED(res = safety_check_title(install->titleId))
       || R_FAILED(res = ticket_install_backup(install))) {
        return res;
    }

    AM_DeleteTicket(install->titleId);

    if(R_FAILED(res = AMNET_InstallTicketBegin(handle))) {
        ticket_install_restore(install);
        ticket_install_free(install);
    }

    return res;
}

Result ticket_install_end(ticket_install* install, bool succeeded, u32 handle) {
    Result res = succeeded ? AMNET_InstallTicketFinish(handle) : AMNET_InstallTicketAbort(handle);
    if(!succeeded || R_FAILED(res)) {
        ticket_install_restore(install);
    }

    ticket_install_free(install);
    return res;
}
