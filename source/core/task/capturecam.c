#include <malloc.h>
#include <string.h>

#include <3ds.h>

#include "capturecam.h"
#include "task.h"
#include "../error.h"

#define EVENT_RECV 0
#define EVENT_BUFFER_ERROR 1

#define EVENT_COUNT 2

// Frames arrive at 30 fps; the timeout only matters if the camera stalls, so cancel is still seen.
#define CAPTURE_WAIT_NS 100000000

static Result task_capture_cam_thread(void* arg) {
    capture_cam_data* data = (capture_cam_data*) arg;

    Handle events[EVENT_COUNT] = {0};

    Result res = 0;

    u32 bufferSize = data->width * data->height * sizeof(u16);
    u16* buffer = (u16*) calloc(1, bufferSize);
    if(buffer != NULL) {
        if(R_SUCCEEDED(res = camInit())) {
            u32 cam = data->camera == CAMERA_OUTER ? SELECT_OUT1 : SELECT_IN1;

            if(R_SUCCEEDED(res = CAMU_SetSize(cam, SIZE_CTR_TOP_LCD, CONTEXT_A))
               && R_SUCCEEDED(res = CAMU_SetOutputFormat(cam, OUTPUT_RGB_565, CONTEXT_A))
               && R_SUCCEEDED(res = CAMU_SetFrameRate(cam, FRAME_RATE_30))
               && R_SUCCEEDED(res = CAMU_SetNoiseFilter(cam, true))
               && R_SUCCEEDED(res = CAMU_SetAutoExposure(cam, true))
               && R_SUCCEEDED(res = CAMU_SetAutoWhiteBalance(cam, true))
               && R_SUCCEEDED(res = CAMU_Activate(cam))) {
                u32 transferUnit = 0;

                if(R_SUCCEEDED(res = CAMU_GetBufferErrorInterruptEvent(&events[EVENT_BUFFER_ERROR], PORT_CAM1))
                   && R_SUCCEEDED(res = CAMU_SetTrimming(PORT_CAM1, true))
                   && R_SUCCEEDED(res = CAMU_SetTrimmingParamsCenter(PORT_CAM1, data->width, data->height, 400, 240))
                   && R_SUCCEEDED(res = CAMU_GetMaxBytes(&transferUnit, data->width, data->height))
                   && R_SUCCEEDED(res = CAMU_SetTransferBytes(PORT_CAM1, transferUnit, data->width, data->height))
                   && R_SUCCEEDED(res = CAMU_ClearBuffer(PORT_CAM1))
                   && R_SUCCEEDED(res = CAMU_SetReceiving(&events[EVENT_RECV], buffer, PORT_CAM1, bufferSize, (s16) transferUnit))
                   && R_SUCCEEDED(res = CAMU_StartCapture(PORT_CAM1))) {
                    while(!task_is_cancelled(&data->state) && R_SUCCEEDED(res)) {
                        s32 index = 0;
                        res = svcWaitSynchronizationN(&index, events, EVENT_COUNT, false, CAPTURE_WAIT_NS);
                        if(R_DESCRIPTION(res) == RD_TIMEOUT) {
                            res = 0;
                        } else if(R_SUCCEEDED(res)) {
                            switch(index) {
                                case EVENT_RECV:
                                    svcCloseHandle(events[EVENT_RECV]);
                                    events[EVENT_RECV] = 0;

                                    LightLock_Lock(&data->lock);
                                    memcpy(data->buffer, buffer, bufferSize);
                                    GSPGPU_FlushDataCache(data->buffer, bufferSize);
                                    LightLock_Unlock(&data->lock);

                                    res = CAMU_SetReceiving(&events[EVENT_RECV], buffer, PORT_CAM1, bufferSize, (s16) transferUnit);
                                    break;
                                case EVENT_BUFFER_ERROR:
                                    svcCloseHandle(events[EVENT_RECV]);
                                    events[EVENT_RECV] = 0;

                                    if(R_SUCCEEDED(res = CAMU_ClearBuffer(PORT_CAM1))
                                       && R_SUCCEEDED(res = CAMU_SetReceiving(&events[EVENT_RECV], buffer, PORT_CAM1, bufferSize, (s16) transferUnit))) {
                                        res = CAMU_StartCapture(PORT_CAM1);
                                    }

                                    break;
                                default:
                                    break;
                            }
                        }
                    }

                    CAMU_StopCapture(PORT_CAM1);

                    bool busy = false;
                    while(R_SUCCEEDED(CAMU_IsBusy(&busy, PORT_CAM1)) && busy) {
                        svcSleepThread(1000000);
                    }

                    CAMU_ClearBuffer(PORT_CAM1);
                }

                CAMU_Activate(SELECT_NONE);
            }

            camExit();
        }

        free(buffer);
    } else {
        res = R_APP_OUT_OF_MEMORY;
    }

    for(int i = 0; i < EVENT_COUNT; i++) {
        if(events[i] != 0) {
            svcCloseHandle(events[i]);
            events[i] = 0;
        }
    }

    return res;
}

Result task_capture_cam(capture_cam_data* data) {
    if(data == NULL || data->buffer == NULL || data->width <= 0 || data->width > 640 || data->height <= 0 || data->height > 480) {
        return R_APP_INVALID_ARGUMENT;
    }

    return task_start(&data->state, task_capture_cam_thread, data, 0x1A, 0);
}