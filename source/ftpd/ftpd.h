#pragma once

// C interface to the ftpd server port, used by FTB's FTP screen (source/app/ftp.c).

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Display text cap, including the terminator; matches the log's line cap.
#define FTPD_TEXT_MAX 129
#define FTPD_RATE_HISTORY 100
#define FTPD_SETTING_MAX 33

typedef enum {
    FTPD_LOG_DEBUG,
    FTPD_LOG_INFO,
    FTPD_LOG_ERROR,
    FTPD_LOG_COMMAND,
    FTPD_LOG_RESPONSE
} ftpd_log_level;

typedef struct {
    ftpd_log_level level;
    char text[FTPD_TEXT_MAX];
} ftpd_log_line;

typedef enum {
    FTPD_SESSION_COMMAND,
    FTPD_SESSION_DATA_CONNECT,
    FTPD_SESSION_DATA_TRANSFER
} ftpd_session_state;

typedef struct {
    char peer[16];
    // Work item or working directory; long paths keep their tail.
    char item[FTPD_TEXT_MAX];
    ftpd_session_state state;
    uint64_t position;
    // 0 when the size is unknown.
    uint64_t size;
    // Bytes per second; negative when no transfer is running.
    float rate;
    // Bytes per frame, oldest first; valid when rate >= 0.
    float history[FTPD_RATE_HISTORY];
} ftpd_session;

typedef struct {
    char user[FTPD_SETTING_MAX];
    char pass[FTPD_SETTING_MAX];
    char hostname[FTPD_SETTING_MAX];
    uint16_t port;
    int deflateLevel;
    bool getMTime;
} ftpd_settings;

// Starts the server thread; it listens once wifi is up.
void ftpd_start(void);
// Closes every connection and joins the server thread.
void ftpd_stop(void);

const char* ftpd_version(void);
void ftpd_toggle_backlight(void);

// Writes "ip:port"; false while waiting for wifi.
bool ftpd_address(char* out, size_t size);

// Fills up to max sessions and returns how many exist. Call once per frame.
size_t ftpd_sessions(ftpd_session* out, size_t max);

// Copies up to max log lines from first. *total gets the stored line count. Returns the number of
// lines ever added, so a scrolled view can stay on the same lines while new ones arrive.
uint64_t ftpd_log_read(size_t first, ftpd_log_line* out, size_t max, size_t* total);

void ftpd_get_settings(ftpd_settings* out);
void ftpd_get_default_settings(ftpd_settings* out);
// Applies settings and restarts listening; open sessions stay connected. False if saving failed.
bool ftpd_apply_settings(const ftpd_settings* settings, bool save);

// Starts a log upload to pastie.io; the link lands in the log. False if one is running.
bool ftpd_upload_log(void);

#ifdef __cplusplus
}
#endif
