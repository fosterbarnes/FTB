#include <ctype.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "resources.h"
#include "screens.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

#define UPDATE_VERSION(major, minor, micro) (((u64) (major) << 32) | ((u64) (minor) << 16) | (u64) (micro))

static bool update_installed = false;

// "v<major>.<minor>.<micro>" (the v optional), packed by UPDATE_VERSION; false when malformed.
static bool update_parse_version(const char* tag, u64* version) {
    if(*tag == 'v') {
        tag++;
    }

    u64 packed = 0;
    for(u32 i = 0; i < 3; i++) {
        if(!isdigit((unsigned char) *tag)) {
            return false;
        }

        char* end = NULL;
        unsigned long part = strtoul(tag, &end, 10);
        if(part > 0xFFFF || *end != (i < 2 ? '.' : '\0')) {
            return false;
        }

        packed = (packed << 16) | part;
        tag = i < 2 ? end + 1 : end;
    }

    *version = packed;
    return true;
}

static void update_finished_url(void* data, u32 index) {
    update_installed = true;
}

static void update_finished_all(void* data) {
    if(!update_installed) {
        return;
    }

    update_installed = false;

    if(R_FAILED(app_restart())) {
        prompt_display_notify("Success", "Update installed.\nRestart FTB to finish.", COLOR_TEXT, NULL, NULL, NULL);
    }
}

typedef struct {
    task_state state;

    // Set when a newer release is found; url stays empty when it has no asset for this build.
    u64 latestVersion;
    char url[DOWNLOAD_URL_MAX];
} update_check_data;

// Prompts keep the text pointer, so the notice lives here.
static char update_notice[128];

// Why this copy of FTB cannot replace itself, or NULL. A game card would relaunch the old card
// after an SD install; a 3dsx can only be updated and relaunched from its SD path
// (fs_set_3dsx_path strips "sdmc:", so a leading '/' is an SD path).
static const char* update_blocked_reason() {
    if(envIsHomebrew()) {
        const char* path = fs_get_3dsx_path();
        return path == NULL || path[0] != '/' ? "Launch FTB.3dsx from the SD card\nto update it." : NULL;
    }

    u64 titleId = 0;
    u8 mediaType = MEDIATYPE_SD;
    if(R_SUCCEEDED(APT_GetAppletInfo((NS_APPID) envGetAptAppId(), &titleId, &mediaType, NULL, NULL, NULL)) && mediaType == MEDIATYPE_GAME_CARD) {
        return "FTB is running from a game card:\ninstall FTB.cia or update the card\nmanually.";
    }

    return NULL;
}

static Result update_check_thread(void* arg) {
    update_check_data* data = (update_check_data*) arg;

    Result res = 0;

    json_t* json = NULL;
    if(R_SUCCEEDED(res = http_download_json("https://api.github.com/repos/fosterbarnes/ftb/releases/latest", &json, 64 * 1024))) {
        if(json_is_object(json)) {
            // Releases are tagged v<major>.<minor>.<micro> (.version/version); the title is free text.
            json_t* tagName = json_object_get(json, "tag_name");
            json_t* assets = json_object_get(json, "assets");

            u64 latestVersion = 0;
            if(json_is_string(tagName) && json_is_array(assets) && update_parse_version(json_string_value(tagName), &latestVersion)) {
                // Only newer versions: AM refuses to install an older title version.
                if(latestVersion > UPDATE_VERSION(VERSION_MAJOR, VERSION_MINOR, VERSION_MICRO)) {
                    data->latestVersion = latestVersion;

                    for(u32 i = 0; i < json_array_size(assets); i++) {
                        json_t* val = json_array_get(assets, i);
                        if(json_is_object(val)) {
                            json_t* assetName = json_object_get(val, "name");
                            json_t* assetUrl = json_object_get(val, "browser_download_url");

                            if(json_is_string(assetName) && json_is_string(assetUrl)) {
                                if(strcmp(json_string_value(assetName), fs_get_3dsx_path() != NULL ? "FTB.3dsx" : "FTB.cia") == 0) {
                                    string_copy(data->url, json_string_value(assetUrl), DOWNLOAD_URL_MAX);
                                    break;
                                }
                            }
                        }
                    }
                }
            } else {
                res = R_APP_BAD_DATA;
            }
        } else {
            res = R_APP_BAD_DATA;
        }

        json_decref(json);
    }

    return res;
}

static void update_check_update(ui_view* view, void* data, float* progress, char* text) {
    update_check_data* checkData = (update_check_data*) data;

    if(!task_is_finished(&checkData->state)) {
        // The request itself cannot be interrupted; it ends within the HTTP timeout.
        if(hidKeysDown() & KEY_B) {
            task_cancel(&checkData->state);
            string_copy(text, "Cancelling...", PROGRESS_TEXT_MAX);
        }

        return;
    }

    ui_pop();
    info_destroy(view);

    Result res = checkData->state.result;
    if(checkData->state.cancelled) {
        // Cancelled with B: nothing to report.
    } else if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to check for update.");
    } else if(checkData->latestVersion == 0) {
        prompt_display_notify("Success", "No updates available.", COLOR_TEXT, NULL, NULL, NULL);
    } else {
        const char* blocked = update_blocked_reason();
        if(blocked != NULL) {
            u64 version = checkData->latestVersion;
            snprintf(update_notice, sizeof(update_notice), "v%u.%u.%u is available.\n%s",
                     (unsigned int) (version >> 32), (unsigned int) (version >> 16 & 0xFFFF), (unsigned int) (version & 0xFFFF), blocked);
            prompt_display_notify("Update Available", update_notice, COLOR_TEXT, NULL, NULL, NULL);
        } else if(checkData->url[0] == '\0') {
            error_display_res(NULL, NULL, R_APP_BAD_DATA, "Failed to check for update.");
        } else {
            action_install_url("Update FTB to the latest version?", checkData->url, fs_get_3dsx_path(), NULL, update_finished_url, update_finished_all, NULL);
        }
    }

    free(checkData);
}

void update_open() {
    update_check_data* data = (update_check_data*) calloc(1, sizeof(update_check_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate update check data.");
        return;
    }

    Result res = task_start(&data->state, update_check_thread, data, 0x18, 1);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to check for update.");
        free(data);
        return;
    }

    info_display("Checking For Updates", "Press B to cancel.", false, data, update_check_update, NULL);
}
