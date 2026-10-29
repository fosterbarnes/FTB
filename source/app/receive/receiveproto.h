#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Receive's HTTP and path rules. Plain C with no 3DS calls, so it can be checked on a PC.

#define RECEIVE_HEAD_MAX 8192
#define RECEIVE_TOKEN_LENGTH 8

typedef enum {
    RECEIVE_PARSE_INCOMPLETE,
    RECEIVE_PARSE_DONE,
    RECEIVE_PARSE_BAD
} receive_parse;

typedef struct {
    char method[8];
    char path[32]; // without the query; "" when longer (no route is that long)
    char token[RECEIVE_TOKEN_LENGTH + 1]; // the query's token= value when it is 8 hex digits, else ""
    bool hostAllowed; // Host is an IPv4 address or a .local name, with an optional port
    bool ftbHeader; // X-FTB: 1, which a page on another site cannot send without asking first
    bool hasLength;
    uint64_t contentLength;
} receive_request;

// Parses a request head (the request line and headers up to the blank line). INCOMPLETE until the
// blank line is in data; BAD for anything malformed, a head over RECEIVE_HEAD_MAX, or a chunked body.
receive_parse receive_parse_head(const char* data, size_t size, receive_request* out);

// Host header value check; DNS names other than .local could be rebound to the console by a website.
bool receive_host_allowed(const char* host, size_t length);

// Joins a page folder ("/3ds/" or "/3ds") and a relative path ("saves/a.sav") into an SD path. False
// when any part is empty, "." or "..", has a character FAT does not allow, ends in a dot or space, or
// the result does not fit. relative may be NULL or "" to check and normalize the folder alone
// (out then ends in "/").
bool receive_join_path(char* out, size_t size, const char* folder, const char* relative);

// Paths whose replacement can stop the console from booting or break installed titles.
bool receive_path_sensitive(const char* path);
