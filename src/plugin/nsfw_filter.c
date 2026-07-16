/*****************************************************************************
 * nsfw_filter.c: VLC video filter module
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

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
# include <wchar.h>
#else
# if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
# endif
# include <dlfcn.h>
# include <unistd.h>
# include <pthread.h>
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_input.h>
#include <vlc_aout.h>
#include <vlc_picture.h>
#include <vlc_image.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_variables.h>

#include "nsfw_filter.h"
#include "nsfw_filter_internal.h"
#include "platform_abstraction.h"
#include "frame_processor.h"

/*****************************************************************************
 * Worker state
 *****************************************************************************/

/* Platform helpers now in platform_abstraction.h / .c */

/*****************************************************************************
 * Module option labels
 *****************************************************************************/

static const char *const kModelProfileValues[] = {
    "marqo",
    "adamcodd",
    "falconsai",
    "legacy",
};

static const char *const kModelProfileLabels[] = {
    "Marqo / nsfw-image-detection-384",
    "AdamCodd / vit-base-nsfw-detector",
    "Falconsai / nsfw_image_detection",
    "Legacy / GantMan",
};

    static const char *const kProviderValues[] = {
        "gpu",
        "cpu",
    };

static const char *const kProviderLabels[] = {
    "GPU (default)",
    "CPU",
};

static const char *const kBlockStyleValues[] = {
    "black",
    "blur",
    "warning",
};

static const char *const kBlockStyleLabels[] = {
    "Black out",
    "Blur",
    "Warning watermark",
};

const char *const kNsfwFilterOptions[] = {
    "model-profile",
    "model-path",
    "provider",
    "block-style",
    "threshold",
    "mute-audio-on-blocked",
    "analysis-stride",
    "block-padding-frames",
    "buffered-frames",
    "worker-threads",
    "cuda-device-id",
    "decision-reload-frames",
    "decision-map-path",
    "scan-status-path",
    "debug-overlay",
    "settings-version",
    NULL
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
int  Open(vlc_object_t *);
void Close(vlc_object_t *);
void Flush(filter_t *);
picture_t *Filter(filter_t *, picture_t *);

static int PackPictureForAnalysis(filter_t *p_filter,
                                  picture_t *p_pic,
                                  uint8_t *rgb_buffer,
                                  size_t rgb_capacity,
                                  int *width,
                                  int *height)
{
    filter_sys_t *sys;

    if (p_filter == NULL || p_pic == NULL || rgb_buffer == NULL ||
        width == NULL || height == NULL) {
        return -1;
    }

    sys = p_filter->p_sys;
    if (sys == NULL)
        return -1;

    if (sys->backend_ops != NULL) {
        return sys->backend_ops->readback_rgb(sys->backend_data, p_pic,
                                               rgb_buffer, rgb_capacity,
                                               width, height);
    }

    if (!sys->opaque_fallback) {
        return nsfw_fp_pack_to_rgb(p_pic, rgb_buffer, rgb_capacity,
                                   sys->analysis_width,
                                   sys->analysis_height,
                                   width, height);
    }

    if (sys->image_handler != NULL) {
        video_format_t source = p_pic->format;
        video_format_t target = source;
        picture_t *software_picture;
        int packed;

        target.i_chroma = sys->opaque_analysis_chroma;
        software_picture = image_Convert(sys->image_handler,
                                         p_pic, &source, &target);
        if (software_picture == NULL)
            return -1;

        packed = nsfw_fp_pack_to_rgb(software_picture, rgb_buffer,
                                     rgb_capacity,
                                     sys->analysis_width,
                                     sys->analysis_height,
                                     width, height);
        ReleasePicture(software_picture);
        return packed;
    }

    return -1;
}

vlc_module_begin()
    set_description("ICOP Filter")
    set_shortname("ICOP Filter")
    set_category(CAT_VIDEO)
    set_subcategory(SUBCAT_VIDEO_VFILTER)
    set_capability("video filter", 0)
    set_callbacks(Open, Close)
    set_section("Model", NULL)
    add_string("nsfw-model-profile", "marqo",
               "Model profile",
               "Built-in detector family to use.", false)
        change_string_list(kModelProfileValues, kModelProfileLabels)
    add_string("nsfw-model-path", "",
               "Model path",
               "Optional custom ONNX file path.", false)
    add_string("nsfw-provider", "gpu",
               "Execution provider",
               "ONNX provider preference.", false)
        change_string_list(kProviderValues, kProviderLabels)
    set_section("Blocking", NULL)
    add_string("nsfw-block-style", "black",
               "Blocked frame style",
               "What to show when a frame is blocked.", false)
        change_string_list(kBlockStyleValues, kBlockStyleLabels)
    add_float("nsfw-threshold", 0.5f,
              "Detection threshold",
              "Score threshold used to black out frames.", false)
        change_float_range(0.0, 1.0)
    set_section("Audio", NULL)
    add_integer("nsfw-mute-audio-on-blocked", 0,
                "Mute audio on blocked frames",
                "Set to 1 to mute playback while blocked frames are shown.", false)
        change_integer_range(0, 1)
    set_section("Performance", NULL)
    add_integer("nsfw-analysis-stride", 0,
                "Analysis stride",
                "Analyze every Nth frame; 0 means automatic.", false)
        change_integer_range(0, 32)
    add_integer("nsfw-block-padding-frames", 0,
                "Block padding",
                "Frames to include before and after a detection; 0 means automatic.", false)
        change_integer_range(0, 32)
    add_integer("nsfw-buffered-frames", 0,
                "Buffered frames",
                "How many frames to hold before playback; 0 means automatic.", false)
        change_integer_range(0, NSFW_MAX_BUFFER_FRAMES)
    add_integer("nsfw-worker-threads", 0,
                "Worker threads",
                "Parallel ONNX worker threads; 0 means automatic.", false)
        change_integer_range(0, NSFW_MAX_WORKER_THREADS)
    add_integer("nsfw-cuda-device-id", 0,
                "CUDA device id",
                "GPU device index for CUDA execution.", false)
        change_integer_range(0, 31)
    set_section("Paths", NULL)
    add_integer("nsfw-decision-reload-frames", 0,
                "Decision reload",
                "How often the decision map is reloaded; 0 means automatic.", false)
        change_integer_range(0, 240)
    add_string("nsfw-decision-map-path", "",
               "Decision map path",
               "Optional decision map file.", false)
    add_string("nsfw-scan-status-path", "",
               "Scan status path",
               "Optional scan status file.", false)
    set_section("Debug", NULL)
    add_integer("nsfw-debug-overlay", 0,
                "Show evaluation overlay",
                "Show the latest score and threshold with a continuous risk color.", false)
        change_integer_range(0, 1)
    add_integer("nsfw-settings-version", 0,
                "Settings version",
                "Internal version for one-time defaults migration.", false)
        change_integer_range(0, NSFW_SETTINGS_VERSION_CURRENT)
        change_private()
    add_shortcut("icop")
vlc_module_end()

/*****************************************************************************
 * Runtime-loaded core helpers
 *****************************************************************************/

/*****************************************************************************
 * Core module loading
 *****************************************************************************/

/*****************************************************************************
 * Runtime provider / file existence helpers
 *****************************************************************************/


/*****************************************************************************
 * VLC option helpers
 *****************************************************************************/







/*****************************************************************************
 * Open: initialize the filter
 *****************************************************************************/
int Open(vlc_object_t *p_this)
{

    filter_t *p_filter = (filter_t *)p_this;

    if (p_filter->fmt_in.video.i_width <= 0 ||
        p_filter->fmt_in.video.i_height <= 0)
        return VLC_EGENERIC;

    p_filter->p_sys = calloc(1, sizeof(filter_sys_t));
    if (p_filter->p_sys == NULL)
        return VLC_ENOMEM;

    ParseVlcFilterOptions(p_filter);
    SyncVlcOptionsToEnv(p_filter);

    p_filter->p_sys->threshold = GetVlcConfigFloat(p_filter, "nsfw-threshold",
                                                   NSFW_DEFAULT_THRESHOLD);
    {
        char *block_style = GetVlcConfigString(p_filter, "nsfw-block-style");
        p_filter->p_sys->block_style = ParseBlockStyle(block_style);
    }
    fprintf(stderr,
            "icop: configured block style=%s\n",
            BlockStyleName(p_filter->p_sys->block_style));
    p_filter->p_sys->mute_audio_on_blocked =
        GetVlcConfigInteger(p_filter, "nsfw-mute-audio-on-blocked", 0) != 0;
    p_filter->p_sys->debug_overlay =
        GetVlcConfigInteger(p_filter, "nsfw-debug-overlay", 0) != 0;
    p_filter->p_sys->debug_score = 0.0f;
    p_filter->p_sys->debug_score_valid = false;
    p_filter->p_sys->analysis_stride =
        ResolveAnalysisStride(&p_filter->fmt_in.video);
    p_filter->p_sys->decision_reload_stride = ResolveDecisionReloadStride();
    p_filter->p_sys->frame_interval_ms =
        (uint64_t)((EstimatedFrameInterval(&p_filter->fmt_in.video) *
                    1000 + CLOCK_FREQ - 1) / CLOCK_FREQ);
    if (p_filter->p_sys->frame_interval_ms == 0)
        p_filter->p_sys->frame_interval_ms = 41;
    p_filter->p_sys->prebuffer_frames =
        ResolvePrebufferFrames(&p_filter->fmt_in.video);
    p_filter->p_sys->block_padding_frames =
        ResolveBlockPaddingFrames(p_filter->p_sys->analysis_stride);
    if (p_filter->p_sys->prebuffer_frames <
        MinimumPrebufferFrames(p_filter->p_sys->analysis_stride,
                               p_filter->p_sys->block_padding_frames)) {
        p_filter->p_sys->prebuffer_frames =
            MinimumPrebufferFrames(p_filter->p_sys->analysis_stride,
                                   p_filter->p_sys->block_padding_frames);
        if (p_filter->p_sys->prebuffer_frames > NSFW_MAX_BUFFER_FRAMES)
            p_filter->p_sys->prebuffer_frames = NSFW_MAX_BUFFER_FRAMES;
    }

    {
        int backend_ret = nsfw_backend_open(p_filter);
        if (backend_ret == VLC_EGENERIC) {
            free(p_filter->p_sys);
            p_filter->p_sys = NULL;
            return VLC_EGENERIC;
        }
        if (backend_ret == VLC_ENOMEM) {
            free(p_filter->p_sys);
            p_filter->p_sys = NULL;
            return VLC_ENOMEM;
        }
    }

    MaybeReplaceLegacyPreset(p_filter);

    {
        const char *decision_map_path = getenv("NSFW_DECISION_MAP_PATH");
        const char *scan_status_path = getenv("NSFW_SCAN_STATUS_PATH");

        if (decision_map_path != NULL && decision_map_path[0] != '\0') {
            p_filter->p_sys->decision_map_path = DuplicateString(decision_map_path);
            p_filter->p_sys->scan_status_path = DuplicateString(scan_status_path);
            if (p_filter->p_sys->decision_map_path == NULL ||
                (scan_status_path != NULL && scan_status_path[0] != '\0' &&
                 p_filter->p_sys->scan_status_path == NULL)) {
                free(p_filter->p_sys->decision_map_path);
                free(p_filter->p_sys->scan_status_path);
                free(p_filter->p_sys);
                p_filter->p_sys = NULL;
                return VLC_ENOMEM;
            }

            p_filter->p_sys->decision_map_mode = true;
            p_filter->p_sys->scan_done =
                scan_status_path == NULL || scan_status_path[0] == '\0';
            RefreshDecisionMap(p_filter->p_sys, true);
            fprintf(stderr,
                    "icop: decision-map mode enabled with %zu blocked ranges\n",
                    p_filter->p_sys->block_range_count);
            p_filter->pf_video_filter = Filter;
            p_filter->pf_flush = Flush;
#ifdef _WIN32
            p_filter->p_sys->vlc_window_icon_active =
                nsfw_plat_window_icon_enable();
#endif
            return VLC_SUCCESS;
        }
    }

    if (LoadCoreModule(p_filter->p_sys)) {
        nsfw_config_t cfg = p_filter->p_sys->config_default_fn();
        const char *model_profile = getenv("NSFW_MODEL_PROFILE");
        const char *model_path = getenv("NSFW_MODEL_PATH");
        nsfw_model_profile_t profile = cfg.model_profile;
        cfg.threshold = p_filter->p_sys->threshold;
        if (model_profile != NULL && model_profile[0] != '\0') {
            if (!p_filter->p_sys->model_profile_parse_fn(model_profile, &profile)) {
                fprintf(stderr,
                        "icop: unrecognized NSFW_MODEL_PROFILE=%s, defaulting to %s\n",
                        model_profile,
                        p_filter->p_sys->model_profile_name_fn(profile));
            }
        }

        if (model_path == NULL || model_path[0] == '\0') {
            nsfw_model_profile_t fallback_profile =
                ResolveUsableModelProfile(profile);
            if (fallback_profile != profile) {
                fprintf(stderr,
                        "icop: requested %s model file is missing, falling back to %s\n",
                        p_filter->p_sys->model_profile_name_fn(profile),
                        p_filter->p_sys->model_profile_name_fn(fallback_profile));
                profile = fallback_profile;
            }
        }

        p_filter->p_sys->config_set_model_profile_fn(&cfg, profile);
        p_filter->p_sys->analysis_width = cfg.model_width;
        p_filter->p_sys->analysis_height = cfg.model_height;

        if (p_filter->p_sys->backend_ops != NULL &&
            p_filter->p_sys->backend_ops->set_analysis_size(
                p_filter->p_sys->backend_data,
                cfg.model_width,
                cfg.model_height) != VLC_SUCCESS) {
            fprintf(stderr,
                    "icop: model-sized staging initialization failed; requesting CPU fallback\n");
            UnloadCoreModule(p_filter->p_sys);
            p_filter->p_sys->backend_ops->close(
                p_filter->p_sys->backend_data);
            p_filter->p_sys->backend_ops = NULL;
            p_filter->p_sys->backend_data = NULL;
            free(p_filter->p_sys);
            p_filter->p_sys = NULL;
            return VLC_EGENERIC;
        }

        if (model_path != NULL && model_path[0] != '\0')
            cfg.model_path = model_path;

        nsfw_plat_set_env("NSFW_MODEL_PROFILE",
                           p_filter->p_sys->model_profile_name_fn(cfg.model_profile));

        fprintf(stderr,
                "icop: using %s model profile (%dx%d)\n",
                p_filter->p_sys->model_profile_name_fn(cfg.model_profile),
                cfg.model_width, cfg.model_height);

        if (!p_filter->p_sys->opaque_fallback &&
            p_filter->p_sys->backend_ops == NULL &&
            StartDetectorWorker(p_filter->p_sys, &cfg) == VLC_SUCCESS) {
            fprintf(stderr,
                    "icop: started %u parallel detector worker(s)\n",
                    p_filter->p_sys->worker_count);
        } else if (p_filter->p_sys->opaque_fallback ||
                   p_filter->p_sys->backend_ops != NULL) {
            p_filter->p_sys->detector = p_filter->p_sys->detector_create_fn(&cfg);
            if (p_filter->p_sys->detector == NULL) {
                fprintf(stderr,
                        "icop: ONNX detector unavailable on hardware backend, dropping all blocked output fail-closed\n");
            } else {
                fprintf(stderr,
                        "icop: using synchronous inference on hardware backend\n");
            }
        } else {
            p_filter->p_sys->detector = p_filter->p_sys->detector_create_fn(&cfg);
            if (p_filter->p_sys->detector == NULL) {
                fprintf(stderr,
                        "icop: ONNX detector unavailable, using heuristic fallback\n");
            } else {
                fprintf(stderr,
                        "icop: unable to start detector worker, using synchronous inference\n");
            }
        }
    } else {
        fprintf(stderr,
                "icop: unable to load core DLL, using heuristic fallback\n");
    }

    p_filter->pf_video_filter = Filter;
    p_filter->pf_flush = Flush;
    if (p_filter->p_sys->backend_ops != NULL) {
        fprintf(stderr,
                "icop: hardware decoder queue depth will be sized from the first decoder texture\n");
    } else {
        fprintf(stderr,
                "icop: holding %u processed frames before playback\n",
                p_filter->p_sys->prebuffer_frames);
    }
#ifdef _WIN32
    p_filter->p_sys->vlc_window_icon_active = nsfw_plat_window_icon_enable();
#endif
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Flush: release delayed frames and reset buffered state
 *****************************************************************************/
void Flush(filter_t *p_filter)
{
    filter_sys_t *sys;

    if (p_filter == NULL || p_filter->p_sys == NULL)
        return;

    sys = p_filter->p_sys;

    if (sys->decision_map_mode) {
        sys->audio_mute_requested = false;
        SyncAudioMutedForBlockedFrame(p_filter, false);
        ResetOutputMaskState(sys);
        sys->frame_count = 0;
        sys->last_frame_timestamp_ms = 0;
        sys->last_frame_timestamp_valid = false;
        sys->timeline_origin_ms = 0;
        sys->timeline_media_origin_ms = 0;
        sys->timeline_origin_valid = false;
        RefreshDecisionMap(sys, true);
        return;
    }

    if (sys->worker_running) {
#ifdef _WIN32
        EnterCriticalSection(&sys->worker_lock);
        while (HasProcessingFramesLocked(sys))
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
        ReleaseQueuedFramesLocked(sys);
        WakeAllConditionVariable(&sys->worker_cond);
        LeaveCriticalSection(&sys->worker_lock);
#else
        pthread_mutex_lock(&sys->worker_lock);
        while (HasProcessingFramesLocked(sys))
            pthread_cond_wait(&sys->worker_cond, &sys->worker_lock);
        ReleaseQueuedFramesLocked(sys);
        pthread_cond_broadcast(&sys->worker_cond);
        pthread_mutex_unlock(&sys->worker_lock);
#endif
    }

    sys->audio_mute_requested = false;
    SyncAudioMutedForBlockedFrame(p_filter, false);
    ResetOutputMaskState(sys);
    ClearTimeBlockRanges(sys);
    sys->frame_count = 0;
    sys->last_frame_timestamp_ms = 0;
    sys->last_frame_timestamp_valid = false;
    sys->timeline_origin_ms = 0;
    sys->timeline_media_origin_ms = 0;
    sys->timeline_origin_valid = false;
}

/*****************************************************************************
 * Close: clean up the filter
 *****************************************************************************/
void Close(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    if (p_filter->p_sys != NULL) {
        Flush(p_filter);
        StopDetectorWorker(p_filter->p_sys);
        if (p_filter->p_sys->detector_destroy_fn != NULL)
            p_filter->p_sys->detector_destroy_fn(p_filter->p_sys->detector);
        p_filter->p_sys->audio_mute_requested = false;
        SyncAudioMutedForBlockedFrame(p_filter, false);
        ClearDecisionMap(p_filter->p_sys);
        ClearTimeBlockRanges(p_filter->p_sys);
        free(p_filter->p_sys->decision_map_path);
        free(p_filter->p_sys->scan_status_path);
        free(p_filter->p_sys->rgb_buffer);
        if (p_filter->p_sys->image_handler != NULL)
            DestroyImageHandler(p_filter->p_sys->image_handler);
        p_filter->p_sys->image_handler = NULL;
        if (p_filter->p_sys->backend_ops != NULL) {
            p_filter->p_sys->backend_ops->close(
                p_filter->p_sys->backend_data);
            p_filter->p_sys->backend_ops = NULL;
            p_filter->p_sys->backend_data = NULL;
        }
        UnloadCoreModule(p_filter->p_sys);
#ifdef _WIN32
        if (p_filter->p_sys->vlc_window_icon_active) {
            nsfw_plat_window_icon_disable();
            p_filter->p_sys->vlc_window_icon_active = false;
        }
#endif
    }
    free(p_filter->p_sys);
    p_filter->p_sys = NULL;
}

/*****************************************************************************
 * Filter: process a video frame
 *****************************************************************************/
picture_t *Filter(filter_t *p_filter, picture_t *p_pic)
{
    filter_sys_t *sys = p_filter->p_sys;
    bool blocked = false;
    bool should_analyze;
    bool output_evaluated = false;
    picture_t *output = NULL;
    nsfw_result_t output_result = { 0, 0.0f, 0.0f };
    size_t needed;
    int width = 0;
    int height = 0;
    uint64_t sequence;
    uint64_t timestamp_ms;
    uint64_t raw_timestamp_ms;

    if (p_pic == NULL)
        return NULL;

#ifdef _WIN32
    if (sys->vlc_window_icon_active &&
        (sys->frame_count % NSFW_ICON_REFRESH_FRAMES) == 0) {
        nsfw_plat_window_icon_refresh();
    }
#endif

    if (sys->backend_ops != NULL && !sys->queue_configured) {
        unsigned surfaces = sys->backend_ops->decoder_surface_count(
            sys->backend_data, p_pic);
        ConstrainDecoderQueue(sys, surfaces);
        sys->queue_configured = true;
    }

    if (sys->frame_count == 0) {
        const char *backend_name = "cpu";
        if (sys->backend_ops != NULL)
            backend_name = sys->backend_ops->adapter_name(
                sys->backend_data);
        fprintf(stderr,
                "icop: received first frame on %s backend (%4.4s)\n",
                backend_name,
                (const char *)&p_pic->format.i_chroma);
    }

    raw_timestamp_ms = RawPictureTimeMs(sys, p_pic);
    if (!sys->timeline_origin_valid)
        SetTimelineOrigin(p_filter, raw_timestamp_ms);
    timestamp_ms = PictureTimeMs(sys, p_pic);
    if (TimelineDiscontinuityDetected(sys, timestamp_ms)) {
        Flush(p_filter);
        SetTimelineOrigin(p_filter, raw_timestamp_ms);
        timestamp_ms = PictureTimeMs(sys, p_pic);
    }
    sys->frame_count++;
    sequence = sys->frame_count;
    sys->last_frame_timestamp_ms = timestamp_ms;
    sys->last_frame_timestamp_valid = true;

    if (sys->decision_map_mode) {
        blocked = DecisionMapShouldBlock(p_filter, p_pic);
        if (sys->frame_count == 1) {
            fprintf(stderr,
                    "icop: first decision-map frame is %llu ms, blocked=%d\n",
                    (unsigned long long)timestamp_ms, blocked ? 1 : 0);
        }
        if (blocked) {
            sys->block_count++;
            if (sys->block_count == 1 || (sys->block_count % 60) == 0) {
                fprintf(stderr,
                        "icop: decision map blocked frame %llu ms (scanned %llu ms)%s\n",
                        (unsigned long long)PictureTimeMs(sys, p_pic),
                        (unsigned long long)sys->last_scanned_ms,
                        sys->scan_done ? "" : ", pending scan");
            }
        }
        return ApplyDisplayOutput(p_filter, p_pic, blocked, NULL);
    }

#ifdef _WIN32
    if (sys->worker_running) {
        EnterCriticalSection(&sys->worker_lock);
        blocked = TimeInBlockedRangeLocked(sys, timestamp_ms);
        LeaveCriticalSection(&sys->worker_lock);
#else
    if (sys->worker_running) {
        pthread_mutex_lock(&sys->worker_lock);
        blocked = TimeInBlockedRangeLocked(sys, timestamp_ms);
        pthread_mutex_unlock(&sys->worker_lock);
#endif

        should_analyze = ((sys->frame_count - 1) % sys->analysis_stride) == 0;
        if (blocked)
            should_analyze = false;

#ifdef _WIN32
        EnterCriticalSection(&sys->worker_lock);
        while (sys->queue_count >= NSFW_MAX_BUFFER_FRAMES &&
               !OldestFrameReadyLocked(sys)) {
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
        }
#else
        pthread_mutex_lock(&sys->worker_lock);
        while (sys->queue_count >= NSFW_MAX_BUFFER_FRAMES &&
               !OldestFrameReadyLocked(sys)) {
            pthread_cond_wait(&sys->worker_cond, &sys->worker_lock);
        }
#endif

        if (sys->queue_count >= NSFW_MAX_BUFFER_FRAMES)
            output = TakeReadyOutputLocked(sys, &blocked, &output_result,
                                           &output_evaluated);

        if (!QueuePictureLocked(sys, p_pic, should_analyze)) {
#ifdef _WIN32
            LeaveCriticalSection(&sys->worker_lock);
#else
            pthread_mutex_unlock(&sys->worker_lock);
#endif
            ReleasePicture(p_pic);
            return NULL;
        }

#ifdef _WIN32
        WakeConditionVariable(&sys->worker_cond);
#else
        pthread_cond_signal(&sys->worker_cond);
#endif

        if (sys->queue_count < sys->prebuffer_frames) {
#ifdef _WIN32
            LeaveCriticalSection(&sys->worker_lock);
#else
            pthread_mutex_unlock(&sys->worker_lock);
#endif
            if (output != NULL) {
                return ApplyDisplayOutput(
                    p_filter, output, blocked,
                    output_evaluated ? &output_result : NULL);
            }
            return NULL;
        }

#ifdef _WIN32
        while (output == NULL && !OldestFrameReadyLocked(sys))
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
#else
        while (output == NULL && !OldestFrameReadyLocked(sys))
            pthread_cond_wait(&sys->worker_cond, &sys->worker_lock);
#endif

        if (output == NULL)
            output = TakeReadyOutputLocked(sys, &blocked, &output_result,
                                           &output_evaluated);
#ifdef _WIN32
        LeaveCriticalSection(&sys->worker_lock);
#else
        pthread_mutex_unlock(&sys->worker_lock);
#endif

        if (output == NULL)
            return NULL;
        return ApplyDisplayOutput(p_filter, output, blocked,
                                  output_evaluated ? &output_result : NULL);
    } else
    if (sys->detector != NULL) {
        nsfw_result_t result = { 0, 0.0f, 0.0f };
        bool result_available = false;

        blocked = TimeInBlockedRangeLocked(sys, timestamp_ms);
        if (blocked) {
            return ApplyDisplayOutput(p_filter, p_pic, true, NULL);
        }

        should_analyze = ((sys->frame_count - 1) % sys->analysis_stride) == 0;
        needed = (size_t)nsfw_fp_clamp_dimension(sys->analysis_width,
                                        nsfw_fp_visible_width(&p_pic->format)) *
                 (size_t)nsfw_fp_clamp_dimension(sys->analysis_height,
                                        nsfw_fp_visible_height(&p_pic->format)) * 3;
        if (should_analyze && needed > 0 &&
            EnsureRgbBuffer(sys, needed) == 0) {
            int packed = PackPictureForAnalysis(
                p_filter, p_pic, sys->rgb_buffer, sys->rgb_capacity,
                &width, &height);

            if (packed == 0) {
                result = sys->detector_classify_fn(
                    sys->detector, sys->rgb_buffer, width, height, 3);
                result_available = true;
                RegisterPositiveDetection(sys, &result, timestamp_ms,
                                          &blocked);
            } else if (sys->backend_ops != NULL) {
                result.is_nsfw = 1;
                result.score = 1.0f;
                result.threshold = sys->threshold;
                result_available = true;
                RegisterPositiveDetection(sys, &result, timestamp_ms,
                                          &blocked);
                if (MarkBackendFailureLogged(sys)) {
                    fprintf(stderr,
                            "icop: hardware backend synchronous analysis failed; blocking affected frames fail-closed\n");
                }
            } else if (sys->opaque_fallback) {
                result.is_nsfw = 1;
                result.score = 1.0f;
                result.threshold = sys->threshold;
                result_available = true;
                RegisterPositiveDetection(sys, &result, timestamp_ms,
                                          &blocked);
                if (!sys->debug_dump_done) {
                    fprintf(stderr,
                            "icop: opaque-fallback analysis conversion failed; dropping blocked output fail-closed\n");
                    sys->debug_dump_done = true;
                }
            } else if (sys->block_count == 0) {
                fprintf(stderr,
                        "icop: unable to pack frame for ONNX inference, using heuristic fallback\n");
            }
        }

        if (blocked) {
            return ApplyDisplayOutput(p_filter, p_pic, true,
                                      result_available ? &result : NULL);
        }

        if (!should_analyze) {
            return ApplyDisplayOutput(p_filter, p_pic, false, NULL);
        }

        if (needed > 0 && width > 0 && height > 0) {
            return ApplyDisplayOutput(p_filter, p_pic, false,
                                      result_available ? &result : NULL);
        }
    }

    if (sys->backend_ops != NULL) {
        nsfw_result_t failed = { 1, 1.0f, sys->threshold };
        if (MarkBackendFailureLogged(sys)) {
            fprintf(stderr,
                    "icop: detector unavailable for hardware backend; blocking fail-closed\n");
        }
        return ApplyDisplayOutput(p_filter, p_pic, true, &failed);
    }

    if (sys->opaque_fallback) {
        nsfw_result_t failed = { 1, 1.0f, sys->threshold };
        if (!sys->debug_dump_done) {
            fprintf(stderr,
                    "icop: detector unavailable for opaque-hw frames; blocking fail-closed\n");
            sys->debug_dump_done = true;
        }
        return ApplyDisplayOutput(p_filter, p_pic, true, &failed);
    }

        {
            float score = nsfw_fp_heuristic_score(p_pic);
            nsfw_result_t result = { 0, score, sys->threshold };
            blocked = score >= sys->threshold;
            result.is_nsfw = blocked ? 1 : 0;
            if (blocked) {
                sys->block_count++;
                if (sys->block_count == 1 || (sys->block_count % 30) == 0) {
                    fprintf(stderr,
                            "icop: heuristic score %.3f >= %.3f, blacking out frame\n",
                            score, sys->threshold);
                }
            }
            return ApplyDisplayOutput(p_filter, p_pic, blocked, &result);
        }
}

#ifndef _WIN32
typedef int (*nsfw_vlc_set_cb)(void *, void *, int, ...);
extern int vlc_entry__3_0_0f(nsfw_vlc_set_cb vlc_set, void *opaque);

__attribute__((visibility("default")))
int vlc_entry__3_0_0ft64(nsfw_vlc_set_cb vlc_set, void *opaque)
{
    return vlc_entry__3_0_0f(vlc_set, opaque);
}
#endif
