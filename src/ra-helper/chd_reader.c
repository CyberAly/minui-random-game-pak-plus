/*
 * chd_reader.c - CHD file reader callbacks for rcheevos hashing
 * Adapted from LoveRetro/NextUI (workspace/all/minarch/chd_reader.c).
 */

#include "chd_reader.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <libchdr/cdrom.h>
#include <libchdr/chd.h>

typedef struct {
    int type;
    int frames;
    int pregap_frames;
    int postgap_frames;
    int start_frame;
} chd_track_info_t;

typedef struct {
    chd_file *chd;
    uint32_t track_num;

    chd_track_info_t tracks[CD_MAX_TRACKS];
    int num_tracks;

    int track_start_frame;
    int track_frames;
    int track_type;
    int track_pregap;

    int sector_header_size;
    int raw_data_size;

    uint32_t hunk_bytes;
    uint32_t frame_size;
    int frames_per_hunk;

    uint8_t *hunk_buffer;
    uint32_t cached_hunk;
} chd_track_handle_t;

static int parse_track_type(const char *type_str) {
    if (strcmp(type_str, "MODE1") == 0)
        return CD_TRACK_MODE1;
    if (strcmp(type_str, "MODE1_RAW") == 0 || strcmp(type_str, "MODE1/2352") == 0)
        return CD_TRACK_MODE1_RAW;
    if (strcmp(type_str, "MODE2") == 0)
        return CD_TRACK_MODE2;
    if (strcmp(type_str, "MODE2_FORM1") == 0)
        return CD_TRACK_MODE2_FORM1;
    if (strcmp(type_str, "MODE2_FORM2") == 0)
        return CD_TRACK_MODE2_FORM2;
    if (strcmp(type_str, "MODE2_FORM_MIX") == 0)
        return CD_TRACK_MODE2_FORM_MIX;
    if (strcmp(type_str, "MODE2_RAW") == 0 || strcmp(type_str, "MODE2/2352") == 0)
        return CD_TRACK_MODE2_RAW;
    if (strcmp(type_str, "AUDIO") == 0)
        return CD_TRACK_AUDIO;

    return CD_TRACK_MODE1;
}

static int parse_chd_tracks(chd_file *chd, chd_track_info_t *tracks, int *num_tracks) {
    char metadata[256];
    uint32_t metadata_size;
    int track_idx = 0;
    int cumulative_frames = 0;

    for (int i = 0; i < CD_MAX_TRACKS; i++) {
        chd_error err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, i,
                                         metadata, sizeof(metadata) - 1,
                                         &metadata_size, NULL, NULL);
        if (err != CHDERR_NONE) {
            err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, i,
                                   metadata, sizeof(metadata) - 1,
                                   &metadata_size, NULL, NULL);
            if (err != CHDERR_NONE) {
                err = chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, i,
                                       metadata, sizeof(metadata) - 1,
                                       &metadata_size, NULL, NULL);
                if (err != CHDERR_NONE)
                    break;
            }
        }

        if (metadata_size >= sizeof(metadata)) {
            metadata_size = sizeof(metadata) - 1;
        }
        metadata[metadata_size] = '\0';

        int track_num = 0, frames = 0, pregap = 0, postgap = 0;
        char type_str[32] = {0};
        char subtype_str[32] = {0};
        char pgtype_str[32] = {0};
        char pgsub_str[32] = {0};

        int parsed = sscanf(metadata,
                            "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d PREGAP:%d PGTYPE:%31s PGSUB:%31s POSTGAP:%d",
                            &track_num, type_str, subtype_str, &frames, &pregap, pgtype_str, pgsub_str, &postgap);
        if (parsed < 4) {
            parsed = sscanf(metadata, "TRACK:%d TYPE:%31s SUBTYPE:%31s FRAMES:%d",
                            &track_num, type_str, subtype_str, &frames);
            pregap = 0;
        }

        if (parsed >= 4) {
            tracks[track_idx].type = parse_track_type(type_str);
            tracks[track_idx].frames = frames;
            tracks[track_idx].pregap_frames = pregap;
            tracks[track_idx].postgap_frames = postgap;
            tracks[track_idx].start_frame = cumulative_frames;

            int padding = ((frames + 3) & ~3) - frames;
            cumulative_frames += frames + padding;
            track_idx++;
        }
    }

    *num_tracks = track_idx;
    return track_idx > 0 ? 0 : -1;
}

static int is_data_track(int type) {
    return type != CD_TRACK_AUDIO;
}

static void get_sector_format(int track_type, int frame_size, int *header_size, int *data_size) {
    *data_size = 2048;
    *header_size = 0;

    switch (track_type) {
        case CD_TRACK_MODE1_RAW:
            if (frame_size >= 2352) {
                *header_size = 16;
                *data_size = 2048;
            }
            break;

        case CD_TRACK_MODE2_RAW:
        case CD_TRACK_MODE2_FORM_MIX:
            if (frame_size >= 2352) {
                *header_size = 24;
                *data_size = 2048;
            }
            break;

        case CD_TRACK_MODE2_FORM1:
        case CD_TRACK_MODE2_FORM2:
            if (frame_size == 2336) {
                *header_size = 8;
                *data_size = 2048;
            }
            break;

        case CD_TRACK_MODE1:
        case CD_TRACK_MODE2:
            *header_size = 0;
            *data_size = 2048;
            break;

        case CD_TRACK_AUDIO:
            *header_size = 0;
            *data_size = 2352;
            break;

        default:
            if (frame_size >= 2352) {
                *header_size = 16;
                *data_size = 2048;
            }
            break;
    }
}

static int find_track(chd_track_handle_t *handle, uint32_t track_request) {
    if (handle->num_tracks == 0)
        return -1;

    if (track_request == (uint32_t)-1) {
        for (int i = 0; i < handle->num_tracks; i++) {
            if (is_data_track(handle->tracks[i].type))
                return i;
        }
        return -1;
    } else if (track_request == (uint32_t)-2) {
        return handle->num_tracks - 1;
    } else if (track_request == (uint32_t)-3) {
        int largest_idx = 0;
        int largest_frames = 0;
        for (int i = 0; i < handle->num_tracks; i++) {
            if (handle->tracks[i].frames > largest_frames) {
                largest_frames = handle->tracks[i].frames;
                largest_idx = i;
            }
        }
        return largest_idx;
    } else if (track_request > 0 && track_request <= (uint32_t)handle->num_tracks) {
        return (int)track_request - 1;
    }

    return -1;
}

void *chd_open_track(const char *path, uint32_t track) {
    return chd_open_track_iterator(path, track, NULL);
}

void *chd_open_track_iterator(const char *path, uint32_t track, const void *iterator) {
    (void)iterator;

    if (!chd_reader_is_chd(path)) {
        return NULL;
    }

    chd_file *chd = NULL;
    if (chd_open(path, CHD_OPEN_READ, NULL, &chd) != CHDERR_NONE) {
        return NULL;
    }

    chd_track_handle_t *handle = (chd_track_handle_t *)calloc(1, sizeof(chd_track_handle_t));
    if (!handle) {
        chd_close(chd);
        return NULL;
    }

    handle->chd = chd;
    handle->track_num = track;
    handle->cached_hunk = (uint32_t)-1;

    const chd_header *header = chd_get_header(chd);
    handle->hunk_bytes = header->hunkbytes;
    handle->frame_size = header->unitbytes ? header->unitbytes : CD_FRAME_SIZE;
    if (handle->frame_size == 0 || handle->hunk_bytes < handle->frame_size) {
        chd_close(chd);
        free(handle);
        return NULL;
    }
    handle->frames_per_hunk = (int)(handle->hunk_bytes / handle->frame_size);

    handle->hunk_buffer = (uint8_t *)malloc(handle->hunk_bytes);
    if (!handle->hunk_buffer) {
        chd_close(chd);
        free(handle);
        return NULL;
    }

    if (parse_chd_tracks(chd, handle->tracks, &handle->num_tracks) != 0) {
        free(handle->hunk_buffer);
        chd_close(chd);
        free(handle);
        return NULL;
    }

    int track_idx = find_track(handle, track);
    if (track_idx < 0) {
        free(handle->hunk_buffer);
        chd_close(chd);
        free(handle);
        return NULL;
    }

    handle->track_num = (uint32_t)(track_idx + 1);
    handle->track_start_frame = handle->tracks[track_idx].start_frame;
    handle->track_frames = handle->tracks[track_idx].frames;
    handle->track_type = handle->tracks[track_idx].type;
    handle->track_pregap = handle->tracks[track_idx].pregap_frames;

    get_sector_format(handle->track_type, (int)handle->frame_size,
                      &handle->sector_header_size, &handle->raw_data_size);

    return handle;
}

size_t chd_read_sector(void *track_handle, uint32_t sector, void *buffer, size_t requested_bytes) {
    chd_track_handle_t *handle = (chd_track_handle_t *)track_handle;
    if (!handle || !handle->chd || handle->frames_per_hunk <= 0)
        return 0;

    uint32_t frame = (uint32_t)handle->track_start_frame + sector;
    if (is_data_track(handle->track_type) && handle->track_pregap > 0) {
        frame += (uint32_t)handle->track_pregap;
    }

    uint32_t hunk_num = frame / (uint32_t)handle->frames_per_hunk;
    uint32_t frame_in_hunk = frame % (uint32_t)handle->frames_per_hunk;

    if (hunk_num != handle->cached_hunk) {
        if (chd_read(handle->chd, hunk_num, handle->hunk_buffer) != CHDERR_NONE) {
            return 0;
        }
        handle->cached_hunk = hunk_num;
    }

    uint32_t offset = frame_in_hunk * handle->frame_size;
    uint8_t *src = handle->hunk_buffer + offset;

    size_t header_skip = (size_t)handle->sector_header_size;
    size_t data_size = (size_t)handle->raw_data_size;

    if (handle->frame_size >= 2352 && header_skip > 0) {
        if (src[0] == 0x00 && src[1] == 0xFF && src[11] == 0x00) {
            int mode = src[15];
            if (mode == 1) {
                header_skip = 16;
                data_size = 2048;
            } else if (mode == 2) {
                header_skip = 24;
                data_size = 2048;
            }
        } else if (handle->frame_size == 2448 &&
                   src[96] == 0x00 && src[97] == 0xFF && src[107] == 0x00) {
            int mode = src[96 + 15];
            src += 96;
            if (mode == 1) {
                header_skip = 16;
                data_size = 2048;
            } else if (mode == 2) {
                header_skip = 24;
                data_size = 2048;
            }
        }
    }

    size_t to_copy = requested_bytes;
    if (to_copy > data_size)
        to_copy = data_size;

    memcpy(buffer, src + header_skip, to_copy);
    return to_copy;
}

void chd_close_track(void *track_handle) {
    chd_track_handle_t *handle = (chd_track_handle_t *)track_handle;
    if (!handle)
        return;

    free(handle->hunk_buffer);
    if (handle->chd)
        chd_close(handle->chd);
    free(handle);
}

uint32_t chd_first_track_sector(void *track_handle) {
    (void)track_handle;
    return 0;
}

int chd_reader_is_chd(const char *path) {
    if (!path) return 0;
    const char *ext = strrchr(path, '.');
    return ext && strcasecmp(ext, ".chd") == 0;
}
