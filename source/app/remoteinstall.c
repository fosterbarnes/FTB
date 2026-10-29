#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "paths.h"
#include "resources.h"
#include "screens.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../libs/quirc/quirc.h"

static bool remoteinstall_get_last_urls(char* out, size_t size) {
    if(out == NULL || size == 0) {
        return false;
    }

    Handle file = 0;
    if(R_FAILED(FSUSER_OpenFileDirectly(&file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, PATHS_FS_LAST_URLS), FS_OPEN_READ, 0))) {
        return false;
    }

    u32 bytesRead = 0;
    FSFILE_Read(file, &bytesRead, 0, out, size - 1);
    out[bytesRead] = '\0';

    FSFILE_Close(file);

    return bytesRead != 0;
}

static Result remoteinstall_set_last_urls(const char* urls) {
    Result res = 0;

    FS_Archive sdmcArchive = 0;
    if(R_SUCCEEDED(res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        FS_Path path = fsMakePath(PATH_ASCII, PATHS_FS_LAST_URLS);

        Handle file = 0;
        if(R_SUCCEEDED(FSUSER_OpenFile(&file, sdmcArchive, path, FS_OPEN_READ, 0))) {
            FSFILE_Close(file);

            res = FSUSER_DeleteFile(sdmcArchive, path);
        }

        if(urls != NULL && strlen(urls) != 0
           && R_SUCCEEDED(res)
           && R_SUCCEEDED(res = fs_ensure_dir(sdmcArchive, PATHS_FS_DATA_DIR))
           && R_SUCCEEDED(res = FSUSER_OpenFile(&file, sdmcArchive, path, FS_OPEN_WRITE | FS_OPEN_CREATE, 0))) {
            u32 bytesWritten = 0;
            res = FSFILE_Write(file, &bytesWritten, 0, urls, strlen(urls), FS_WRITE_FLUSH | FS_WRITE_UPDATE_TIME);

            Result closeRes = FSFILE_Close(file);
            if(R_SUCCEEDED(res)) {
                res = closeRes;
            }
        }

        Result closeRes = FSUSER_CloseArchive(sdmcArchive);
        if(R_SUCCEEDED(res)) {
            res = closeRes;
        }
    }

    return res;
}

#define QR_IMAGE_WIDTH 400
#define QR_IMAGE_HEIGHT 240

typedef struct {
    struct quirc* qrContext;
    u32 tex;

    bool capturing;
    capture_cam_data captureInfo;
} remoteinstall_qr_data;

static void remoteinstall_qr_stop_capture(remoteinstall_qr_data* data) {
    task_cancel_and_wait(&data->captureInfo.state);

    data->capturing = false;

    if(data->captureInfo.buffer != NULL) {
        memset(data->captureInfo.buffer, 0, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16));
    }
}

static void remoteinstall_qr_free_data(remoteinstall_qr_data* data) {
    remoteinstall_qr_stop_capture(data);

    if(data->captureInfo.buffer != NULL) {
        free(data->captureInfo.buffer);
        data->captureInfo.buffer = NULL;
    }

    if(data->tex != 0) {
        screen_unload_texture(data->tex);
        data->tex = 0;
    }

    if(data->qrContext != NULL) {
        quirc_destroy(data->qrContext);
        data->qrContext = NULL;
    }

    free(data);
}

static void remoteinstall_qr_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    remoteinstall_qr_data* installData = (remoteinstall_qr_data*) data;

    if(installData->tex != 0) {
        LightLock_Lock(&installData->captureInfo.lock);
        screen_load_texture_untiled(installData->tex, installData->captureInfo.buffer, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16), QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT, GPU_RGB565, false);
        LightLock_Unlock(&installData->captureInfo.lock);

        screen_draw_texture(installData->tex, 0, 0, QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT);
    }
}

static void remoteinstall_qr_update(ui_view* view, void* data, float* progress, char* text) {
    remoteinstall_qr_data* installData = (remoteinstall_qr_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);

        remoteinstall_qr_free_data(installData);

        return;
    }

    if(hidKeysDown() & KEY_X) {
        remoteinstall_qr_stop_capture(installData);

        installData->captureInfo.camera = installData->captureInfo.camera == CAMERA_OUTER ? CAMERA_INNER : CAMERA_OUTER;

        return;
    }

    if(!installData->capturing) {
        Result capRes = task_capture_cam(&installData->captureInfo);
        if(R_FAILED(capRes)) {
            ui_pop();
            info_destroy(view);

            error_display_res(NULL, NULL, capRes, "Failed to start camera capture.");

            remoteinstall_qr_free_data(installData);
            return;
        } else {
            installData->capturing = true;
        }
    }

    if(task_is_finished(&installData->captureInfo.state)) {
        ui_pop();
        info_destroy(view);

        if(R_FAILED(installData->captureInfo.state.result)) {
            error_display_res(NULL, NULL, installData->captureInfo.state.result, "Error while capturing camera frames.");
        }

        remoteinstall_qr_free_data(installData);

        return;
    }

    int w = 0;
    int h = 0;
    uint8_t* qrBuf = quirc_begin(installData->qrContext, &w, &h);

    LightLock_Lock(&installData->captureInfo.lock);

    for(int x = 0; x < w; x++) {
        for(int y = 0; y < h; y++) {
            u16 px = installData->captureInfo.buffer[y * QR_IMAGE_WIDTH + x];
            qrBuf[y * w + x] = (u8) (((((px >> 11) & 0x1F) << 3) + (((px >> 5) & 0x3F) << 2) + ((px & 0x1F) << 3)) / 3);
        }
    }

    LightLock_Unlock(&installData->captureInfo.lock);

    quirc_end(installData->qrContext);

    int qrCount = quirc_count(installData->qrContext);
    for(int i = 0; i < qrCount; i++) {
        struct quirc_code qrCode;
        quirc_extract(installData->qrContext, i, &qrCode);

        struct quirc_data qrData;
        quirc_decode_error_t err = quirc_decode(&qrCode, &qrData);

        if(err == 0) {
            remoteinstall_qr_stop_capture(installData);

            remoteinstall_set_last_urls((const char*) qrData.payload);

            action_install_url("Install from the scanned QR code?", (const char*) qrData.payload, NULL, NULL, NULL, NULL, NULL);
            return;
        }
    }

    snprintf(text, PROGRESS_TEXT_MAX, "Waiting for QR code...");
}

static void remoteinstall_scan_qr_code() {
    remoteinstall_qr_data* data = (remoteinstall_qr_data*) calloc(1, sizeof(remoteinstall_qr_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate QR install data.");

        return;
    }

    data->tex = 0;

    data->capturing = false;

    data->captureInfo.width = QR_IMAGE_WIDTH;
    data->captureInfo.height = QR_IMAGE_HEIGHT;

    data->captureInfo.camera = CAMERA_OUTER;

    LightLock_Init(&data->captureInfo.lock);
    data->captureInfo.state.finished = true;

    data->qrContext = quirc_new();
    if(data->qrContext == NULL) {
        error_display(NULL, NULL, "Failed to create QR context.");

        remoteinstall_qr_free_data(data);
        return;
    }

    if(quirc_resize(data->qrContext, QR_IMAGE_WIDTH, QR_IMAGE_HEIGHT) != 0) {
        error_display(NULL, NULL, "Failed to resize QR context.");

        remoteinstall_qr_free_data(data);
        return;
    }

    data->captureInfo.buffer = (u16*) calloc(1, QR_IMAGE_WIDTH * QR_IMAGE_HEIGHT * sizeof(u16));
    if(data->captureInfo.buffer == NULL) {
        error_display(NULL, NULL, "Failed to create image buffer.");

        remoteinstall_qr_free_data(data);
        return;
    }

    data->tex = screen_allocate_free_texture();

    info_display("QR Code Install", "B: Return, X: Switch Camera", false, data, remoteinstall_qr_update, remoteinstall_qr_draw_top);
}

static void remoteinstall_manually_enter_urls_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button == SWKBD_BUTTON_CONFIRM) {
        remoteinstall_set_last_urls(response);

        action_install_url("Install from the entered URL(s)?", response, NULL, NULL, NULL, NULL, NULL);
    }
}

static void remoteinstall_manually_enter_urls() {
    kbd_display("Enter URL(s)", "", SWKBD_TYPE_NORMAL, SWKBD_MULTILINE, SWKBD_NOTEMPTY_NOTBLANK, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX, NULL, remoteinstall_manually_enter_urls_onresponse);
}

static void remoteinstall_repeat_last_request() {
    char* textBuf = (char*) calloc(1, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX);
    if(textBuf != NULL) {
        if(remoteinstall_get_last_urls(textBuf, DOWNLOAD_URL_MAX * INSTALL_URLS_MAX)) {
            action_install_url("Install from the last requested URL(s)?", textBuf, NULL, NULL, NULL, NULL, NULL);
        } else {
            prompt_display_notify("Failure", "No previously requested URL(s) could be found.", COLOR_TEXT, NULL, NULL, NULL);
        }

        free(textBuf);
    } else {
        error_display_res(NULL, NULL, R_APP_OUT_OF_MEMORY, "Failed to allocate URL text buffer.");
    }
}

static void remoteinstall_forget_last_request() {
    Result forgetRes = remoteinstall_set_last_urls(NULL);
    if(R_SUCCEEDED(forgetRes)) {
        prompt_display_notify("Success", "Last requested URL(s) forgotten.", COLOR_TEXT, NULL, NULL, NULL);
    } else {
        error_display_res(NULL, NULL, forgetRes, "Failed to forget last requested URL(s).");
    }
}

static list_item scan_qr_code = {.name = "Scan QR Code", .color = COLOR_TEXT, .data = remoteinstall_scan_qr_code};
static list_item manually_enter_urls = {.name = "Manually enter URLs", .color = COLOR_TEXT, .data = remoteinstall_manually_enter_urls};
static list_item repeat_last_request = {.name = "Repeat last request", .color = COLOR_TEXT, .data = remoteinstall_repeat_last_request};
static list_item forget_last_request = {.name = "Forget last request", .color = COLOR_TEXT, .data = remoteinstall_forget_last_request};

static void remoteinstall_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && (selectedTouched || hidKeysDown() & KEY_A) && selected->data != NULL) {
        ((void(*)()) selected->data)();
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &scan_qr_code);
        linked_list_add(items, &manually_enter_urls);
        linked_list_add(items, &repeat_last_request);
        linked_list_add(items, &forget_last_request);
    }
}

void remoteinstall_open() {
    list_display("Remote Install", "A: Select, B: Return", NULL, remoteinstall_update, NULL);
}
