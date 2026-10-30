#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Web Transfer's HTTP and path rules. Plain C with no 3DS calls, so it can be checked on a PC.

#define WEB_HEAD_MAX 8192
#define WEB_TOKEN_LENGTH 8
#define WEB_DEVICE_KEY_LENGTH 32 // the page's ftbDevice cookie: lowercase hex
#define WEB_TARGET_MAX 512 // FILE_PATH_MAX
// web_content_disposition's output for any name that fits an SD path.
#define WEB_DISPOSITION_MAX (WEB_TARGET_MAX * 4 + 64)

typedef enum {
    WEB_PARSE_INCOMPLETE,
    WEB_PARSE_DONE,
    WEB_PARSE_BAD
} web_parse;

typedef struct {
    char method[8];
    char path[32]; // without the query; "" when longer (no route is that long)
    char token[WEB_TOKEN_LENGTH + 1]; // the query's token= value when it is 8 hex digits, else ""
    // The query's path= value, percent-decoded ('+' is a space); "" when missing, cut short, or not
    // valid UTF-8. Routes still check it with web_join_path.
    char target[WEB_TARGET_MAX];
    bool hostAllowed; // Host is an IPv4 address or a .local name, with an optional port
    bool ftbHeader; // X-FTB: 1, which a page on another site cannot send without asking first
    // The Cookie header's ftbDevice= value when web_device_key_valid, else "". The page sets it
    // SameSite=Strict, so requests started by other sites never carry it.
    char device[WEB_DEVICE_KEY_LENGTH + 1];
    bool hasLength;
    uint64_t contentLength;
} web_request;

// Parses a request head (the request line and headers up to the blank line). INCOMPLETE until the
// blank line is in data; BAD for anything malformed, a head over WEB_HEAD_MAX, or a chunked body.
web_parse web_parse_head(const char* data, size_t size, web_request* out);

// A remembered device key: exactly WEB_DEVICE_KEY_LENGTH lowercase hex digits.
bool web_device_key_valid(const char* key, size_t length);

// Host header value check; DNS names other than .local could be rebound to the console by a website.
bool web_host_allowed(const char* host, size_t length);

// Joins a page folder ("/3ds/" or "/3ds") and a relative path ("saves/a.sav") into an SD path. False
// when any part is empty, "." or "..", over 255 UTF-16 units, has a character FAT does not allow,
// ends in a dot or space, or the result does not fit. relative may be NULL or "" to check and
// normalize the folder alone (out then ends in "/").
bool web_join_path(char* out, size_t size, const char* folder, const char* relative);

// Paths whose replacement can stop the console from booting or break installed titles.
bool web_path_sensitive(const char* path);

// The name a saved link gets: the last part of the URL's path without query or fragment,
// percent-decoded ("http://host/a%20b.cia?x" = "a b.cia"). False when the URL has no path part or
// the name breaks web_join_path's rules. The page computes the same name for its pre-check.
bool web_url_file_name(char* out, size_t size, const char* url);

// The Content-Disposition value that saves a download as name (UTF-8): an ASCII filename= for old
// browsers and the exact name as filename*= (RFC 6266). False when it does not fit.
bool web_content_disposition(char* out, size_t size, const char* name);
