/*****************************************************************************
 * nsfw_filter.h: NSFW filter module definitions
 *****************************************************************************
 * Copyright (C) 2025 VLC authors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *****************************************************************************/

#ifndef VLC_NSFW_FILTER_H
#define VLC_NSFW_FILTER_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
# include <windows.h>
#endif

#include <vlc_picture.h>

#include "nsfw_filter_core.h"

/*****************************************************************************
 * Filter configuration
 *****************************************************************************/

/* Sensitivity threshold (0.0 - 1.0) for NSFW detection */
#define NSFW_DEFAULT_THRESHOLD 0.5

/* Maximum number of frames to hold in the delayed output pipeline */
#define NSFW_MAX_BUFFER_FRAMES 24

/* Maximum number of parallel ONNX worker threads. */
#define NSFW_MAX_WORKER_THREADS 8

typedef enum nsfw_block_style_t
{
    NSFW_BLOCK_STYLE_BLACK = 0,
    NSFW_BLOCK_STYLE_BLUR = 1,
    NSFW_BLOCK_STYLE_WARNING = 2,
} nsfw_block_style_t;

struct nsfw_worker_state_t;
typedef struct nsfw_worker_state_t nsfw_worker_state_t;

typedef struct nsfw_frame_slot_t
{
    picture_t      *picture;
    nsfw_result_t   result;
    uint64_t        sequence;
    uint64_t        timestamp_ms;
    bool            analyze;
    bool            processing;
    bool            decision_ready;
    bool            blocked;
} nsfw_frame_slot_t;

typedef struct nsfw_block_range_t
{
    uint64_t start_ms;
    uint64_t end_ms;
} nsfw_block_range_t;

typedef struct nsfw_time_block_range_t
{
    uint64_t start_ms;
    uint64_t end_ms;
} nsfw_time_block_range_t;

/*****************************************************************************
 * Filter state (to be expanded)
 *****************************************************************************/

struct filter_sys_t
{
    float threshold;
    unsigned frame_count;
    unsigned block_count;
    void            *core_module;
    nsfw_config_t   (*config_default_fn)(void);
    const char      *(*model_profile_name_fn)(nsfw_model_profile_t profile);
    int             (*model_profile_parse_fn)(const char *text,
                                              nsfw_model_profile_t *profile);
    void            (*config_set_model_profile_fn)(nsfw_config_t *config,
                                                    nsfw_model_profile_t profile);
    nsfw_detector_t *(*detector_create_fn)(const nsfw_config_t *config);
    void            (*detector_destroy_fn)(nsfw_detector_t *detector);
    nsfw_result_t   (*detector_classify_fn)(nsfw_detector_t *detector,
                                            const uint8_t   *frame_data,
                                            int              width,
                                            int              height,
                                            int              channels);
    nsfw_detector_t *detector;
    uint8_t         *rgb_buffer;
    size_t           rgb_capacity;
    char            *decision_map_path;
    char            *scan_status_path;
    nsfw_block_range_t *block_ranges;
    size_t           block_range_count;
    size_t           block_range_capacity;
    nsfw_time_block_range_t *time_block_ranges;
    size_t           time_block_range_count;
    size_t           time_block_range_capacity;
    uint64_t         last_scanned_ms;
    uint64_t         decision_map_mtime;
    uint64_t         scan_status_mtime;
    uint64_t         frame_interval_ms;
    int              analysis_width;
    int              analysis_height;
    unsigned         analysis_stride;
    unsigned         decision_reload_stride;
    unsigned         prebuffer_frames;
    unsigned         block_padding_frames;
    nsfw_block_style_t block_style;
    bool             mute_audio_on_blocked;
    bool             audio_muted_by_filter;
    bool             audio_previous_mute;
    bool             audio_mute_warning_logged;
    bool             output_mask_active;
    unsigned         output_mask_frame_count;
    uint64_t         output_mask_start_ms;
    bool             debug_dump_done;
#ifdef _WIN32
    CRITICAL_SECTION worker_lock;
    CONDITION_VARIABLE worker_cond;
#endif
    bool             worker_running;
    bool             worker_stop;
    bool             decision_map_mode;
    bool             scan_done;
    unsigned         worker_count;
    nsfw_worker_state_t *workers;
    unsigned         queue_head;
    unsigned         queue_count;
    nsfw_frame_slot_t frame_queue[NSFW_MAX_BUFFER_FRAMES];
};

#endif /* VLC_NSFW_FILTER_H */
