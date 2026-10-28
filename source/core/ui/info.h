#pragma once

#define PROGRESS_TEXT_MAX 512

typedef struct ui_view_s ui_view;
typedef struct data_op_data_s data_op_data;

ui_view* info_display(const char* name, const char* info, bool bar, void* data, void (*update)(ui_view* view, void* data, float* progress, char* text),
                                                                                void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2));
// Draws the data op's own progress layout (item bar, batch bar, stage, speed) instead of the
// update callback's text. The callback still handles cancel and finish.
void info_set_data_op(ui_view* view, data_op_data* op);
void info_destroy(ui_view* view);