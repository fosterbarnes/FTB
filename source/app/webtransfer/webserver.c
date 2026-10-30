#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

#include <3ds.h>
#include <jansson.h>

#include "webproto.h"
#include "webserver.h"
#include "webtls.h"
#include "../paths.h"
#include "../resources.h"
#include "../settings.h"
#include "../action/action.h"
#include "../action/contentinstall.h"
#include "../../core/core.h"

#define WEB_PORT 80
#define WEB_FALLBACK_PORT 8080
#define WEB_TLS_PORT 443
#define WEB_BACKLOG 4

#define WEB_POLL_MS 100
#define WEB_IP_CHECK_MS 1000
// Browsers open spare connections they may never use; one must not hold up the next request.
#define WEB_FIRST_BYTE_MS 1000
#define WEB_HEAD_MS 5000
#define WEB_STALL_MS 10000
#define WEB_DATA_WAIT_MS 30000
#define WEB_CLOSE_MS 2000

#define WEB_SOCKET_BUFFER 32768 // ftpd's tuned 3DS value
#define WEB_BUFFER_SIZE (128 * 1024)
#define WEB_DRAIN_SIZE (32 * 1024)
#define WEB_BODY_MAX (256 * 1024) // JSON request bodies (offers, checks)
#define WEB_FILES_MAX 1000
#define WEB_LIST_MAX 4000
#define WEB_FILE_SIZE_MAX 0xFFFFFFFFULL // FAT32
#define WEB_ADDRESS_MAX 16
#define WEB_DEVICES_MAX 16
#define WEB_REMEMBERED_MAX 32 // the oldest is dropped for a new one
#define WEB_PREVIEW_MAX 8
#define WEB_NETWORK_GONE (-2)

typedef enum {
    WEB_ITEM_PENDING, // not reached
    WEB_ITEM_DONE,
    WEB_ITEM_SKIPPED, // declined prompt, or the file exists without replace
    WEB_ITEM_FAILED,
    WEB_ITEM_CANCELLED
} web_item_state;

typedef struct {
    // The SD path (saves and downloads) or the sender's relative path (file installs); NULL for
    // installed links.
    char* path;
    const char* name; // last part of path or of the URL
    u64 size;
    bool install;
    web_item_state state;
    Result result;
} web_item;

typedef enum {
    WEB_JOB_FILES, // an upload whose bytes come on /data
    WEB_JOB_LINKS, // links the 3DS downloads itself
    WEB_JOB_DOWNLOAD // one SD file sent to the browser (GET /file)
} web_job_kind;

struct web_job_s {
    web_state* shared;
    char from[WEB_ADDRESS_MAX]; // the peer
    web_job_kind kind;

    u32 count;
    web_item* items;
    char (*urls)[DOWNLOAD_URL_MAX]; // links

    // Uploads.
    char folder[FILE_PATH_MAX]; // where saves go, ends in "/"
    bool replace;
    bool sensitive; // a save touches boot.firm, /luma/, or /Nintendo 3DS/
    // "Allow all" or a remembered browser: no 3DS prompts but the red ones (sensitive saves, system
    // titles) and failures.
    bool trusted;
    u32 installs;
    u64 totalBytes;
    u64 saveBytes;

    // The stream: the /data connection (files) or the /file connection (download).
    int socket;
    bool broken; // files: the stream lost its place, so no later item can be read
    u64 itemRead;
    u8* drain;

    Handle file; // download: the SD file being sent
    bool headSent; // download: the response head went out, so no error answer can follow

    content_install content;
};

typedef struct {
    web_state* shared;
    int web;
    int https; // redirects to the page; only open while the page is served
    web_tls* tls; // NULL when TLS failed to start
    char head[WEB_HEAD_MAX];

    // Devices answered this session (IPv4, network order). Not saved: home network addresses change
    // hands, so an address approval only lasts until the screen closes.
    u32 approved[WEB_DEVICES_MAX];
    u32 approvedCount;
    u32 declined[WEB_DEVICES_MAX];
    u32 declinedCount;

    // Browsers approved with "Ask once" (their ftbDevice cookie), oldest first; PATHS_WEB_DEVICES.
    char remembered[WEB_REMEMBERED_MAX][WEB_DEVICE_KEY_LENGTH + 1];
    u32 rememberedCount;

    // GET /file requests answered, and how many of them did not send the whole file (GET /status).
    u32 downloads;
    u32 downloadsFailed;
} webserver;

// One accepted connection.
typedef struct {
    int socket;
    u32 address; // IPv4, network order
    char from[WEB_ADDRESS_MAX]; // the address as text
} web_client;

typedef enum {
    WEB_ROUTE_LIST,
    WEB_ROUTE_FILE,
    WEB_ROUTE_STATUS,
    WEB_ROUTE_CHECK,
    WEB_ROUTE_OFFER,
    WEB_ROUTE_DATA
} web_route;

// Every route but GET / needs the device approved (webserver_check_device).
static const struct {
    const char* path;
    bool post;
    // X-FTB: 1, which other sites cannot send. /file is opened by a download link, which cannot add
    // headers; another site can only start a download onto its visitor's own device, never read it.
    bool ftbHeader;
    web_route route;
} webserver_routes[] = {
        {"/list", false, true, WEB_ROUTE_LIST},
        {"/file", false, false, WEB_ROUTE_FILE},
        {"/status", false, true, WEB_ROUTE_STATUS},
        {"/check", true, true, WEB_ROUTE_CHECK},
        {"/offer", true, true, WEB_ROUTE_OFFER},
        {"/data", true, true, WEB_ROUTE_DATA},
};

#define WEB_ROUTE_COUNT (sizeof(webserver_routes) / sizeof(webserver_routes[0]))

static void webserver_log(web_state* state, u32 color, const char* format, ...) __attribute__((format(printf, 3, 4)));

static void webserver_log(web_state* state, u32 color, const char* format, ...) {
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

// Moves exactly length bytes. Stops on the worker's cancel, on the transfer's cancel when batch is
// set, and after WEB_STALL_MS without progress; a peer that closes early is an error.
static Result webserver_io(web_state* state, int fd, void* buffer, size_t length, bool reading, bool batch) {
    size_t done = 0;
    u64 lastProgress = osGetTime();
    while(done < length) {
        int ret = reading ? recv(fd, (u8*) buffer + done, length - done, 0) : send(fd, (const u8*) buffer + done, length - done, 0);
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

        if(osGetTime() - lastProgress >= WEB_STALL_MS) {
            return R_APP_TIMED_OUT;
        }

        struct pollfd pfd = {.fd = fd, .events = reading ? POLLIN : POLLOUT};
        poll(&pfd, 1, WEB_POLL_MS);
    }

    return 0;
}

// Reads a request head byte by byte, so the body stays in the socket for whoever reads it next.
static bool webserver_read_head(webserver* w, int fd, web_request* request) {
    size_t size = 0;
    u64 start = osGetTime();
    while(true) {
        int ret = recv(fd, w->head + size, 1, 0);
        if(ret > 0) {
            size++;

            if(size >= 4 && memcmp(w->head + size - 4, "\r\n\r\n", 4) == 0) {
                return web_parse_head(w->head, size, request) == WEB_PARSE_DONE;
            }

            if(size >= WEB_HEAD_MAX) {
                return false;
            }

            continue;
        }

        if(ret == 0 || errno != EAGAIN || task_is_cancelled(&w->shared->state)) {
            return false;
        }

        u64 waited = osGetTime() - start;
        if(waited >= WEB_HEAD_MS || (size == 0 && waited >= WEB_FIRST_BYTE_MS)) {
            return false;
        }

        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        poll(&pfd, 1, WEB_POLL_MS);
    }
}

// A bare close() with unsent data makes the 3DS reset the connection, cutting off the reply. Like
// ftpd's closeSocket: end our side, then read until the peer closes (or WEB_CLOSE_MS passes).
static void webserver_close_client(web_state* state, int fd) {
    shutdown(fd, SHUT_WR);

    char discard[512];
    u64 start = osGetTime();
    while(!task_is_cancelled(&state->state) && osGetTime() - start < WEB_CLOSE_MS) {
        int ret = recv(fd, discard, sizeof(discard), 0);
        if(ret == 0 || (ret < 0 && errno != EAGAIN)) {
            break;
        }

        if(ret < 0) {
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            poll(&pfd, 1, WEB_POLL_MS);
        }
    }

    close(fd);
}

static const char* webserver_reason(int status) {
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

// A response head for a body of length bytes; disposition is a Content-Disposition value or NULL.
static Result webserver_send_head(web_state* state, int fd, int status, const char* type, u64 length, const char* disposition) {
    char head[WEB_DISPOSITION_MAX + 256];
    int headLength = snprintf(head, sizeof(head),
                              "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n%s%s%sConnection: close\r\nCache-Control: no-store\r\n\r\n",
                              status, webserver_reason(status), type, (unsigned long long) length,
                              disposition != NULL ? "Content-Disposition: " : "", disposition != NULL ? disposition : "", disposition != NULL ? "\r\n" : "");
    if(headLength < 0 || headLength >= (int) sizeof(head)) {
        return R_APP_OUT_OF_RANGE;
    }

    return webserver_io(state, fd, head, headLength, false, false);
}

static void webserver_respond(web_state* state, int fd, int status, const char* type, const char* body, size_t length) {
    if(R_SUCCEEDED(webserver_send_head(state, fd, status, type, length, NULL)) && length > 0) {
        webserver_io(state, fd, (void*) body, length, false, false);
    }
}

// Sends json and releases it.
static void webserver_respond_json(web_state* state, int fd, int status, json_t* json) {
    char* text = json != NULL ? json_dumps(json, JSON_COMPACT) : NULL;
    if(text != NULL) {
        webserver_respond(state, fd, status, "application/json", text, strlen(text));
    } else {
        webserver_respond(state, fd, 500, "text/plain", "", 0);
    }

    free(text);
    json_decref(json);
}

static void webserver_respond_error(web_state* state, int fd, int status, const char* message) {
    webserver_respond_json(state, fd, status, json_pack("{s:s}", "error", message));
}

static bool webserver_has_device(const u32* devices, u32 count, u32 address) {
    for(u32 i = 0; i < count; i++) {
        if(devices[i] == address) {
            return true;
        }
    }

    return false;
}

static bool webserver_is_remembered(const webserver* w, const char* key) {
    for(u32 i = 0; i < w->rememberedCount; i++) {
        if(strcmp(w->remembered[i], key) == 0) {
            return true;
        }
    }

    return false;
}

static void webserver_load_devices(webserver* w) {
    FILE* fd = fopen(PATHS_WEB_DEVICES, "r");
    if(fd == NULL) {
        return;
    }

    // One key per line; anything else is skipped.
    char line[64];
    while(w->rememberedCount < WEB_REMEMBERED_MAX && fgets(line, sizeof(line), fd) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        if(web_device_key_valid(line, strlen(line)) && !webserver_is_remembered(w, line)) {
            string_copy(w->remembered[w->rememberedCount++], line, WEB_DEVICE_KEY_LENGTH + 1);
        }
    }

    fclose(fd);
}

static bool webserver_write_devices(FILE* fd, void* data) {
    const webserver* w = (const webserver*) data;

    bool written = true;
    for(u32 i = 0; i < w->rememberedCount && written; i++) {
        written = fprintf(fd, "%s\n", w->remembered[i]) > 0;
    }

    return written;
}

// False when saving failed; the key is still remembered until the screen closes.
static bool webserver_remember_device(webserver* w, const char* key) {
    if(w->rememberedCount == WEB_REMEMBERED_MAX) {
        memmove(w->remembered[0], w->remembered[1], sizeof(w->remembered[0]) * (WEB_REMEMBERED_MAX - 1));
        w->rememberedCount--;
    }

    string_copy(w->remembered[w->rememberedCount++], key, WEB_DEVICE_KEY_LENGTH + 1);

    paths_ensure_data_dir();
    return fs_write_file_safe(PATHS_WEB_DEVICES, webserver_write_devices, w);
}

// "Allow all", or "Ask once" with a browser that was already remembered (this session's yes included).
static bool webserver_is_trusted(const webserver* w, const web_request* request) {
    u8 mode = settings_get()->webApproveDevices;
    return mode == WEB_APPROVE_NONE || (mode == WEB_APPROVE_REMEMBER && request->device[0] != '\0' && webserver_is_remembered(w, request->device));
}

bool webserver_forget_devices() {
    return remove(PATHS_WEB_DEVICES) == 0 || errno == ENOENT;
}

// Every worker prompt goes through here. B with no job running closes the screen, which then waits
// for this worker; a prompt pushed after that could never be answered. The screen decides under
// jobLock, so a prompt starts only while the screen is open, and the screen stays open until it ends.
// False (no) when the screen is closing.
static bool webserver_ask(web_state* state, const char* text, u32 color, void* data,
                          void (*drawTop)(ui_view* view, void* data, float x1, float y1, float x2, float y2)) {
    LightLock_Lock(&state->jobLock);
    bool open = !state->state.cancelled && !task_is_quit_all();
    state->prompting = open;
    LightLock_Unlock(&state->jobLock);

    if(!open) {
        return false;
    }

    bool yes = prompt_wait_yes_no("3DS Web Server", text, color, data, drawTop);

    LightLock_Lock(&state->jobLock);
    state->prompting = false;
    LightLock_Unlock(&state->jobLock);

    return yes;
}

// Unless the setting is "Allow all", a device's first API request asks on the 3DS (the request waits
// on the prompt). With "Ask once", a browser that sent its device key is remembered after a yes.
// True when the client may go on; otherwise it has been answered.
static bool webserver_check_device(webserver* w, const web_client* client, const web_request* request) {
    web_state* state = w->shared;
    u8 mode = settings_get()->webApproveDevices;
    bool remember = mode == WEB_APPROVE_REMEMBER && request->device[0] != '\0';

    if(mode == WEB_APPROVE_NONE || webserver_has_device(w->approved, w->approvedCount, client->address)
       || (remember && webserver_is_remembered(w, request->device))) {
        return true;
    }

    // Declined devices stay declined until the screen closes, so a page that retries cannot keep
    // putting the prompt back up.
    if(webserver_has_device(w->declined, w->declinedCount, client->address)) {
        webserver_respond_error(state, client->socket, 403, "Declined on the 3DS. Reopen 3DS Web Server there to ask again.");
        return false;
    }

    if(w->approvedCount == WEB_DEVICES_MAX || w->declinedCount == WEB_DEVICES_MAX) {
        webserver_respond_error(state, client->socket, 503, "Too many devices. Reopen 3DS Web Server on the 3DS.");
        return false;
    }

    char text[160];
    snprintf(text, sizeof(text),
             "Allow %s to use\n"
             "3DS Web Server?\n"
             "\n"
             "It can browse, download, and send\n"
             "files %s.",
             client->from, remember ? "from now on" : "until this screen closes");

    if(webserver_ask(state, text, COLOR_TEXT, NULL, NULL)) {
        w->approved[w->approvedCount++] = client->address;
        if(!remember) {
            webserver_log(state, COLOR_TEXT, "Allowed: %s", client->from);
        } else if(webserver_remember_device(w, request->device)) {
            webserver_log(state, COLOR_TEXT, "Allowed and remembered: %s", client->from);
        } else {
            webserver_log(state, COLOR_WARNING, "Allowed: %s (saving webDevices.cfg failed)", client->from);
        }

        return true;
    }

    w->declined[w->declinedCount++] = client->address;
    webserver_log(state, COLOR_TEXT_DIM, "Declined: %s until 3DS Web Server closes", client->from);
    webserver_respond_error(state, client->socket, 403, "Declined on the 3DS.");
    return false;
}

// Reads a JSON object body. NULL after answering the error itself.
static json_t* webserver_read_json(web_state* state, int fd, const web_request* request) {
    if(!request->hasLength || request->contentLength == 0 || request->contentLength > WEB_BODY_MAX) {
        webserver_respond_error(state, fd, 413, "Request too large.");
        return NULL;
    }

    size_t length = (size_t) request->contentLength;
    char* body = (char*) malloc(length);
    if(body == NULL) {
        webserver_respond_error(state, fd, 503, "Out of memory.");
        return NULL;
    }

    json_t* root = NULL;
    if(R_SUCCEEDED(webserver_io(state, fd, body, length, true, false))) {
        json_error_t parseError;
        root = json_loadb(body, length, 0, &parseError);
    }

    free(body);

    if(!json_is_object(root)) {
        json_decref(root);
        webserver_respond_error(state, fd, 400, "Bad request body.");
        return NULL;
    }

    return root;
}

static const char* webserver_last_part(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash != NULL && slash[1] != '\0' ? slash + 1 : path;
}

static web_job* webserver_new_job(web_state* state, const char* from, web_job_kind kind, u32 count) {
    web_job* job = (web_job*) calloc(1, sizeof(web_job));
    if(job == NULL) {
        return NULL;
    }

    job->items = (web_item*) calloc(count, sizeof(web_item));
    if(job->items == NULL) {
        free(job);
        return NULL;
    }

    job->shared = state;
    job->kind = kind;
    job->count = count;
    job->socket = -1;
    string_copy(job->from, from, sizeof(job->from));

    return job;
}

static void webserver_free_job(web_job* job) {
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

// A save's SD path for item, checked against the batch's earlier saves. Returns the HTTP status: 200,
// or the error's status with error set.
static int webserver_add_save(web_job* job, web_item* item, const char* path, const char* shown, char* error, size_t errorSize) {
    for(u32 i = 0; i < job->count && &job->items[i] != item; i++) {
        if(job->items[i].path != NULL && !job->items[i].install && strcasecmp(job->items[i].path, path) == 0) {
            snprintf(error, errorSize, "Sent twice: %.200s", shown);
            return 400;
        }
    }

    item->path = strdup(path);
    if(item->path == NULL) {
        snprintf(error, errorSize, "Out of memory.");
        return 503;
    }

    item->name = webserver_last_part(item->path);
    job->sensitive = job->sensitive || web_path_sensitive(item->path);

    return 200;
}

// Joins the offer's links into the newline text action_parse_urls reads.
static char* webserver_links_text(json_t* links) {
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

// Builds a links job from an offer: installed, or saved to dest under each URL's file name. Returns
// the HTTP status: 200, or the error's status with error set.
static int webserver_links_job(web_state* state, const char* from, json_t* root, web_job** out, char* error, size_t errorSize) {
    json_t* dest = json_object_get(root, "dest");
    json_t* replace = json_object_get(root, "replace");
    json_t* install = json_object_get(root, "install");

    if((dest != NULL && !json_is_string(dest)) || (replace != NULL && !json_is_boolean(replace)) || (install != NULL && !json_is_boolean(install))) {
        snprintf(error, errorSize, "Bad destination, replace, or install value.");
        return 400;
    }

    char* text = webserver_links_text(json_object_get(root, "links"));
    if(text == NULL) {
        snprintf(error, errorSize, "Send 1 to %d links, one per entry.", INSTALL_URLS_MAX);
        return 400;
    }

    char (*urls)[DOWNLOAD_URL_MAX] = (char (*)[DOWNLOAD_URL_MAX]) calloc(INSTALL_URLS_MAX, DOWNLOAD_URL_MAX);
    u32 count = urls != NULL ? action_parse_urls(text, urls, INSTALL_URLS_MAX) : 0;
    free(text);

    if(urls != NULL && count == 0) {
        free(urls);
        snprintf(error, errorSize, "Send 1 to %d links, one per entry.", INSTALL_URLS_MAX);
        return 400;
    }

    web_job* job = urls != NULL ? webserver_new_job(state, from, WEB_JOB_LINKS, count) : NULL;
    if(job == NULL) {
        free(urls);
        snprintf(error, errorSize, "Out of memory.");
        return 503;
    }

    job->urls = urls;
    job->replace = json_is_true(replace);
    job->installs = json_is_true(install) ? count : 0;

    int status = 400;
    if(!web_join_path(job->folder, sizeof(job->folder), dest != NULL ? json_string_value(dest) : "/", NULL)) {
        snprintf(error, errorSize, "Bad destination folder.");
        goto fail;
    }

    for(u32 i = 0; i < count; i++) {
        web_item* item = &job->items[i];
        if(job->installs > 0) {
            item->install = true;
            item->name = webserver_last_part(urls[i]);
            continue;
        }

        char name[FILE_PATH_MAX];
        char path[FILE_PATH_MAX];
        if(!web_url_file_name(name, sizeof(name), urls[i]) || !web_join_path(path, sizeof(path), job->folder, name)) {
            snprintf(error, errorSize, "No usable file name in %.200s", urls[i]);
            goto fail;
        }

        // A 200 kept in status would make a later failure return 200 with *out unset.
        int added = webserver_add_save(job, item, path, name, error, errorSize);
        if(added != 200) {
            status = added;
            goto fail;
        }
    }

    *out = job;
    return 200;

fail:
    webserver_free_job(job);
    return status;
}

// Builds a files job from an offer. Returns the HTTP status: 200, or the error's status with error set.
static int webserver_files_job(web_state* state, const char* from, json_t* root, web_job** out, char* error, size_t errorSize) {
    json_t* files = json_object_get(root, "files");
    json_t* dest = json_object_get(root, "dest");
    json_t* replace = json_object_get(root, "replace");

    size_t count = json_array_size(files);
    if(!json_is_array(files) || count == 0 || count > WEB_FILES_MAX) {
        snprintf(error, errorSize, "Send 1 to %d files at a time.", WEB_FILES_MAX);
        return 400;
    }

    if((dest != NULL && !json_is_string(dest)) || (replace != NULL && !json_is_boolean(replace))) {
        snprintf(error, errorSize, "Bad destination or replace value.");
        return 400;
    }

    web_job* job = webserver_new_job(state, from, WEB_JOB_FILES, count);
    if(job == NULL) {
        snprintf(error, errorSize, "Out of memory.");
        return 503;
    }

    int status = 400;
    if(!web_join_path(job->folder, sizeof(job->folder), dest != NULL ? json_string_value(dest) : "/", NULL)) {
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

        web_item* item = &job->items[i];
        item->install = json_is_true(install);

        json_int_t bytes = json_integer_value(size);
        if(bytes < 0 || (u64) bytes > WEB_FILE_SIZE_MAX || (item->install && bytes < 4)) {
            snprintf(error, errorSize, "%.200s: %s.", relative, bytes < 0 ? "bad size" : bytes < 4 ? "too small to install" : "over 4 GiB");
            goto fail;
        }

        item->size = (u64) bytes;
        job->totalBytes += item->size;

        // Installs check the name only; their destination comes from the content.
        char path[FILE_PATH_MAX];
        if(!web_join_path(path, sizeof(path), item->install ? "/" : job->folder, relative)) {
            snprintf(error, errorSize, "Bad file name: %.200s", relative);
            goto fail;
        }

        if(item->install) {
            item->path = strdup(relative);
            if(item->path == NULL) {
                snprintf(error, errorSize, "Out of memory.");
                status = 503;
                goto fail;
            }

            item->name = webserver_last_part(item->path);
            job->installs++;
            continue;
        }

        int added = webserver_add_save(job, item, path, relative, error, errorSize);
        if(added != 200) {
            status = added;
            goto fail;
        }

        job->saveBytes += item->size;
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
    webserver_free_job(job);
    return status;
}

// Confirm prompt's top screen: the first item names.
static void webserver_draw_preview(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    web_job* job = (web_job*) data;

    char text[1024] = "";
    size_t pos = 0;
    for(u32 i = 0; i < job->count && i < WEB_PREVIEW_MAX; i++) {
        if(i > 0) {
            string_append(text, sizeof(text), &pos, "\n");
        }

        string_append(text, sizeof(text), &pos, job->kind == WEB_JOB_LINKS ? job->urls[i] : job->items[i].path);
    }

    if(job->count > WEB_PREVIEW_MAX) {
        char more[32];
        snprintf(more, sizeof(more), "\n+ %lu more", job->count - WEB_PREVIEW_MAX);
        string_append(text, sizeof(text), &pos, more);
    }

    ui_draw_centered_text(text, x1, y1, x2, y2);
}

// Install prompts' top screen (lock, New 3DS, reinstall): the item being installed.
static void webserver_draw_item(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    web_job* job = (web_job*) data;

    u32 index = job->shared->op.processed;
    if(index < job->count) {
        ui_draw_centered_text(job->kind == WEB_JOB_LINKS ? job->urls[index] : job->items[index].path, x1, y1, x2, y2);
    }
}

static bool webserver_confirm(web_state* state, web_job* job) {
    if((settings_get()->webAutoAccept || job->trusted) && !job->sensitive) {
        return true;
    }

    char text[512];
    size_t pos = 0;
    text[0] = '\0';

    char line[FILE_PATH_MAX + 64];
    if(job->kind == WEB_JOB_LINKS) {
        snprintf(line, sizeof(line), "%s sends %lu link%s.\n", job->from, job->count, job->count == 1 ? "" : "s");
    } else {
        snprintf(line, sizeof(line), "%s sends %lu file%s (%.1f %s).\n", job->from, job->count, job->count == 1 ? "" : "s",
                 ui_get_display_size(job->totalBytes), ui_get_display_size_units(job->totalBytes));
    }

    string_append(text, sizeof(text), &pos, line);

    u32 saves = job->count - job->installs;
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

    string_append(text, sizeof(text), &pos, "\n\nAccept?");

    return webserver_ask(state, text, job->sensitive ? COLOR_WARNING : COLOR_TEXT, job, webserver_draw_preview);
}

static Result webserver_is_src_directory(void* data, u32 index, bool* isDirectory) {
    *isDirectory = false;
    return 0;
}

// Uploads: the sources are the /data stream, one item after another.

static Result webserver_open_src(void* data, u32 index, u32* handle) {
    web_job* job = (web_job*) data;

    job->itemRead = 0;
    *handle = index;

    return job->broken ? R_APP_BAD_DATA : 0;
}

static Result webserver_get_src_size(void* data, u32 handle, u64* size) {
    *size = ((web_job*) data)->items[handle].size;
    return 0;
}

static Result webserver_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    web_job* job = (web_job*) data;

    // The whole block: cia_install_begin reads the CIA header from the first one.
    u64 remaining = job->items[handle].size - offset;
    u32 length = remaining < size ? (u32) remaining : size;

    Result res = webserver_io(job->shared, job->socket, buffer, length, true, true);
    if(R_FAILED(res)) {
        job->broken = true;
        return res;
    }

    *bytesRead = length;
    job->itemRead = offset + length;

    return 0;
}

// Reads past what an item left unread (declined or failed), so the next item starts at its own bytes.
static Result webserver_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    web_job* job = (web_job*) data;
    web_item* item = &job->items[index];

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
        u32 length = remaining < WEB_DRAIN_SIZE ? (u32) remaining : WEB_DRAIN_SIZE;
        if(R_FAILED(webserver_io(job->shared, job->socket, job->drain, length, true, true))) {
            job->broken = true;
            break;
        }

        job->itemRead += length;
    }

    return 0;
}

static Result webserver_get_src_url(void* data, u32 index, char* url, size_t maxSize) {
    string_copy(url, ((web_job*) data)->urls[index], maxSize);
    return 0;
}

static Result webserver_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    web_job* job = (web_job*) data;
    web_item* item = &job->items[index];

    job->shared->op.stage = "Preparing";

    if(!item->install) {
        return content_save_begin(&job->content, item->path, job->replace, handle);
    }

    char fileName[FILE_NAME_MAX];
    if(job->kind == WEB_JOB_LINKS) {
        string_get_path_file(fileName, job->urls[index], FILE_NAME_MAX);
    } else {
        string_copy(fileName, item->name, FILE_NAME_MAX);
    }

    // The page checks uploaded CIAs against installed titles before sending; links it cannot check,
    // so the 3DS asks, unless the sender is trusted.
    job->content.cia.askInstalled = job->kind == WEB_JOB_LINKS && !job->trusted;
    job->content.cia.allowNew3ds = job->trusted;

    return content_install_begin(&job->content, fileName, "", initialReadBlock, initialReadSize, job, webserver_draw_item, handle);
}

static Result webserver_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    web_job* job = (web_job*) data;

    Result res = content_end(&job->content, &job->shared->op, succeeded, handle);
    if(succeeded && R_SUCCEEDED(res)) {
        job->items[index].state = WEB_ITEM_DONE;
    }

    return res;
}

static Result webserver_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    return FSFILE_Write(handle, bytesWritten, offset, buffer, size, 0);
}

// Downloads: the source is the open SD file, the destination the /file connection, whose response
// head goes out with the first block.

static Result webserver_file_open_src(void* data, u32 index, u32* handle) {
    *handle = ((web_job*) data)->file;
    return 0;
}

// The file is the caller's to close.
static Result webserver_file_close_src(void* data, u32 index, bool succeeded, u32 handle) {
    return 0;
}

static Result webserver_file_get_src_size(void* data, u32 handle, u64* size) {
    *size = ((web_job*) data)->items[0].size;
    return 0;
}

static Result webserver_file_read_src(void* data, u32 handle, u32* bytesRead, void* buffer, u64 offset, u32 size) {
    return FSFILE_Read(handle, bytesRead, offset, buffer, size);
}

static Result webserver_file_open_dst(void* data, u32 index, void* initialReadBlock, u32 initialReadSize, u64 size, u32* handle) {
    web_job* job = (web_job*) data;

    char disposition[WEB_DISPOSITION_MAX];
    if(!web_content_disposition(disposition, sizeof(disposition), job->items[index].name)) {
        return R_APP_OUT_OF_RANGE;
    }

    // Even a head cut off partway means no other answer can follow.
    job->headSent = true;

    Result res = webserver_send_head(job->shared, job->socket, 200, "application/octet-stream", size, disposition);
    if(R_SUCCEEDED(res)) {
        *handle = 1; // nonzero: the engine closes only destinations that opened
    }

    return res;
}

static Result webserver_file_close_dst(void* data, u32 index, bool succeeded, u32 handle) {
    if(succeeded) {
        ((web_job*) data)->items[index].state = WEB_ITEM_DONE;
    }

    return 0;
}

static Result webserver_file_write_dst(void* data, u32 handle, u32* bytesWritten, void* buffer, u64 offset, u32 size) {
    web_job* job = (web_job*) data;

    Result res = webserver_io(job->shared, job->socket, buffer, size, false, true);
    if(R_SUCCEEDED(res)) {
        *bytesWritten = size;
    }

    return res;
}

// No error prompt per item: the log and the summary report failures. A broken stream stops the batch.
static bool webserver_error(void* data, u32 index, Result res, ui_view** errorView) {
    web_job* job = (web_job*) data;

    job->items[index].state = WEB_ITEM_FAILED;
    job->items[index].result = res;

    return job->kind == WEB_JOB_LINKS || !job->broken;
}

static Result webserver_run(web_state* state, web_job* job) {
    if(job->kind == WEB_JOB_FILES) {
        job->drain = (u8*) malloc(WEB_DRAIN_SIZE);
        if(job->drain == NULL) {
            return R_APP_OUT_OF_MEMORY;
        }
    }

    data_op_data* op = &state->op;
    memset(op, 0, sizeof(*op));

    op->data = job;
    op->op = job->kind == WEB_JOB_LINKS ? DATAOP_DOWNLOAD : DATAOP_COPY;
    op->total = job->count;
    op->totalBytes = job->totalBytes;
    op->bufferSize = WEB_BUFFER_SIZE;
    op->copyEmpty = true;
    op->noRetry = true;

    op->isSrcDirectory = webserver_is_src_directory;
    op->error = webserver_error;

    if(job->kind == WEB_JOB_DOWNLOAD) {
        op->openSrc = webserver_file_open_src;
        op->closeSrc = webserver_file_close_src;
        op->getSrcSize = webserver_file_get_src_size;
        op->readSrc = webserver_file_read_src;

        op->openDst = webserver_file_open_dst;
        op->closeDst = webserver_file_close_dst;
        op->writeDst = webserver_file_write_dst;
    } else {
        op->openSrc = webserver_open_src;
        op->closeSrc = webserver_close_src;
        op->getSrcSize = webserver_get_src_size;
        op->readSrc = webserver_read_src;
        op->getSrcUrl = webserver_get_src_url;

        op->openDst = webserver_open_dst;
        op->closeDst = webserver_close_dst;
        op->writeDst = webserver_write_dst;
    }

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
        web_item* item = &job->items[i];
        if(item->state != WEB_ITEM_PENDING) {
            continue;
        }

        if(i < op->processed) {
            item->state = WEB_ITEM_SKIPPED;
        } else if(i == op->processed && res == R_APP_CANCELLED) {
            item->state = WEB_ITEM_CANCELLED;
        }
    }

    return res;
}

static json_t* webserver_results(web_job* job) {
    json_t* results = json_array();
    for(u32 i = 0; i < job->count && results != NULL; i++) {
        web_item* item = &job->items[i];

        char error[128] = "";
        switch(item->state) {
            case WEB_ITEM_DONE:
                json_array_append_new(results, json_pack("{s:b}", "ok", 1));
                continue;
            case WEB_ITEM_SKIPPED:
                json_array_append_new(results, json_pack("{s:b, s:b, s:s}", "ok", 0, "skipped", 1, "error", item->install ? "declined" : "exists"));
                continue;
            case WEB_ITEM_FAILED:
                snprintf(error, sizeof(error), "%s (0x%08lX)", error_get_description(item->result), item->result);
                break;
            case WEB_ITEM_CANCELLED:
                string_copy(error, "cancelled", sizeof(error));
                break;
            case WEB_ITEM_PENDING:
                string_copy(error, "never arrived", sizeof(error));
                break;
        }

        json_array_append_new(results, json_pack("{s:b, s:s}", "ok", 0, "error", error));
    }

    return results;
}

// Log rows for every item, then one summary prompt (the engine already said when it was cancelled).
static void webserver_report(web_state* state, web_job* job, Result result) {
    u32 done = 0;
    u32 failed = 0;
    u32 missing = 0;
    const web_item* firstFailure = NULL;

    for(u32 i = 0; i < job->count; i++) {
        const web_item* item = &job->items[i];
        const char* shown = item->path != NULL ? item->path : item->name;

        switch(item->state) {
            case WEB_ITEM_DONE:
                done++;
                webserver_log(state, COLOR_TEXT, "%s: %s", item->install ? "Installed" : "Saved", shown);
                break;
            case WEB_ITEM_SKIPPED:
                webserver_log(state, COLOR_TEXT_DIM, "Skipped: %s%s", shown, item->install ? "" : " (exists)");
                break;
            case WEB_ITEM_FAILED:
                failed++;
                firstFailure = firstFailure != NULL ? firstFailure : item;
                webserver_log(state, COLOR_WARNING, "Failed: %s (0x%08lX)", shown, item->result);
                break;
            case WEB_ITEM_CANCELLED:
                webserver_log(state, COLOR_TEXT_DIM, "Cancelled: %s", shown);
                break;
            case WEB_ITEM_PENDING:
                missing++;
                break;
        }
    }

    if(missing > 0) {
        webserver_log(state, COLOR_TEXT_DIM, "Never arrived: %lu more from %s", missing, job->from);
    }

    if(result == R_APP_CANCELLED) {
        return;
    }

    if(firstFailure != NULL) {
        error_display_res(NULL, NULL, firstFailure->result, "%lu of %lu failed. First:\n%.60s", failed, job->count, firstFailure->name);
        return;
    }

    // Trusted senders see the results in the browser; the log rows above stay on the 3DS.
    if(job->trusted) {
        return;
    }

    // The prompt keeps the text pointer; one worker writes it.
    static char summary[128];
    char headline[64];
    const char* verb = job->installs == job->count ? "Installed" : job->installs == 0 ? "Saved" : "Transferred";
    snprintf(headline, sizeof(headline), "%s %lu of %lu.", verb, done, job->count);
    task_data_op_format_summary(&state->op, headline, summary, sizeof(summary));
    prompt_display_notify("Success", summary, COLOR_TEXT, NULL, NULL, NULL);
}

static void webserver_log_declined(web_state* state, const web_job* job) {
    webserver_log(state, COLOR_TEXT_DIM, "Declined: %lu %s from %s", job->count, job->kind == WEB_JOB_LINKS ? "links" : "files", job->from);
}

// POST /offer. Links run here; an accepted files offer is returned (with its token) for the /data
// request that brings the bytes.
static web_job* webserver_serve_offer(webserver* w, const web_client* client, const web_request* request, char* token) {
    web_state* state = w->shared;
    int fd = client->socket;

    json_t* root = webserver_read_json(state, fd, request);
    if(root == NULL) {
        return NULL;
    }

    char error[256];
    web_job* job = NULL;
    int status = json_object_get(root, "links") != NULL ? webserver_links_job(state, client->from, root, &job, error, sizeof(error))
                                                        : webserver_files_job(state, client->from, root, &job, error, sizeof(error));
    json_decref(root);

    if(status != 200) {
        webserver_respond_error(state, fd, status, error);
        return NULL;
    }

    job->trusted = webserver_is_trusted(w, request);

    if(!webserver_confirm(state, job)) {
        webserver_log_declined(state, job);
        webserver_respond_json(state, fd, 200, json_pack("{s:b}", "accepted", 0));
        webserver_free_job(job);
        return NULL;
    }

    if(job->kind == WEB_JOB_LINKS) {
        webserver_report(state, job, webserver_run(state, job));
        webserver_respond_json(state, fd, 200, json_pack("{s:b, s:o}", "accepted", 1, "results", webserver_results(job)));
        webserver_free_job(job);
        return NULL;
    }

    u32 value = (u32) (svcGetSystemTick() ^ (osGetTime() << 16));
    snprintf(token, WEB_TOKEN_LENGTH + 1, "%08lx", value);

    webserver_respond_json(state, fd, 200, json_pack("{s:b, s:s}", "accepted", 1, "token", token));
    return job;
}

// POST /data for the accepted job: the files' bytes back to back. Returns true when it ran the batch.
static bool webserver_serve_data(webserver* w, const web_client* client, const web_request* request, web_job* job, const char* token) {
    web_state* state = w->shared;
    int fd = client->socket;

    if(job == NULL) {
        webserver_respond_error(state, fd, 409, "No accepted offer.");
        return false;
    }

    if(strcmp(request->token, token) != 0 || strcmp(client->from, job->from) != 0) {
        webserver_respond_error(state, fd, 403, "Wrong token or sender.");
        return false;
    }

    if(!request->hasLength || request->contentLength != job->totalBytes) {
        webserver_respond_error(state, fd, 400, "Content-Length must equal the offered files' total size.");
        return false;
    }

    job->socket = fd;
    Result res = webserver_run(state, job);
    job->socket = -1;

    // A cut or cancelled stream gets no answer: the sender sees the connection end.
    if(res != R_APP_CANCELLED && !job->broken) {
        webserver_respond_json(state, fd, 200, json_pack("{s:o}", "results", webserver_results(job)));
    }

    webserver_report(state, job, res);
    return true;
}

// Directories first, then names, ASCII letters compared without case (like strcasecmp on UTF-8).
static int webserver_compare_entries(const void* a, const void* b) {
    const FS_DirectoryEntry* entry1 = (const FS_DirectoryEntry*) a;
    const FS_DirectoryEntry* entry2 = (const FS_DirectoryEntry*) b;

    bool dir1 = (entry1->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
    bool dir2 = (entry2->attributes & FS_ATTRIBUTE_DIRECTORY) != 0;
    if(dir1 != dir2) {
        return dir1 ? -1 : 1;
    }

    for(u32 i = 0; i < sizeof(entry1->name) / sizeof(entry1->name[0]); i++) {
        u16 c1 = entry1->name[i];
        u16 c2 = entry2->name[i];
        c1 = c1 >= 'A' && c1 <= 'Z' ? c1 + ('a' - 'A') : c1;
        c2 = c2 >= 'A' && c2 <= 'Z' ? c2 + ('a' - 'A') : c2;
        if(c1 != c2 || c1 == 0) {
            return (int) c1 - (int) c2;
        }
    }

    return 0;
}

// GET /list?path=/3ds/: the folder's visible entries for the page's SD browser.
static void webserver_serve_list(web_state* state, int fd, const web_request* request) {
    char folder[FILE_PATH_MAX];
    if(!web_join_path(folder, sizeof(folder), request->target, NULL)) {
        webserver_respond_error(state, fd, 400, "Bad path.");
        return;
    }

    FS_DirectoryEntry* entries = NULL;
    u32 count = 0;

    FS_Archive sdmcArchive = 0;
    Result res = FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
    if(R_SUCCEEDED(res)) {
        res = fs_read_dir(sdmcArchive, folder, &entries, &count);
        FSUSER_CloseArchive(sdmcArchive);
    }

    if(R_FAILED(res)) {
        bool memory = res == R_APP_OUT_OF_MEMORY;
        webserver_respond_error(state, fd, memory ? 503 : 404, memory ? "Out of memory." : "Folder not found.");
        return;
    }

    if(count > WEB_LIST_MAX) {
        free(entries);
        webserver_respond_error(state, fd, 413, "Too many entries to list here. Use the SD File Browser or FTP for this folder.");
        return;
    }

    qsort(entries, count, sizeof(FS_DirectoryEntry), webserver_compare_entries);

    json_t* list = json_array();
    for(u32 i = 0; i < count && list != NULL; i++) {
        const FS_DirectoryEntry* entry = &entries[i];
        if(entry->attributes & FS_ATTRIBUTE_HIDDEN) {
            continue;
        }

        // Up to 3 bytes per UTF-16 unit, so long names are never cut.
        char name[sizeof(entry->name) / sizeof(entry->name[0]) * 3 + 1];
        memset(name, 0, sizeof(name));
        utf16_to_utf8((uint8_t*) name, entry->name, sizeof(name) - 1);

        // A name jansson rejects (not valid UTF-8) makes a NULL entry, which the append skips.
        if(entry->attributes & FS_ATTRIBUTE_DIRECTORY) {
            json_array_append_new(list, json_pack("{s:s, s:b}", "name", name, "dir", 1));
        } else {
            json_array_append_new(list, json_pack("{s:s, s:b, s:I}", "name", name, "dir", 0, "size", (json_int_t) entry->fileSize));
        }
    }

    free(entries);

    // SD free space for the page's bottom bar; left out when FS cannot report it.
    json_t* reply = list != NULL ? json_pack("{s:s, s:o}", "path", folder, "entries", list) : NULL;
    FS_ArchiveResource resource = {0};
    if(reply != NULL && R_SUCCEEDED(FSUSER_GetArchiveResource(&resource, SYSTEM_MEDIATYPE_SD))) {
        json_object_set_new(reply, "free", json_integer((json_int_t) ((u64) resource.freeClusters * resource.clusterSize)));
    }

    webserver_respond_json(state, fd, 200, reply);
}

// GET /file?path=/3ds/a.cia: streams the file to the browser as a download. True when all of it went.
static bool webserver_serve_file(web_state* state, const web_client* client, const web_request* request) {
    int fd = client->socket;

    char path[FILE_PATH_MAX];
    if(request->target[0] != '/' || !web_join_path(path, sizeof(path), "/", request->target + 1) || strcmp(path, "/") == 0) {
        webserver_respond_error(state, fd, 400, "Bad path.");
        return false;
    }

    web_job* job = webserver_new_job(state, client->from, WEB_JOB_DOWNLOAD, 1);
    web_item* item = job != NULL ? &job->items[0] : NULL;
    if(item == NULL || (item->path = strdup(path)) == NULL) {
        webserver_free_job(job);
        webserver_respond_error(state, fd, 503, "Out of memory.");
        return false;
    }

    item->name = webserver_last_part(item->path);
    job->socket = fd;

    Result res = R_APP_OUT_OF_MEMORY;
    FS_Path* fsPath = fs_make_path_utf8(path);
    if(fsPath != NULL) {
        res = FSUSER_OpenFileDirectly(&job->file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), *fsPath, FS_OPEN_READ, 0);
        fs_free_path_utf8(fsPath);
    }

    if(R_SUCCEEDED(res) && R_FAILED(res = FSFILE_GetSize(job->file, &item->size))) {
        FSFILE_Close(job->file);
    }

    if(R_FAILED(res)) {
        webserver_log(state, COLOR_WARNING, "Not sent: %s (0x%08lX)", path, res);
        webserver_respond_error(state, fd, 404, "File not found.");
        webserver_free_job(job);
        return false;
    }

    job->totalBytes = item->size;
    res = webserver_run(state, job);
    FSFILE_Close(job->file);

    if(!job->headSent) {
        char error[128];
        snprintf(error, sizeof(error), "%s (0x%08lX)", res == R_APP_CANCELLED ? "Cancelled on the 3DS" : error_get_description(res), res);
        webserver_respond_error(state, fd, 500, error);
    }

    bool sent = item->state == WEB_ITEM_DONE;
    if(sent) {
        webserver_log(state, COLOR_TEXT, "Sent: %s to %s", path, client->from);
    } else if(item->state == WEB_ITEM_CANCELLED) {
        webserver_log(state, COLOR_TEXT_DIM, "Cancelled: %s", path);
    } else {
        webserver_log(state, COLOR_WARNING, "Not sent: %s (0x%08lX)", path, item->state == WEB_ITEM_FAILED ? item->result : res);
    }

    webserver_free_job(job);
    return sent;
}

// Reads a 16-digit hex title ID.
static bool webserver_parse_title_id(const char* text, u64* out) {
    if(text == NULL || strlen(text) != 16) {
        return false;
    }

    u64 value = 0;
    for(u32 i = 0; i < 16; i++) {
        char c = (char) (text[i] | 0x20); // lowercase letters; digits keep their value
        int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if(digit < 0) {
            return false;
        }

        value = (value << 4) | (u64) digit;
    }

    *out = value;
    return true;
}

// A missing value or an array of up to WEB_FILES_MAX strings.
static bool webserver_is_string_list(json_t* value) {
    if(value == NULL) {
        return true;
    }

    if(!json_is_array(value) || json_array_size(value) > WEB_FILES_MAX) {
        return false;
    }

    for(size_t i = 0; i < json_array_size(value); i++) {
        if(!json_is_string(json_array_get(value, i))) {
            return false;
        }
    }

    return true;
}

// POST /check {dest, saves, titles, homebrew}: which saves already exist in dest, which title IDs are
// installed (and their versions), and which homebrew already has /3ds/<name>/<name>.3dsx, so the
// page can ask before anything is sent. Uses the same paths the upload would.
static void webserver_serve_check(web_state* state, int fd, const web_request* request) {
    json_t* root = webserver_read_json(state, fd, request);
    if(root == NULL) {
        return;
    }

    json_t* dest = json_object_get(root, "dest");
    json_t* saves = json_object_get(root, "saves");
    json_t* titles = json_object_get(root, "titles");
    json_t* homebrew = json_object_get(root, "homebrew");

    char folder[FILE_PATH_MAX];
    if(!webserver_is_string_list(saves) || !webserver_is_string_list(titles) || !webserver_is_string_list(homebrew) || (dest != NULL && !json_is_string(dest))
       || !web_join_path(folder, sizeof(folder), dest != NULL ? json_string_value(dest) : "/", NULL)) {
        json_decref(root);
        webserver_respond_error(state, fd, 400, "Bad check.");
        return;
    }

    FS_Archive sdmcArchive = 0;
    if(R_FAILED(FSUSER_OpenArchive(&sdmcArchive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")))) {
        json_decref(root);
        webserver_respond_error(state, fd, 500, "Could not open the SD card.");
        return;
    }

    json_t* reply = json_pack("{s:[], s:[], s:[]}", "exists", "installed", "homebrew");
    json_t* exists = json_object_get(reply, "exists");
    json_t* installed = json_object_get(reply, "installed");
    json_t* foundHomebrew = json_object_get(reply, "homebrew");

    const char* bad = NULL;
    char path[FILE_PATH_MAX];

    for(size_t i = 0; i < json_array_size(saves) && bad == NULL; i++) {
        json_t* save = json_array_get(saves, i);
        if(!web_join_path(path, sizeof(path), folder, json_string_value(save))) {
            bad = json_string_value(save);
        } else if(fs_file_exists(sdmcArchive, path)) {
            json_array_append(exists, save);
        }
    }

    for(size_t i = 0; i < json_array_size(titles) && bad == NULL; i++) {
        const char* text = json_string_value(json_array_get(titles, i));

        u64 titleId = 0;
        AM_TitleEntry entry;
        if(!webserver_parse_title_id(text, &titleId)) {
            bad = text;
        } else if(R_SUCCEEDED(AM_GetTitleInfo(fs_get_title_destination(titleId), 1, &titleId, &entry))) {
            char version[16];
            ui_format_version(version, sizeof(version), entry.version);
            json_array_append_new(installed, json_pack("{s:s, s:s}", "titleId", text, "version", version));
        }
    }

    // content_install_begin's rule: /3ds/<name>/<name>.3dsx, name = the file name without extension.
    for(size_t i = 0; i < json_array_size(homebrew) && bad == NULL; i++) {
        json_t* file = json_array_get(homebrew, i);
        const char* fileName = json_string_value(file);

        char name[FILE_PATH_MAX];
        if(strchr(fileName, '/') != NULL || !web_join_path(path, sizeof(path), "/", fileName)) {
            bad = fileName;
            continue;
        }

        string_get_file_name(name, fileName, sizeof(name));
        if(snprintf(path, sizeof(path), "/3ds/%s/%s.3dsx", name, name) < (int) sizeof(path) && fs_file_exists(sdmcArchive, path)) {
            json_array_append(foundHomebrew, file);
        }
    }

    FSUSER_CloseArchive(sdmcArchive);

    if(bad != NULL) {
        char error[256];
        snprintf(error, sizeof(error), "Bad file name or title ID: %.200s", bad);
        json_decref(reply);
        webserver_respond_error(state, fd, 400, error);
    } else {
        webserver_respond_json(state, fd, 200, reply);
    }

    json_decref(root);
}

static bool webserver_await_data(webserver* w, web_job* job, const char* token);

// One HTTP request. waiting is the accepted files job whose /data is expected, or NULL. Returns true
// when it ran that job.
static bool webserver_serve_web(webserver* w, const web_client* client, web_job* waiting, const char* token) {
    web_state* state = w->shared;
    int fd = client->socket;

    web_request request;
    if(!webserver_read_head(w, fd, &request)) {
        close(fd);
        return false;
    }

    bool ran = false;
    web_job* accepted = NULL;
    char acceptedToken[WEB_TOKEN_LENGTH + 1] = "";

    bool get = strcmp(request.method, "GET") == 0;
    bool post = strcmp(request.method, "POST") == 0;

    u32 route = 0;
    while(route < WEB_ROUTE_COUNT && strcmp(request.path, webserver_routes[route].path) != 0) {
        route++;
    }

    if(!request.hostAllowed) {
        webserver_respond_error(state, fd, 403, "Open this page by the console's IP address.");
    } else if(get && strcmp(request.path, "/") == 0) {
        webserver_respond(state, fd, 200, "text/html; charset=utf-8", state->page, state->pageSize);
    } else if(route == WEB_ROUTE_COUNT) {
        // Unknown paths (favicon.ico) end here, before they could ask to approve the device.
        webserver_respond_error(state, fd, 404, "Not found.");
    } else if(webserver_routes[route].post ? !post : !get) {
        webserver_respond_error(state, fd, 405, "Method not allowed.");
    } else if(webserver_routes[route].ftbHeader && !request.ftbHeader) {
        webserver_respond_error(state, fd, 403, "Missing X-FTB header.");
    } else if(webserver_check_device(w, client, &request)) {
        switch(webserver_routes[route].route) {
            case WEB_ROUTE_LIST:
                webserver_serve_list(state, fd, &request);
                break;
            case WEB_ROUTE_FILE:
                w->downloads++;
                if(!webserver_serve_file(state, client, &request)) {
                    w->downloadsFailed++;
                }

                break;
            case WEB_ROUTE_STATUS:
                // Answered only between transfers, so the page can wait here for a download to end.
                webserver_respond_json(state, fd, 200, json_pack("{s:i, s:i}", "downloads", (int) w->downloads, "failed", (int) w->downloadsFailed));
                break;
            case WEB_ROUTE_CHECK:
                webserver_serve_check(state, fd, &request);
                break;
            case WEB_ROUTE_OFFER:
                if(waiting != NULL) {
                    webserver_respond_error(state, fd, 503, "Busy with another sender.");
                } else {
                    accepted = webserver_serve_offer(w, client, &request, acceptedToken);
                }

                break;
            case WEB_ROUTE_DATA:
                ran = webserver_serve_data(w, client, &request, waiting, token);
                break;
        }
    }

    webserver_close_client(state, fd);

    if(accepted != NULL) {
        if(!webserver_await_data(w, accepted, acceptedToken)) {
            webserver_log(state, COLOR_WARNING, "Not sent: %lu files from %s", accepted->count, accepted->from);
        }

        webserver_free_job(accepted);
    }

    return ran;
}

// Fills client. Returns 0, -1 when none is waiting, or WEB_NETWORK_GONE.
static int webserver_accept(int listener, web_client* client) {
    struct sockaddr_in address;
    socklen_t addressLength = sizeof(address);

    int fd = accept(listener, (struct sockaddr*) &address, &addressLength);
    if(fd < 0) {
        // newlib: 115 is ENETDOWN. Both mean Wi-Fi went away.
        return errno == EINVAL || errno == ENETDOWN ? WEB_NETWORK_GONE : -1;
    }

    client->socket = fd;
    client->address = address.sin_addr.s_addr;
    string_copy(client->from, inet_ntoa(address.sin_addr), WEB_ADDRESS_MAX);

    int buffer = WEB_SOCKET_BUFFER;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

    return 0;
}

// Waits for the accepted offer's /data, serving the page meanwhile. False when it never came.
static bool webserver_await_data(webserver* w, web_job* job, const char* token) {
    u64 start = osGetTime();
    while(!task_is_cancelled(&w->shared->state) && osGetTime() - start < WEB_DATA_WAIT_MS) {
        struct pollfd pfd = {.fd = w->web, .events = POLLIN};
        if(poll(&pfd, 1, WEB_POLL_MS) <= 0) {
            continue;
        }

        web_client client;
        int accepted = webserver_accept(w->web, &client);
        if(accepted == WEB_NETWORK_GONE) {
            return false;
        }

        if(accepted == 0 && webserver_serve_web(w, &client, job, token)) {
            return true;
        }
    }

    return false;
}

// Browsers try https:// first for a bare typed address; this sends them to the page.
static void webserver_redirect_https(webserver* w, int client) {
    web_state* state = w->shared;

    struct in_addr address = {.s_addr = state->ip};
    char location[32];
    if(state->webPort == WEB_PORT) {
        snprintf(location, sizeof(location), "http://%s/", inet_ntoa(address));
    } else {
        snprintf(location, sizeof(location), "http://%s:%u/", inet_ntoa(address), state->webPort);
    }

    web_tls_redirect(w->tls, client, location, &state->state);
    webserver_close_client(state, client);
}

static int webserver_open_listener(u16 port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
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

    if(bind(fd, (struct sockaddr*) &address, sizeof(address)) < 0 || listen(fd, WEB_BACKLOG) < 0
       || fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void webserver_close_socket(int* fd) {
    if(*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static void webserver_close_listeners(webserver* w) {
    webserver_close_socket(&w->web);
    webserver_close_socket(&w->https);

    w->shared->webPort = 0;
}

static void webserver_open_listeners(webserver* w) {
    u16 webPort = WEB_PORT;
    w->web = webserver_open_listener(webPort);
    if(w->web < 0) {
        webPort = WEB_FALLBACK_PORT;
        w->web = webserver_open_listener(webPort);
    }

    if(w->web >= 0 && w->tls != NULL) {
        w->https = webserver_open_listener(WEB_TLS_PORT);
    }

    w->shared->webPort = w->web >= 0 ? webPort : 0;
}

static Result webserver_thread(void* arg) {
    webserver* w = (webserver*) arg;
    web_state* state = w->shared;

    webserver_load_devices(w);

    w->tls = web_tls_create();
    if(w->tls == NULL) {
        webserver_log(state, COLOR_WARNING, "HTTPS redirect off: TLS failed to start");
    }

    u64 lastIpCheck = 0;
    while(!task_is_cancelled(&state->state)) {
        // Wi-Fi can drop, come back, or change address while the screen is open.
        if(osGetTime() - lastIpCheck >= WEB_IP_CHECK_MS) {
            lastIpCheck = osGetTime();

            u32 ip = (u32) gethostid();
            if(ip != state->ip) {
                webserver_close_listeners(w);
                state->ip = ip;
            }

            if(ip != 0 && w->web < 0 && w->https < 0) {
                webserver_open_listeners(w);
            }
        }

        struct pollfd fds[2];
        nfds_t count = 0;
        int listeners[2] = {w->web, w->https};
        for(u32 i = 0; i < 2; i++) {
            if(listeners[i] >= 0) {
                fds[count++] = (struct pollfd) {.fd = listeners[i], .events = POLLIN};
            }
        }

        if(count == 0) {
            svcSleepThread(WEB_POLL_MS * 1000000LL);
            continue;
        }

        if(poll(fds, count, WEB_POLL_MS) <= 0) {
            continue;
        }

        for(nfds_t i = 0; i < count; i++) {
            if(!(fds[i].revents & POLLIN)) {
                continue;
            }

            web_client client;
            int accepted = webserver_accept(fds[i].fd, &client);
            if(accepted == WEB_NETWORK_GONE) {
                webserver_close_listeners(w);
                state->ip = 0;
                break;
            }

            if(accepted < 0) {
                continue;
            }

            if(fds[i].fd == w->https) {
                webserver_redirect_https(w, client.socket);
            } else {
                webserver_serve_web(w, &client, NULL, NULL);
            }

            // Serving may have taken long enough for the listeners to change.
            break;
        }
    }

    webserver_close_listeners(w);
    web_tls_free(w->tls);
    free(w);

    return 0;
}

Result webserver_start(web_state* state) {
    LightLock_Init(&state->jobLock);
    LightLock_Init(&state->logLock);
    linked_list_init(&state->rows);
    state->op.state.finished = true;

    webserver* w = (webserver*) calloc(1, sizeof(webserver));
    if(w == NULL) {
        return R_APP_OUT_OF_MEMORY;
    }

    w->shared = state;
    w->web = -1;
    w->https = -1;

    Result res = task_start(&state->state, webserver_thread, w, 0x18, 1);
    if(R_FAILED(res)) {
        free(w);
    }

    return res;
}

void webserver_job_line(const web_job* job, u32 index, char* out, size_t size) {
    const char* name = "";
    if(index < job->count) {
        const web_item* item = &job->items[index];
        name = item->path != NULL ? item->path : item->name;
    }

    snprintf(out, size, "%s %s: %s", job->kind == WEB_JOB_DOWNLOAD ? "To" : "From", job->from, name);
}
