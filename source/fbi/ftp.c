#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "section.h"
#include "../core/core.h"
#include "../ftpd/ftpd.h"

// FTP server screen: ftpd (source/ftpd) runs on its own thread while this view is open. The top
// screen shows the server status and log, the bottom screen one panel per client.

#define FTP_LOG_SCALE 0.4f
#define FTP_SESSION_SCALE 0.4f
#define FTP_LOG_VISIBLE_MAX 32
#define FTP_SESSIONS_MAX 8
#define FTP_ADDRESS_MAX 32

typedef struct {
    ftpd_log_line lines[FTP_LOG_VISIBLE_MAX];
    // Lines up from the newest; 0 follows new lines as they arrive.
    u32 scroll;
    u32 visibleLines;
    u64 lastSerial;
    u64 nextScrollTime;

    // Buttons held since SELECT went down; a lone SELECT press toggles the backlight.
    u32 selectButtons;

    ftpd_session sessions[FTP_SESSIONS_MAX];
    size_t sessionCount;

    bool online;
    char address[FTP_ADDRESS_MAX];

    ftpd_settings edit;
    char aboutText[512];
} ftp_data;

static u32 ftp_log_color(ftpd_log_level level) {
    switch(level) {
        case FTPD_LOG_ERROR:
            return COLOR_FTP_ERROR;
        case FTPD_LOG_COMMAND:
            return COLOR_FTP_COMMAND;
        case FTPD_LOG_RESPONSE:
            return COLOR_FTP_RESPONSE;
        default:
            return COLOR_TEXT;
    }
}

static void ftp_draw_log(ftp_data* data, float x1, float y1, float x2, float y2) {
    char status[FTP_ADDRESS_MAX + 64];
    snprintf(status, sizeof(status), "%s  %s", ftpd_version(), data->online ? data->address : "Waiting for WiFi...");

    float statusHeight = screen_get_font_height(0.5f);
    screen_draw_string(status, x1 + 2, y1 + 1, 0.5f, 0.5f, COLOR_TEXT, false);

    float logY1 = y1 + statusHeight + 2;
    float lineHeight = screen_get_font_height(FTP_LOG_SCALE);

    u32 visible = (u32) ((y2 - logY1) / lineHeight);
    if(visible > FTP_LOG_VISIBLE_MAX) {
        visible = FTP_LOG_VISIBLE_MAX;
    }

    data->visibleLines = visible;

    size_t total = 0;
    u64 serial = ftpd_log_read(0, NULL, 0, &total);

    // Keep a scrolled view on the same lines while new ones arrive.
    if(data->scroll > 0 && serial > data->lastSerial) {
        data->scroll += (u32) (serial - data->lastSerial);
    }

    data->lastSerial = serial;

    u32 maxScroll = total > visible ? (u32) (total - visible) : 0;
    if(data->scroll > maxScroll) {
        data->scroll = maxScroll;
    }

    size_t first = total > visible + data->scroll ? total - visible - data->scroll : 0;
    ftpd_log_read(first, data->lines, visible, &total);

    size_t count = total > first ? total - first : 0;
    if(count > visible) {
        count = visible;
    }

    for(size_t i = 0; i < count; i++) {
        screen_draw_string(data->lines[i].text, x1 + 2, logY1 + i * lineHeight, FTP_LOG_SCALE, FTP_LOG_SCALE, ftp_log_color(data->lines[i].level), false);
    }

    if(total > visible) {
        u32 scrollBarWidth = 0;
        screen_get_texture_size(&scrollBarWidth, NULL, TEXTURE_SCROLL_BAR);

        float viewHeight = y2 - logY1;
        float scrollBarHeight = viewHeight * visible / total;
        float scrollBarY = logY1 + viewHeight * first / total;

        screen_draw_texture(TEXTURE_SCROLL_BAR, x2 - scrollBarWidth, scrollBarY, scrollBarWidth, scrollBarHeight);
    }
}

static void ftp_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ftp_draw_log((ftp_data*) data, x1, y1, x2, y2);
}

static void ftp_list_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    ftp_draw_log((ftp_data*) data, x1, y1, x2, y2);
}

// Draws text clipped from the left with "..." so the end of a long path stays visible.
static void ftp_draw_tail(const char* text, float x, float y, float maxWidth, u32 color) {
    float width = 0;
    screen_get_string_size(&width, NULL, text, FTP_SESSION_SCALE, FTP_SESSION_SCALE);
    if(width <= maxWidth) {
        screen_draw_string(text, x, y, FTP_SESSION_SCALE, FTP_SESSION_SCALE, color, false);
        return;
    }

    char clipped[FTPD_TEXT_MAX + 3] = "...";
    size_t length = strlen(text);

    // Estimate from the average glyph width, then trim until it fits; a few measures at most.
    size_t start = (size_t) (length * (1.0f - maxWidth / width)) + 3;
    while(start < length) {
        while(start < length && ((u8) text[start] & 0xC0) == 0x80) {
            start++;
        }

        snprintf(clipped, sizeof(clipped), "...%s", text + start);
        screen_get_string_size(&width, NULL, clipped, FTP_SESSION_SCALE, FTP_SESSION_SCALE);
        if(width <= maxWidth) {
            break;
        }

        start += 2;
    }

    screen_draw_string(clipped, x, y, FTP_SESSION_SCALE, FTP_SESSION_SCALE, color, false);
}

static void ftp_draw_rate_history(const ftpd_session* session, float x, float y, float height) {
    float max = 0;
    for(u32 i = 0; i < FTPD_RATE_HISTORY; i++) {
        if(session->history[i] > max) {
            max = session->history[i];
        }
    }

    if(max <= 0) {
        return;
    }

    for(u32 i = 0; i < FTPD_RATE_HISTORY; i++) {
        float barHeight = height * session->history[i] / max;
        if(barHeight > 0) {
            screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, x + i, y + height - barHeight, 1, barHeight);
        }
    }
}

static void ftp_draw_session(const ftpd_session* session, float x1, float y, float x2, float lineHeight, float boxHeight) {
    static const char* stateNames[] = {"Idle", "Connecting", "Transferring"};

    screen_draw_texture(TEXTURE_META_INFO_BOX, x1, y, x2 - x1, boxHeight);

    float textX = x1 + 4;
    float textY = y + 2;
    float textWidth = x2 - x1 - 8;

    screen_draw_string(session->peer, textX, textY, FTP_SESSION_SCALE, FTP_SESSION_SCALE, COLOR_TEXT, false);

    const char* state = stateNames[session->state];
    float stateWidth = 0;
    screen_get_string_size(&stateWidth, NULL, state, FTP_SESSION_SCALE, FTP_SESSION_SCALE);
    screen_draw_string(state, x2 - 4 - stateWidth, textY, FTP_SESSION_SCALE, FTP_SESSION_SCALE, COLOR_TEXT, false);

    ftp_draw_tail(session->item, textX, textY + lineHeight, textWidth, COLOR_TEXT);

    if(session->rate < 0) {
        return;
    }

    // The rate starts at 0 and only goes wrong on a zero-length frame; keep the cast in range.
    u64 rate = session->rate < 1e12f ? (u64) session->rate : 0;

    char progress[96];
    if(session->size > 0) {
        snprintf(progress, sizeof(progress), "%.1f %s / %.1f %s  %.1f %s/s",
                 ui_get_display_size(session->position), ui_get_display_size_units(session->position),
                 ui_get_display_size(session->size), ui_get_display_size_units(session->size),
                 ui_get_display_size(rate), ui_get_display_size_units(rate));
    } else {
        snprintf(progress, sizeof(progress), "%.1f %s  %.1f %s/s",
                 ui_get_display_size(session->position), ui_get_display_size_units(session->position),
                 ui_get_display_size(rate), ui_get_display_size_units(rate));
    }

    screen_draw_string(progress, textX, textY + lineHeight * 2, FTP_SESSION_SCALE, FTP_SESSION_SCALE, COLOR_TEXT, false);

    ftp_draw_rate_history(session, x2 - 4 - FTPD_RATE_HISTORY, textY + lineHeight * 2, lineHeight - 2);

    if(session->size > 0) {
        float fraction = session->position >= session->size ? 1.0f : (float) session->position / (float) session->size;
        screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, textX, y + boxHeight - 3, textWidth * fraction, 2);
    }
}

static void ftp_draw_bottom(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ftp_data* ftpData = (ftp_data*) data;

    if(ftpData->sessionCount == 0) {
        char text[FTP_ADDRESS_MAX + 64];
        if(ftpData->online) {
            snprintf(text, sizeof(text), "ftp://%s\nNo clients connected", ftpData->address);
        } else {
            snprintf(text, sizeof(text), "Waiting for WiFi...");
        }

        float textWidth;
        float textHeight;
        screen_get_string_size(&textWidth, &textHeight, text, 0.5f, 0.5f);
        screen_draw_string(text, x1 + (x2 - x1 - textWidth) / 2, y1 + (y2 - y1 - textHeight) / 2, 0.5f, 0.5f, COLOR_TEXT, true);
        return;
    }

    float lineHeight = screen_get_font_height(FTP_SESSION_SCALE);
    float boxHeight = lineHeight * 3 + 6;

    size_t shown = ftpData->sessionCount < FTP_SESSIONS_MAX ? ftpData->sessionCount : FTP_SESSIONS_MAX;
    float y = y1 + 4;

    size_t drawn = 0;
    for(; drawn < shown && y + boxHeight <= y2; drawn++) {
        ftp_draw_session(&ftpData->sessions[drawn], x1 + 4, y, x2 - 4, lineHeight, boxHeight);
        y += boxHeight + 4;
    }

    if(drawn < ftpData->sessionCount) {
        char more[64];
        snprintf(more, sizeof(more), "%u more connection(s)", (unsigned) (ftpData->sessionCount - drawn));
        screen_draw_string(more, x1 + 8, y2 - lineHeight - 1, FTP_SESSION_SCALE, FTP_SESSION_SCALE, COLOR_TEXT, false);
    }
}

static void ftp_close_list(ui_view* view) {
    ui_pop();
    list_destroy(view);
}

static void ftp_kbd_text(ui_view* view, void* data, SwkbdButton button, const char* response, char* out) {
    if(button == SWKBD_BUTTON_CONFIRM) {
        string_copy(out, response, FTPD_SETTING_MAX);
    }
}

static void ftp_user_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    ftp_kbd_text(view, data, button, response, ((ftp_data*) data)->edit.user);
}

static void ftp_pass_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    ftp_kbd_text(view, data, button, response, ((ftp_data*) data)->edit.pass);
}

static void ftp_hostname_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    ftp_kbd_text(view, data, button, response, ((ftp_data*) data)->edit.hostname);
}

static void ftp_port_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    unsigned long port = strtoul(response, NULL, 10);
    if(port < 1 || port > 65535) {
        prompt_display_notify("Failure", "Port must be from 1 to 65535.", COLOR_TEXT, data, ftp_draw_top, NULL);
        return;
    }

    ((ftp_data*) data)->edit.port = (u16) port;
}

static void ftp_deflate_onresponse(ui_view* view, void* data, SwkbdButton button, const char* response) {
    if(button != SWKBD_BUTTON_CONFIRM) {
        return;
    }

    unsigned long level = strtoul(response, NULL, 10);
    if(level > 9) {
        prompt_display_notify("Failure", "Deflate level must be from 0 to 9.", COLOR_TEXT, data, ftp_draw_top, NULL);
        return;
    }

    ((ftp_data*) data)->edit.deflateLevel = (int) level;
}

static void ftp_settings_edit_user(ui_view* view, ftp_data* data) {
    kbd_display("User (empty: no login)", data->edit.user, SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, FTPD_SETTING_MAX - 1, data, ftp_user_onresponse);
}

static void ftp_settings_edit_pass(ui_view* view, ftp_data* data) {
    kbd_display("Password (empty: none)", data->edit.pass, SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, FTPD_SETTING_MAX - 1, data, ftp_pass_onresponse);
}

static void ftp_settings_edit_hostname(ui_view* view, ftp_data* data) {
    kbd_display("mDNS hostname (empty: 3ds-ftpd)", data->edit.hostname, SWKBD_TYPE_NORMAL, 0, SWKBD_ANYTHING, FTPD_SETTING_MAX - 1, data, ftp_hostname_onresponse);
}

static void ftp_settings_edit_port(ui_view* view, ftp_data* data) {
    char port[8];
    snprintf(port, sizeof(port), "%u", data->edit.port);
    kbd_display("Port (1-65535)", port, SWKBD_TYPE_NUMPAD, 0, SWKBD_NOTEMPTY_NOTBLANK, 6, data, ftp_port_onresponse);
}

static void ftp_settings_edit_deflate(ui_view* view, ftp_data* data) {
    char level[4];
    snprintf(level, sizeof(level), "%d", data->edit.deflateLevel);
    kbd_display("Deflate level (0-9)", level, SWKBD_TYPE_NUMPAD, 0, SWKBD_NOTEMPTY_NOTBLANK, 2, data, ftp_deflate_onresponse);
}

static void ftp_settings_toggle_mtime(ui_view* view, ftp_data* data) {
    data->edit.getMTime = !data->edit.getMTime;
}

static void ftp_settings_apply(ui_view* view, ftp_data* data) {
    ftpd_apply_settings(&data->edit, false);
    ftp_close_list(view);
}

static void ftp_settings_save(ui_view* view, ftp_data* data) {
    bool saved = ftpd_apply_settings(&data->edit, true);
    ftp_close_list(view);

    if(!saved) {
        prompt_display_notify("Failure", "Settings applied, but saving\nsdmc:/ftb/ftpd.cfg failed.", COLOR_TEXT, data, ftp_draw_top, NULL);
    }
}

static void ftp_settings_reset(ui_view* view, ftp_data* data) {
    ftpd_get_default_settings(&data->edit);
}

static list_item ftp_setting_user = {"", COLOR_TEXT, ftp_settings_edit_user};
static list_item ftp_setting_pass = {"", COLOR_TEXT, ftp_settings_edit_pass};
static list_item ftp_setting_hostname = {"", COLOR_TEXT, ftp_settings_edit_hostname};
static list_item ftp_setting_port = {"", COLOR_TEXT, ftp_settings_edit_port};
static list_item ftp_setting_deflate = {"", COLOR_TEXT, ftp_settings_edit_deflate};
static list_item ftp_setting_mtime = {"", COLOR_TEXT, ftp_settings_toggle_mtime};
static list_item ftp_setting_apply = {"Apply", COLOR_TEXT, ftp_settings_apply};
static list_item ftp_setting_save = {"Save", COLOR_TEXT, ftp_settings_save};
static list_item ftp_setting_reset = {"Reset to Defaults", COLOR_TEXT, ftp_settings_reset};

static void ftp_settings_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    ftp_data* ftpData = (ftp_data*) data;

    if(hidKeysDown() & KEY_B) {
        ftp_close_list(view);
        return;
    }

    if(selected != NULL && (selectedTouched || hidKeysDown() & KEY_A) && selected->data != NULL) {
        ((void (*)(ui_view*, ftp_data*)) selected->data)(view, ftpData);
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &ftp_setting_user);
        linked_list_add(items, &ftp_setting_pass);
        linked_list_add(items, &ftp_setting_hostname);
        linked_list_add(items, &ftp_setting_port);
        linked_list_add(items, &ftp_setting_deflate);
        linked_list_add(items, &ftp_setting_mtime);
        linked_list_add(items, &ftp_setting_apply);
        linked_list_add(items, &ftp_setting_save);
        linked_list_add(items, &ftp_setting_reset);
    }

    ftpd_settings* edit = &ftpData->edit;
    snprintf(ftp_setting_user.name, LIST_ITEM_NAME_MAX, "User: %s", edit->user[0] != '\0' ? edit->user : "(no login)");
    snprintf(ftp_setting_pass.name, LIST_ITEM_NAME_MAX, "Password: %s", edit->pass[0] != '\0' ? "********" : "(none)");
    snprintf(ftp_setting_hostname.name, LIST_ITEM_NAME_MAX, "Hostname: %s", edit->hostname[0] != '\0' ? edit->hostname : "3ds-ftpd");
    snprintf(ftp_setting_port.name, LIST_ITEM_NAME_MAX, "Port: %u", edit->port);
    snprintf(ftp_setting_deflate.name, LIST_ITEM_NAME_MAX, "Deflate Level: %d", edit->deflateLevel);
    snprintf(ftp_setting_mtime.name, LIST_ITEM_NAME_MAX, "Get mtime: %s", edit->getMTime ? "On" : "Off");
    ftp_setting_mtime.color = edit->getMTime ? COLOR_ENABLED : COLOR_DISABLED;
}

static void ftp_menu_settings(ui_view* view, ftp_data* data) {
    ftp_close_list(view);

    ftpd_get_settings(&data->edit);
    list_display("FTP Settings", "A: Select, B: Cancel", data, ftp_settings_update, ftp_list_draw_top);
}

static void ftp_menu_upload_log(ui_view* view, ftp_data* data) {
    ftp_close_list(view);

    if(!ftpd_upload_log()) {
        prompt_display_notify("Failure", "A log upload is already running.", COLOR_TEXT, data, ftp_draw_top, NULL);
    }
}

static void ftp_menu_about(ui_view* view, ftp_data* data) {
    ftp_close_list(view);

    snprintf(data->aboutText, sizeof(data->aboutText),
             "%s, built into FTB\n"
             "Copyright (C) 2026 Michael Theall,\n"
             "Dave Murphy, TuxSH\n"
             "\n"
             "Free software under the GNU GPL v3 or later,\n"
             "with ABSOLUTELY NO WARRANTY.\n"
             "Source: github.com/fosterbarnes/FTB",
             ftpd_version());

    prompt_display_notify("About", data->aboutText, COLOR_TEXT, data, ftp_draw_top, NULL);
}

static list_item ftp_menu_item_settings = {"Settings", COLOR_TEXT, ftp_menu_settings};
static list_item ftp_menu_item_upload_log = {"Upload Log", COLOR_TEXT, ftp_menu_upload_log};
static list_item ftp_menu_item_about = {"About", COLOR_TEXT, ftp_menu_about};

static void ftp_menu_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    if(hidKeysDown() & (KEY_B | KEY_Y)) {
        ftp_close_list(view);
        return;
    }

    if(selected != NULL && (selectedTouched || hidKeysDown() & KEY_A) && selected->data != NULL) {
        ((void (*)(ui_view*, ftp_data*)) selected->data)(view, (ftp_data*) data);
        return;
    }

    if(linked_list_size(items) == 0) {
        linked_list_add(items, &ftp_menu_item_settings);
        linked_list_add(items, &ftp_menu_item_upload_log);
        linked_list_add(items, &ftp_menu_item_about);
    }
}

// Mirrors ftpd: SELECT toggles the backlight only when pressed and released alone, so the
// Rosalina menu combo (L + D-Pad Down + SELECT) never toggles it.
static void ftp_update_backlight(ftp_data* data) {
    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();

    if(down == KEY_SELECT && held == KEY_SELECT) {
        data->selectButtons = KEY_SELECT;
    } else if(hidKeysUp() & KEY_SELECT) {
        if(data->selectButtons == KEY_SELECT) {
            ftpd_toggle_backlight();
        }
    } else {
        data->selectButtons |= held;
    }
}

static void ftp_update_scroll(ftp_data* data) {
    u32 down = hidKeysDown();
    u32 held = hidKeysHeld();
    u64 now = osGetTime();

    u32 step = 0;
    u32 keys = KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT;
    if(down & keys) {
        step = 1;
        data->nextScrollTime = now + 500;
    } else if((held & keys) && now >= data->nextScrollTime) {
        step = 1;
        data->nextScrollTime = now + 100;
    }

    if(step == 0) {
        return;
    }

    u32 page = data->visibleLines > 1 ? data->visibleLines - 1 : 1;
    u32 pressed = (down & keys) ? down : held;

    // Up moves toward older lines; the draw clamps scroll to the log size.
    if(pressed & KEY_UP) {
        data->scroll += 1;
    } else if(pressed & KEY_LEFT) {
        data->scroll += page;
    } else if(pressed & KEY_DOWN) {
        data->scroll = data->scroll > 1 ? data->scroll - 1 : 0;
    } else if(pressed & KEY_RIGHT) {
        data->scroll = data->scroll > page ? data->scroll - page : 0;
    }
}

static void ftp_update(ui_view* view, void* data, float bx1, float by1, float bx2, float by2) {
    ftp_data* ftpData = (ftp_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();

        ftpd_stop();

        free(ftpData);
        ui_destroy(view);
        return;
    }

    if(hidKeysDown() & KEY_Y) {
        list_display("FTP Menu", "A: Select, B: Return", ftpData, ftp_menu_update, ftp_list_draw_top);
        return;
    }

    ftp_update_backlight(ftpData);
    ftp_update_scroll(ftpData);

    ftpData->online = ftpd_address(ftpData->address, sizeof(ftpData->address));
    ftpData->sessionCount = ftpd_sessions(ftpData->sessions, FTP_SESSIONS_MAX);
}

void ftp_open() {
    ftp_data* data = (ftp_data*) calloc(1, sizeof(ftp_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate FTP data.");

        return;
    }

    ftpd_start();

    ui_view* view = ui_create();
    view->name = "FTP Server";
    view->info = "B: Return, Y: Menu, D-Pad: Scroll Log, SELECT: Backlight";
    view->data = data;
    view->update = ftp_update;
    view->drawTop = ftp_draw_top;
    view->drawBottom = ftp_draw_bottom;
    ui_push(view);
}
