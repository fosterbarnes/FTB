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
    const char* name; // bottom screen's top bar, centered
    const char* info;
    bool nameLeft; // name pinned left instead of centered
    const char* topBarText; // optional: top screen's top bar, pinned left
    // Optional touch buttons. A button activates when a touch that started on it lifts while still
    // on it; ui_update sets buttonActivated to its index for that frame's update call, else -1.
    const ui_button* buttons;
    u32 buttonCount;
    int buttonActivated;
    int buttonHeld; // drawn held; -1 once the touch leaves the button
    bool buttonTouch; // the current touch started on a button, so the view must ignore it
    u8 buttonAlpha; // 0xFF (default) = opaque; scales with the view fade
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
// A full stack is fatal (error_panic), like running out of memory, so a pushed view is always shown.
void ui_push(ui_view* view);
void ui_pop();
bool ui_update();

// Three accent squares of edge dot at (x, y), one lit at a time; gaps are dot - 1.
void ui_draw_loading_dots(float x, float y, float dot);
float ui_get_loading_dots_width(float dot);
// Text centered in the rect, lines centered and wrapped 10 px inside each side.
void ui_draw_centered_text(const char* text, float x1, float y1, float x2, float y2);

const char* ui_get_display_eta(u32 seconds);
double ui_get_display_size(u64 size);
const char* ui_get_display_size_units(u64 size);
// Title version as "major.minor.micro" (6, 6, and 4 bits).
void ui_format_version(char* out, size_t size, u16 version);

#include "error.h"
#include "info.h"
#include "kbd.h"
#include "list.h"
#include "prompt.h"