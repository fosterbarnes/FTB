#pragma once

typedef struct ui_view_s {
    const char* name;
    const char* info;
    // Optional shoulder button hints in the left and right corners of the bottom screen's top bar.
    const char* hintLeft;
    const char* hintRight;
    void* data;
    void (*update)(struct ui_view_s* view, void* data, float bx1, float by1, float bx2, float by2);
    void (*drawTop)(struct ui_view_s* view, void* data, float x1, float y1, float x2, float y2);
    void (*drawBottom)(struct ui_view_s* view, void* data, float x1, float y1, float x2, float y2);

    Handle active;
} ui_view;

void ui_init();
void ui_exit();

ui_view* ui_create();
void ui_destroy(ui_view* view);

ui_view* ui_top();
bool ui_push(ui_view* view);
void ui_pop();
bool ui_update();

// Three accent squares of edge dot at (x, y), one lit at a time; gaps are dot - 1.
void ui_draw_loading_dots(float x, float y, float dot);
float ui_get_loading_dots_width(float dot);

const char* ui_get_display_eta(u32 seconds);
double ui_get_display_size(u64 size);
const char* ui_get_display_size_units(u64 size);

#include "error.h"
#include "info.h"
#include "kbd.h"
#include "list.h"
#include "prompt.h"