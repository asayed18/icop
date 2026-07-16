/*****************************************************************************
 * nsfw_filter_decision.c: extracted from nsfw_filter.c
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
# include <wchar.h>
#else
# include <pthread.h>
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_picture.h>
#include <vlc_variables.h>

#include "nsfw_filter.h"
#include "nsfw_filter_internal.h"
#include "platform_abstraction.h"
#include "frame_processor.h"

bool EnsureBlockRangeCapacity(filter_sys_t *sys, size_t required)
{
    nsfw_block_range_t *ranges;
    size_t capacity;

    if (sys == NULL)
        return false;
    if (required <= sys->block_range_capacity)
        return true;

    capacity = sys->block_range_capacity ? sys->block_range_capacity : 16;
    while (capacity < required)
        capacity *= 2;

    ranges = (nsfw_block_range_t *)realloc(sys->block_ranges,
                                           capacity * sizeof(*ranges));
    if (ranges == NULL)
        return false;

    sys->block_ranges = ranges;
    sys->block_range_capacity = capacity;
    return true;
}

bool EnsureTimeBlockRangeCapacity(filter_sys_t *sys, size_t required)
{
    nsfw_time_block_range_t *ranges;
    size_t capacity;

    if (sys == NULL)
        return false;
    if (required <= sys->time_block_range_capacity)
        return true;

    capacity = sys->time_block_range_capacity ?
               sys->time_block_range_capacity : 16;
    while (capacity < required)
        capacity *= 2;

    ranges = (nsfw_time_block_range_t *)realloc(
        sys->time_block_ranges, capacity * sizeof(*ranges));
    if (ranges == NULL)
        return false;

    sys->time_block_ranges = ranges;
    sys->time_block_range_capacity = capacity;
    return true;
}

void ClearDecisionMap(filter_sys_t *sys)
{
    if (sys == NULL)
        return;

    free(sys->block_ranges);
    sys->block_ranges = NULL;
    sys->block_range_count = 0;
    sys->block_range_capacity = 0;
    sys->last_scanned_ms = 0;
    sys->scan_done = false;
    sys->decision_map_mtime = 0;
    sys->scan_status_mtime = 0;
}

void ClearTimeBlockRanges(filter_sys_t *sys)
{
    if (sys == NULL)
        return;

    free(sys->time_block_ranges);
    sys->time_block_ranges = NULL;
    sys->time_block_range_count = 0;
    sys->time_block_range_capacity = 0;
}

void ParseScanStatus(filter_sys_t *sys)
{
    FILE *file;
    char line[256];
    uint64_t last_scanned_ms = 0;
    bool done = false;

    if (sys == NULL || sys->scan_status_path == NULL ||
        sys->scan_status_path[0] == '\0') {
        return;
    }

    file = fopen(sys->scan_status_path, "rb");
    if (file == NULL)
        return;

    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long value = 0;
        int done_value = 0;

        if (sscanf(line, "last_scanned_ms=%llu", &value) == 1) {
            last_scanned_ms = (uint64_t)value;
        } else if (sscanf(line, "done=%d", &done_value) == 1) {
            done = done_value != 0;
        }
    }

    fclose(file);
    sys->last_scanned_ms = last_scanned_ms;
    sys->scan_done = done;
}

bool ParseDecisionMap(filter_sys_t *sys)
{
    FILE *file;
    char line[256];
    size_t count = 0;

    if (sys == NULL || sys->decision_map_path == NULL ||
        sys->decision_map_path[0] == '\0') {
        return false;
    }

    file = fopen(sys->decision_map_path, "rb");
    if (file == NULL)
        return false;

    sys->block_range_count = 0;
    while (fgets(line, sizeof(line), file) != NULL) {
        unsigned long long start_ms = 0;
        unsigned long long end_ms = 0;

        if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
            continue;
        if (sscanf(line, "blocked %llu %llu", &start_ms, &end_ms) != 2)
            continue;
        if (!EnsureBlockRangeCapacity(sys, count + 1)) {
            fclose(file);
            return false;
        }

        sys->block_ranges[count].start_ms = (uint64_t)start_ms;
        sys->block_ranges[count].end_ms = (uint64_t)end_ms;
        count++;
    }

    fclose(file);
    sys->block_range_count = count;
    return true;
}

void RefreshDecisionMap(filter_sys_t *sys, bool force)
{
    uint64_t map_signature;
    uint64_t status_signature;

    if (sys == NULL || !sys->decision_map_mode)
        return;

    map_signature = nsfw_plat_file_signature(sys->decision_map_path);
    status_signature = nsfw_plat_file_signature(sys->scan_status_path);

    if (force || map_signature != sys->decision_map_mtime) {
        if (ParseDecisionMap(sys))
            sys->decision_map_mtime = map_signature;
    }

    if (force || status_signature != sys->scan_status_mtime) {
        ParseScanStatus(sys);
        sys->scan_status_mtime = status_signature;
    }
}
uint64_t RawPictureTimeMs(const filter_sys_t *sys,
                                 const picture_t *pic)
{
    if (pic != NULL && pic->date != VLC_TICK_INVALID && pic->date >= 0)
        return (uint64_t)pic->date * 1000 / CLOCK_FREQ;

    if (sys != NULL) {
        vlc_tick_t interval = EstimatedFrameInterval(&pic->format);
        if (interval <= 0)
            interval = CLOCK_FREQ / 24;
        return (uint64_t)(sys->frame_count ? sys->frame_count - 1 : 0) *
               (uint64_t)interval * 1000 / CLOCK_FREQ;
    }

    return 0;
}

uint64_t PictureTimeMs(const filter_sys_t *sys, const picture_t *pic)
{
    uint64_t raw = RawPictureTimeMs(sys, pic);

    if (sys != NULL && sys->timeline_origin_valid &&
        raw >= sys->timeline_origin_ms) {
        return sys->timeline_media_origin_ms + raw - sys->timeline_origin_ms;
    }
    return raw;
}

void SetTimelineOrigin(filter_t *filter, uint64_t raw_timestamp_ms)
{
    filter_sys_t *sys;
    uint64_t media_time_ms = 0;

    if (filter == NULL || filter->p_sys == NULL)
        return;
    sys = filter->p_sys;
    GetInputMediaTimeMs(filter, &media_time_ms);
    sys->timeline_origin_ms = raw_timestamp_ms;
    sys->timeline_media_origin_ms = media_time_ms;
    sys->timeline_origin_valid = true;
}

void ResetOutputMaskState(filter_sys_t *sys)
{
    if (sys == NULL)
        return;

    sys->output_mask_active = false;
    sys->output_mask_frame_count = 0;
    sys->output_mask_start_ms = 0;
}

uint64_t SeekResetThresholdMs(const filter_sys_t *sys)
{
    uint64_t interval_ms;
    uint64_t threshold_ms;

    interval_ms = (sys != NULL && sys->frame_interval_ms > 0)
        ? sys->frame_interval_ms
        : 41;
    threshold_ms = interval_ms * 8;
    if (threshold_ms < 250)
        threshold_ms = 250;
    return threshold_ms;
}

bool TimelineDiscontinuityDetected(const filter_sys_t *sys,
                                          uint64_t timestamp_ms)
{
    if (sys == NULL || !sys->last_frame_timestamp_valid)
        return false;

    if (timestamp_ms < sys->last_frame_timestamp_ms)
        return true;

    return timestamp_ms - sys->last_frame_timestamp_ms >
           SeekResetThresholdMs(sys);
}

void DumpBlockedFrameIfRequested(filter_t *filter, picture_t *pic)
{
    filter_sys_t *sys;
    const char *prefix;
    int width;
    int height;
    int packed_width = 0;
    int packed_height = 0;
    size_t needed;
    uint8_t *rgb = NULL;
    FILE *file = NULL;
    char path[1024];
    char chroma[5];

    if (filter == NULL || filter->p_sys == NULL || pic == NULL)
        return;

    sys = filter->p_sys;
    if (sys->debug_dump_done)
        return;

    prefix = getenv("NSFW_DEBUG_DUMP_PREFIX");
    if (prefix == NULL || prefix[0] == '\0')
        return;

    width = nsfw_fp_visible_width(&pic->format);
    height = nsfw_fp_visible_height(&pic->format);
    if (width <= 0 || height <= 0)
        return;

    nsfw_fp_fourcc_to_string(pic->format.i_chroma, chroma);
    if (snprintf(path, sizeof(path), "%s-%s-%s-%u.ppm",
                 prefix, BlockStyleName(sys->block_style), chroma,
                 sys->output_mask_frame_count + 1) <= 0) {
        return;
    }

    if (sys->backend_ops != NULL && sys->backend_ops->dump_ppm != NULL) {
        if (sys->backend_ops->dump_ppm(sys->backend_data, pic, path) == 0) {
            sys->debug_dump_done = true;
            fprintf(stderr,
                    "icop: dumped blocked hardware frame to %s (%dx%d, style %s)\n",
                    path, width, height, BlockStyleName(sys->block_style));
        }
        return;
    }

    needed = (size_t)width * (size_t)height * 3;
    rgb = (uint8_t *)malloc(needed);
    if (rgb == NULL)
        return;

    if (nsfw_fp_pack_to_rgb(pic, rgb, needed, width, height,
                       &packed_width, &packed_height) != 0 ||
        packed_width != width || packed_height != height) {
        free(rgb);
        return;
    }

    file = fopen(path, "wb");
    if (file == NULL) {
        free(rgb);
        return;
    }

    fprintf(file, "P6\n%d %d\n255\n", width, height);
    fwrite(rgb, 1, needed, file);
    fclose(file);
    free(rgb);

    sys->debug_dump_done = true;
    fprintf(stderr,
            "icop: dumped blocked frame to %s (%dx%d, chroma %s, style %s)\n",
            path, width, height, chroma, BlockStyleName(sys->block_style));
}

void UpdateOutputMaskState(filter_t *filter, uint64_t timestamp_ms,
                                  bool blocked)
{
    filter_sys_t *sys;
    char chroma[5];

    if (filter == NULL || filter->p_sys == NULL)
        return;

    sys = filter->p_sys;
    if (blocked) {
        if (!sys->output_mask_active) {
            sys->output_mask_active = true;
            sys->output_mask_frame_count = 0;
            sys->output_mask_start_ms = timestamp_ms;
            nsfw_fp_fourcc_to_string(filter->fmt_in.video.i_chroma, chroma);
            fprintf(stderr,
                    "icop: output masking started at %llu ms using %s style (filter input %s)\n",
                    (unsigned long long)timestamp_ms,
                    BlockStyleName(sys->block_style), chroma);
        }
        sys->output_mask_frame_count++;
        return;
    }

    if (!sys->output_mask_active)
        return;

    fprintf(stderr,
            "icop: output masking ended before %llu ms after %u frame(s) starting at %llu ms\n",
            (unsigned long long)timestamp_ms,
            sys->output_mask_frame_count,
            (unsigned long long)sys->output_mask_start_ms);
    ResetOutputMaskState(sys);
}

static picture_t *ApplyBlockedOutput(filter_t *filter, picture_t *pic,
                                     bool blocked)
{
    uint64_t timestamp_ms;
    char chroma[5];
    filter_sys_t *sys;
    bool was_output_mask_active;
    bool mute_requested;

    if (filter == NULL || filter->p_sys == NULL || pic == NULL)
        return pic;

    sys = filter->p_sys;
    timestamp_ms = PictureTimeMs(sys, pic);
    was_output_mask_active = sys->output_mask_active;
    if (blocked)
        sys->audio_mute_requested = true;
    UpdateOutputMaskState(filter, timestamp_ms, blocked);
    if (!blocked && was_output_mask_active)
        sys->audio_mute_requested = false;
    mute_requested = blocked || sys->audio_mute_requested;
    SyncAudioMutedForBlockedFrame(filter, mute_requested);
    if (blocked) {
        nsfw_fp_fourcc_to_string(pic->format.i_chroma, chroma);
        if (sys->backend_ops != NULL) {
            pic = sys->backend_ops->render_blocked(filter,
                                                     sys->backend_data,
                                                     pic,
                                                     sys->block_style);
            if (pic == NULL) {
                if (MarkBackendFailureLogged(sys)) {
                    fprintf(stderr,
                            "icop: hardware backend blocked-frame rendering failed; dropping output fail-closed\n");
                }
                return NULL;
            }
        } else if (sys->opaque_fallback) {
            if (!sys->debug_dump_done) {
                fprintf(stderr,
                        "icop: opaque-fallback blocked output cannot be composited directly; dropping blocked frames fail-closed\n");
                sys->debug_dump_done = true;
            }
            return NULL;
        } else {
            nsfw_fp_block_frame(pic, sys->block_style);
        }
        if (filter->p_sys->output_mask_frame_count == 1) {
            fprintf(stderr,
                    "icop: applied %s style to output picture chroma %s at %llu ms\n",
                    BlockStyleName(filter->p_sys->block_style), chroma,
                    (unsigned long long)timestamp_ms);
        }
    }
    return pic;
}

picture_t *ApplyDisplayOutput(filter_t *filter, picture_t *pic,
                                     bool blocked,
                                     const nsfw_result_t *evaluation)
{
    filter_sys_t *sys;

    if (filter == NULL || pic == NULL)
        return pic;

    sys = filter->p_sys;
    pic = ApplyBlockedOutput(filter, pic, blocked);
    if (pic == NULL)
        return NULL;
    if (sys != NULL && sys->debug_overlay) {
        if (evaluation != NULL) {
            sys->debug_score = evaluation->score;
            sys->debug_score_valid = true;
        }
        if (sys->debug_score_valid) {
            if (sys->backend_ops != NULL &&
                sys->backend_ops->render_debug_overlay != NULL) {
                pic = sys->backend_ops->render_debug_overlay(
                    filter, sys->backend_data, pic, sys->debug_score,
                    sys->threshold);
            } else {
                nsfw_fp_debug_overlay(pic, sys->debug_score, sys->threshold);
            }
        }
    }
    if (blocked)
        DumpBlockedFrameIfRequested(filter, pic);
    return pic;
}

bool DecisionMapContains(const filter_sys_t *sys, uint64_t pts_ms)
{
    size_t i;

    if (sys == NULL)
        return false;

    for (i = 0; i < sys->block_range_count; ++i) {
        if (pts_ms < sys->block_ranges[i].start_ms)
            return false;
        if (pts_ms >= sys->block_ranges[i].start_ms &&
            pts_ms < sys->block_ranges[i].end_ms) {
            return true;
        }
    }

    return false;
}

bool DecisionMapShouldBlock(filter_t *p_filter, picture_t *p_pic)
{
    filter_sys_t *sys = p_filter->p_sys;
    uint64_t pts_ms;

    if (sys == NULL || !sys->decision_map_mode)
        return false;

    if (sys->decision_reload_stride == 0 ||
        ((sys->frame_count - 1) % sys->decision_reload_stride) == 0) {
        RefreshDecisionMap(sys, false);
    }

    pts_ms = PictureTimeMs(sys, p_pic);
    if (!sys->scan_done && pts_ms > sys->last_scanned_ms)
        return true;

    return DecisionMapContains(sys, pts_ms);
}
