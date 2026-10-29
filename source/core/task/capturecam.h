#pragma once

#include "task.h"

typedef enum capture_cam_camera_e {
    CAMERA_OUTER,
    CAMERA_INNER
} capture_cam_camera;

typedef struct capture_cam_data_s {
    u16* buffer;
    s16 width;
    s16 height;
    capture_cam_camera camera;

    // Guards buffer. The caller initializes it once (LightLock_Init) before drawing or starting;
    // it has no handle, so it stays usable between captures.
    LightLock lock;

    task_state state;
} capture_cam_data;

Result task_capture_cam(capture_cam_data* data);