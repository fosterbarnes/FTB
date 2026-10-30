#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

#include "receiveproto.h"
#include "receiver.h"
#include "../resources.h"
#include "../settings.h"
#include "../action/action.h"
#include "../action/contentinstall.h"
#include "../../core/core.h"

#define RECEIVE_FBI_PORT 5000
#define RECEIVE_DISCOVERY_PORT 5000
#define RECEIVE_WEB_PORT 80
#define RECEIVE_WEB_FALLBACK_PORT 8080
#define RECEIVE_BACKLOG 4

#define RECEIVE_POLL_MS 100
#define RECEIVE_IP_CHECK_MS 1000
// Browsers open spare connections they may never use; one must not hold up the next request.
#define RECEIVE_FIRST_BYTE_MS 1000
#define RECEIVE_HEAD_MS 5000
#define RECEIVE_STALL_MS 10000
#define RECEIVE_DATA_WAIT_MS 30000
#define RECEIVE_CLOSE_MS 2000

#define RECEIVE_SOCKET_BUFFER 32768 // ftpd's tuned 3DS value
#define RECEIVE_BUFFER_SIZE (128 * 1024)
#define RECEIVE_DRAIN_SIZE (32 * 1024)
#define RECEIVE_OFFER_MAX (256 * 1024)
#define RECEIVE_FILES_MAX 1000
#define RECEIVE_FILE_SIZE_MAX 0xFFFFFFFFULL // FAT32
#define RECEIVE_FBI_PAYLOAD_MAX (INSTALL_URLS_MAX * DOWNLOAD_URL_MAX)
#define RECEIVE_ADDRESS_MAX 16
#define RECEIVE_PREVIEW_MAX 8
#define RECEIVE_NETWORK_GONE (-2)

typedef enum {
    RECEIVE_ITEM_PENDING, // not reached
    RECEIVE_ITEM_DONE,
    RECEIVE_ITEM_SKIPPED, // declined prompt, or the file exists without replace
    RECEIVE_ITEM_FAILED,
    RECEIVE_ITEM_CANCELLED
} receive_item_state;

typedef struct {
    char* path; // files: the SD path (saves) or the sender's relative path (installs); NULL for links
    const char* name; // last part of path or of the URL
    u64 size;
    bool install;
    receive_item_state state;
    Result result;
} receive_item;

struct receive_job_s {
    receive_state* shared;
    char from[RECEIVE_ADDRESS_MAX];

    bool links;
    u32 count;
    receive_item* items;
    char (*urls)[DOWNLOAD_URL_MAX];

    // Files.
    char folder[FILE_PATH_MAX]; // where saves go, ends in "/"
    bool replace;
    bool sensitive; // a save touches boot.firm, /luma/, or /Nintendo 3DS/
    u32 installs;
    u64 totalBytes;
    u64 saveBytes;

    // The batch's stream (files): the /data connection.
    int socket;
    bool broken; // the stream lost its place, so no later item can be read
    u64 itemRead;
    u8* drain;

    content_install content;
};

typedef struct {
    receive_state* shared;
    int fbi;
    int web;
    int udp;
    char head[RECEIVE_HEAD_MAX];
} receiver;

static void receiver_log(receive_state* state, u32 color, const char* format, ...) __attribute__((format(printf, 3, 4)));

static void receiver_log(receive_state* state, u32 color, const char* format, ...) {
    list_item* row = (list_item*) calloc(1, sizeof(list_item));
    if(row == NULL) {
        return;
    }

    va_list args;
    va_start(args, format);
    vsnprintf(row->name, LIST_ITEM_NAME_MAX, format, args);
    va_end(args);

    row->color = color;

    LightLock_Lock(&state->logLock);
    bool added = linked_list_add(&state->rows, row);
    LightLock_Unlock(&state->logLock);

    if(!added) {
        free(row);
    }
}

// Moves exactly length bytes. Stops on the worker's cancel, on the batch's cancel when batch is set,
// and after RECEIVE_STALL_MS without progress; a peer that closes early is an error.
static Result receiver_io(receive_state* state, int fd, void* buffer, size_t length, bool receive, bool batch) {
    size_t done = 0;
    u64 lastProgress = osGetTime();
    while(done < length) {
        int ret = receive ? recv(fd, (u8*) buffer + done, length - done, 0) : send(fd, (const u8*) buffer + done, length - done, 0);
        if(ret > 0) {
            done += ret;
            lastProgress = osGetTime();
            continue;
        }

        if(ret == 0 || errno != EAGAIN) {
            return R_APP_BAD_DATA;
        }

        if(task_is_cancelled(&state->state) || (batch && state->op.state.cancelled)) {
            return R_APP_CANCELLED;
        }

        if(osGetTime() - lastProgress >= RECEIVE_STALL_MS) {
            return R_APP_TIMED_OUT;
        }

        struct pollfd pfd = {.fd = fd, .events = receive ? POLLIN : POLLOUT};
        poll(&pfd, 1, RECEIVE_POLL_MS);
    }

    return 0;
}

// Reads a request head byte by byte, so the body stays in the socket for whoever reads it next.
static bool receiver_read_head(receiver* r, int fd, receive_request* request) {
    size_t size = 0;
    u64 start = osGetTime();
    while(true) {
        int ret = recv(fd, r->head + size, 1, 0);
        if(ret > 0) {
            size++;

            if(size >= 4 && memcmp(r->head + size - 4, "\r\n\r\n", 4) == 0) {
                return receive_parse_head(r->head, size, request) == RECEIVE_PARSE_DONE;
            }

            if(size >= RECEIVE_HEAD_MAX) {
                return false;
            }

            continue;
        }

        if(ret == 0 || errno != EAGAIN || task_is_cancelled(&r->shared->state)) {
            return false;
        }

        u64 waited = osGetTime() - start;
        if(waited >= RECEIVE_HEAD_MS || (size == 0 && waited >= RECEIVE_FIRST_BYTE_MS)) {
            return false;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        poll(&pfd, 1, RECEIVE_POLL_MS);
    }
}

// A bare close() with unsent data makes the 3DS reset the connection, cutting off the reply. Like
// ftpd's closeSocket: end our side, then read until the peer closes (or RECEIVE_CLOSE_MS passes).
static void receiver_close_client(receive_state* state, int fd) {
    shutdown(fd, SHUT_WR);

    char discard[512];
    u64 start = osGetTime();
    while(!task_is_cancelled(&state->state) && osGetTime() - start < RECEIVE_CLOSE_MS) {
        int ret = recv(fd, discard, sizeof(discard), 0);
        if(ret == 0 || (ret < 0 && errno != EAGAIN)) {
            break;
        }

        if(ret < 0) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            poll(&pfd, 1, RECEIVE_POLL_MS);
        }
    }

    close(fd);
}

static const char* receiver_reason(int status) {
    switch(status) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 403:
            return "Forbidden";
        case 404:
            return "Not Found";
        case 405:
            return "Method Not Allowed";
        case 409:
            return "Conflict";
        case 413:
            return "Payload Too Large";
        case 503:
            return "Service Unavailable";
        case 507:
            return "Insufficient Storage";
        default:
            return "Internal Server Error";
    }
}

static void receiver_respond(receive_state* state, int fd, int status, const char* type, const char* body, size_t length) {
    char head[256];
    int headLength = snprintf(head, sizeof(head),
                              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %u\r\nConnection: close\r\nCache-Control: no-store\r\n\r\n",
                              status, receiver_reason(status), type, (unsigned int) length);

    if(R_SUCCEEDED(receiver_io(state, fd, head, headLength, false, false)) && length > 0) {
        receiver_io(state, fd, (void*) body, length, false, false);
    }
}

// Sends json and releases it.
static void receiver_respond_json(receive_state* state, int fd, int status, json_t* json) {
    char* text = json != NULL ? json_dumps(json, JSON_COMPACT) : NULL;
    if(text != NULL) {
        receiver_respond(state, fd, status, "application/json", text, strlen(text));
    } else {
        receiver_respond(state, fd, 500, "text/plain", "", 0);
    }

    free(text);
    json_decref(json);
}

static void receiver_respond_error(receive_state* state, int fd, int status, const char* message) {
    receiver_respond_json(state, fd, status, json_pack("{s:s}", "error", message));
}

static const char* receiver_last_part(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static receive_job* receiver_new_job(receive_state* state, const char* from, u32 count) {
    receive_job* job = (receive_job*) calloc(1, sizeof(receive_job));
    if(job == NULL) {
        return NULL;
    }

    job->items = (receive_item*) calloc(count, sizeof(receive_item));
    if(job->items == NULL) {
        free(job);
        return NULL;
    }

    job->shared = state;
    job->count = count;
    job->socket = -1;
    string_copy(job->from, from, sizeof(job->from));

    return job;
}

static void receiver_free_job(receive_job* job) {
    if(job == NULL) {
        return;
    }

    for(u32 i = 0; i < job->count; i++) {
        free(job->items[i].path);
    }

    free(job->items);
    free(job->urls);
    free(job->drain);
    free(job);
}

static receive_job* receiver_links_job(receive_state* state, const char* from, const char* text) {
    char (*urls)[DOWNLOAD_URL_MAX] = (char (*)[DOWNLOAD_URL_MAX]) calloc(INSTALL_URLS_MAX, DOWNLOAD_URL_MAX);
    if(urls == NULL) {
        return NULL;
    }

    u32 count = action_parse_urls(text, urls, INSTALL_URLS_MAX);
    receive_job* job = count > 0 ? receiver_new_job(state, from, count) : NULL;
    if(job == NULL) {
        free(urls);
        return NULL;
    }

    job->links = true;
    job->urls = urls;

    for(u32 i = 0; i < count; i++) {
        job->items[i].name = receiver_last_part(urls[i]);
        job->items[i].install = true;
    }

    return job;
}

// Builds a files job from an offer. Returns the HTTP status: 200, or the error's status with error set.
static int receiver_files_job(receive_state* state, const char* from, json_t* root, receive_job** out, char* error, size_t errorSize) {
    json_t* files = json_object_get(root, "files");
    json_t* dest = json_object_get(root, "dest");
    json_t* replace = json_object_get(root, "replace");

    size_t count = json_array_size(files);
    if(!json_is_array(files) || count == 0 || count > RECEIVE_FILES_MAX) {
        snprintf(error, errorSize, "Send 1 to %d files at a time.", RECEIVE_FILES_MAX);
        return 400;
    }

    if((dest != NULL && !json_is_string(dest)) || (replace != NULL && !json_is_boolean(replace))) {
        snprintf(error, errorSize, "Bad destination or replace value.");
        return 400;
    }

    receive_job* job = receiver_new_job(state, from, count);
    if(job == NULL) {
        snprintf(error, errorSize, "Out of memory.");
        return 503;
    }

    int status = 400;
    if(!receive_join_path(job->folder, sizeof(job->folder), dest != NULL ? json_string_value(dest) : "/", NULL)) {
        snprintf(error, errorSize, "Bad destination folder.");
        goto fail;
    }

    job->replace = json_is_true(replace);

    for(u32 i = 0; i < count; i++) {
        json_t* file = json_array_get(files, i);
        const char* relative = json_string_value(json_object_get(file, "path"));
        json_t* size = json_object_get(file, "size");
        json_t* install = json_object_get(file, "install");

        if(relative == NULL || !json_is_integer(size) || (install != NULL && !json_is_boolean(install))) {
            snprintf(error, errorSize, "Bad file entry %lu.", i + 1);
            goto fail;
        }

        receive_item* item = &job->items[i];
        item->install = json_is_true(install);

        json_int_t bytes = json_integer_value(size);
        if(bytes < 0 || (u64) bytes > RECEIVE_FILE_SIZE_MAX || (item->install && bytes < 4)) {
            snprintf(error, errorSize, "%.200s: %s.", relative, bytes < 0 ? "bad size" : bytes < 4 ? "too small to install" : "over 4 GiB");
            goto fail;
        }

        item->size = (u64) bytes;

        // Installs check the name only; their destination comes from the content.
        char path[FILE_PATH_MAX];
        if(!receive_join_path(path, sizeof(path), item->install ? "/" : job->folder, relative)) {
            snprintf(error, errorSize, "Bad file name: %.200s", relative);
            goto fail;
        }

        item->path = strdup(item->install ? relative : path);
        if(item->path == NULL) {
            snprintf(error, errorSize, "Out of memory.");
            status = 503;
            goto fail;
        }

        item->name = receiver_last_part(item->path);
        job->totalBytes += item->size;

        if(item->install) {
            job->installs++;
            continue;
        }

        job->saveBytes += item->size;
        job->sensitive = job->sensitive || receive_path_sensitive(item->path);

        for(u32 j = 0; j < i; j++) {
            if(!job->items[j].install && strcasecmp(job->items[j].path, item->path) == 0) {
                snprintf(error, errorSize, "Sent twice: %.200s", relative);
                goto fail;
            }
        }
    }

    FS_ArchiveResource resource;
    if(R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD)) && (u64) resource.freeClusters * resource.clusterSize < job->saveBytes) {
        snprintf(error, errorSize, "Not enough free space on the SD card.");
        status = 507;
        goto fail;
    }

    *out = job;
    return 200;

fail:
    receiver_free_job(job);
    return status;
}

// Confirm prompt's top screen: the first item names.
static void receiver_draw_preview(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    receive_job* job = (receive_job*) data;

    char text[1024] = "";
    size_t pos = 0;
    for(u32 i = 0; i < job->count && i < RECEIVE_PREVIEW_MAX; i++) {
        if(i > 0) {
            string_append(text, sizeof(text), &pos, "\n");
        }

        string_append(text, sizeof(text), &pos, job->links ? job->urls[i] : job->items[i].path);
    }

    if(job->count > RECEIVE_PREVIEW_MAX) {
        char more[32];
        snprintf(more, sizeof(more), "\n+ %lu more", job->count - RECEIVE_PREVIEW_MAX);
        string_append(text, sizeof(text), &pos, more);
    }

    ui_draw_centered_text(text, x1, y1, x2, y2);
}

static bool receiver_confirm(receive_state* state, receive_job* job) {
    if(settings_get()->receiveAutoAccept && !job->sensitive) {
        return true;
    }

    char text[512];
    size_t pos = 0;
    text[0] = '\0';

    char line[FILE_PATH_MAX + 64];
    if(job->links) {
        snprintf(line, sizeof(line), "%s sends %lu link%s to install.", job->from, job->count, job->count == 1 ? "" : "s");
        string_append(text, sizeof(text), &pos, line);
    } else {
        u32 saves = job->count - job->installs;
        snprintf(line, sizeof(line), "%s sends %lu file%s (%.1f %s).\n", job->from, job->count, job->count == 1 ? "" : "s",
                 ui_get_display_size(job->totalBytes), ui_get_display_size_units(job->totalBytes));
        string_append(text, sizeof(text), &pos, line);

        if(saves == 0) {
            snprintf(line, sizeof(line), "Install %s.", job->installs == 1 ? "it" : "all");
        } else if(job->installs == 0) {
            snprintf(line, sizeof(line), "Save to %.200s%s.", job->folder, job->replace ? ", replacing files" : "");
        } else {
            snprintf(line, sizeof(line), "Install %lu, save %lu to %.200s%s.", job->installs, saves, job->folder, job->replace ? ", replacing files" : "");
        }

        string_append(text, sizeof(text), &pos, line);

        if(job->sensitive) {
            string_append(text, sizeof(text), &pos, "\nWrites to boot.firm, luma, or Nintendo 3DS.");
        }
    }

    string_append(text, sizeof(text), &pos, "\n\nAccept?");

    return prompt_wait_yes_no("Receive", text, job->sensitive ? COLOR_WARNING : COLOR_TEXT, job, receiver_draw_preview);
}

static Result receiver_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

static Result receiver_open_src(void* data, u32 index, u32* handle) {
    receive_job* job = (receive_job*) data;

    job->itemRead = 0;
    *handle = index;

    return job->broken ? R_APP_BAD_DATA : 0;
}

static Result receiver_get_src_size(void* data, u32 handle, u64* size) {
    *size = ((receive_job*) data)->items[handle].size;
    return 0;
}

static Result receiver_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    receive_job* job = (receive_job*) data;

    // The whole block: cia_install_begin reads the CIA header from the first one.
    u64 remaining = job->items[handle].size - offset;
    u32 length = remaining < size ? (u32) remaining : size;

    Result res = receiver_io(job->shared, job->socket, buffer, length, true, true);
    if(R_FAILED(res)) {
        job->broken = true;
        return res;
    }

    *bytesRead = length;
    job->itemRead = offset + length;

    return 0;
}

// Reads past what an item left unread (declined or failed), so the next item starts at its own bytes.
static Result receiver_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    receive_job* job = (receive_job*) data;
    receive_item* item = &job->items[index];

    if(job->shared->op.state.cancelled || task_is_quit_all()) {
        job->broken = true;
        return 0;
    }

    if(job->broken || job->itemRead >= item->size) {
        return 0;
    }

    job->shared->op.stage = "Skipping";

    while(job->itemRead < item->size) {
        u64 remaining = item->size - job->itemRead;
        u32 length = remaining < RECEIVE_DRAIN_SIZE ? (u32) remaining : RECEIVE_DRAIN_SIZE;
        if(R_FAILED(receiver_io(job->shared, job->socket, job->drain, length, true, true))) {
            job->broken = true;
            break;
        }

        job->itemRead += length;
    }

    return 0;
}

static Result receiver_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    string_copy(url, ((receive_job*) data)->urls[index], maxSize);
    return 0;
}

static Result receiver_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    receive_job* job = (receive_job*) data;
    receive_item* item = &job->items[index];

    job->shared->op.stage = "Preparing";

    if(!item->install) {
        return content_save_begin(&job->content, item->path, job->replace, handle);
    }

    char fileName[FILE_NAME_MAX];
    if(job->links) {
        string_get_path_file(fileName, job->urls[index], FILE_NAME_MAX);
    } else {
        string_copy(fileName, item->name, FILE_NAME_MAX);
    }

    return content_install_begin(&job->content, fileName, "", initialReadBlock, initialReadSize, NULL, NULL, handle);
}

static Result receiver_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    receive_job* job = (receive_job*) data;

    Result res = content_end(&job->content, &job->shared->op, succeeded, handle);
    if(succeeded && R_SUCCEEDED(res)) {
        job->items[index].state = RECEIVE_ITEM_DONE;
    }

    return res;
}

static Result receiver_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

// No error prompt per item: the log and the summary report failures. A broken stream stops the batch.
static bool receiver_error(void* data, u32 index, Result res, ui_view** errorView) {
    receive_job* job = (receive_job*) data;

    job->items[index].state = RECEIVE_ITEM_FAILED;
    job->items[index].result = res;

    return job->links || !job->broken;
}

static Result receiver_run(receive_state* state, receive_job* job) {
    if(!job->links) {
        job->drain = (u8*) malloc(RECEIVE_DRAIN_SIZE);
        if(job->drain == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }
    }

    data_op_data* op = &state->op;
    memset(op, 0, sizeof(*op));

    op->data = job;
    op->op = job->links ? DATAOP_DOWNLOAD : DATAOP_COPY;
    op->total = job->count;
    op->totalBytes = job->links ? 0 : job->totalBytes;
    op->bufferSize = RECEIVE_BUFFER_SIZE;
    op->copyEmpty = true;
    op->noRetry = true;

    op->isSrcDirectory = receiver_is_src_directory;
    op->openSrc = receiver_open_src;
    op->closeSrc = receiver_close_src;
    op->getSrcSize = receiver_get_src_size;
    op->readSrc = receiver_read_src;
    op->getSrcUrl = receiver_get_src_url;

    op->openDst = receiver_open_dst;
    op->closeDst = receiver_close_dst;
    op->writeDst = receiver_write_dst;

    op->error = receiver_error;

    op->state.finished = true;

    LightLock_Lock(&state->jobLock);
    state->job = job;
    LightLock_Unlock(&state->jobLock);

    Result res = task_data_op_run(op);

    LightLock_Lock(&state->jobLock);
    state->job = NULL;
    LightLock_Unlock(&state->jobLock);

    // Items the engine walked past without finishing or failing them were skipped.
    for(u32 i = 0; i < job->count; i++) {
        receive_item* item = &job->items[i];
        if(item->state != RECEIVE_ITEM_PENDING) {
            continue;
        }

        if(i < op->processed) {
            item->state = RECEIVE_ITEM_SKIPPED;
        } else if(i == op->processed && res == R_APP_CANCELLED) {
            item->state = RECEIVE_ITEM_CANCELLED;
        }
    }

    return res;
}

static json_t* receiver_results(receive_job* job) {
    json_t* results = json_array();
    for(u32 i = 0; i < job->count && results != NULL; i++) {
        receive_item* item = &job->items[i];

        char error[128] = "";
        switch(item->state) {
            case RECEIVE_ITEM_DONE:
                json_array_append_new(results, json_pack("{s:b}", "ok", 1));
                continue;
            case RECEIVE_ITEM_SKIPPED:
                json_array_append_new(results, json_pack("{s:b, s:b, s:s}", "ok", 0, "skipped", 1, "error", item->install ? "declined" : "exists"));
                continue;
            case RECEIVE_ITEM_FAILED:
                snprintf(error, sizeof(error), "%s (0x%08lX)", error_get_description(item->result), item->result);
                break;
            case RECEIVE_ITEM_CANCELLED:
                string_copy(error, "cancelled", sizeof(error));
                break;
            case RECEIVE_ITEM_PENDING:
                string_copy(error, "not received", sizeof(error));
                break;
        }

        json_array_append_new(results, json_pack("{s:b, s:s}", "ok", 0, "error", error));
    }

    return results;
}

// Log rows for every item, then one summary prompt (the engine already said when it was cancelled).
static void receiver_report(receive_state* state, receive_job* job, Result result) {
    u32 done = 0;
    u32 failed = 0;
    u32 missing = 0;
    const receive_item* firstFailure = NULL;

    for(u32 i = 0; i < job->count; i++) {
        const receive_item* item = &job->items[i];
        const char* shown = item->path != NULL ? item->path : item->name;

        switch(item->state) {
            case RECEIVE_ITEM_DONE:
                done++;
                receiver_log(state, COLOR_TEXT, "%s: %s", item->install ? "Installed" : "Saved", shown);
                break;
            case RECEIVE_ITEM_SKIPPED:
                receiver_log(state, COLOR_TEXT_DIM, "Skipped: %s%s", shown, item->install ? "" : " (exists)");
                break;
            case RECEIVE_ITEM_FAILED:
                failed++;
                firstFailure = firstFailure != NULL ? firstFailure : item;
                receiver_log(state, COLOR_WARNING, "Failed: %s (0x%08lX)", shown, item->result);
                break;
            case RECEIVE_ITEM_CANCELLED:
                receiver_log(state, COLOR_TEXT_DIM, "Cancelled: %s", shown);
                break;
            case RECEIVE_ITEM_PENDING:
                missing++;
                break;
        }
    }

    if(missing > 0) {
        receiver_log(state, COLOR_TEXT_DIM, "Not received: %lu more from %s", missing, job->from);
    }

    if(result == R_APP_CANCELLED) {
        return;
    }

    if(firstFailure != NULL) {
        error_display_res(NULL, NULL, firstFailure->result, "%lu of %lu failed. First:\n%.60s", failed, job->count, firstFailure->name);
        return;
    }

    // The prompt keeps the text pointer; one worker writes it.
    static char summary[128];
    char headline[64];
    snprintf(headline, sizeof(headline), "%s %lu of %lu.", job->links ? "Installed" : "Received", done, job->count);
    task_data_op_format_summary(&state->op, headline, summary, sizeof(summary));
    prompt_display_notify("Success", summary, COLOR_TEXT, NULL, NULL, NULL);
}

static void receiver_log_declined(receive_state* state, const receive_job* job) {
    receiver_log(state, COLOR_TEXT_DIM, "Declined: %lu %s from %s", job->count, job->links ? "links" : "files", job->from);
}

// FBI's protocol: a big-endian length, newline-separated URLs, then one byte back once they are done.
static void receiver_serve_fbi(receiver* r, int client, const char* from) {
    receive_state* state = r->shared;

    u32 length = 0;
    if(R_FAILED(receiver_io(state, client, &length, sizeof(length), true, false))) {
        return;
    }

    length = ntohl(length);
    if(length == 0 || length >= RECEIVE_FBI_PAYLOAD_MAX) {
        receiver_log(state, COLOR_WARNING, "Rejected: %lu bytes of URLs from %s", length, from);
        return;
    }

    char* text = (char*) calloc(length + 1, 1);
    if(text == NULL) {
        return;
    }

    receive_job* job = NULL;
    if(R_SUCCEEDED(receiver_io(state, client, text, length, true, false))) {
        job = receiver_links_job(state, from, text);
    }

    free(text);

    if(job == NULL) {
        return;
    }

    if(receiver_confirm(state, job)) {
        receiver_report(state, job, receiver_run(state, job));
    } else {
        receiver_log_declined(state, job);
    }

    u8 ack = 0;
    receiver_io(state, client, &ack, sizeof(ack), false, false);

    receiver_free_job(job);
}

// Joins the offer's links into the newline text action_parse_urls reads.
static char* receiver_links_text(json_t* links) {
    size_t count = json_array_size(links);
    if(!json_is_array(links) || count == 0 || count > INSTALL_URLS_MAX) {
        return NULL;
    }

    char* text = (char*) calloc(count, DOWNLOAD_URL_MAX);
    if(text == NULL) {
        return NULL;
    }

    size_t pos = 0;
    for(size_t i = 0; i < count; i++) {
        const char* link = json_string_value(json_array_get(links, i));
        size_t length = link != NULL ? strlen(link) : 0;
        if(length == 0 || length >= DOWNLOAD_URL_MAX - 1 || strchr(link, '\n') != NULL) {
            free(text);
            return NULL;
        }

        memcpy(text + pos, link, length);
        pos += length;
        text[pos++] = '\n';
    }

    return text;
}

// POST /offer. Links run here; an accepted files offer is returned (with its token) for the /data
// request that brings the bytes.
static receive_job* receiver_serve_offer(receiver* r, int client, const char* from, const receive_request* request, char* token) {
    receive_state* state = r->shared;

    if(!request->hasLength || request->contentLength == 0 || request->contentLength > RECEIVE_OFFER_MAX) {
        receiver_respond_error(state, client, 413, "Offer too large.");
        return NULL;
    }

    size_t length = (size_t) request->contentLength;
    char* body = (char*) malloc(length);
    if(body == NULL) {
        receiver_respond_error(state, client, 503, "Out of memory.");
        return NULL;
    }

    json_t* root = NULL;
    if(R_SUCCEEDED(receiver_io(state, client, body, length, true, false))) {
        json_error_t parseError;
        root = json_loadb(body, length, 0, &parseError);
    }

    free(body);

    if(!json_is_object(root)) {
        json_decref(root);
        receiver_respond_error(state, client, 400, "Bad offer.");
        return NULL;
    }

    receive_job* job = NULL;
    json_t* links = json_object_get(root, "links");
    if(links != NULL) {
        char* text = receiver_links_text(links);
        job = text != NULL ? receiver_links_job(state, from, text) : NULL;
        free(text);

        if(job == NULL) {
            json_decref(root);
            receiver_respond_error(state, client, 400, "Send 1 to 128 links, one per entry.");
            return NULL;
        }
    } else {
        char error[256];
        int status = receiver_files_job(state, from, root, &job, error, sizeof(error));
        if(status != 200) {
            json_decref(root);
            receiver_respond_error(state, client, status, error);
            return NULL;
        }
    }

    json_decref(root);

    if(!receiver_confirm(state, job)) {
        receiver_log_declined(state, job);
        receiver_respond_json(state, client, 200, json_pack("{s:b}", "accepted", 0));
        receiver_free_job(job);
        return NULL;
    }

    if(job->links) {
        receiver_report(state, job, receiver_run(state, job));
        receiver_respond_json(state, client, 200, json_pack("{s:b, s:o}", "accepted", 1, "results", receiver_results(job)));
        receiver_free_job(job);
        return NULL;
    }

    u32 value = (u32) (svcGetSystemTick() ^ (osGetTime() << 16));
    snprintf(token, RECEIVE_TOKEN_LENGTH + 1, "%08lx", value);

    receiver_respond_json(state, client, 200, json_pack("{s:b, s:s}", "accepted", 1, "token", token));
    return job;
}

// POST /data for the accepted job: the files' bytes back to back. Returns true when it ran the batch.
static bool receiver_serve_data(receiver* r, int client, const char* from, const receive_request* request, receive_job* job, const char* token) {
    receive_state* state = r->shared;

    if(job == NULL) {
        receiver_respond_error(state, client, 409, "No accepted offer.");
        return false;
    }

    if(strcmp(request->token, token) != 0 || strcmp(from, job->from) != 0) {
        receiver_respond_error(state, client, 403, "Wrong token or sender.");
        return false;
    }

    if(!request->hasLength || request->contentLength != job->totalBytes) {
        receiver_respond_error(state, client, 400, "Content-Length must equal the offered files' total size.");
        return false;
    }

    job->socket = client;
    Result res = receiver_run(state, job);
    job->socket = -1;

    // A cut or cancelled stream gets no answer: the sender sees the connection end.
    if(res != R_APP_CANCELLED && !job->broken) {
        receiver_respond_json(state, client, 200, json_pack("{s:o}", "results", receiver_results(job)));
    }

    receiver_report(state, job, res);
    return true;
}

static bool receiver_await_data(receiver* r, receive_job* job, const char* token);

// One HTTP request. waiting is the accepted files job whose /data is expected, or NULL. Returns true
// when it ran that job.
static bool receiver_serve_web(receiver* r, int client, const char* from, receive_job* waiting, const char* token) {
    receive_state* state = r->shared;

    receive_request request;
    if(!receiver_read_head(r, client, &request)) {
        close(client);
        return false;
    }

    bool ran = false;
    receive_job* accepted = NULL;
    char acceptedToken[RECEIVE_TOKEN_LENGTH + 1] = "";

    bool get = strcmp(request.method, "GET") == 0;
    bool post = strcmp(request.method, "POST") == 0;

    if(!request.hostAllowed) {
        receiver_respond_error(state, client, 403, "Open this page by the console's IP address.");
    } else if(get) {
        if(strcmp(request.path, "/") == 0) {
            receiver_respond(state, client, 200, "text/html; charset=utf-8", state->page, state->pageSize);
        } else {
            receiver_respond_error(state, client, 404, "Not found.");
        }
    } else if(!post) {
        receiver_respond_error(state, client, 405, "Method not allowed.");
    } else if(!request.ftbHeader) {
        receiver_respond_error(state, client, 403, "Missing X-FTB header.");
    } else if(strcmp(request.path, "/offer") == 0) {
        if(waiting != NULL) {
            receiver_respond_error(state, client, 503, "Busy with another sender.");
        } else {
            accepted = receiver_serve_offer(r, client, from, &request, acceptedToken);
        }
    } else if(strcmp(request.path, "/data") == 0) {
        ran = receiver_serve_data(r, client, from, &request, waiting, token);
    } else {
        receiver_respond_error(state, client, 404, "Not found.");
    }

    receiver_close_client(state, client);

    if(accepted != NULL) {
        if(!receiver_await_data(r, accepted, acceptedToken)) {
            receiver_log(state, COLOR_WARNING, "Not sent: %lu files from %s", accepted->count, accepted->from);
        }

        receiver_free_job(accepted);
    }

    return ran;
}

// Returns the client, -1 when none is waiting, or RECEIVE_NETWORK_GONE.
static int receiver_accept(int listener, char* from) {
    struct sockaddr_in address;
    socklen_t addressLength = sizeof(address);

    int client = accept(listener, (struct sockaddr*) &address, &addressLength);
    if(client < 0) {
        // newlib: 115 is ENETDOWN. Both mean Wi-Fi went away.
        return errno == EINVAL || errno == ENETDOWN ? RECEIVE_NETWORK_GONE : -1;
    }

    string_copy(from, inet_ntoa(address.sin_addr), RECEIVE_ADDRESS_MAX);

    int buffer = RECEIVE_SOCKET_BUFFER;
    setsockopt(client, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(client, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    fcntl(client, F_SETFL, fcntl(client, F_GETFL, 0) | O_NONBLOCK);

    return client;
}

// Waits for the accepted offer's /data, serving the page meanwhile. False when it never came.
static bool receiver_await_data(receiver* r, receive_job* job, const char* token) {
    u64 start = osGetTime();
    while(!task_is_cancelled(&r->shared->state) && osGetTime() - start < RECEIVE_DATA_WAIT_MS) {
        struct pollfd pfd = {.fd = r->web, .events = POLLIN};
        if(poll(&pfd, 1, RECEIVE_POLL_MS) <= 0) {
            continue;
        }

        char from[RECEIVE_ADDRESS_MAX];
        int client = receiver_accept(r->web, from);
        if(client == RECEIVE_NETWORK_GONE) {
            return false;
        }

        if(client >= 0 && receiver_serve_web(r, client, from, job, token)) {
            return true;
        }
    }

    return false;
}

static void receiver_answer_discovery(receiver* r) {
    char message[16];
    struct sockaddr_in address;
    socklen_t addressLength = sizeof(address);

    int length = recvfrom(r->udp, message, sizeof(message), 0, (struct sockaddr*) &address, &addressLength);
    if(length == 7 && memcmp(message, "ftbfind", 7) == 0 && r->shared->webPort != 0) {
        char reply[16];
        int replyLength = snprintf(reply, sizeof(reply), "ftbhere %u", r->shared->webPort);
        sendto(r->udp, reply, replyLength, 0, (struct sockaddr*) &address, addressLength);
    }
}

static int receiver_open_socket(int type, u16 port) {
    int fd = socket(AF_INET, type, 0);
    if(fd < 0) {
        return -1;
    }

    int reuse = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if(bind(fd, (struct sockaddr*) &address, sizeof(address)) < 0 || (type == SOCK_STREAM && listen(fd, RECEIVE_BACKLOG) < 0)
       || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void receiver_close_socket(int* fd) {
    if(*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void receiver_close_listeners(receiver* r) {
    receiver_close_socket(&r->fbi);
    receiver_close_socket(&r->web);
    receiver_close_socket(&r->udp);

    r->shared->fbiListening = false;
    r->shared->webPort = 0;
}

static void receiver_open_listeners(receiver* r) {
    u16 webPort = RECEIVE_WEB_PORT;
    r->web = receiver_open_socket(SOCK_STREAM, webPort);
    if(r->web < 0) {
        webPort = RECEIVE_WEB_FALLBACK_PORT;
        r->web = receiver_open_socket(SOCK_STREAM, webPort);
    }

    r->fbi = receiver_open_socket(SOCK_STREAM, RECEIVE_FBI_PORT);
    r->udp = receiver_open_socket(SOCK_DGRAM, RECEIVE_DISCOVERY_PORT);

    r->shared->fbiListening = r->fbi >= 0;
    r->shared->webPort = r->web >= 0 ? webPort : 0;
}

static Result receiver_thread(void* arg) {
    receiver* r = (receiver*) arg;
    receive_state* state = r->shared;

    u64 lastIpCheck = 0;
    while(!task_is_cancelled(&state->state)) {
        // Wi-Fi can drop, come back, or change address while the screen is open.
        if(osGetTime() - lastIpCheck >= RECEIVE_IP_CHECK_MS) {
            lastIpCheck = osGetTime();

            u32 ip = (u32) gethostid();
            if(ip != state->ip) {
                receiver_close_listeners(r);
                state->ip = ip;
            }

            if(ip != 0 && r->fbi < 0 && r->web < 0 && r->udp < 0) {
                receiver_open_listeners(r);
            }
        }

        struct pollfd fds[3];
        nfds_t count = 0;
        int listeners[3] = {r->udp, r->fbi, r->web};
        for(u32 i = 0; i < 3; i++) {
            if(listeners[i] >= 0) {
                fds[count++] = (struct pollfd) {.fd = listeners[i], .events = POLLIN};
            }
        }

        if(count == 0) {
            svcSleepThread(RECEIVE_POLL_MS * 1000000LL);
            continue;
        }

        if(poll(fds, count, RECEIVE_POLL_MS) <= 0) {
            continue;
        }

        for(nfds_t i = 0; i < count; i++) {
            if(!(fds[i].revents & POLLIN)) {
                continue;
            }

            if(fds[i].fd == r->udp) {
                receiver_answer_discovery(r);
                continue;
            }

            char from[RECEIVE_ADDRESS_MAX];
            int client = receiver_accept(fds[i].fd, from);
            if(client == RECEIVE_NETWORK_GONE) {
                receiver_close_listeners(r);
                state->ip = 0;
                break;
            }

            if(client < 0) {
                continue;
            }

            if(fds[i].fd == r->fbi) {
                receiver_serve_fbi(r, client, from);
                receiver_close_client(state, client);
            } else {
                receiver_serve_web(r, client, from, NULL, NULL);
            }

            // Serving may have taken long enough for the listeners to change.
            break;
        }
    }

    receiver_close_listeners(r);
    free(r);

    return 0;
}

Result receiver_start(receive_state* state) {
    LightLock_Init(&state->jobLock);
    LightLock_Init(&state->logLock);
    linked_list_init(&state->rows);
    state->op.state.finished = true;

    receiver* r = (receiver*) calloc(1, sizeof(receiver));
    if(r == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    r->shared = state;
    r->fbi = -1;
    r->web = -1;
    r->udp = -1;

    Result res = task_start(&state->state, receiver_thread, r, 0x18, 1);
    if(R_FAILED(res)) {
        free(r);
    }

    return res;
}

const char* receiver_job_from(const receive_job* job) {
    return job->from;
}

const char* receiver_job_item_name(const receive_job* job, u32 index) {
    if(index >= job->count) {
        return "";
    }

    const receive_item* item = &job->items[index];
    return item->path != NULL ? item->path : item->name;
}
