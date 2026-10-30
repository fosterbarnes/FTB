#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "receiver.h"
#include "../resources.h"
#include "../screens.h"
#include "../../core/core.h"
#include "../../libs/qrcodegen/qrcodegen.h"

// Receive screen: the worker (receiver.c) runs while this view is open. Bottom: what came in, newest
// first. Top: how to send (address, QR code of the page), or the running batch's progress.

#define RECEIVE_PAGE_PATH "romfs:/receive.html"
#define RECEIVE_PAGE_MAX (256 * 1024)
#define RECEIVE_ROWS_MAX 500
#define RECEIVE_URL_MAX 32
#define RECEIVE_QR_VERSION_MAX 5 // "http://255.255.255.255:8080/" needs version 2 at low error correction
#define RECEIVE_QR_QUIET 2 // light modules around the code
#define RECEIVE_QR_SCALE 3
#define RECEIVE_MARGIN 12

typedef struct {
    receive_state shared;
    char* page;

    // The QR code and address of the page, rebuilt when the console's address or port changes.
    u32 qrTexture; // 0 when no texture was free
    u32 qrSize; // modules with the quiet zone; 0 = no code
    u32 shownIp;
    u16 shownPort;
    char url[RECEIVE_URL_MAX];

    info_op_draw opDraw;
} receive_data;

static char* receive_load_page(u32* size) {
    FILE* fd = fopen(RECEIVE_PAGE_PATH, "rb");
    if(fd == NULL) {
        return NULL;
    }

    char* page = (char*) malloc(RECEIVE_PAGE_MAX);
    size_t read = page != NULL ? fread(page, 1, RECEIVE_PAGE_MAX, fd) : 0;
    fclose(fd);

    if(read == 0 || read == RECEIVE_PAGE_MAX) {
        free(page);
        return NULL;
    }

    *size = read;
    return page;
}

static void receive_update_address(receive_data* data) {
    u32 ip = data->shared.ip;
    u16 port = data->shared.webPort;
    if(ip == data->shownIp && port == data->shownPort) {
        return;
    }

    data->shownIp = ip;
    data->shownPort = port;
    data->url[0] = '\0';
    data->qrSize = 0;

    if(ip == 0 || port == 0) {
        return;
    }

    struct in_addr address = {.s_addr = ip};
    if(port == 80) {
        snprintf(data->url, sizeof(data->url), "http://%s/", inet_ntoa(address));
    } else {
        snprintf(data->url, sizeof(data->url), "http://%s:%u/", inet_ntoa(address), port);
    }

    if(data->qrTexture == 0) {
        return;
    }

    u8 code[qrcodegen_BUFFER_LEN_FOR_VERSION(RECEIVE_QR_VERSION_MAX)];
    u8 temp[qrcodegen_BUFFER_LEN_FOR_VERSION(RECEIVE_QR_VERSION_MAX)];
    if(!qrcodegen_encodeText(data->url, temp, code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, RECEIVE_QR_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return;
    }

    int modules = qrcodegen_getSize(code);
    u32 size = modules + RECEIVE_QR_QUIET * 2;

    u16* pixels = (u16*) malloc(size * size * sizeof(u16));
    if(pixels == NULL) {
        return;
    }

    // Black on white whatever the theme: phone cameras read dark modules on a light background.
    for(u32 y = 0; y < size; y++) {
        for(u32 x = 0; x < size; x++) {
            bool dark = qrcodegen_getModule(code, (int) x - RECEIVE_QR_QUIET, (int) y - RECEIVE_QR_QUIET);
            pixels[y * size + x] = dark ? 0x0000 : 0xFFFF;
        }
    }

    // No filtering: modules stay sharp when drawn scaled.
    screen_load_texture_untiled(data->qrTexture, pixels, size * size * sizeof(u16), size, size, GPU_RGB565, false);
    free(pixels);

    data->qrSize = size;
}

static void receive_draw_idle(receive_data* data, float x1, float y1, float x2, float y2) {
    if(data->shownIp == 0) {
        ui_draw_centered_text("Waiting for WiFi...", x1, y1, x2, y2);
        return;
    }

    char text[64];
    if(data->url[0] != '\0') {
        snprintf(text, sizeof(text), "Browser: %s", data->url);
    } else {
        snprintf(text, sizeof(text), "Browser: ports 80 and 8080 in use");
    }

    float textX = x1 + RECEIVE_MARGIN;
    if(data->qrSize != 0) {
        float qrPixels = data->qrSize * RECEIVE_QR_SCALE;
        screen_draw_texture(data->qrTexture, x1 + RECEIVE_MARGIN, y1 + (y2 - y1 - qrPixels) / 2, qrPixels, qrPixels);

        textX += qrPixels + RECEIVE_MARGIN;
    }

    float textWidth = 0;
    float textHeight = 0;
    screen_get_string_size_wrap(&textWidth, &textHeight, text, 0.5f, 0.5f, x2 - RECEIVE_MARGIN - textX);
    screen_draw_string_wrap(text, textX, y1 + (y2 - y1 - textHeight) / 2, 0.5f, 0.5f, COLOR_TEXT, false, x2 - RECEIVE_MARGIN);
}

static void receive_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    receive_data* receiveData = (receive_data*) data;

    LightLock_Lock(&receiveData->shared.jobLock);

    const receive_job* job = receiveData->shared.job;
    if(job != NULL) {
        char line[FILE_PATH_MAX + 32];
        snprintf(line, sizeof(line), "%s: %s", receiver_job_from(job), receiver_job_item_name(job, receiveData->shared.op.processed));

        float lineHeight = screen_get_font_height(0.5f);
        screen_draw_string_wrap(line, x1 + RECEIVE_MARGIN, y1 + RECEIVE_MARGIN, 0.5f, 0.5f, COLOR_TEXT, false, x2 - RECEIVE_MARGIN);
        info_draw_op(&receiveData->opDraw, &receiveData->shared.op, x1, y1 + RECEIVE_MARGIN + lineHeight * 2, x2, y2);
    }

    LightLock_Unlock(&receiveData->shared.jobLock);

    if(job == NULL) {
        memset(&receiveData->opDraw, 0, sizeof(receiveData->opDraw));
        receive_draw_idle(receiveData, x1, y1, x2, y2);
    }
}

static void receive_free_rows(linked_list* rows) {
    linked_list_iter iter;
    linked_list_iterate(rows, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
    }

    linked_list_clear(rows);
}

static void receive_free(receive_data* data, linked_list* items) {
    task_cancel_and_wait(&data->shared.state);

    receive_free_rows(items);
    receive_free_rows(&data->shared.rows);
    linked_list_destroy(&data->shared.rows);

    if(data->qrTexture != 0) {
        screen_unload_texture(data->qrTexture);
    }

    free(data->page);
    free(data);
}

static void receive_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    receive_data* receiveData = (receive_data*) data;

    // B cancels a running batch; only an idle screen closes.
    if(hidKeysDown() & KEY_B) {
        LightLock_Lock(&receiveData->shared.jobLock);
        bool running = receiveData->shared.job != NULL;
        if(running) {
            task_cancel(&receiveData->shared.op.state);
        }

        LightLock_Unlock(&receiveData->shared.jobLock);

        if(!running) {
            ui_pop();
            receive_free(receiveData, items);
            list_destroy(view);
        }

        return;
    }

    LightLock_Lock(&receiveData->shared.logLock);

    linked_list_iter iter;
    linked_list_iterate(&receiveData->shared.rows, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* row = (list_item*) linked_list_iter_next(&iter);
        if(!linked_list_add_at(items, 0, row)) {
            free(row);
        }
    }

    linked_list_clear(&receiveData->shared.rows);

    LightLock_Unlock(&receiveData->shared.logLock);

    while(linked_list_size(items) > RECEIVE_ROWS_MAX) {
        u32 last = linked_list_size(items) - 1;
        list_item* row = (list_item*) linked_list_get(items, last);
        linked_list_remove_at(items, last);
        free(row);
    }

    receive_update_address(receiveData);
}

void receive_open() {
    receive_data* data = (receive_data*) calloc(1, sizeof(receive_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate Receive data.");
        return;
    }

    data->page = receive_load_page(&data->shared.pageSize);
    if(data->page == NULL) {
        error_display(NULL, NULL, "Failed to load " RECEIVE_PAGE_PATH ".");
        free(data);
        return;
    }

    data->shared.page = data->page;
    data->qrTexture = screen_allocate_free_texture();

    Result res = receiver_start(&data->shared);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to start Receive.");

        if(data->qrTexture != 0) {
            screen_unload_texture(data->qrTexture);
        }

        free(data->page);
        free(data);
        return;
    }

    list_display("Receive", "B: Cancel transfer or return", data, receive_update, receive_draw_top);
}
