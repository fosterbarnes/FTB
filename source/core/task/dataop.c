#include <malloc.h>
#include <string.h>

#include <3ds.h>
#include <jansson.h>

#include "dataop.h"
#include "../core.h"

#define DATAOP_RATE_INTERVAL_MS 250
#define DATAOP_RATE_SMOOTHING 0.3f

// Speed is a moving average of all bytes moved, sampled every 250 ms, so it stays steady across items
// and slow non-transfer steps. ETA covers the whole batch when its size is known.
static void task_data_op_update_rate(data_op_data* data) {
    u64 time = osGetTime();
    u64 elapsed = time - data->rateTime;
    if(elapsed < DATAOP_RATE_INTERVAL_MS) {
        return;
    }

    u64 moved = data->doneBytes + data->currProcessed;

    // A retried item restarts at 0; skip that sample instead of reading it as negative speed.
    if(moved >= data->rateBytes) {
        float instant = (moved - data->rateBytes) * 1000.0f / elapsed;
        float average = data->bytesPerSecond == 0 ? instant : data->bytesPerSecond + (instant - data->bytesPerSecond) * DATAOP_RATE_SMOOTHING;
        data->bytesPerSecond = (u32) average;
    }

    u64 remaining = 0;
    if(data->totalBytes != 0) {
        remaining = data->totalBytes > moved ? data->totalBytes - moved : 0;
    } else if(data->currTotal > data->currProcessed) {
        remaining = data->currTotal - data->currProcessed;
    }

    data->estimatedRemainingSeconds = data->bytesPerSecond != 0 ? (u32) (remaining / data->bytesPerSecond) : 0;

    data->rateTime = time;
    data->rateBytes = moved;
}

static Result task_data_op_check_running(data_op_data* data) {
    Result res = 0;

    if(task_is_quit_all() || data->state.cancelled) {
        res = R_APP_CANCELLED;
    } else {
        bool suspended = svcWaitSynchronization(task_get_suspend_event(), 0) != 0;
        if(suspended) {
            if(data->suspend != NULL && R_SUCCEEDED(res)) {
                res = data->suspend(data->data, data->processed);
            }
        }

        svcWaitSynchronization(task_get_pause_event(), U64_MAX);

        if(suspended) {
            if(data->restore != NULL && R_SUCCEEDED(res)) {
                res = data->restore(data->data, data->processed);
            }
        }
    }

    return res;
}

static Result task_data_op_copy(data_op_data* data, u32 index) {
    data->currProcessed = 0;
    data->currTotal = 0;

    Result res = 0;

    bool isDir = false;
    if(R_SUCCEEDED(res = data->isSrcDirectory(data->data, index, &isDir)) && isDir) {
        res = data->makeDstDirectory(data->data, index);
    } else {
        u32 srcHandle = 0;
        if(R_SUCCEEDED(res = data->openSrc(data->data, index, &srcHandle))) {
            if(R_SUCCEEDED(res = data->getSrcSize(data->data, srcHandle, &data->currTotal))) {
                if(data->currTotal == 0) {
                    if(data->copyEmpty) {
                        u32 dstHandle = 0;
                        if(R_SUCCEEDED(res = data->openDst(data->data, index, NULL, 0, data->currTotal, &dstHandle))) {
                            res = data->closeDst(data->data, index, true, dstHandle);
                        }
                    } else {
                        res = R_APP_BAD_DATA;
                    }
                } else {
                    // Every byte written was read first, so the buffer needs no zeroing.
                    u8* buffer = (u8*) malloc(data->bufferSize);
                    if(buffer != NULL) {
                        u32 dstHandle = 0;

                        bool firstRun = true;
                        while(data->currProcessed < data->currTotal) {
                            if(R_FAILED(res = task_data_op_check_running(data))) {
                                break;
                            }

                            u32 bytesRead = 0;
                            if(R_FAILED(res = data->readSrc(data->data, srcHandle, &bytesRead, buffer, data->currProcessed, data->bufferSize))) {
                                break;
                            }

                            // The source got shorter than its size said; reading on would spin forever.
                            if(bytesRead == 0) {
                                res = R_APP_BAD_DATA;
                                break;
                            }

                            if(firstRun) {
                                firstRun = false;

                                // AM and FS write *handle even when they fail; a failed open leaves nothing to close.
                                if(R_FAILED(res = data->openDst(data->data, index, buffer, bytesRead, data->currTotal, &dstHandle))) {
                                    dstHandle = 0;
                                    break;
                                }

                                data->stage = NULL;
                            }

                            u32 bytesWritten = 0;
                            if(R_FAILED(res = data->writeDst(data->data, dstHandle, &bytesWritten, buffer, data->currProcessed, bytesRead))) {
                                break;
                            }

                            data->currProcessed += bytesWritten;
                            task_data_op_update_rate(data);
                        }

                        if(dstHandle != 0) {
                            Result closeDstRes = data->closeDst(data->data, index, res == 0, dstHandle);
                            if(R_SUCCEEDED(res)) {
                                res = closeDstRes;
                            }
                        }

                        free(buffer);
                    } else {
                        res = R_APP_OUT_OF_MEMORY;
                    }
                }
            }

            Result closeSrcRes = data->closeSrc(data->data, index, res == 0, srcHandle);
            if(R_SUCCEEDED(res)) {
                res = closeSrcRes;
            }
        }
    }

    return res;
}

typedef struct {
    data_op_data* data;

    u32 index;

    u32 dstHandle;
    bool firstRun;

    u64 writeOffset;
} data_op_download_data;

static Result task_data_op_download_callback(void* userData, void* buffer, size_t size) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    if(downloadData->firstRun) {
        downloadData->firstRun = false;

        Result res = data->openDst(data->data, downloadData->index, buffer, size, data->currTotal, &downloadData->dstHandle);
        if(R_FAILED(res)) {
            downloadData->dstHandle = 0; // see task_data_op_copy
            return res;
        }

        data->stage = NULL;
    }

    u32 bytesWritten = 0;
    Result res = data->writeDst(data->data, downloadData->dstHandle, &bytesWritten, buffer, downloadData->writeOffset, size);
    downloadData->writeOffset += bytesWritten;

    return res;
}

static Result task_data_op_download_check_running(void* userData) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;

    return task_data_op_check_running(downloadData->data);
}

static Result task_data_op_download_progress(void* userData, u64 total, u64 curr) {
    data_op_download_data* downloadData = (data_op_download_data*) userData;
    data_op_data* data = downloadData->data;

    data->currTotal = total;
    data->currProcessed = curr;

    task_data_op_update_rate(data);

    return 0;
}

static Result task_data_op_download(data_op_data* data, u32 index) {
    data->currProcessed = 0;
    data->currTotal = 0;

    Result res = 0;

    char url[DOWNLOAD_URL_MAX];
    if(R_SUCCEEDED(res = data->getSrcUrl(data->data, index, url, DOWNLOAD_URL_MAX))) {
        // Cleared once the destination opens on the first received block.
        data->stage = "Connecting";

        data_op_download_data downloadData = {data, index, 0, true, 0};
        res = http_download_callback(url, data->bufferSize, &downloadData, task_data_op_download_callback, task_data_op_download_check_running, task_data_op_download_progress);

        // An empty body never opened the destination, so nothing was installed.
        if(R_SUCCEEDED(res) && downloadData.firstRun) {
            res = R_APP_BAD_DATA;
        }

        if(downloadData.dstHandle != 0) {
            Result closeDstRes = data->closeDst(data->data, index, res == 0, downloadData.dstHandle);
            if(R_SUCCEEDED(res)) {
                res = closeDstRes;
            }
        }
    }

    return res;
}

static Result task_data_op_delete(data_op_data* data, u32 index) {
    return data->delete(data->data, index);
}

// The op's result is the last item's: the UI shows success only when the final item succeeded.
static Result task_data_op_loop(data_op_data* data) {
    Result result = 0;
    for(data->processed = 0; data->processed < data->total; data->processed++) {
        Result res = 0;

        data->stage = NULL;

        if(R_SUCCEEDED(res = task_data_op_check_running(data))) {
            switch(data->op) {
                case DATAOP_COPY:
                    res = task_data_op_copy(data, data->processed);
                    break;
                case DATAOP_DOWNLOAD:
                    res = task_data_op_download(data, data->processed);
                    break;
                case DATAOP_DELETE:
                    res = task_data_op_delete(data, data->processed);
                    break;
                default:
                    break;
            }
        }

        result = res;

        // Bytes of this item count toward the batch; a retry below takes them back out. The UI sums
        // doneBytes + currProcessed, reading doneBytes first, so zeroing currProcessed before the add
        // (with barriers on both sides) can only show a one-frame dip, never the item counted twice.
        u64 itemBytes = data->currProcessed;
        data->currProcessed = 0;
        __dmb();
        data->doneBytes += itemBytes;

        if(R_FAILED(res)) {
            if(res == R_APP_CANCELLED) {
                // Waits until dismissed, like the error view below: if the op ended while the notice
                // was on top, the progress view's finish (ui_pop, then destroy itself) would pop the
                // notice and free a view still on the stack. Quitting answers no prompts.
                if(!task_is_quit_all()) {
                    prompt_wait_notify("Failure", "Operation cancelled.", COLOR_TEXT);
                }

                break;
            } else if(res != R_APP_SKIPPED) {
                ui_view* errorView = NULL;
                bool proceed = data->error(data->data, data->processed, res, &errorView);

                if(errorView != NULL) {
                    svcWaitSynchronization(errorView->active, U64_MAX);
                }

                // A locked system change fails the same way again, so there is nothing to retry.
                if(res == R_APP_SYSTEM_LOCKED || data->noRetry) {
                    if(!proceed) {
                        break;
                    }

                    continue;
                }

                // prompt_wait_yes_no, not a wait on the view: ui_pop signals the view before the
                // response callback runs.
                if(prompt_wait_yes_no("Confirmation", "Retry?", COLOR_TEXT, NULL, NULL)) {
                    if(proceed) {
                        data->processed--;
                        data->doneBytes -= itemBytes;
                    } else {
                        data->processed = 0;
                        data->doneBytes = 0;
                    }
                } else if(!proceed) {
                    break;
                }
            }
        }
    }

    return result;
}

static Result task_data_op_thread(void* arg) {
    Result result = task_data_op_loop((data_op_data*) arg);

    task_sleep_unblock();
    return result;
}

void task_data_op_format_summary(const data_op_data* data, const char* headline, char* out, size_t size) {
    u64 bytes = data->doneBytes;
    u32 seconds = (u32) ((osGetTime() - data->startTime) / 1000);

    snprintf(out, size, "%s\n%.1f %s in %s", headline, ui_get_display_size(bytes), ui_get_display_size_units(bytes), ui_get_display_eta(seconds));
}

static void task_data_op_reset(data_op_data* data) {
    data->processed = 0;

    data->currProcessed = 0;
    data->currTotal = 0;

    data->bytesPerSecond = 0;
    data->estimatedRemainingSeconds = 0;
    data->doneBytes = 0;
    data->stage = NULL;

    data->startTime = osGetTime();
    data->rateTime = data->startTime;
    data->rateBytes = 0;
}

Result task_data_op_run(data_op_data* data) {
    if(data == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_data_op_reset(data);

    task_sleep_block();
    Result result = task_data_op_loop(data);
    task_sleep_unblock();

    return result;
}

Result task_data_op(data_op_data* data) {
    if(data == NULL) {
        return R_APP_INVALID_ARGUMENT;
    }

    task_data_op_reset(data);

    // Blocked before the worker starts: it unblocks when it ends, and a quick op could end before
    // this thread got here.
    task_sleep_block();

    Result res = task_start(&data->state, task_data_op_thread, data, 0x18, 1);
    if(R_FAILED(res)) {
        task_sleep_unblock();
    }

    return res;
}