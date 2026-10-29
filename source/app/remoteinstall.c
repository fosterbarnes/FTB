#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <3ds.h>

#include "paths.h"
#include "resources.h"
#include "screens.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"
#include "../libs/quirc/quirc_internal.h"

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

// The transfer runs on the UI thread: a sender that stops for this long is dropped.
#define REMOTEINSTALL_STALL_MS 10000
#define REMOTEINSTALL_POLL_MS 100

typedef struct {
    // -1 = not open.
    int serverSocket;
    int clientSocket;
} remoteinstall_network_data;

// Moves all len bytes, polling so B (errno ECANCELED) and a stalled sender (ETIMEDOUT) end it
// instead of freezing FTB. Returns len, or -1 with errno set.
static int remoteinstall_network_transfer(int sockfd, void* buf, size_t len, bool receive) {
    size_t done = 0;
    u64 lastProgress = osGetTime();
    while(done < len) {
        hidScanInput();
        if(hidKeysDown() & KEY_B) {
            errno = ECANCELED;
            return -1;
        }

        if(osGetTime() - lastProgress >= REMOTEINSTALL_STALL_MS) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct pollfd pfd = {sockfd, receive ? POLLIN : POLLOUT, 0};
        int ready = poll(&pfd, 1, REMOTEINSTALL_POLL_MS);
        if(ready < 0) {
            return -1;
        }

        if(ready == 0) {
            continue;
        }

        int ret = receive ? recv(sockfd, (u8*) buf + done, len - done, 0) : send(sockfd, (u8*) buf + done, len - done, 0);
        if(ret < 0) {
            if(errno == EAGAIN) {
                continue;
            }

            return -1;
        }

        // The sender closed the connection early.
        if(ret == 0) {
            errno = ECONNRESET;
            return -1;
        }

        done += ret;
        lastProgress = osGetTime();
    }

    return (int) done;
}

static void remoteinstall_network_close_client(void* data) {
    remoteinstall_network_data* networkData = (remoteinstall_network_data*) data;

    if(networkData->clientSocket >= 0) {
        u8 ack = 0;
        remoteinstall_network_transfer(networkData->clientSocket, &ack, sizeof(ack), false);

        close(networkData->clientSocket);
        networkData->clientSocket = -1;
    }
}

static void remoteinstall_network_free_data(remoteinstall_network_data* data) {
    remoteinstall_network_close_client(data);

    if(data->serverSocket >= 0) {
        close(data->serverSocket);
        data->serverSocket = -1;
    }

    free(data);
}

// Shows why a receive failed; pressing B is not an error.
static void remoteinstall_network_receive_failed(const char* text) {
    if(errno != ECANCELED) {
        error_display_errno(NULL, NULL, errno, "%s", text);
    }
}

static void remoteinstall_network_update(ui_view* view, void* data, float* progress, char* text) {
    remoteinstall_network_data* networkData = (remoteinstall_network_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        info_destroy(view);

        remoteinstall_network_free_data(networkData);

        return;
    }

    struct sockaddr_in client;
    socklen_t clientLen = sizeof(client);

    int sock = accept(networkData->serverSocket, (struct sockaddr*) &client, &clientLen);
    int acceptErr = errno;
    if(sock >= 0) {
        networkData->clientSocket = sock;

        u32 size = 0;
        if(remoteinstall_network_transfer(networkData->clientSocket, &size, sizeof(size), true) != sizeof(size)) {
            remoteinstall_network_receive_failed("Failed to read payload length.");

            remoteinstall_network_close_client(data);
            return;
        }

        size = ntohl(size);
        if(size >= DOWNLOAD_URL_MAX * INSTALL_URLS_MAX) {
            error_display(NULL, NULL, "Payload too large.");

            remoteinstall_network_close_client(data);
            return;
        }

        char* urls = (char*) calloc(size + 1, sizeof(char));
        if(urls == NULL) {
            error_display(NULL, NULL, "Failed to allocate URL buffer.");

            remoteinstall_network_close_client(data);
            return;
        }

        if(remoteinstall_network_transfer(networkData->clientSocket, urls, size, true) != size) {
            remoteinstall_network_receive_failed("Failed to read URL(s).");

            free(urls);
            remoteinstall_network_close_client(data);
            return;
        }

        remoteinstall_set_last_urls(urls);
        action_install_url("Install from the received URL(s)?", urls, NULL, data, NULL, remoteinstall_network_close_client, NULL);

        free(urls);
    } else if(acceptErr != EAGAIN) {
        // The network went away (Wi-Fi off or lost): waiting longer cannot help.
        bool networkGone = acceptErr == EINVAL || acceptErr == ENETDOWN;
        if(networkGone) {
            ui_pop();
            info_destroy(view);
        }

        error_display_errno(NULL, NULL, acceptErr, "Failed to open socket.");

        if(networkGone) {
            remoteinstall_network_free_data(networkData);

            return;
        }
    }

    struct in_addr addr = {(in_addr_t) gethostid()};
    snprintf(text, PROGRESS_TEXT_MAX, "Waiting for connection...\nIP: %s\nPort: 5000", inet_ntoa(addr));
}

static void remoteinstall_receive_urls_network() {
    remoteinstall_network_data* data = (remoteinstall_network_data*) calloc(1, sizeof(remoteinstall_network_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate network install data.");

        return;
    }

    data->serverSocket = -1;
    data->clientSocket = -1;

    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if(sock < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to open server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    data->serverSocket = sock;

    struct sockaddr_in server;
    server.sin_family = AF_INET;
    server.sin_port = htons(5000);
    server.sin_addr.s_addr = (in_addr_t) gethostid();

    if(bind(data->serverSocket, (struct sockaddr*) &server, sizeof(server)) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to bind server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    fcntl(data->serverSocket, F_SETFL, fcntl(data->serverSocket, F_GETFL, 0) | O_NONBLOCK);

    if(listen(data->serverSocket, 5) < 0) {
        error_display_errno(NULL, NULL, errno, "Failed to listen on server socket.");

        remoteinstall_network_free_data(data);
        return;
    }

    info_display("Receive URL(s)", "B: Return", false, data, remoteinstall_network_update, NULL);
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

static list_item receive_urls_network = {"Receive URLs over the network", COLOR_TEXT, remoteinstall_receive_urls_network};
static list_item scan_qr_code = {"Scan QR Code", COLOR_TEXT, remoteinstall_scan_qr_code};
static list_item manually_enter_urls = {"Manually enter URLs", COLOR_TEXT, remoteinstall_manually_enter_urls};
static list_item repeat_last_request = {"Repeat last request", COLOR_TEXT, remoteinstall_repeat_last_request};
static list_item forget_last_request = {"Forget last request", COLOR_TEXT, remoteinstall_forget_last_request};

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
        linked_list_add(items, &receive_urls_network);
        linked_list_add(items, &scan_qr_code);
        linked_list_add(items, &manually_enter_urls);
        linked_list_add(items, &repeat_last_request);
        linked_list_add(items, &forget_last_request);
    }
}

void remoteinstall_open() {
    list_display("Remote Install", "A: Select, B: Return", NULL, remoteinstall_update, NULL);
}
