#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include <3ds.h>

#include "ciafolders.h"
#include "folderpicker.h"
#include "paths.h"
#include "resources.h"
#include "screens.h"
#include "../core/core.h"

#define CIA_FOLDERS_PATH PATHS_CIA_FOLDERS
#define CIA_FOLDERS_TEXT_MARGIN 10

static char folders[CIA_FOLDERS_MAX][FILE_PATH_MAX];
static u32 folderCount;
static u32 version;

u32 ciafolders_count() {
    return folderCount;
}

const char* ciafolders_get(u32 index) {
    return folders[index];
}

u32 ciafolders_version() {
    return version;
}

// Writes path as an absolute SD path ending in '/'; false when it is not absolute or does not fit.
static bool ciafolders_normalize(char* out, const char* path) {
    size_t len = strlen(path);
    if(len == 0 || path[0] != '/') {
        return false;
    }

    bool slash = path[len - 1] == '/';
    if(len + (slash ? 0 : 1) >= FILE_PATH_MAX) {
        return false;
    }

    memcpy(out, path, len);
    if(!slash) {
        out[len++] = '/';
    }

    out[len] = '\0';
    return true;
}

// The SD card ignores case, so /CIA/ and /cia/ are the same folder.
static bool ciafolders_contains(const char* path) {
    for(u32 i = 0; i < folderCount; i++) {
        if(strcasecmp(folders[i], path) == 0) {
            return true;
        }
    }

    return false;
}

void ciafolders_load() {
    folderCount = 0;
    version++;

    FILE* fd = fopen(CIA_FOLDERS_PATH, "r");
    if(fd == NULL) {
        return;
    }

    // A line longer than the buffer arrives in pieces; partial marks the pieces after the first.
    char line[FILE_PATH_MAX + 2];
    bool partial = false;
    while(folderCount < CIA_FOLDERS_MAX && fgets(line, sizeof(line), fd) != NULL) {
        bool lineEnd = strchr(line, '\n') != NULL || feof(fd);
        bool skip = partial || !lineEnd;
        partial = !lineEnd;

        line[strcspn(line, "\r\n")] = '\0';

        char path[FILE_PATH_MAX];
        if(!skip && ciafolders_normalize(path, line) && !ciafolders_contains(path)) {
            string_copy(folders[folderCount++], path, FILE_PATH_MAX);
        }
    }

    fclose(fd);
}

static bool ciafolders_write(FILE* fd, void* data) {
    bool written = true;
    for(u32 i = 0; i < folderCount && written; i++) {
        written = fprintf(fd, "%s\n", folders[i]) > 0;
    }

    return written;
}

static bool ciafolders_save() {
    paths_ensure_data_dir();
    return fs_write_file_safe(CIA_FOLDERS_PATH, ciafolders_write, NULL);
}

const char* ciafolders_add(const char* path) {
    char normalized[FILE_PATH_MAX];
    if(!ciafolders_normalize(normalized, path)) {
        return "Invalid folder path.";
    }

    if(ciafolders_contains(normalized)) {
        return "That folder is already in the list.";
    }

    if(folderCount >= CIA_FOLDERS_MAX) {
        return "The CIA folder list is full.";
    }

    string_copy(folders[folderCount++], normalized, FILE_PATH_MAX);
    version++;

    return ciafolders_save() ? NULL : "Saving " CIA_FOLDERS_PATH " failed.";
}

bool ciafolders_remove(const char* path) {
    for(u32 i = 0; i < folderCount; i++) {
        if(strcmp(folders[i], path) == 0) {
            memmove(folders[i], folders[i + 1], (folderCount - i - 1) * sizeof(folders[0]));
            folderCount--;
            version++;
            break;
        }
    }

    return ciafolders_save();
}

// Folder manager screen: one row per folder, then the add row.

typedef struct {
    bool built;
    u32 version;
} ciafolders_view_data;

// A row owns a copy of its path (item.data), so a row from before a change still names its folder.
typedef struct {
    list_item item;
    char path[FILE_PATH_MAX];
} ciafolders_row;

static list_item add_folder_item = {"Add folder", COLOR_TEXT, NULL};

static char removePath[FILE_PATH_MAX];
static char removeMessage[FILE_PATH_MAX + 48];

static void ciafolders_clear_rows(linked_list* items) {
    linked_list_iter iter;
    linked_list_iterate(items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        list_item* item = (list_item*) linked_list_iter_next(&iter);
        linked_list_iter_remove(&iter);

        if(item != &add_folder_item) {
            free(item);
        }
    }
}

static void ciafolders_build_rows(linked_list* items) {
    FS_Archive archive = 0;
    bool sdOpen = R_SUCCEEDED(fs_open_archive(&archive, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, "")));

    for(u32 i = 0; i < folderCount; i++) {
        ciafolders_row* row = (ciafolders_row*) calloc(1, sizeof(ciafolders_row));
        if(row == NULL) {
            continue;
        }

        bool exists = sdOpen && fs_is_dir(archive, folders[i]);
        string_copy(row->path, folders[i], FILE_PATH_MAX);
        snprintf(row->item.name, LIST_ITEM_NAME_MAX, exists ? "%s" : "%s (missing)", folders[i]);
        row->item.color = exists ? COLOR_TEXT : COLOR_DISABLED;
        row->item.data = row->path;

        if(!linked_list_add(items, &row->item)) {
            free(row);
        }
    }

    if(sdOpen) {
        fs_close_archive(archive);
    }

    if(folderCount < CIA_FOLDERS_MAX) {
        linked_list_add(items, &add_folder_item);
    }
}

static void ciafolders_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected == NULL) {
        return;
    }

    char text[FILE_PATH_MAX + 64];
    if(selected == &add_folder_item) {
        snprintf(text, sizeof(text), "Adds an SD card folder to scan for CIAs.\nSubfolders are scanned too. Up to %d folders.", CIA_FOLDERS_MAX);
    } else if(selected->color == COLOR_DISABLED) {
        snprintf(text, sizeof(text), "%s\nNot found on the SD card: skipped when scanning.", (const char*) selected->data);
    } else {
        snprintf(text, sizeof(text), "%s\nScanned with its subfolders.", (const char*) selected->data);
    }

    float wrapWidth = x2 - x1 - CIA_FOLDERS_TEXT_MARGIN * 2;

    float width = 0;
    float height = 0;
    screen_get_string_size_wrap(&width, &height, text, 0.5f, 0.5f, wrapWidth);

    float x = x1 + (x2 - x1 - width) / 2;
    screen_draw_string_wrap(text, x, y1 + (y2 - y1 - height) / 2, 0.5f, 0.5f, COLOR_TEXT, true, x + wrapWidth);
}

static void ciafolders_notify_failure(const char* message) {
    if(message != NULL) {
        prompt_display_notify("Failure", message, COLOR_TEXT, NULL, NULL, NULL);
    }
}

static void ciafolders_picked(void* data, const char* path) {
    ciafolders_notify_failure(ciafolders_add(path));
}

static void ciafolders_remove_onresponse(ui_view* view, void* data, u32 response) {
    if(response == PROMPT_YES && !ciafolders_remove(removePath)) {
        ciafolders_notify_failure("Saving " CIA_FOLDERS_PATH " failed.");
    }
}

static void ciafolders_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    ciafolders_view_data* viewData = (ciafolders_view_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();

        ciafolders_clear_rows(items);
        list_destroy(view);

        free(viewData);
        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        if(selected == &add_folder_item) {
            folderpicker_open("Add CIA Folder", NULL, ciafolders_picked);
        } else {
            string_copy(removePath, (const char*) selected->data, FILE_PATH_MAX);
            snprintf(removeMessage, sizeof(removeMessage), "Remove %s from the CIA folders?", removePath);
            prompt_display_yes_no("Confirmation", removeMessage, COLOR_TEXT, NULL, NULL, ciafolders_remove_onresponse);
        }

        return;
    }

    if(!viewData->built || viewData->version != version) {
        ciafolders_clear_rows(items);
        ciafolders_build_rows(items);

        viewData->built = true;
        viewData->version = version;
    }
}

void ciafolders_open() {
    ciafolders_view_data* viewData = (ciafolders_view_data*) calloc(1, sizeof(ciafolders_view_data));
    if(viewData == NULL) {
        error_display(NULL, NULL, "Failed to allocate CIA folder data.");

        return;
    }

    list_display("CIA Folders", "A: Select, B: Return", viewData, ciafolders_update, ciafolders_draw_top);
}
