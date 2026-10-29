#pragma once

#include "task.h"

typedef struct json_t json_t;

typedef struct ui_view_s ui_view;

#define DOWNLOAD_URL_MAX 1024

typedef enum data_op_e {
    DATAOP_COPY,
    DATAOP_DOWNLOAD,
    DATAOP_DELETE
} data_op;

typedef struct data_op_data_s {
    void* data;

    data_op op;

    u32 processed;
    u32 total;

    // Copy/Download
    u64 currProcessed;
    u64 currTotal;

    u32 bytesPerSecond;
    u32 estimatedRemainingSeconds;

    // Batch progress: totalBytes is set by the caller (0 = unknown, progress by item count);
    // doneBytes counts bytes moved by finished items.
    u64 totalBytes;
    u64 doneBytes;

    // What the worker is doing while no bytes move ("Finishing install"); NULL while transferring.
    // Set by the engine and by callbacks, shown by the progress view.
    const char* volatile stage;

    u32 bufferSize;

    Result (*openDst)(void* data, u32 index, void* initialReadBlock, u64 size, u32* handle);
    Result (*closeDst)(void* data, u32 index, bool succeeded, u32 handle);

    Result (*writeDst)(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size);

    // Copy
    bool copyEmpty;

    Result (*isSrcDirectory)(void* data, u32 index, bool* isDirectory);
    Result (*makeDstDirectory)(void* data, u32 index);

    Result (*openSrc)(void* data, u32 index, u32* handle);
    Result (*closeSrc)(void* data, u32 index, bool succeeded, u32 handle);

    Result (*getSrcSize)(void* data, u32 handle, u64* size);
    Result (*readSrc)(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size);

    // Download
    Result (*getSrcUrl)(void* data, u32 index, char* url, size_t maxSize);

    // Delete
    Result (*delete)(void* data, u32 index);

    // Suspend
    Result (*suspend)(void* data, u32 index);
    Result (*restore)(void* data, u32 index);

    // Errors
    bool (*error)(void* data, u32 index, Result res, ui_view** errorView);

    // General
    task_state state;
    u64 startTime;

    // Internal
    volatile bool retryResponse;
    u64 rateTime;
    u64 rateBytes;
} data_op_data;

Result task_data_op(data_op_data* data);
// "<headline>\n25.9 GiB in 00:12:31" for finish prompts.
void task_data_op_format_summary(const data_op_data* data, const char* headline, char* out, size_t size);