#include <malloc.h>
#include <stdio.h>

#include <3ds.h>

#include "error.h"
#include "info.h"
#include "ui.h"
#include "../screen.h"
#include "../task/dataop.h"
#include "../../app/resources.h"

#define INFO_BAR_WIDTH 280
#define INFO_ITEM_BAR_HEIGHT 8
#define INFO_TOTAL_BAR_HEIGHT 4
#define INFO_ROW_GAP 4
#define INFO_BLOCK_GAP 14
#define INFO_EASE_MS 80.0f
#define INFO_SWEEP_MS 1200
#define INFO_SWEEP_FRACTION 0.3f

typedef struct {
    bool bar;
    void* data;
    float progress;
    char text[PROGRESS_TEXT_MAX];
    void (*update)(ui_view* view, void* data, float* progress, char* text);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2);

    // Data op views draw their own layout from the op's live fields.
    data_op_data* op;
    info_op_draw opDraw;
} info_data;

static void info_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    info_data* infoData = (info_data*) data;

    if(infoData->update != NULL) {
        infoData->update(view, infoData->data, &infoData->progress, infoData->text);
    }
}

static void info_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    info_data* infoData = (info_data*) data;

    if(infoData->drawTop != NULL) {
        infoData->drawTop(view, infoData->data, x1, y1, x2, y2);
    }
}

// Bars are stretched solid fills: the track, then the filled part.
static void info_draw_bar(float x, float y, float height, float fraction) {
    screen_draw_texture(TEXTURE_PROGRESS_BAR_BG, x, y, INFO_BAR_WIDTH, height);

    float fillWidth = INFO_BAR_WIDTH * fraction;
    if(fillWidth >= 1) {
        screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, x, y, fillWidth, height);
    }
}

// Indeterminate bar: a segment sweeps across while a step has no byte count, so it never looks frozen.
static void info_draw_sweep(float x, float y, float height) {
    screen_draw_texture(TEXTURE_PROGRESS_BAR_BG, x, y, INFO_BAR_WIDTH, height);

    float segmentWidth = INFO_BAR_WIDTH * INFO_SWEEP_FRACTION;
    float phase = (float) (osGetTime() % INFO_SWEEP_MS) / INFO_SWEEP_MS;

    float start = x - segmentWidth + phase * (INFO_BAR_WIDTH + segmentWidth);
    float end = start + segmentWidth;
    if(start < x) {
        start = x;
    }

    if(end > x + INFO_BAR_WIDTH) {
        end = x + INFO_BAR_WIDTH;
    }

    if(end - start >= 1) {
        screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, start, y, end - start, height);
    }
}

static void info_draw_row(const char* left, const char* right, float x, float y) {
    screen_draw_string(left, x, y, 0.5f, 0.5f, COLOR_TEXT, false);

    float rightWidth;
    screen_get_string_size(&rightWidth, NULL, right, 0.5f, 0.5f);
    screen_draw_string(right, x + INFO_BAR_WIDTH - rightWidth, y, 0.5f, 0.5f, COLOR_TEXT, false);
}

// Moves a shown bar toward its target over ~80 ms so 256 KiB steps glide; drops back instantly.
static float info_ease(float shown, float target, u64 dt) {
    if(target <= shown) {
        return target;
    }

    float step = dt / INFO_EASE_MS;
    return shown + (target - shown) * (step < 1 ? step : 1);
}

static float info_clamp(float fraction) {
    return fraction < 0 ? 0 : fraction > 1 ? 1 : fraction;
}

static void info_format_size(char* out, size_t size, u64 done, u64 total) {
    snprintf(out, size, "%.1f %s / %.1f %s", ui_get_display_size(done), ui_get_display_size_units(done), ui_get_display_size(total), ui_get_display_size_units(total));
}

void info_draw_op(info_op_draw* draw, data_op_data* op, float x1, float y1, float x2, float y2) {
    u64 now = osGetTime();
    u64 dt = draw->lastDrawTime != 0 ? now - draw->lastDrawTime : 0;
    draw->lastDrawTime = now;

    // Snapshot the worker's fields once per frame.
    const char* stage = op->stage;
    u32 total = op->total != 0 ? op->total : 1;
    u32 processed = op->processed < total ? op->processed : total - 1;
    u64 currTotal = op->currTotal;
    // doneBytes before currProcessed: the worker's item handoff (dataop.c) depends on this order.
    u64 doneBytes = op->doneBytes;
    __dmb();
    u64 currProcessed = op->currProcessed;
    u32 bytesPerSecond = op->bytesPerSecond;
    u32 eta = op->estimatedRemainingSeconds;

    bool busy = stage != NULL || currTotal == 0;
    bool batch = total > 1;

    float itemFraction = currTotal != 0 ? info_clamp((float) ((double) currProcessed / (double) currTotal)) : 0;
    if(!busy) {
        draw->itemShown = info_ease(draw->itemShown, itemFraction, dt);
    }

    float totalFraction = op->totalBytes != 0 ? info_clamp((float) ((double) (doneBytes + currProcessed) / (double) op->totalBytes))
                                              : info_clamp((processed + itemFraction) / total);

    // The batch bar only moves backward when the worker restarts an item; a one-frame dip while it
    // hands bytes from one item to the next must not reset it.
    if(processed < draw->lastProcessed || totalFraction > draw->totalShown) {
        draw->totalShown = info_ease(draw->totalShown, totalFraction, dt);
    }

    draw->lastProcessed = processed;

    float lineHeight = screen_get_font_height(0.5f);
    float itemHeight = lineHeight + INFO_ROW_GAP + INFO_ITEM_BAR_HEIGHT + INFO_ROW_GAP + lineHeight;
    float totalHeight = INFO_BLOCK_GAP + lineHeight + INFO_ROW_GAP + INFO_TOTAL_BAR_HEIGHT + INFO_ROW_GAP + lineHeight;

    float x = x1 + (x2 - x1 - INFO_BAR_WIDTH) / 2;
    float y = y1 + (y2 - y1 - itemHeight - (batch ? totalHeight : 0)) / 2;

    char left[96] = "";
    char right[64] = "";

    if(stage != NULL) {
        if(batch) {
            snprintf(left, sizeof(left), "%s (%lu / %lu)", stage, processed + 1, total);
        } else {
            snprintf(left, sizeof(left), "%s", stage);
        }
    } else if(batch) {
        snprintf(left, sizeof(left), "%lu / %lu", processed + 1, total);
    } else if(!busy && bytesPerSecond != 0) {
        // A single item's ETA takes the empty top-left slot: beside bytes and speed it overflows the bar.
        snprintf(left, sizeof(left), "ETA %s", ui_get_display_eta(eta));
    }

    if(!busy) {
        snprintf(right, sizeof(right), "%d%%", (int) (itemFraction * 100));
    }

    info_draw_row(left, right, x, y);
    y += lineHeight + INFO_ROW_GAP;

    if(busy) {
        info_draw_sweep(x, y, INFO_ITEM_BAR_HEIGHT);
    } else {
        info_draw_bar(x, y, INFO_ITEM_BAR_HEIGHT, draw->itemShown);
    }

    y += INFO_ITEM_BAR_HEIGHT + INFO_ROW_GAP;

    left[0] = '\0';
    right[0] = '\0';

    if(currTotal != 0) {
        info_format_size(left, sizeof(left), currProcessed, currTotal);
    }

    if(!busy && bytesPerSecond != 0) {
        snprintf(right, sizeof(right), "%.1f %s/s", ui_get_display_size(bytesPerSecond), ui_get_display_size_units(bytesPerSecond));
    }

    info_draw_row(left, right, x, y);

    if(!batch) {
        return;
    }

    y += lineHeight + INFO_BLOCK_GAP;

    snprintf(right, sizeof(right), "%d%%", (int) (totalFraction * 100));
    info_draw_row("Total", right, x, y);
    y += lineHeight + INFO_ROW_GAP;

    info_draw_bar(x, y, INFO_TOTAL_BAR_HEIGHT, draw->totalShown);
    y += INFO_TOTAL_BAR_HEIGHT + INFO_ROW_GAP;

    left[0] = '\0';
    right[0] = '\0';

    // Byte totals and ETA need the batch size; downloads without it show only the item count above.
    if(op->totalBytes != 0) {
        info_format_size(left, sizeof(left), doneBytes + currProcessed, op->totalBytes);

        if(bytesPerSecond != 0) {
            snprintf(right, sizeof(right), "ETA %s", ui_get_display_eta(eta));
        }
    }

    info_draw_row(left, right, x, y);
}

static void info_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    info_data* infoData = (info_data*) data;

    if(infoData->op != NULL) {
        info_draw_op(&infoData->opDraw, infoData->op, x1, y1, x2, y2);
        return;
    }

    float textWidth;
    float textHeight;
    screen_get_string_size(&textWidth, &textHeight, infoData->text, 0.5f, 0.5f);

    float textX = x1 + (x2 - x1 - textWidth) / 2;
    float textY = y1 + (y2 - y1 - textHeight) / 2;

    if(infoData->bar) {
        float barY = y1 + (y2 - y1 - INFO_ITEM_BAR_HEIGHT) / 2;
        info_draw_bar(x1 + (x2 - x1 - INFO_BAR_WIDTH) / 2, barY, INFO_ITEM_BAR_HEIGHT, info_clamp(infoData->progress));

        textY = barY + INFO_ITEM_BAR_HEIGHT + 10;
    }

    screen_draw_string(infoData->text, textX, textY, 0.5f, 0.5f, COLOR_TEXT, true);
}

ui_view* info_display(const char* name, const char* info, bool bar, void* data, void (*update)(ui_view* view, void* data, float* progress, char* text),
                                                                                void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2)) {
    info_data* infoData = (info_data*) calloc(1, sizeof(info_data));
    if(infoData == NULL) {
        error_display(NULL, NULL, "Failed to allocate info data.");

        return NULL;
    }

    infoData->bar = bar;
    infoData->data = data;
    infoData->progress = 0;
    snprintf(infoData->text, PROGRESS_TEXT_MAX, "Please wait...");
    infoData->update = update;
    infoData->drawTop = drawTop;

    ui_view* view = ui_create();
    view->name = name;
    view->info = info;
    view->data = infoData;
    view->update = info_update;
    view->drawTop = info_draw_top;
    view->drawBottom = info_draw_bottom;
    ui_push(view);

    return view;
}

void info_set_data_op(ui_view* view, data_op_data* op) {
    if(view != NULL) {
        ((info_data*) view->data)->op = op;
    }
}

void info_destroy(ui_view* view) {
    if(view != NULL) {
        free(view->data);
        ui_destroy(view);
    }
}
