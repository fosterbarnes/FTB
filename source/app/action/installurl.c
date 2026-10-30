#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "action.h"
#include "contentinstall.h"
#include "../resources.h"
#include "../task/uitask.h"
#include "../../core/core.h"

typedef struct {
    char urls[INSTALL_URLS_MAX][DOWNLOAD_URL_MAX];

    char paths[INSTALL_URLS_MAX][FILE_PATH_MAX];

    void* userData;
    void (*finishedURL)(void* data, u32 index);
    void (*finishedAll)(void* data);
    void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index);

    content_install content;

    data_op_data installInfo;
} install_url_data;

static void action_install_url_free_data(install_url_data* data) {
    if(data->finishedAll != NULL) {
        data->finishedAll(data->userData);
    }

    free(data);
}

static void action_install_url_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    install_url_data* installData = (install_url_data*) data;

    if(installData->drawTop != NULL) {
        installData->drawTop(view, installData->userData, x1, y1, x2, y2, installData->installInfo.processed);
    } else if(installData->installInfo.processed >= installData->installInfo.total) {
        // >=: retrying the first URL wraps processed to 0xFFFFFFFF until the loop's ++.
        float urlY = y1 + 5;
        u32 index = 0;
        while(urlY < y2 && index < installData->installInfo.total) {
            float urlWidth = 0;
            float urlHeight = 0;
            screen_get_string_size_wrap(&urlWidth, &urlHeight, installData->urls[index], 0.5f, 0.5f, x2 - x1 - 10);

            float urlX = x1 + (x2 - x1 - urlWidth) / 2;
            screen_draw_string_wrap(installData->urls[index], urlX, urlY, 0.5f, 0.5f, COLOR_TEXT, true, urlX + urlWidth + 1);

            urlY += urlHeight;
            index++;
        }
    } else {
        float urlWidth = 0;
        float urlHeight = 0;
        screen_get_string_size_wrap(&urlWidth, &urlHeight, installData->urls[installData->installInfo.processed], 0.5f, 0.5f, x2 - x1 - 10);

        float urlX = x1 + (x2 - x1 - urlWidth) / 2;
        float urlY = y1 + (y2 - y1 - urlHeight) / 2;
        screen_draw_string_wrap(installData->urls[installData->installInfo.processed], urlX, urlY, 0.5f, 0.5f, COLOR_TEXT, true, urlX + urlWidth + 1);
    }
}

static Result action_install_url_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    install_url_data* installData = (install_url_data*) data;

    strncpy(url, installData->urls[index], maxSize);
    return 0;
}

static Result action_install_url_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    install_url_data* installData = (install_url_data*) data;

    installData->installInfo.stage = "Preparing";

    char fileName[FILE_NAME_MAX];
    string_get_url_file(fileName, installData->urls[index], FILE_NAME_MAX);

    return content_install_begin(&installData->content, fileName, installData->paths[index], initialReadBlock, initialReadSize, data,
                                 action_install_url_draw_top, handle);
}

static Result action_install_url_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    install_url_data* installData = (install_url_data*) data;

    Result res = content_end(&installData->content, &installData->installInfo, succeeded, handle);

    if(succeeded && R_SUCCEEDED(res) && installData->finishedURL != NULL) {
        installData->finishedURL(installData->userData, index);
    }

    return res;
}

static Result action_install_url_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

static Result action_install_url_suspend(void* data, u32 index) {
    return 0;
}

static Result action_install_url_restore(void* data, u32 index) {
    return 0;
}

static bool action_install_url_error(void* data, u32 index, Result res, ui_view** errorView) {
    install_url_data* installData = (install_url_data*) data;

    char* url = installData->urls[index];
    if(strlen(url) > 38) {
        *errorView = error_display_res(data, action_install_url_draw_top, res, "Failed to install from URL.\n%.35s...", url);
    } else {
        *errorView = error_display_res(data, action_install_url_draw_top, res, "Failed to install from URL.\n%.38s", url);
    }

    return true;
}

static void action_install_url_install_update(ui_view* view, void* data, float* progress, char* text) {
    install_url_data* installData = (install_url_data*) data;

    if(task_is_finished(&installData->installInfo.state)) {
        ui_pop();
        info_destroy(view);

        if(R_SUCCEEDED(installData->installInfo.state.result)) {
            static char summary[64];
            task_data_op_format_summary(&installData->installInfo, "Install finished.", summary, sizeof(summary));
            prompt_display_notify("Success", summary, COLOR_TEXT, NULL, NULL, NULL);
        }

        action_install_url_free_data(installData);

        return;
    }

    if(hidKeysDown() & KEY_B) {
        task_cancel(&installData->installInfo.state);
    }
}

static void action_install_url_confirm_onresponse(ui_view* view, void* data, u32 response) {
    install_url_data* installData = (install_url_data*) data;

    if(response == PROMPT_YES) {
        Result res = task_data_op(&installData->installInfo);
        if(R_SUCCEEDED(res)) {
            ui_view* progressView = info_display("Installing From URL(s)", "Press B to cancel.", true, data, action_install_url_install_update, action_install_url_draw_top);
            info_set_data_op(progressView, &installData->installInfo);
        } else {
            error_display_res(NULL, NULL, res, "Failed to initiate installation.");

            action_install_url_free_data(installData);
        }
    } else {
        action_install_url_free_data(installData);
    }
}

u32 action_parse_urls(const char* text, char (*out)[DOWNLOAD_URL_MAX], u32 max) {
    u32 count = 0;

    size_t textLen = strlen(text);
    const char* currStart = text;
    while(count < max && (size_t) (currStart - text) < textLen) {
        const char* currEnd = strchr(currStart, '\n');
        if(currEnd == NULL) {
            currEnd = text + textLen;
        }

        u32 len = currEnd - currStart;

        // Blank lines (and the \r of \r\n endings) would become "http://" entries that fail.
        while(len > 0 && (currStart[len - 1] == '\r' || currStart[len - 1] == ' ')) {
            len--;
        }

        if(len == 0) {
            currStart = currEnd + 1;
            continue;
        }

        if((len < 7 || strncmp(currStart, "http://", 7) != 0) && (len < 8 || strncmp(currStart, "https://", 8) != 0)) {
            if(len > DOWNLOAD_URL_MAX - 8) {
                len = DOWNLOAD_URL_MAX - 8;
            }

            string_copy(out[count], "http://", 8);
            string_copy(&out[count][7], currStart, len + 1);
        } else {
            if(len > DOWNLOAD_URL_MAX - 1) {
                len = DOWNLOAD_URL_MAX - 1;
            }

            string_copy(out[count], currStart, len + 1);
        }

        count++;
        currStart = currEnd + 1;
    }

    return count;
}

void action_install_url(const char* confirmMessage, const char* urls, const char* paths, void* userData,
                        void (*finishedURL)(void* data, u32 index),
                        void (*finishedAll)(void* data),
                        void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2, u32 index)) {
    install_url_data* data = (install_url_data*) calloc(1, sizeof(install_url_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate URL install data.");

        return;
    }

    data->installInfo.total = action_parse_urls(urls, data->urls, INSTALL_URLS_MAX);

    if(paths != NULL) {
        size_t pathsLen = strlen(paths);
        if(pathsLen > 0) {
            const char* currStart = paths;
            for(u32 i = 0; i < data->installInfo.total && currStart - paths < pathsLen; i++) {
                const char* currEnd = strchr(currStart, '\n');
                if(currEnd == NULL) {
                    currEnd = paths + pathsLen;
                }

                u32 len = currEnd - currStart;
                if(len > FILE_PATH_MAX - 1) {
                    len = FILE_PATH_MAX - 1;
                }

                string_copy(data->paths[i], currStart, len + 1);

                currStart = currEnd + 1;
            }
        }
    }

    data->userData = userData;
    data->finishedURL = finishedURL;
    data->finishedAll = finishedAll;
    data->drawTop = drawTop;

    data->installInfo.data = data;

    data->installInfo.op = DATAOP_DOWNLOAD;

    data->installInfo.bufferSize = 128 * 1024;

    data->installInfo.processed = data->installInfo.total;

    data->installInfo.getSrcUrl = action_install_url_get_src_url;

    data->installInfo.openDst = action_install_url_open_dst;
    data->installInfo.closeDst = action_install_url_close_dst;
    data->installInfo.writeDst = action_install_url_write_dst;

    data->installInfo.suspend = action_install_url_suspend;
    data->installInfo.restore = action_install_url_restore;

    data->installInfo.error = action_install_url_error;

    data->installInfo.state.finished = true;

    prompt_display_yes_no("Confirmation", confirmMessage, COLOR_TEXT, data, action_install_url_draw_top, action_install_url_confirm_onresponse);
}