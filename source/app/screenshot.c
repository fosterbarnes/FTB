#include <stdio.h>
#include <string.h>
#include <time.h>

#include <3ds.h>

#include "paths.h"
#include "screenshot.h"
#include "../core/core.h"

#define SCREENSHOT_WIDTH TOP_SCREEN_WIDTH
#define SCREENSHOT_HEIGHT (TOP_SCREEN_HEIGHT + BOTTOM_SCREEN_HEIGHT)
#define SCREENSHOT_BOTTOM_X ((TOP_SCREEN_WIDTH - BOTTOM_SCREEN_WIDTH) / 2)
// 1200 bytes, already the multiple of 4 BMP rows need.
#define SCREENSHOT_ROW_SIZE (SCREENSHOT_WIDTH * 3)
#define SCREENSHOT_HEADER_SIZE 54

static bool screenshot_mode = false;

typedef struct {
    const u8* top;
    const u8* bottom;
} screenshot_frames;

bool screenshot_mode_on() {
    return screenshot_mode;
}

void screenshot_mode_toggle() {
    screenshot_mode = !screenshot_mode;
}

static void screenshot_put_u32(u8* out, u32 value) {
    out[0] = (u8) value;
    out[1] = (u8) (value >> 8);
    out[2] = (u8) (value >> 16);
    out[3] = (u8) (value >> 24);
}

static bool screenshot_write(FILE* fd, void* data) {
    const screenshot_frames* frames = (const screenshot_frames*) data;

    u8 header[SCREENSHOT_HEADER_SIZE] = {'B', 'M'};
    screenshot_put_u32(&header[2], SCREENSHOT_HEADER_SIZE + SCREENSHOT_ROW_SIZE * SCREENSHOT_HEIGHT);
    screenshot_put_u32(&header[10], SCREENSHOT_HEADER_SIZE);
    screenshot_put_u32(&header[14], 40);
    screenshot_put_u32(&header[18], SCREENSHOT_WIDTH);
    screenshot_put_u32(&header[22], SCREENSHOT_HEIGHT);
    header[26] = 1;
    header[28] = 24;
    screenshot_put_u32(&header[34], SCREENSHOT_ROW_SIZE * SCREENSHOT_HEIGHT);

    if(fwrite(header, 1, sizeof(header), fd) != sizeof(header)) {
        return false;
    }

    // BMP rows run bottom-up in BGR, the framebuffer's byte order. Frames store each x as a
    // 240-pixel column starting at the bottom pixel.
    u8 row[SCREENSHOT_ROW_SIZE];
    for(u32 y = SCREENSHOT_HEIGHT; y-- > 0;) {
        bool top = y < TOP_SCREEN_HEIGHT;
        const u8* frame = top ? frames->top : frames->bottom;
        u32 frameY = top ? y : y - TOP_SCREEN_HEIGHT;
        u32 frameWidth = top ? TOP_SCREEN_WIDTH : BOTTOM_SCREEN_WIDTH;
        u32 offsetX = top ? 0 : SCREENSHOT_BOTTOM_X;

        memset(row, 0, sizeof(row));
        for(u32 x = 0; x < frameWidth; x++) {
            memcpy(&row[(offsetX + x) * 3], &frame[(x * TOP_SCREEN_HEIGHT + TOP_SCREEN_HEIGHT - 1 - frameY) * 3], 3);
        }

        if(fwrite(row, 1, sizeof(row), fd) != sizeof(row)) {
            return false;
        }
    }

    return true;
}

void screenshot_take() {
    u8* top = screen_capture(GFX_TOP);
    u8* bottom = top != NULL ? screen_capture(GFX_BOTTOM) : NULL;
    if(bottom == NULL) {
        if(top != NULL) {
            linearFree(top);
        }

        error_display(NULL, NULL, "Failed to take screenshot:\nout of memory.");
        return;
    }

    // Milliseconds keep presses within the same second apart.
    time_t t = time(NULL);
    struct tm* now = localtime(&t);

    char path[96];
    snprintf(path, sizeof(path), PATHS_SCREENSHOTS_DIR "/ftb_%04d-%02d-%02d_%02d-%02d-%02d-%03d.bmp",
             now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec, (int) (osGetTime() % 1000));

    paths_ensure_data_dir();
    mkdir(PATHS_SCREENSHOTS_DIR, 0777);

    screenshot_frames frames = {top, bottom};
    bool saved = fs_write_file_safe(path, screenshot_write, &frames);

    linearFree(top);
    linearFree(bottom);

    if(!saved) {
        error_display(NULL, NULL, "Failed to save screenshot to\n" PATHS_SCREENSHOTS_DIR "/.");
    }
}
