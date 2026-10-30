#include <string.h>
#include <strings.h>

#include "webproto.h"

static bool web_is_digit(char c) {
    return c >= '0' && c <= '9';
}

// 0-15, or -1 when c is not a hex digit.
static int web_hex_value(char c) {
    if(web_is_digit(c)) {
        return c - '0';
    }

    if(c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }

    return c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

// Well-formed UTF-8: no overlong forms, surrogates, or code points past U+10FFFF.
static bool web_utf8_valid(const char* text, size_t length) {
    const unsigned char* s = (const unsigned char*) text;
    size_t i = 0;
    while(i < length) {
        unsigned char c = s[i];
        size_t extra = 0;
        unsigned char min = 0x80;
        unsigned char max = 0xBF;
        if(c < 0x80) {
            i++;
            continue;
        } else if(c >= 0xC2 && c <= 0xDF) {
            extra = 1;
        } else if(c >= 0xE0 && c <= 0xEF) {
            extra = 2;
            min = c == 0xE0 ? 0xA0 : 0x80;
            max = c == 0xED ? 0x9F : 0xBF;
        } else if(c >= 0xF0 && c <= 0xF4) {
            extra = 3;
            min = c == 0xF0 ? 0x90 : 0x80;
            max = c == 0xF4 ? 0x8F : 0xBF;
        } else {
            return false;
        }

        if(length - i <= extra || s[i + 1] < min || s[i + 1] > max) {
            return false;
        }

        for(size_t j = 2; j <= extra; j++) {
            if((s[i + j] & 0xC0) != 0x80) {
                return false;
            }
        }

        i += extra + 1;
    }

    return true;
}

// Decodes %XX escapes (and '+' as a space when plusIsSpace, as in queries) into out, terminated.
// False, with out empty, on a bad escape, a NUL, invalid UTF-8, or no room.
static bool web_percent_decode(char* out, size_t size, const char* text, size_t length, bool plusIsSpace) {
    size_t pos = 0;
    for(size_t i = 0; i < length; i++) {
        char c = text[i];
        if(c == '%') {
            int high = i + 2 < length ? web_hex_value(text[i + 1]) : -1;
            int low = i + 2 < length ? web_hex_value(text[i + 2]) : -1;
            if(high < 0 || low < 0) {
                out[0] = '\0';
                return false;
            }

            c = (char) (high * 16 + low);
            i += 2;
        } else if(c == '+' && plusIsSpace) {
            c = ' ';
        }

        if(c == '\0' || pos + 1 >= size) {
            out[0] = '\0';
            return false;
        }

        out[pos++] = c;
    }

    out[pos] = '\0';
    if(!web_utf8_valid(out, pos)) {
        out[0] = '\0';
        return false;
    }

    return true;
}

static bool web_name_is(const char* name, size_t length, const char* expected) {
    return strlen(expected) == length && strncasecmp(name, expected, length) == 0;
}

static void web_trim(const char** text, size_t* length) {
    while(*length > 0 && (**text == ' ' || **text == '\t')) {
        (*text)++;
        (*length)--;
    }

    while(*length > 0 && ((*text)[*length - 1] == ' ' || (*text)[*length - 1] == '\t')) {
        (*length)--;
    }
}

static bool web_parse_length(const char* text, size_t length, uint64_t* out) {
    if(length == 0 || length > 19) {
        return false;
    }

    uint64_t value = 0;
    for(size_t i = 0; i < length; i++) {
        if(!web_is_digit(text[i])) {
            return false;
        }

        value = value * 10 + (uint64_t) (text[i] - '0');
    }

    *out = value;
    return true;
}

// A token= value, kept only when it is WEB_TOKEN_LENGTH hex digits.
static void web_parse_token(const char* value, size_t length, char* out) {
    out[0] = '\0';
    if(length != WEB_TOKEN_LENGTH) {
        return;
    }

    for(size_t i = 0; i < WEB_TOKEN_LENGTH; i++) {
        if(web_hex_value(value[i]) < 0) {
            return;
        }
    }

    memcpy(out, value, WEB_TOKEN_LENGTH);
    out[WEB_TOKEN_LENGTH] = '\0';
}

bool web_device_key_valid(const char* key, size_t length) {
    if(length != WEB_DEVICE_KEY_LENGTH) {
        return false;
    }

    for(size_t i = 0; i < length; i++) {
        if(!web_is_digit(key[i]) && !(key[i] >= 'a' && key[i] <= 'f')) {
            return false;
        }
    }

    return true;
}

// The Cookie header's ftbDevice= value; other cookies are ignored.
static void web_parse_cookies(const char* text, size_t length, web_request* out) {
    size_t pos = 0;
    while(pos < length) {
        size_t end = pos;
        while(end < length && text[end] != ';') {
            end++;
        }

        const char* cookie = text + pos;
        size_t cookieLength = end - pos;
        web_trim(&cookie, &cookieLength);
        if(cookieLength >= 10 && strncmp(cookie, "ftbDevice=", 10) == 0 && web_device_key_valid(cookie + 10, cookieLength - 10)) {
            memcpy(out->device, cookie + 10, WEB_DEVICE_KEY_LENGTH);
            out->device[WEB_DEVICE_KEY_LENGTH] = '\0';
        }

        pos = end + 1;
    }
}

// The query's token= and path= parameters; others are ignored.
static void web_parse_query(const char* query, size_t length, web_request* out) {
    size_t pos = 0;
    while(pos < length) {
        size_t end = pos;
        while(end < length && query[end] != '&') {
            end++;
        }

        const char* param = query + pos;
        size_t paramLength = end - pos;
        if(paramLength >= 6 && strncmp(param, "token=", 6) == 0) {
            web_parse_token(param + 6, paramLength - 6, out->token);
        } else if(paramLength >= 5 && strncmp(param, "path=", 5) == 0) {
            web_percent_decode(out->target, sizeof(out->target), param + 5, paramLength - 5, true);
        }

        pos = end + 1;
    }
}

static bool web_parse_request_line(const char* line, size_t length, web_request* out) {
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
        web_parse_query(query + 1, targetEnd - query - 1, out);
    }

    return true;
}

static const char* web_line_end(const char* line, const char* end) {
    while(line + 1 < end && !(line[0] == '\r' && line[1] == '\n')) {
        line++;
    }

    return line;
}

web_parse web_parse_head(const char* data, size_t size, web_request* out) {
    memset(out, 0, sizeof(*out));

    const char* end = NULL;
    for(size_t i = 0; i + 3 < size; i++) {
        if(memcmp(data + i, "\r\n\r\n", 4) == 0) {
            end = data + i + 2; // keeps the last header line's \r\n
            break;
        }
    }

    if(end == NULL) {
        return size >= WEB_HEAD_MAX ? WEB_PARSE_BAD : WEB_PARSE_INCOMPLETE;
    }

    // Every line, the last included, ends in \r\n before end.
    const char* line = data;
    const char* lineEnd = web_line_end(line, end);
    if(!web_parse_request_line(line, lineEnd - line, out)) {
        return WEB_PARSE_BAD;
    }

    for(line = lineEnd + 2; line < end; line = lineEnd + 2) {
        lineEnd = web_line_end(line, end);

        const char* colon = memchr(line, ':', lineEnd - line);
        if(colon == NULL || colon == line) {
            return WEB_PARSE_BAD;
        }

        size_t nameLength = colon - line;
        const char* value = colon + 1;
        size_t valueLength = lineEnd - value;
        web_trim(&value, &valueLength);

        if(web_name_is(line, nameLength, "Host")) {
            out->hostAllowed = web_host_allowed(value, valueLength);
        } else if(web_name_is(line, nameLength, "Content-Length")) {
            if(out->hasLength || !web_parse_length(value, valueLength, &out->contentLength)) {
                return WEB_PARSE_BAD;
            }

            out->hasLength = true;
        } else if(web_name_is(line, nameLength, "Transfer-Encoding")) {
            return WEB_PARSE_BAD;
        } else if(web_name_is(line, nameLength, "X-FTB")) {
            out->ftbHeader = valueLength == 1 && value[0] == '1';
        } else if(web_name_is(line, nameLength, "Cookie")) {
            web_parse_cookies(value, valueLength, out);
        }
    }

    return WEB_PARSE_DONE;
}

bool web_host_allowed(const char* host, size_t length) {
    // An optional :port.
    for(size_t i = length; i > 0; i--) {
        if(host[i - 1] == ':') {
            size_t portLength = length - i;
            if(portLength == 0 || portLength > 5) {
                return false;
            }

            for(size_t j = i; j < length; j++) {
                if(!web_is_digit(host[j])) {
                    return false;
                }
            }

            length = i - 1;
            break;
        }

        if(!web_is_digit(host[i - 1])) {
            break;
        }
    }

    if(length > 6 && strncasecmp(host + length - 6, ".local", 6) == 0) {
        for(size_t i = 0; i < length; i++) {
            char c = host[i];
            if(!web_is_digit(c) && !(c >= 'a' && c <= 'z') && !(c >= 'A' && c <= 'Z') && c != '-' && c != '.') {
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
        while(pos < length && web_is_digit(host[pos]) && digits < 4) {
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

static bool web_component_allowed(const char* name, size_t length) {
    if(length == 0 || (length == 1 && name[0] == '.') || (length == 2 && name[0] == '.' && name[1] == '.')) {
        return false;
    }

    if(name[length - 1] == '.' || name[length - 1] == ' ') {
        return false;
    }

    // FAT long names hold 255 UTF-16 units; code points past U+FFFF (4-byte UTF-8) take two.
    size_t units = 0;
    for(size_t i = 0; i < length; i++) {
        unsigned char c = (unsigned char) name[i];
        if(c < 0x20 || c == 0x7F || (c < 0x80 && strchr("\\/:*?\"<>|", c) != NULL)) {
            return false;
        }

        if((c & 0xC0) != 0x80) {
            units += c >= 0xF0 ? 2 : 1;
        }
    }

    return units <= 255;
}

static bool web_append(char* out, size_t size, size_t* pos, const char* text, size_t length) {
    if(*pos + length >= size) {
        return false;
    }

    memcpy(out + *pos, text, length);
    *pos += length;
    out[*pos] = '\0';

    return true;
}

bool web_join_path(char* out, size_t size, const char* folder, const char* relative) {
    if(size < 2 || folder == NULL || folder[0] != '/') {
        return false;
    }

    size_t pos = 0;
    web_append(out, size, &pos, "/", 1);

    // Folder parts; empty ones ("/", "//", a trailing "/") are skipped.
    for(const char* part = folder + 1; *part != '\0';) {
        const char* partEnd = strchr(part, '/');
        size_t partLength = partEnd != NULL ? (size_t) (partEnd - part) : strlen(part);

        if(partLength > 0 && (!web_component_allowed(part, partLength) || !web_append(out, size, &pos, part, partLength)
                              || !web_append(out, size, &pos, "/", 1))) {
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

        if(!web_component_allowed(part, partLength) || !web_append(out, size, &pos, part, partLength)) {
            return false;
        }

        if(partEnd == NULL) {
            return true;
        }

        if(!web_append(out, size, &pos, "/", 1)) {
            return false;
        }

        part = partEnd + 1;
    }
}

bool web_path_sensitive(const char* path) {
    return strcasecmp(path, "/boot.firm") == 0 || strncasecmp(path, "/luma/", 6) == 0 || strncasecmp(path, "/nintendo 3ds/", 14) == 0;
}

bool web_url_file_name(char* out, size_t size, const char* url) {
    const char* rest = strstr(url, "://");
    if(rest == NULL) {
        return false;
    }

    rest += 3;
    size_t end = strcspn(rest, "?#");

    const char* slash = NULL;
    for(size_t i = 0; i < end; i++) {
        if(rest[i] == '/') {
            slash = rest + i;
        }
    }

    if(slash == NULL) {
        return false;
    }

    const char* name = slash + 1;
    return web_percent_decode(out, size, name, rest + end - name, false) && web_component_allowed(out, strlen(out));
}

bool web_content_disposition(char* out, size_t size, const char* name) {
    static const char hex[] = "0123456789ABCDEF";

    size_t pos = 0;
    if(!web_append(out, size, &pos, "attachment; filename=\"", 22)) {
        return false;
    }

    // One '_' per character the quoted ASCII form cannot hold.
    for(const unsigned char* c = (const unsigned char*) name; *c != '\0'; c++) {
        if((*c & 0xC0) == 0x80) {
            continue;
        }

        char ascii = *c >= 0x20 && *c < 0x7F && *c != '"' && *c != '\\' && *c != '%' ? (char) *c : '_';
        if(!web_append(out, size, &pos, &ascii, 1)) {
            return false;
        }
    }

    if(!web_append(out, size, &pos, "\"; filename*=UTF-8''", 20)) {
        return false;
    }

    // RFC 5987 attr-char stays as it is; every other byte is %XX.
    for(const unsigned char* c = (const unsigned char*) name; *c != '\0'; c++) {
        bool plain = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || web_is_digit((char) *c) || (*c < 0x80 && strchr("!#$&+-.^_`|~", *c) != NULL);
        char escaped[3] = {'%', hex[*c >> 4], hex[*c & 0xF]};
        if(!(plain ? web_append(out, size, &pos, (const char*) c, 1) : web_append(out, size, &pos, escaped, 3))) {
            return false;
        }
    }

    return true;
}
