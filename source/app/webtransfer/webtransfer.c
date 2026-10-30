#include <arpa/inet.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "webserver.h"
#include "../resources.h"
#include "../screens.h"
#include "../../core/core.h"
#include "../../libs/qrcodegen/qrcodegen.h"

// Web Transfer screen: the worker (webserver.c) runs while this view is open. Bottom: what happened,
// newest first. Top: how to connect (address, QR code of the page), or the running transfer's progress.

#define WEB_PAGE_PATH "romfs:/webtransfer.html"
#define WEB_PAGE_MAX (256 * 1024)
#define WEB_ROWS_MAX 500
#define WEB_URL_MAX 32
#define WEB_QR_VERSION_MAX 5 // "http://255.255.255.255:8080/" needs version 2 at low error correction
#define WEB_QR_QUIET 2 // light modules around the code
#define WEB_QR_SCALE_MAX 5
#define WEB_TEXT_MIN_WIDTH 180 // keeps the URL readable when the info panel is on the 320 px screen
#define WEB_URL_SCALE 0.7f
#define WEB_LINE_GAP 6
#define WEB_MARGIN 12

typedef struct {
    web_state shared;
    char* page;

    // The QR code and address of the page, rebuilt when the console's address or port changes.
    u32 qrTexture; // 0 when no texture was free
    u32 qrSize; // modules with the quiet zone; 0 = no code
    u32 shownIp;
    u16 shownPort;
    char url[WEB_URL_MAX];

    info_op_draw opDraw;
} webtransfer_data;

static char* webtransfer_load_page(u32* size) {
    FILE* fd = fopen(WEB_PAGE_PATH, "rb");
    if(fd == NULL) {
        return NULL;
    }

    char* page = (char*) malloc(WEB_PAGE_MAX);
    size_t read = page != NULL ? fread(page, 1, WEB_PAGE_MAX, fd) : 0;
    fclose(fd);

    if(read == 0 || read == WEB_PAGE_MAX) {
        free(page);
        return NULL;
    }

    *size = read;
    return page;
}

static void webtransfer_update_address(webtransfer_data* data) {
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

    u8 code[qrcodegen_BUFFER_LEN_FOR_VERSION(WEB_QR_VERSION_MAX)];
    u8 temp[qrcodegen_BUFFER_LEN_FOR_VERSION(WEB_QR_VERSION_MAX)];
    if(!qrcodegen_encodeText(data->url, temp, code, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, WEB_QR_VERSION_MAX, qrcodegen_Mask_AUTO, true)) {
        return;
    }

    int modules = qrcodegen_getSize(code);
    u32 size = modules + WEB_QR_QUIET * 2;

    u16* pixels = (u16*) malloc(size * size * sizeof(u16));
    if(pixels == NULL) {
        return;
    }

    // Black on white whatever the theme: phone cameras read dark modules on a light background.
    for(u32 y = 0; y < size; y++) {
        for(u32 x = 0; x < size; x++) {
            bool dark = qrcodegen_getModule(code, (int) x - WEB_QR_QUIET, (int) y - WEB_QR_QUIET);
            pixels[y * size + x] = dark ? 0x0000 : 0xFFFF;
        }
    }

    // No filtering: modules stay sharp when drawn scaled.
    screen_load_texture_untiled(data->qrTexture, pixels, size * size * sizeof(u16), size, size, GPU_RGB565, false);
    free(pixels);

    data->qrSize = size;
}

static void webtransfer_draw_idle(webtransfer_data* data, float x1, float y1, float x2, float y2) {
    if(data->shownIp == 0) {
        ui_draw_centered_text("Waiting for Wi-Fi...", x1, y1, x2, y2);
        return;
    }

    if(data->url[0] == '\0') {
        ui_draw_centered_text("Ports 80 and 8080 are in use.", x1, y1, x2, y2);
        return;
    }

    // QR code on the left, as large as the height allows while leaving a readable text column.
    float textX = x1 + WEB_MARGIN;
    if(data->qrSize != 0) {
        float room = y2 - y1 - WEB_MARGIN * 2;
        float roomBeside = x2 - x1 - WEB_MARGIN * 3 - WEB_TEXT_MIN_WIDTH;
        int scale = (int) (roomBeside < room ? roomBeside : room) / (int) data->qrSize;
        scale = scale < 1 ? 1 : scale > WEB_QR_SCALE_MAX ? WEB_QR_SCALE_MAX : scale;

        float qrPixels = data->qrSize * scale;
        screen_draw_texture(data->qrTexture, textX, y1 + (y2 - y1 - qrPixels) / 2, qrPixels, qrPixels);

        textX += qrPixels + WEB_MARGIN;
    }

    // Hint, URL, scan hint: one group centered vertically. The URL shrinks to fit the column.
    float textX2 = x2 - WEB_MARGIN;
    float columnWidth = textX2 - textX;

    float urlScale = WEB_URL_SCALE;
    float urlWidth = 0;
    screen_get_string_size(&urlWidth, NULL, data->url, urlScale, urlScale);
    if(urlWidth > columnWidth) {
        urlScale *= columnWidth / urlWidth;
    }

    const char* hint = "On the same Wi-Fi, open";
    const char* scan = data->qrSize != 0 ? "or scan the QR code." : NULL;

    float hintHeight = 0;
    float scanHeight = 0;
    screen_get_string_size_wrap(NULL, &hintHeight, hint, 0.5f, 0.5f, columnWidth);
    if(scan != NULL) {
        screen_get_string_size_wrap(NULL, &scanHeight, scan, 0.5f, 0.5f, columnWidth);
    }

    float urlHeight = screen_get_font_height(urlScale);
    float total = hintHeight + WEB_LINE_GAP + urlHeight + (scan != NULL ? WEB_LINE_GAP + scanHeight : 0);

    float y = y1 + (y2 - y1 - total) / 2;
    screen_draw_string_wrap(hint, textX, y, 0.5f, 0.5f, COLOR_TEXT_DIM, false, textX2);
    y += hintHeight + WEB_LINE_GAP;
    screen_draw_string(data->url, textX, y, urlScale, urlScale, COLOR_TEXT, false);
    y += urlHeight + WEB_LINE_GAP;
    if(scan != NULL) {
        screen_draw_string_wrap(scan, textX, y, 0.5f, 0.5f, COLOR_TEXT_DIM, false, textX2);
    }
}

static void webtransfer_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    webtransfer_data* transferData = (webtransfer_data*) data;

    LightLock_Lock(&transferData->shared.jobLock);

    const web_job* job = transferData->shared.job;
    if(job != NULL) {
        char line[FILE_PATH_MAX + 32];
        webserver_job_line(job, transferData->shared.op.processed, line, sizeof(line));

        float lineHeight = screen_get_font_height(0.5f);
        screen_draw_string_wrap(line, x1 + WEB_MARGIN, y1 + WEB_MARGIN, 0.5f, 0.5f, COLOR_TEXT, false, x2 - WEB_MARGIN);
        info_draw_op(&transferData->opDraw, &transferData->shared.op, x1, y1 + WEB_MARGIN + lineHeight * 2, x2, y2);
    }

    LightLock_Unlock(&transferData->shared.jobLock);

    if(job == NULL) {
        memset(&transferData->opDraw, 0, sizeof(transferData->opDraw));
        webtransfer_draw_idle(transferData, x1, y1, x2, y2);
    }
}

static void webtransfer_free_rows(linked_list* rows) {
    linked_list_iter iter;
    linked_list_iterate(rows, &iter);
    while(linked_list_iter_has_next(&iter)) {
        free(linked_list_iter_next(&iter));
    }

    linked_list_clear(rows);
}

static void webtransfer_free(webtransfer_data* data, linked_list* items) {
    task_cancel_and_wait(&data->shared.state);

    webtransfer_free_rows(items);
    webtransfer_free_rows(&data->shared.rows);
    linked_list_destroy(&data->shared.rows);

    if(data->qrTexture != 0) {
        screen_unload_texture(data->qrTexture);
    }

    free(data->page);
    free(data);
}

static void webtransfer_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    webtransfer_data* transferData = (webtransfer_data*) data;

    // B cancels a running batch; only an idle screen closes.
    if(hidKeysDown() & KEY_B) {
        LightLock_Lock(&transferData->shared.jobLock);
        bool running = transferData->shared.job != NULL;
        bool closing = !running && !transferData->shared.prompting;
        if(running) {
            task_cancel(&transferData->shared.op.state);
        } else if(closing) {
            // Under the lock: the worker checks this before it starts a prompt (webserver_ask).
            task_cancel(&transferData->shared.state);
        }

        LightLock_Unlock(&transferData->shared.jobLock);

        if(closing) {
            ui_pop();
            webtransfer_free(transferData, items);
            list_destroy(view);
        }

        return;
    }

    LightLock_Lock(&transferData->shared.logLock);

    linked_list_iter iter;
    linked_list_iterate(&transferData->shared.rows, &iter);
    while(linked_list_iter_has_next(&iter)) {
        list_item* row = (list_item*) linked_list_iter_next(&iter);
        if(!linked_list_add_at(items, 0, row)) {
            free(row);
        }
    }

    linked_list_clear(&transferData->shared.rows);

    LightLock_Unlock(&transferData->shared.logLock);

    while(linked_list_size(items) > WEB_ROWS_MAX) {
        u32 last = linked_list_size(items) - 1;
        list_item* row = (list_item*) linked_list_get(items, last);
        linked_list_remove_at(items, last);
        free(row);
    }

    webtransfer_update_address(transferData);
}

void webtransfer_open() {
    webtransfer_data* data = (webtransfer_data*) calloc(1, sizeof(webtransfer_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate 3DS Web Server data.");
        return;
    }

    data->page = webtransfer_load_page(&data->shared.pageSize);
    if(data->page == NULL) {
        error_display(NULL, NULL, "Failed to load " WEB_PAGE_PATH ".");
        free(data);
        return;
    }

    data->shared.page = data->page;
    data->qrTexture = screen_allocate_free_texture();

    Result res = webserver_start(&data->shared);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to start 3DS Web Server.");

        if(data->qrTexture != 0) {
            screen_unload_texture(data->qrTexture);
        }

        free(data->page);
        free(data);
        return;
    }

    list_display("3DS Web Server", "B: Cancel transfer or return", data, webtransfer_update, webtransfer_draw_top);
}
