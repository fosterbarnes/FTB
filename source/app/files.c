#include <malloc.h>
#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "resources.h"
#include "safety.h"
#include "screens.h"
#include "action/action.h"
#include "task/uitask.h"
#include "../core/core.h"

// Every action row's data is a files_action; its kind picks how the row runs.
typedef enum {
    FILES_ACTION_RUN, // run(items, selected)
    FILES_ACTION_RUN_FILTERED, // runFiltered(items, selected, ...): only rows the Options filters show
    FILES_ACTION_COPY, // clipboard: the selected file or folder
    FILES_ACTION_COPY_CONTENTS // clipboard: the selected folder's contents
} files_action_kind;

typedef struct {
    files_action_kind kind;
    void (*run)(linked_list* items, list_item* selected);
    void (*runFiltered)(linked_list* items, list_item* selected, bool (*filter)(void* data, const char* name, u32 attributes), void* filterData);
} files_action;

#define FILES_ACTION_ITEM(label, ...) {.name = label, .color = COLOR_TEXT, .data = &(files_action) {__VA_ARGS__}}

static list_item rename_opt = FILES_ACTION_ITEM("Rename", .kind = FILES_ACTION_RUN, .run = action_rename);
static list_item copy = FILES_ACTION_ITEM("Copy", .kind = FILES_ACTION_COPY);
static list_item paste = FILES_ACTION_ITEM("Paste", .kind = FILES_ACTION_RUN, .run = action_paste_contents);

static list_item delete_file = FILES_ACTION_ITEM("Delete", .kind = FILES_ACTION_RUN, .run = action_delete_file);

static list_item install_cia = FILES_ACTION_ITEM("Install CIA", .kind = FILES_ACTION_RUN, .run = action_install_cia);
static list_item install_and_delete_cia = FILES_ACTION_ITEM("Install and delete CIA", .kind = FILES_ACTION_RUN, .run = action_install_cia_delete);

static list_item install_ticket = FILES_ACTION_ITEM("Install ticket", .kind = FILES_ACTION_RUN, .run = action_install_ticket);
static list_item install_and_delete_ticket = FILES_ACTION_ITEM("Install and delete ticket", .kind = FILES_ACTION_RUN, .run = action_install_ticket_delete);

static list_item delete_dir = FILES_ACTION_ITEM("Delete", .kind = FILES_ACTION_RUN, .run = action_delete_dir);
static list_item copy_all_contents = FILES_ACTION_ITEM("Copy all contents", .kind = FILES_ACTION_COPY_CONTENTS);
static list_item delete_all_contents = FILES_ACTION_ITEM("Delete all contents", .kind = FILES_ACTION_RUN, .run = action_delete_dir_contents);
static list_item new_folder = FILES_ACTION_ITEM("New folder", .kind = FILES_ACTION_RUN, .run = action_new_folder);

static list_item install_all_cias = FILES_ACTION_ITEM("Install all CIAs", .kind = FILES_ACTION_RUN_FILTERED, .runFiltered = action_install_cias);
static list_item install_and_delete_all_cias = FILES_ACTION_ITEM("Install and delete all CIAs", .kind = FILES_ACTION_RUN_FILTERED, .runFiltered = action_install_cias_delete);
static list_item delete_all_cias = FILES_ACTION_ITEM("Delete all CIAs", .kind = FILES_ACTION_RUN, .run = action_delete_dir_cias);

static list_item install_all_tickets = FILES_ACTION_ITEM("Install all tickets", .kind = FILES_ACTION_RUN_FILTERED, .runFiltered = action_install_tickets);
static list_item install_and_delete_all_tickets = FILES_ACTION_ITEM("Install and delete all tickets", .kind = FILES_ACTION_RUN_FILTERED, .runFiltered = action_install_tickets_delete);
static list_item delete_all_tickets = FILES_ACTION_ITEM("Delete all tickets", .kind = FILES_ACTION_RUN, .run = action_delete_dir_tickets);

typedef struct {
    populate_files_data populateData;

    bool populated;
    unsigned int checkedVersion; // items->version when the current directory was last checked

    FS_ArchiveID archiveId;
    FS_Path archivePath;
    FS_Archive archive;

    bool showHidden;
    bool showDirectories;
    bool showFiles;
    bool showCias;
    bool showTickets;

    char currDir[FILE_PATH_MAX];
} files_data;

typedef struct {
    linked_list* items;
    list_item* selected;
    files_data* parent;

    bool containsCias;
    bool containsTickets;
} files_action_data;

// System storage opens read-only while system changes are locked (safety.h).
static bool files_read_only(FS_ArchiveID archiveId) {
    switch(archiveId) {
        case ARCHIVE_NAND_CTR_FS:
        case ARCHIVE_NAND_TWL_FS:
        case ARCHIVE_SYSTEM_SAVEDATA:
        case ARCHIVE_SHARED_EXTDATA:
            return !safety_allowed();
        default:
            return false;
    }
}

static bool files_filter(void* data, const char* name, u32 attributes) {
    files_data* listData = (files_data*) data;

    if((attributes & FS_ATTRIBUTE_HIDDEN) != 0 && !listData->showHidden) {
        return false;
    }

    if((attributes & FS_ATTRIBUTE_DIRECTORY) != 0) {
        return listData->showDirectories;
    } else {
        if((fs_filter_cias(NULL, name, attributes) && !listData->showCias) || (fs_filter_tickets(NULL, name, attributes) && !listData->showTickets)) {
            return false;
        }

        return listData->showFiles;
    }
}

static void files_action_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    task_draw_file_info(view, ((files_action_data*) data)->selected->data, x1, y1, x2, y2);
}

static void files_action_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    files_action_data* actionData = (files_action_data*) data;

    if(hidKeysDown() & KEY_B) {
        ui_pop();
        list_destroy(view);

        free(data);

        return;
    }

    if(selected != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        const files_action* action = (const files_action*) selected->data;

        ui_pop();
        list_destroy(view);

        switch(action->kind) {
            case FILES_ACTION_COPY:
            case FILES_ACTION_COPY_CONTENTS: {
                file_info* info = (file_info*) actionData->selected->data;
                bool contents = action->kind == FILES_ACTION_COPY_CONTENTS;

                Result res = 0;
                if(R_SUCCEEDED(res = clipboard_set_contents(actionData->parent->archive, info->path, contents))) {
                    prompt_display_notify("Success", contents ? "Current directory contents copied to clipboard." : (info->attributes & FS_ATTRIBUTE_DIRECTORY) ? "Current directory copied to clipboard." : "File copied to clipboard.", COLOR_TEXT, info, task_draw_file_info, NULL);
                } else {
                    error_display_res(info, task_draw_file_info, res, "Failed to copy to clipboard.");
                }

                break;
            }
            case FILES_ACTION_RUN_FILTERED:
                action->runFiltered(actionData->items, actionData->selected, files_filter, actionData->parent);
                break;
            case FILES_ACTION_RUN:
                action->run(actionData->items, actionData->selected);
                break;
        }

        free(data);

        return;
    }

    if(linked_list_size(items) == 0) {
        file_info* info = (file_info*) actionData->selected->data;

        // Read-only: copying out (and installing, which checks the lock itself) only.
        if(files_read_only(actionData->parent->archiveId)) {
            if(info->attributes & FS_ATTRIBUTE_DIRECTORY) {
                if(actionData->containsCias) {
                    linked_list_add(items, &install_all_cias);
                }

                if(actionData->containsTickets) {
                    linked_list_add(items, &install_all_tickets);
                }

                linked_list_add(items, &copy_all_contents);
            } else {
                if(info->isCia) {
                    linked_list_add(items, &install_cia);
                }

                if(info->isTicket) {
                    linked_list_add(items, &install_ticket);
                }
            }

            linked_list_add(items, &copy);
            return;
        }

        if(info->attributes & FS_ATTRIBUTE_DIRECTORY) {
            if(actionData->containsCias) {
                linked_list_add(items, &install_all_cias);
                linked_list_add(items, &install_and_delete_all_cias);
                linked_list_add(items, &delete_all_cias);
            }

            if(actionData->containsTickets) {
                linked_list_add(items, &install_all_tickets);
                linked_list_add(items, &install_and_delete_all_tickets);
                linked_list_add(items, &delete_all_tickets);
            }

            linked_list_add(items, &copy_all_contents);
            linked_list_add(items, &delete_all_contents);

            linked_list_add(items, &new_folder);

            linked_list_add(items, &delete_dir);
        } else {
            if(info->isCia) {
                linked_list_add(items, &install_cia);
                linked_list_add(items, &install_and_delete_cia);
            }

            if(info->isTicket) {
                linked_list_add(items, &install_ticket);
                linked_list_add(items, &install_and_delete_ticket);
            }

            linked_list_add(items, &delete_file);
        }

        linked_list_add(items, &rename_opt);
        linked_list_add(items, &copy);
        linked_list_add(items, &paste);
    }
}

static void files_action_open(linked_list* items, list_item* selected, files_data* parent) {
    files_action_data* data = (files_action_data*) calloc(1, sizeof(files_action_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate files action data.");

        return;
    }

    data->items = items;
    data->selected = selected;
    data->parent = parent;

    data->containsCias = false;
    data->containsTickets = false;

    linked_list_iter iter;
    linked_list_iterate(data->items, &iter);

    while(linked_list_iter_has_next(&iter)) {
        file_info* info = (file_info*) ((list_item*) linked_list_iter_next(&iter))->data;

        if(info->isCia) {
            data->containsCias = true;
        } else if(info->isTicket) {
            data->containsTickets = true;
        }
    }

    list_display((((file_info*) selected->data)->attributes & FS_ATTRIBUTE_DIRECTORY) ? "Directory Action" : "File Action", "A: Select, B: Return", data, files_action_update, files_action_draw_top);
}

static void files_options_add_entry(linked_list* items, const char* name, bool* val) {
    list_item* item = (list_item*) calloc(1, sizeof(list_item));
    if(item != NULL) {
        snprintf(item->name, LIST_ITEM_NAME_MAX, "%s", name);
        item->color = *val ? COLOR_ENABLED : COLOR_DISABLED;
        item->data = val;

        linked_list_add(items, item);
    }
}

static void files_options_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    files_data* listData = (files_data*) data;

    if(hidKeysDown() & KEY_B) {
        linked_list_iter iter;
        linked_list_iterate(items, &iter);

        while(linked_list_iter_has_next(&iter)) {
            free(linked_list_iter_next(&iter));
            linked_list_iter_remove(&iter);
        }

        ui_pop();
        list_destroy(view);

        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        bool* val = (bool*) selected->data;
        *val = !(*val);

        selected->color = *val ? COLOR_ENABLED : COLOR_DISABLED;

        listData->populated = false;
    }

    if(linked_list_size(items) == 0) {
        files_options_add_entry(items, "Show hidden", &listData->showHidden);
        files_options_add_entry(items, "Show directories", &listData->showDirectories);
        files_options_add_entry(items, "Show files", &listData->showFiles);
        files_options_add_entry(items, "Show CIAs", &listData->showCias);
        files_options_add_entry(items, "Show tickets", &listData->showTickets);
    }
}

static void files_options_open(files_data* data) {
    list_display("Options", "A: Toggle, B: Return", data, files_options_update, NULL);
}

static void files_draw_top(ui_view* view, void* data, float x1, float y1, float x2, float y2, list_item* selected) {
    if(selected != NULL && selected->data != NULL) {
        task_draw_file_info(view, selected->data, x1, y1, x2, y2);
    }
}

static void files_repopulate(files_data* listData, linked_list* items) {
    task_cancel_and_wait(&listData->populateData.state);

    listData->populateData.items = items;
    listData->populateData.archive = listData->archive;
    string_copy(listData->populateData.path, listData->currDir, FILE_PATH_MAX);

    Result res = task_populate_files(&listData->populateData);
    if(R_FAILED(res)) {
        error_display_res(NULL, NULL, res, "Failed to initiate file list population.");
    }

    listData->populated = true;
}

static void files_navigate(files_data* listData, linked_list* items, const char* path) {
    string_copy(listData->currDir, path, FILE_PATH_MAX);

    listData->populated = false;
}

static void files_free_data(files_data* data) {
    task_cancel_and_wait(&data->populateData.state);

    if(data->archive != 0) {
        fs_close_archive(data->archive);
        data->archive = 0;
    }

    if(data->archivePath.data != NULL) {
        free((void*) data->archivePath.data);
        data->archivePath.data = NULL;
    }

    free(data);
}

static void files_update(ui_view* view, void* data, linked_list* items, list_item* selected, bool selectedTouched) {
    files_data* listData = (files_data*) data;

    if(listData->populated) {
        // Detect whether the current directory was renamed by an action.
        list_item* currDirItem = linked_list_get(items, 0);
        if(currDirItem != NULL && strncmp(listData->currDir, ((file_info*) currDirItem->data)->path, FILE_PATH_MAX) != 0) {
            string_copy(listData->currDir, ((file_info*) currDirItem->data)->path, FILE_PATH_MAX);
        }
    }

    // The current directory can only vanish through an action, and actions change the rows; so the
    // FS probe runs before a listing and after the rows change, not every frame. The root stops the
    // walk (a removed SD card fails every path).
    bool rowsChanged = task_is_finished(&listData->populateData.state) && items->version != listData->checkedVersion;
    if(!listData->populated || rowsChanged) {
        while(strcmp(listData->currDir, "/") != 0 && !fs_is_dir(listData->archive, listData->currDir)) {
            char parentDir[FILE_PATH_MAX] = {'\0'};
            string_get_parent_path(parentDir, listData->currDir, FILE_PATH_MAX);

            files_navigate(listData, items, parentDir);
        }

        listData->checkedVersion = items->version;
    }

    if(hidKeysDown() & KEY_B) {
        if(strncmp(listData->currDir, "/", FILE_PATH_MAX) == 0) {
            ui_pop();

            files_free_data(listData);

            task_clear_files(items);
            list_destroy(view);

            return;
        } else {
            char parentDir[FILE_PATH_MAX] = {'\0'};
            string_get_parent_path(parentDir, listData->currDir, FILE_PATH_MAX);

            files_navigate(listData, items, parentDir);
        }
    }

    if(hidKeysDown() & KEY_SELECT) {
        files_options_open(listData);
        return;
    }

    if(selected != NULL && selected->data != NULL && (selectedTouched || (hidKeysDown() & KEY_A))) {
        file_info* fileInfo = (file_info*) selected->data;

        if((fileInfo->attributes & FS_ATTRIBUTE_DIRECTORY) && !fileInfo->isBase) {
            files_navigate(listData, items, fileInfo->path);
        } else if(task_is_finished(&listData->populateData.state)) {
            // Actions remove rows, so they wait until population no longer inserts into the list.
            files_action_open(items, selected, listData);
            return;
        }
    }

    if(!listData->populated || (hidKeysDown() & KEY_X)) {
        files_repopulate(listData, items);
    }

    task_state* state = &listData->populateData.state;
    if(task_is_finished(state) && R_FAILED(state->result)) {
        error_display_res(NULL, NULL, state->result, "Failed to populate file list.");

        state->result = 0;
    }
}

void files_open(FS_ArchiveID archiveId, FS_Path archivePath) {
    files_data* data = (files_data*) calloc(1, sizeof(files_data));
    if(data == NULL) {
        error_display(NULL, NULL, "Failed to allocate files data.");

        return;
    }

    data->populateData.recursive = false;
    data->populateData.includeBase = true;
    data->populateData.meta = true;

    data->populateData.filter = files_filter;
    data->populateData.filterData = data;

    data->populateData.state.finished = true;

    data->populated = false;

    data->showHidden = false;
    data->showDirectories = true;
    data->showFiles = true;
    data->showCias = true;
    data->showTickets = true;

    data->archiveId = archiveId;
    data->archivePath.type = archivePath.type;
    data->archivePath.size = archivePath.size;
    if(archivePath.data != NULL) {
        data->archivePath.data = calloc(1, data->archivePath.size);
        if(data->archivePath.data == NULL) {
            error_display(NULL, NULL, "Failed to allocate files data.");

            files_free_data(data);
            return;
        }

        memcpy((void*) data->archivePath.data, archivePath.data, data->archivePath.size);
    } else {
        data->archivePath.data = NULL;
    }

    snprintf(data->currDir, FILE_PATH_MAX, "/");

    Result res = 0;
    if(R_FAILED(res = fs_open_archive(&data->archive, archiveId, archivePath))) {
        error_display_res(NULL, NULL, res, "Failed to open file listing archive.");

        files_free_data(data);
        return;
    }

    list_display(files_read_only(archiveId) ? "Files (Read-only)" : "Files", "A: Select, B: Back, X: Refresh, Select: Options", data, files_update, files_draw_top);
}

static void files_open_nand_warning_onresponse(ui_view* view, void* data, u32 response) {
    FS_ArchiveID archive = (FS_ArchiveID) data;

    if(response == PROMPT_YES) {
        files_open(archive, fsMakePath(PATH_EMPTY, ""));
    }
}

void files_open_nand_warning(FS_ArchiveID archive) {
    // Read-only NAND cannot be modified, so it needs no warning.
    if(files_read_only(archive)) {
        files_open(archive, fsMakePath(PATH_EMPTY, ""));
        return;
    }

    prompt_display_yes_no("Confirmation", "Modifying the NAND is dangerous and can render\n the system inoperable.\nMake sure you know what you are doing.\n\nProceed?", COLOR_TEXT, (void*) archive, NULL, files_open_nand_warning_onresponse);
}

void files_open_sd() {
    files_open(ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""));
}

void files_open_ctr_nand() {
    files_open_nand_warning(ARCHIVE_NAND_CTR_FS);
}

void files_open_twl_nand() {
    files_open_nand_warning(ARCHIVE_NAND_TWL_FS);
}

void files_open_twl_photo() {
    files_open(ARCHIVE_TWL_PHOTO, fsMakePath(PATH_EMPTY, ""));
}

void files_open_twl_sound() {
    files_open(ARCHIVE_TWL_SOUND, fsMakePath(PATH_EMPTY, ""));
}