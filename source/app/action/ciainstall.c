#include <stdio.h>

#include <3ds.h>

#include "ciainstall.h"
#include "../resources.h"
#include "../safety.h"
#include "../../core/core.h"

Result cia_install_begin(cia_install* install, void* block, u32 blockSize, void* data,
                         void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), u32* handle) {
    install->titleId = 0;
    install->seedLookup = cia_get_seed_state((u8*) block, blockSize) != CIA_SEED_NO;

    u64 titleId = 0;
    Result res = 0;
    if(R_FAILED(res = cia_get_title_id(&titleId, (u8*) block, blockSize)) || R_FAILED(res = safety_confirm_install(titleId, data, drawTop))) {
        return res;
    }

    bool n3ds = false;
    if(R_SUCCEEDED(APT_CheckNew3DS(&n3ds)) && !n3ds && ((titleId >> 28) & 0xF) == 2
       && !prompt_wait_yes_no("Confirmation", "Title is intended for New 3DS systems.\nContinue?", COLOR_TEXT, data, drawTop)) {
        return R_APP_SKIPPED;
    }

    FS_MediaType dest = fs_get_title_destination(titleId);

    AM_TitleEntry installed;
    if(install->askInstalled && R_SUCCEEDED(AM_GetTitleInfo(dest, 1, &titleId, &installed))) {
        char version[16];
        ui_format_version(version, sizeof(version), installed.version);

        // Reinstalling deletes the title first (below); whether its save data survives that is not
        // confirmed on hardware, hence "may".
        char text[192];
        snprintf(text, sizeof(text),
                 "%016llX (v%s)\n"
                 "is already installed. Reinstalling\n"
                 "deletes it first; its save data may\n"
                 "be deleted too.\n"
                 "\n"
                 "Reinstall?",
                 titleId, version);

        if(!prompt_wait_yes_no("Confirmation", text, COLOR_WARNING, data, drawTop)) {
            return R_APP_SKIPPED;
        }
    }

    // Deleting the running app before it reinstalls itself causes issues.
    u64 currTitleId = 0;
    u8 currMediaType = MEDIATYPE_NAND;

    if(envIsHomebrew() || R_FAILED(APT_GetAppletInfo((NS_APPID) envGetAptAppId(), &currTitleId, &currMediaType, NULL, NULL, NULL)) || titleId != currTitleId || dest != currMediaType) {
        AM_DeleteTitle(dest, titleId);
        AM_DeleteTicket(titleId);

        if(dest == MEDIATYPE_SD) {
            AM_QueryAvailableExternalTitleDatabase(NULL);
        }
    }

    if(R_SUCCEEDED(res = AM_StartCiaInstall(dest, handle))) {
        install->titleId = titleId;
    }

    return res;
}

Result cia_install_end(const cia_install* install, data_op_data* op, bool succeeded, u32 handle) {
    if(!succeeded) {
        return AM_CancelCIAInstall(handle);
    }

    op->stage = "Finishing install";

    Result res = 0;
    if(R_FAILED(res = AM_FinishCiaInstall(handle))) {
        return res;
    }

    if(install->seedLookup) {
        op->stage = "Importing seed";
        http_download_seed(install->titleId);
    }

    if(safety_is_native_firm(install->titleId)) {
        op->stage = "Installing firmware";
        res = AM_InstallFirm(install->titleId);
    }

    return res;
}
