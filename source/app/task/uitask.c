#include <stdio.h>
#include <string.h>

#include <3ds.h>

#include "uitask.h"
#include "../resources.h"
#include "../../core/core.h"

void task_draw_meta_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    meta_info* info = (meta_info*) data;

    u32 metaInfoBoxShadowWidth;
    u32 metaInfoBoxShadowHeight;
    screen_get_texture_size(&metaInfoBoxShadowWidth, &metaInfoBoxShadowHeight, TEXTURE_META_INFO_BOX_SHADOW);

    float metaInfoBoxShadowX = x1 + (x2 - x1 - metaInfoBoxShadowWidth) / 2;
    float metaInfoBoxShadowY = y1 + (y2 - y1) / 4 - metaInfoBoxShadowHeight / 2;
    screen_draw_texture(TEXTURE_META_INFO_BOX_SHADOW, metaInfoBoxShadowX, metaInfoBoxShadowY, metaInfoBoxShadowWidth, metaInfoBoxShadowHeight);

    u32 metaInfoBoxWidth;
    u32 metaInfoBoxHeight;
    screen_get_texture_size(&metaInfoBoxWidth, &metaInfoBoxHeight, TEXTURE_META_INFO_BOX);

    float metaInfoBoxX = x1 + (x2 - x1 - metaInfoBoxWidth) / 2;
    float metaInfoBoxY = y1 + (y2 - y1) / 4 - metaInfoBoxHeight / 2;
    screen_draw_texture(TEXTURE_META_INFO_BOX, metaInfoBoxX, metaInfoBoxY, metaInfoBoxWidth, metaInfoBoxHeight);

    if(info->texture != 0) {
        u32 iconWidth;
        u32 iconHeight;
        screen_get_texture_size(&iconWidth, &iconHeight, info->texture);

        float iconX = metaInfoBoxX + (64 - iconWidth) / 2;
        float iconY = metaInfoBoxY + (metaInfoBoxHeight - iconHeight) / 2;
        screen_draw_texture(info->texture, iconX, iconY, iconWidth, iconHeight);
    }

    float metaTextX = metaInfoBoxX + 64;

    float shortDescriptionHeight;
    screen_get_string_size_wrap(NULL, &shortDescriptionHeight, info->shortDescription, 0.5f, 0.5f, metaInfoBoxX + metaInfoBoxWidth - 8 - metaTextX);

    float longDescriptionHeight;
    screen_get_string_size_wrap(NULL, &longDescriptionHeight, info->longDescription, 0.5f, 0.5f, metaInfoBoxX + metaInfoBoxWidth - 8 - metaTextX);

    float publisherHeight;
    screen_get_string_size_wrap(NULL, &publisherHeight, info->publisher, 0.5f, 0.5f, metaInfoBoxX + metaInfoBoxWidth - 8 - metaTextX);

    float shortDescriptionY = metaInfoBoxY + (64 - shortDescriptionHeight - 2 - longDescriptionHeight - 2 - publisherHeight) / 2;
    screen_draw_string_wrap(info->shortDescription, metaTextX, shortDescriptionY, 0.5f, 0.5f, COLOR_TEXT, false, metaInfoBoxX + metaInfoBoxWidth - 8);

    float longDescriptionY = shortDescriptionY + shortDescriptionHeight + 2;
    screen_draw_string_wrap(info->longDescription, metaTextX, longDescriptionY, 0.5f, 0.5f, COLOR_TEXT, false, metaInfoBoxX + metaInfoBoxWidth - 8);

    float publisherY = longDescriptionY + longDescriptionHeight + 2;
    screen_draw_string_wrap(info->publisher, metaTextX, publisherY, 0.5f, 0.5f, COLOR_TEXT, false, metaInfoBoxX + metaInfoBoxWidth - 8);
}

// Info text under the meta box: centered horizontally, starting just above the rect's middle.
static void task_draw_info_text(const char* text, float x1, float y1, float x2, float y2) {
    float infoWidth;
    screen_get_string_size(&infoWidth, NULL, text, 0.5f, 0.5f);

    float infoX = x1 + (x2 - x1 - infoWidth) / 2;
    float infoY = y1 + (y2 - y1) / 2 - 8;
    screen_draw_string(text, infoX, infoY, 0.5f, 0.5f, COLOR_TEXT, true);
}

void task_draw_ext_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ext_save_data_info* info = (ext_save_data_info*) data;

    if(info->hasMeta) {
        task_draw_meta_info(view, &info->meta, x1, y1, x2, y2);
    }

    char infoText[512];

    snprintf(infoText, sizeof(infoText),
             "Ext Save Data ID: %016llX\n"
                     "Shared: %s",
             info->extSaveDataId,
             info->shared ? "Yes" : "No");

    task_draw_info_text(infoText, x1, y1, x2, y2);
}

static const struct {
    u32 attribute;
    const char* name;
} task_file_attributes[] = {
    {FS_ATTRIBUTE_DIRECTORY, "Directory"},
    {FS_ATTRIBUTE_HIDDEN, "Hidden"},
    {FS_ATTRIBUTE_ARCHIVE, "Archive"},
    {FS_ATTRIBUTE_READ_ONLY, "Read Only"},
};

void task_draw_file_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    file_info* info = (file_info*) data;

    char infoText[512];
    size_t pos = 0;
    char line[256];

    if(strlen(info->name) > 48) {
        snprintf(line, sizeof(line), "Name: %.45s...\n", info->name);
    } else {
        snprintf(line, sizeof(line), "Name: %.48s\n", info->name);
    }

    string_append(infoText, sizeof(infoText), &pos, line);
    string_append(infoText, sizeof(infoText), &pos, "Attributes: ");

    size_t attributesStart = pos;
    for(u32 i = 0; i < sizeof(task_file_attributes) / sizeof(task_file_attributes[0]); i++) {
        if(info->attributes & task_file_attributes[i].attribute) {
            if(pos > attributesStart) {
                string_append(infoText, sizeof(infoText), &pos, ", ");
            }

            string_append(infoText, sizeof(infoText), &pos, task_file_attributes[i].name);
        }
    }

    if(pos == attributesStart) {
        string_append(infoText, sizeof(infoText), &pos, "None");
    }

    string_append(infoText, sizeof(infoText), &pos, "\n");

    if(!(info->attributes & FS_ATTRIBUTE_DIRECTORY)) {
        snprintf(line, sizeof(line), "Size: %.2f %s\n", ui_get_display_size(info->size), ui_get_display_size_units(info->size));
        string_append(infoText, sizeof(infoText), &pos, line);

        if(info->isCia && info->ciaInfo.loaded) {
            char regionString[64];

            if(info->ciaInfo.hasMeta) {
                task_draw_meta_info(view, &info->ciaInfo.meta, x1, y1, x2, y2);

                smdh_region_to_string(regionString, info->ciaInfo.meta.region, sizeof(regionString));
            } else {
                snprintf(regionString, sizeof(regionString), "Unknown");
            }

            char version[16];
            ui_format_version(version, sizeof(version), info->ciaInfo.version);

            snprintf(line, sizeof(line),
                     "Title ID: %016llX\n"
                             "Version: %hu (%s)\n"
                             "Region: %s\n"
                             "Installed Size: %.2f %s",
                     info->ciaInfo.titleId,
                     info->ciaInfo.version, version,
                     regionString,
                     ui_get_display_size(info->ciaInfo.installedSize),
                     ui_get_display_size_units(info->ciaInfo.installedSize));
            string_append(infoText, sizeof(infoText), &pos, line);
        } else if(info->isTicket && info->ticketInfo.loaded) {
            snprintf(line, sizeof(line), "Ticket ID: %016llX", info->ticketInfo.titleId);
            string_append(infoText, sizeof(infoText), &pos, line);
        }
    }

    task_draw_info_text(infoText, x1, y1, x2, y2);
}

void task_draw_pending_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    pending_title_info* info = (pending_title_info*) data;

    char version[16];
    ui_format_version(version, sizeof(version), info->version);

    char infoText[512];

    snprintf(infoText, sizeof(infoText),
             "Pending Title ID: %016llX\n"
                     "Media Type: %s\n"
                     "Version: %hu (%s)",
             info->titleId,
             fs_media_type_name(info->mediaType),
             info->version, version);

    task_draw_info_text(infoText, x1, y1, x2, y2);
}

void task_draw_system_save_data_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    system_save_data_info* info = (system_save_data_info*) data;

    char infoText[512];

    snprintf(infoText, sizeof(infoText), "System Save Data ID: %08lX", info->systemSaveDataId);

    task_draw_info_text(infoText, x1, y1, x2, y2);
}

void task_draw_ticket_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    ticket_info* info = (ticket_info*) data;

    if(info->loaded) {
        char infoText[512];

        snprintf(infoText, sizeof(infoText), "Title ID: %016llX", info->titleId);

        task_draw_info_text(infoText, x1, y1, x2, y2);
    }
}

void task_draw_title_info(ui_view* view, void* data, float x1, float y1, float x2, float y2) {
    title_info* info = (title_info*) data;

    char regionString[64];

    if(info->hasMeta) {
        task_draw_meta_info(view, &info->meta, x1, y1, x2, y2);

        smdh_region_to_string(regionString, info->meta.region, sizeof(regionString));
    } else {
        snprintf(regionString, sizeof(regionString), "Unknown");
    }

    char version[16];
    ui_format_version(version, sizeof(version), info->version);

    char infoText[512];

    snprintf(infoText, sizeof(infoText),
             "Title ID: %016llX\n"
                     "Media Type: %s\n"
                     "Version: %hu (%s)\n"
                     "Product Code: %s\n"
                     "Region: %s\n"
                     "Size: %.2f %s",
             info->titleId,
             fs_media_type_name(info->mediaType),
             info->version, version,
             info->productCode,
             regionString,
             ui_get_display_size(info->installedSize), ui_get_display_size_units(info->installedSize));

    task_draw_info_text(infoText, x1, y1, x2, y2);
}

// Loading strip at the bottom of the rect: animated dots and stage on the left, count on the
// right, progress bar under both. The title info text above is centered and its last line is
// short, so the strip's left and right ends stay clear of it.
#define TITLES_PROGRESS_DELAY_MS 200 // a fast open finishes before the strip would flash
#define TITLES_PROGRESS_SCALE 0.4f
#define TITLES_PROGRESS_MARGIN 8
#define TITLES_PROGRESS_DOT 4
#define TITLES_PROGRESS_BAR 2

void task_draw_title_progress(populate_titles_data* populateData, u64 start, float x1, float x2, float y2) {
    if(task_is_finished(&populateData->state) || osGetTime() - start < TITLES_PROGRESS_DELAY_MS) {
        return;
    }

    // Snapshot: the populate thread keeps writing these. A total read before the thread set it can
    // pair with a newer count, so clamp.
    title_load_stage stage = populateData->stage;
    u32 total = populateData->total;
    u32 loaded = populateData->loaded;
    if(loaded > total) {
        loaded = total;
    }

    const char* label = "Listing titles";
    if(stage == TITLE_STAGE_CACHE) {
        label = "Loading from cache";
    } else if(stage == TITLE_STAGE_LOADING) {
        label = populateData->cacheMode == TITLE_CACHE_OFF ? "Loading titles" : "Creating cache";
    } else if(stage == TITLE_STAGE_SAVING) {
        label = "Saving cache";
    }

    float left = x1 + TITLES_PROGRESS_MARGIN;
    float right = x2 - TITLES_PROGRESS_MARGIN;
    float barY = y2 - TITLES_PROGRESS_BAR - 2;

    float labelHeight = 0;
    screen_get_string_size(NULL, &labelHeight, label, TITLES_PROGRESS_SCALE, TITLES_PROGRESS_SCALE);
    float textY = barY - 1 - labelHeight;

    ui_draw_loading_dots(left, textY + (labelHeight - TITLES_PROGRESS_DOT) / 2, TITLES_PROGRESS_DOT);

    screen_set_base_alpha(0x40);
    screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, left, barY, right - left, TITLES_PROGRESS_BAR);
    screen_set_base_alpha(0xFF);

    float labelX = left + ui_get_loading_dots_width(TITLES_PROGRESS_DOT) + 6;
    screen_draw_string(label, labelX, textY, TITLES_PROGRESS_SCALE, TITLES_PROGRESS_SCALE, COLOR_TEXT, false);

    if(total == 0) {
        return;
    }

    char count[32];
    snprintf(count, sizeof(count), "%lu / %lu", loaded, total);

    float countWidth = 0;
    screen_get_string_size(&countWidth, NULL, count, TITLES_PROGRESS_SCALE, TITLES_PROGRESS_SCALE);
    screen_draw_string(count, right - countWidth, textY, TITLES_PROGRESS_SCALE, TITLES_PROGRESS_SCALE, COLOR_TEXT, false);

    screen_draw_texture(TEXTURE_PROGRESS_BAR_CONTENT, left, barY, (right - left) * loaded / total, TITLES_PROGRESS_BAR);
}