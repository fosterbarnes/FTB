#include <string.h>
#include <strings.h>

#include "receiveproto.h"

static bool receive_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool receive_is_hex(char c) {
    return receive_is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool receive_name_is(const char* name, size_t length, const char* expected) {
    return strlen(expected) == length && strncasecmp(name, expected, length) == 0;
}

static void receive_trim(const char** text, size_t* length) {
    while(*length > 0 && (**text == ' ' || **text == '\t')) {
        (*text)++;
        (*length)--;
    }

    while(*length > 0 && ((*text)[*length - 1] == ' ' || (*text)[*length - 1] == '\t')) {
        (*length)--;
    }
}

static bool receive_parse_length(const char* text, size_t length, uint64_t* out) {
    if(length == 0 || length > 19) {
        return false;
    }

    uint64_t value = 0;
    for(size_t i = 0; i < length; i++) {
        if(!receive_is_digit(text[i])) {
            return false;
        }

        value = value * 10 + (uint64_t) (text[i] - '0');
    }

    *out = value;
    return true;
}

// The token= parameter of a query, kept only when it is RECEIVE_TOKEN_LENGTH hex digits.
static void receive_parse_token(const char* query, size_t length, char* out) {
    out[0] = '\0';

    size_t pos = 0;
    while(pos < length) {
        size_t end = pos;
        while(end < length && query[end] != '&') {
            end++;
        }

        if(end - pos == 6 + RECEIVE_TOKEN_LENGTH && strncmp(query + pos, "token=", 6) == 0) {
            const char* value = query + pos + 6;
            for(size_t i = 0; i < RECEIVE_TOKEN_LENGTH; i++) {
                if(!receive_is_hex(value[i])) {
                    return;
                }
            }

            memcpy(out, value, RECEIVE_TOKEN_LENGTH);
            out[RECEIVE_TOKEN_LENGTH] = '\0';
            return;
        }

        pos = end + 1;
    }
}

static bool receive_parse_request_line(const char* line, size_t length, receive_request* out) {
    const char* methodEnd = memchr(line, ' ', length);
    if(methodEnd == NULL || methodEnd == line || (size_t) (methodEnd - line) >= sizeof(out->method)) {
        return false;
    }

    memcpy(out->method, line, methodEnd - line);
    out->method[methodEnd - line] = '\0';

    const char* target = methodEnd + 1;
    size_t rest = length - (target - line);
    const char* targetEnd = memchr(target, ' ', rest);
    if(targetEnd == NULL || targetEnd == target || *target != '/') {
        return false;
    }

    const char* version = targetEnd + 1;
    if(length - (version - line) != 8 || strncmp(version, "HTTP/1.", 7) != 0) {
        return false;
    }

    size_t targetLength = targetEnd - target;
    const char* query = memchr(target, '?', targetLength);
    size_t pathLength = query != NULL ? (size_t) (query - target) : targetLength;

    if(pathLength < sizeof(out->path)) {
        memcpy(out->path, target, pathLength);
        out->path[pathLength] = '\0';
    }

    if(query != NULL) {
        receive_parse_token(query + 1, targetEnd - query - 1, out->token);
    }

    return true;
}

static const char* receive_line_end(const char* line, const char* end) {
    while(line + 1 < end && !(line[0] == '\r' && line[1] == '\n')) {
        line++;
    }

    return line;
}

receive_parse receive_parse_head(const char* data, size_t size, receive_request* out) {
    memset(out, 0, sizeof(*out));

    const char* end = NULL;
    for(size_t i = 0; i + 3 < size; i++) {
        if(memcmp(data + i, "\r\n\r\n", 4) == 0) {
            end = data + i + 2; // keeps the last header line's \r\n
            break;
        }
    }

    if(end == NULL) {
        return size >= RECEIVE_HEAD_MAX ? RECEIVE_PARSE_BAD : RECEIVE_PARSE_INCOMPLETE;
    }

    // Every line, the last included, ends in \r\n before end.
    const char* line = data;
    const char* lineEnd = receive_line_end(line, end);
    if(!receive_parse_request_line(line, lineEnd - line, out)) {
        return RECEIVE_PARSE_BAD;
    }

    for(line = lineEnd + 2; line < end; line = lineEnd + 2) {
        lineEnd = receive_line_end(line, end);

        const char* colon = memchr(line, ':', lineEnd - line);
        if(colon == NULL || colon == line) {
            return RECEIVE_PARSE_BAD;
        }

        size_t nameLength = colon - line;
        const char* value = colon + 1;
        size_t valueLength = lineEnd - value;
        receive_trim(&value, &valueLength);

        if(receive_name_is(line, nameLength, "Host")) {
            out->hostAllowed = receive_host_allowed(value, valueLength);
        } else if(receive_name_is(line, nameLength, "Content-Length")) {
            if(out->hasLength || !receive_parse_length(value, valueLength, &out->contentLength)) {
                return RECEIVE_PARSE_BAD;
            }

            out->hasLength = true;
        } else if(receive_name_is(line, nameLength, "Transfer-Encoding")) {
            return RECEIVE_PARSE_BAD;
        } else if(receive_name_is(line, nameLength, "X-FTB")) {
            out->ftbHeader = valueLength == 1 && value[0] == '1';
        }
    }

    return RECEIVE_PARSE_DONE;
}

bool receive_host_allowed(const char* host, size_t length) {
    // An optional :port.
    for(size_t i = length; i > 0; i--) {
        if(host[i - 1] == ':') {
            size_t portLength = length - i;
            if(portLength == 0 || portLength > 5) {
                return false;
            }

            for(size_t j = i; j < length; j++) {
                if(!receive_is_digit(host[j])) {
                    return false;
                }
            }

            length = i - 1;
            break;
        }

        if(!receive_is_digit(host[i - 1])) {
            break;
        }
    }

    if(length > 6 && strncasecmp(host + length - 6, ".local", 6) == 0) {
        for(size_t i = 0; i < length; i++) {
            char c = host[i];
            if(!receive_is_digit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-' && c != '.') {
                return false;
            }
        }

        return true;
    }

    // IPv4: four decimal parts of 1-3 digits, each 255 or less.
    unsigned int parts = 0;
    size_t pos = 0;
    while(pos < length) {
        size_t digits = 0;
        unsigned int value = 0;
        while(pos < length && receive_is_digit(host[pos]) && digits < 4) {
            value = value * 10 + (unsigned int) (host[pos] - '0');
            pos++;
            digits++;
        }

        if(digits == 0 || digits > 3 || value > 255) {
            return false;
        }

        parts++;
        if(pos == length) {
            break;
        }

        if(host[pos] != '.' || parts == 4) {
            return false;
        }

        pos++;
        if(pos == length) {
            return false;
        }
    }

    return parts == 4;
}

static bool receive_component_allowed(const char* name, size_t length) {
    if(length == 0 || length > 255 || (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }

    if(name[length - 1] == '.' || name[length - 1] == ' ') {
        return false;
    }

    for(size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) name[i];
        if(c < 0x20 || c == 0x7F || (c < 0x80 && strchr("\\:*?\"<>|", c) != NULL)) {
            return false;
        }
    }

    return true;
}

static bool receive_append(char* out, size_t size, size_t* pos, const char* text, size_t length) {
    if(*pos + length >= size) {
        return false;
    }

    memcpy(out + *pos, text, length);
    *pos += length;
    out[*pos] = '\0';

    return true;
}

bool receive_join_path(char* out, size_t size, const char* folder, const char* relative) {
    if(size < 2 || folder == NULL || folder[0] != '/') {
        return false;
    }

    size_t pos = 0;
    receive_append(out, size, &pos, "/", 1);

    // Folder parts; empty ones ("/", "//", a trailing "/") are skipped.
    for(const char* part = folder + 1; *part != '\0';) {
        const char* partEnd = strchr(part, '/');
        size_t partLength = partEnd != NULL ? (size_t) (partEnd - part) : strlen(part);

        if(partLength > 0 && (!receive_component_allowed(part, partLength) || !receive_append(out, size, &pos, part, partLength)
                              || !receive_append(out, size, &pos, "/", 1))) {
            return false;
        }

        part += partLength + (partEnd != NULL ? 1 : 0);
    }

    if(relative == NULL || relative[0] == '\0') {
        return true;
    }

    // Relative parts: none may be empty, so no leading, doubled, or trailing "/".
    const char* part = relative;
    while(true) {
        const char* partEnd = strchr(part, '/');
        size_t partLength = partEnd != NULL ? (size_t) (partEnd - part) : strlen(part);

        if(!receive_component_allowed(part, partLength) || !receive_append(out, size, &pos, part, partLength)) {
            return false;
        }

        if(partEnd == NULL) {
            return true;
        }

        if(!receive_append(out, size, &pos, "/", 1)) {
            return false;
        }

        part = partEnd + 1;
    }
}

bool receive_path_sensitive(const char* path) {
    return strcasecmp(path, "/boot.firm") == 0 || strncasecmp(path, "/luma/", 6) == 0 || strncasecmp(path, "/nintendo 3ds/", 14) == 0;
}
