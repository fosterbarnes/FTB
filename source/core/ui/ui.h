#pragma once

// Square icon button on the bottom screen, drawn over the view's bottom content. The icon texture is
// centered on the button.
#define UI_BUTTON_SIZE 26

typedef struct {
    u32 icon;
    float x;
    float y;
} ui_button;

typedef struct ui_view_s {
    const char* name;
    const char* info;
    // Optional touch buttons. A button activates when a touch that started on it lifts while still
    // on it; ui_update sets buttonActivated to its index for that frame's update call, else -1.
    const ui_button* buttons;
    u32 buttonCount;
    int buttonActivated;
    int buttonHeld; // drawn held; -1 once the touch leaves the button
    bool buttonTouch; // the current touch started on a button, so the view must ignore it
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