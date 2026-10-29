#pragma once

typedef struct ui_view_s ui_view;

ui_view* error_display(void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), const char* text, ...);
ui_view* error_display_res(void* data, void (* drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), Result result, const char* text, ...);
// The result's description ("Not enough space"), "<unknown>" when there is none.
const char* error_get_description(Result result);
ui_view* error_display_errno(void* data, void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2), int err, const char* text, ...);