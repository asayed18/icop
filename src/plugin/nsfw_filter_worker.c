/*****************************************************************************
 * nsfw_filter_worker.c: extracted from nsfw_filter.c
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <poll.h>
#include <ctype.h>
#include <math.h>

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

struct nsfw_worker_state_t {
#ifdef _WIN32
    HANDLE          thread;
#else
    pthread_t       thread;
#endif
    filter_sys_t   *sys;
    nsfw_detector_t *detector;
    uint8_t        *rgb_buffer;
    size_t          rgb_capacity;
    bool            running;
    bool            stop;
};

unsigned QueueIndex(const filter_sys_t *sys, unsigned offset)
{
    return (sys->queue_head + offset) % NSFW_MAX_BUFFER_FRAMES;
}

static nsfw_frame_slot_t *GetFrameSlotLocked(filter_sys_t *sys, unsigned offset)
{
    if (!sys || offset >= sys->queue_count)
        return NULL;
    return &sys->frame_queue[QueueIndex(sys, offset)];
}

bool TimeInBlockedRangeLocked(const filter_sys_t *sys, uint64_t timestamp_ms);

bool OldestFrameReadyLocked(filter_sys_t *sys)
{
    nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, 0);

    return slot != NULL &&
           slot->picture != NULL &&
           slot->decision_ready &&
           !slot->processing;
}

static nsfw_frame_slot_t *FindNextPendingFrameLocked(filter_sys_t *sys)
{
    unsigned i;

    if (!sys)
        return NULL;

    for (i = 0; i < sys->queue_count; ++i) {
        nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, i);

        if (slot != NULL &&
            slot->picture != NULL &&
            !slot->decision_ready &&
            !slot->processing) {
            return slot;
        }
    }

    return NULL;
}

bool HasProcessingFramesLocked(filter_sys_t *sys)
{
    unsigned i;

    if (!sys)
        return false;

    for (i = 0; i < sys->queue_count; ++i) {
        nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, i);

        if (slot != NULL && slot->processing)
            return true;
    }

    return false;
}

bool QueuePictureLocked(filter_sys_t *sys, picture_t *pic, bool analyze)
{
    nsfw_frame_slot_t *slot;

    if (!sys || !pic || sys->queue_count >= NSFW_MAX_BUFFER_FRAMES)
        return false;

    slot = &sys->frame_queue[QueueIndex(sys, sys->queue_count)];
    slot->picture = pic;
    slot->result.is_nsfw = 0;
    slot->result.score = 0.0f;
    slot->result.threshold = sys->threshold;
    slot->sequence = sys->frame_count;
    slot->timestamp_ms = PictureTimeMs(sys, pic);
    slot->analyze = analyze;
    slot->processing = false;
    slot->decision_ready = false;
    slot->blocked = TimeInBlockedRangeLocked(sys, slot->timestamp_ms);
    sys->queue_count++;
    return true;
}

bool TimeInBlockedRangeLocked(const filter_sys_t *sys, uint64_t timestamp_ms)
{
    size_t i;

    if (!sys)
        return false;

    for (i = 0; i < sys->time_block_range_count; ++i) {
        const nsfw_time_block_range_t *range =
            &sys->time_block_ranges[i];

        if (timestamp_ms < range->start_ms)
            return false;
        if (timestamp_ms <= range->end_ms)
            return true;
    }

    return false;
}

void PruneExpiredTimeBlockRangesLocked(filter_sys_t *sys,
                                             uint64_t min_timestamp_ms)
{
    size_t drop = 0;
    size_t remaining;

    if (!sys || sys->time_block_range_count == 0)
        return;

    while (drop < sys->time_block_range_count &&
           sys->time_block_ranges[drop].end_ms < min_timestamp_ms) {
        drop++;
    }

    if (drop == 0)
        return;

    remaining = sys->time_block_range_count - drop;
    if (remaining > 0) {
        memmove(sys->time_block_ranges,
                sys->time_block_ranges + drop,
                remaining * sizeof(*sys->time_block_ranges));
    }
    sys->time_block_range_count = remaining;
}

void EnsureQueuedFramesInWindowLocked(filter_sys_t *sys,
                                             uint64_t start_ms,
                                             uint64_t end_ms)
{
    unsigned i;

    if (!sys || end_ms < start_ms)
        return;

    for (i = 0; i < sys->queue_count; ++i) {
        nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, i);

        if (slot != NULL &&
            slot->timestamp_ms >= start_ms &&
            slot->timestamp_ms <= end_ms) {
            slot->blocked = true;
        }
    }
}

void ApplyBlockWindowLocked(filter_sys_t *sys, uint64_t start_ms,
                                   uint64_t end_ms)
{
    size_t pos;
    size_t count;
    nsfw_time_block_range_t *ranges;

    if (!sys || end_ms < start_ms)
        return;

    PruneExpiredTimeBlockRangesLocked(sys, start_ms);

    count = sys->time_block_range_count;
    ranges = sys->time_block_ranges;

    if (count == 0) {
        if (sys->time_block_range_capacity == 0 &&
            !EnsureTimeBlockRangeCapacity(sys, 1))
            return;
        ranges = sys->time_block_ranges;
        ranges[0].start_ms = start_ms;
        ranges[0].end_ms = end_ms;
        sys->time_block_range_count = 1;
        EnsureQueuedFramesInWindowLocked(sys, start_ms, end_ms);
        return;
    }

    if (count + 1 > sys->time_block_range_capacity &&
        !EnsureTimeBlockRangeCapacity(sys, count + 1)) {
        return;
    }

    ranges = sys->time_block_ranges;
    pos = 0;
    while (pos < count && ranges[pos].start_ms < start_ms)
        pos++;

    if (pos > 0 && ranges[pos - 1].end_ms >= start_ms) {
        if (start_ms < ranges[pos - 1].start_ms)
            ranges[pos - 1].start_ms = start_ms;
        if (end_ms > ranges[pos - 1].end_ms)
            ranges[pos - 1].end_ms = end_ms;
        pos--;
    } else {
        memmove(&ranges[pos + 1], &ranges[pos],
                (count - pos) * sizeof(*ranges));
        ranges[pos].start_ms = start_ms;
        ranges[pos].end_ms = end_ms;
        count++;
    }

    while (pos + 1 < count && ranges[pos].end_ms >= ranges[pos + 1].start_ms) {
        if (ranges[pos + 1].end_ms > ranges[pos].end_ms)
            ranges[pos].end_ms = ranges[pos + 1].end_ms;
        memmove(&ranges[pos + 1], &ranges[pos + 2],
                (count - pos - 2) * sizeof(*ranges));
        count--;
    }

    sys->time_block_range_count = count;
    EnsureQueuedFramesInWindowLocked(sys, start_ms, end_ms);
}

picture_t *TakeReadyOutputLocked(filter_sys_t *sys, bool *blocked,
                                        nsfw_result_t *result,
                                        bool *evaluated)
{
    nsfw_frame_slot_t *slot;
    picture_t *picture;

    if (!sys || !blocked || !result || !evaluated ||
        !OldestFrameReadyLocked(sys))
        return NULL;

    slot = GetFrameSlotLocked(sys, 0);
    picture = slot->picture;
    *blocked = slot->blocked ||
               TimeInBlockedRangeLocked(sys, slot->timestamp_ms);
    *result = slot->result;
    *evaluated = slot->analyze;
    memset(slot, 0, sizeof(*slot));
    sys->queue_head = (sys->queue_head + 1) % NSFW_MAX_BUFFER_FRAMES;
    sys->queue_count--;
    if (sys->queue_count == 0)
        sys->queue_head = 0;
    return picture;
}

void ReleaseQueuedFramesLocked(filter_sys_t *sys)
{
    unsigned i;

    if (!sys)
        return;

    for (i = 0; i < sys->queue_count; ++i) {
        nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, i);

        if (slot != NULL && slot->picture != NULL)
            ReleasePicture(slot->picture);
    }

    memset(sys->frame_queue, 0, sizeof(sys->frame_queue));
    sys->queue_head = 0;
    sys->queue_count = 0;
}

void RegisterPositiveDetection(filter_sys_t *sys,
                                      const nsfw_result_t *result,
                                      uint64_t timestamp_ms,
                                      bool *blocked)
{
    uint64_t start_ms;
    uint64_t end_ms;
    uint64_t padding_ms;

    if (!sys || !result || !blocked || !result->is_nsfw)
        return;

    *blocked = true;
    sys->audio_mute_requested = true;
    padding_ms = (uint64_t)sys->block_padding_frames *
                 (sys->frame_interval_ms > 0 ? sys->frame_interval_ms : 41);
    start_ms = timestamp_ms > padding_ms ? timestamp_ms - padding_ms : 0;
    end_ms = timestamp_ms + padding_ms;
    ApplyBlockWindowLocked(sys, start_ms, end_ms);
    sys->block_count++;
    if (sys->block_count == 1 || (sys->block_count % 30) == 0) {
        fprintf(stderr,
                "icop: ONNX score %.3f >= %.3f, blocking output with %s style around %llu ms (+/-%u frames)\n",
                result->score, result->threshold,
                BlockStyleName(sys->block_style),
                (unsigned long long)timestamp_ms, sys->block_padding_frames);
    }
}

#ifdef _WIN32
static unsigned __stdcall DetectorWorkerThread(void *data)
{
    nsfw_worker_state_t *worker = (nsfw_worker_state_t *)data;
    filter_sys_t *sys;

    if (worker == NULL)
        return 0;

    sys = worker->sys;
    if (sys == NULL)
        return 0;

    for (;;) {
        nsfw_frame_slot_t *slot;
        picture_t *picture;
        nsfw_result_t result = { 0, 0.0f, 0.0f };
        int width = 0;
        int height = 0;
        bool blocked = false;
        bool packed = false;
        bool gpu_readback_failed = false;

        EnterCriticalSection(&sys->worker_lock);
        while (!sys->worker_stop &&
               (slot = FindNextPendingFrameLocked(sys)) == NULL) {
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
        }

        if (sys->worker_stop) {
            LeaveCriticalSection(&sys->worker_lock);
            break;
        }

        slot->processing = true;
        picture = slot->picture;
        LeaveCriticalSection(&sys->worker_lock);

        if (slot->analyze && picture != NULL) {
            size_t needed = (size_t)nsfw_fp_clamp_dimension(sys->analysis_width,
                                                   nsfw_fp_visible_width(&picture->format)) *
                            (size_t)nsfw_fp_clamp_dimension(sys->analysis_height,
                                                   nsfw_fp_visible_height(&picture->format)) * 3;

            if (needed > worker->rgb_capacity) {
                uint8_t *buf = (uint8_t *)realloc(worker->rgb_buffer, needed);
                if (buf != NULL) {
                    worker->rgb_buffer = buf;
                    worker->rgb_capacity = needed;
                }
            }

            if (worker->rgb_buffer != NULL &&
                worker->rgb_capacity >= needed) {
                if (sys->backend_ops != NULL) {
                    packed = sys->backend_ops->readback_rgb(
                        sys->backend_data, picture, worker->rgb_buffer,
                        worker->rgb_capacity, &width, &height) == 0;
                    gpu_readback_failed = !packed;
                } else {
                    packed = nsfw_fp_pack_to_rgb(
                        picture, worker->rgb_buffer,
                        worker->rgb_capacity, sys->analysis_width,
                        sys->analysis_height, &width, &height) == 0;
                }
            }

            if (packed) {
                result = sys->detector_classify_fn(worker->detector,
                                                   worker->rgb_buffer,
                                                   width, height, 3);
            } else if (gpu_readback_failed) {
                result.is_nsfw = 1;
                result.score = 1.0f;
                result.threshold = sys->threshold;
                if (MarkBackendFailureLogged(sys)) {
                    fprintf(stderr,
                            "icop: hardware backend analysis readback failed; blocking affected frames fail-closed\n");
                }
            } else {
                float score = nsfw_fp_heuristic_score(picture);

                result.is_nsfw = score >= sys->threshold;
                result.score = score;
                result.threshold = sys->threshold;
                if (sys->block_count == 0) {
                    fprintf(stderr,
                            "icop: unable to pack frame for ONNX inference, using heuristic fallback\n");
                }
            }
        }

        EnterCriticalSection(&sys->worker_lock);
        blocked = slot->blocked ||
                  TimeInBlockedRangeLocked(sys, slot->timestamp_ms);
        if (result.is_nsfw)
            RegisterPositiveDetection(sys, &result, slot->timestamp_ms, &blocked);

        slot->result = result;
        slot->blocked = blocked;
        slot->decision_ready = true;
        slot->processing = false;
        WakeAllConditionVariable(&sys->worker_cond);
        LeaveCriticalSection(&sys->worker_lock);
    }

    return 0;
}
#else
static void *DetectorWorkerThreadPthread(void *data)
{
    nsfw_worker_state_t *worker = (nsfw_worker_state_t *)data;
    filter_sys_t *sys;

    if (worker == NULL)
        return NULL;

    sys = worker->sys;
    if (sys == NULL)
        return NULL;

    for (;;) {
        nsfw_frame_slot_t *slot;
        picture_t *picture;
        nsfw_result_t result = { 0, 0.0f, 0.0f };
        int width = 0;
        int height = 0;
        bool blocked = false;
        bool packed = false;

        pthread_mutex_lock(&sys->worker_lock);
        while (!sys->worker_stop &&
               (slot = FindNextPendingFrameLocked(sys)) == NULL) {
            pthread_cond_wait(&sys->worker_cond, &sys->worker_lock);
        }

        if (sys->worker_stop) {
            pthread_mutex_unlock(&sys->worker_lock);
            break;
        }

        slot->processing = true;
        picture = slot->picture;
        pthread_mutex_unlock(&sys->worker_lock);

        if (slot->analyze && picture != NULL) {
            size_t needed = (size_t)nsfw_fp_clamp_dimension(sys->analysis_width,
                                                   nsfw_fp_visible_width(&picture->format)) *
                            (size_t)nsfw_fp_clamp_dimension(sys->analysis_height,
                                                   nsfw_fp_visible_height(&picture->format)) * 3;

            if (needed > worker->rgb_capacity) {
                uint8_t *buf = (uint8_t *)realloc(worker->rgb_buffer, needed);
                if (buf != NULL) {
                    worker->rgb_buffer = buf;
                    worker->rgb_capacity = needed;
                }
            }

            if (worker->rgb_buffer != NULL &&
                worker->rgb_capacity >= needed) {
                packed = nsfw_fp_pack_to_rgb(
                    picture, worker->rgb_buffer,
                    worker->rgb_capacity, sys->analysis_width,
                    sys->analysis_height, &width, &height) == 0;
            }

            if (packed) {
                result = sys->detector_classify_fn(worker->detector,
                                                   worker->rgb_buffer,
                                                   width, height, 3);
            } else {
                float score = nsfw_fp_heuristic_score(picture);

                result.is_nsfw = score >= sys->threshold;
                result.score = score;
                result.threshold = sys->threshold;
                if (sys->block_count == 0) {
                    fprintf(stderr,
                            "icop: unable to pack frame for ONNX inference, using heuristic fallback\n");
                }
            }
        }

        pthread_mutex_lock(&sys->worker_lock);
        blocked = slot->blocked ||
                  TimeInBlockedRangeLocked(sys, slot->timestamp_ms);
        if (result.is_nsfw)
            RegisterPositiveDetection(sys, &result, slot->timestamp_ms, &blocked);

        slot->result = result;
        slot->blocked = blocked;
        slot->decision_ready = true;
        slot->processing = false;
        pthread_cond_broadcast(&sys->worker_cond);
        pthread_mutex_unlock(&sys->worker_lock);
    }

    return NULL;
}
#endif
int StartDetectorWorker(filter_sys_t *sys, const nsfw_config_t *cfg)
{
    unsigned i;
    unsigned started = 0;
    unsigned desired_workers;

    if (!sys || !cfg || sys->detector_classify_fn == NULL)
        return VLC_EGENERIC;

    desired_workers = ResolveWorkerCount();
    if (desired_workers == 0)
        desired_workers = 1;

    sys->workers = (nsfw_worker_state_t *)calloc(desired_workers,
                                                 sizeof(*sys->workers));
    if (sys->workers == NULL)
        return VLC_ENOMEM;

#ifdef _WIN32
    InitializeCriticalSection(&sys->worker_lock);
    InitializeConditionVariable(&sys->worker_cond);
#else
    pthread_mutex_init(&sys->worker_lock, NULL);
    pthread_cond_init(&sys->worker_cond, NULL);
#endif
    sys->worker_stop = false;
    sys->worker_running = false;
    sys->worker_count = 0;

    for (i = 0; i < desired_workers; ++i) {
        nsfw_worker_state_t *worker = &sys->workers[started];

        worker->sys = sys;
        worker->detector = sys->detector_create_fn(cfg);
        if (worker->detector == NULL)
            continue;

#ifdef _WIN32
        worker->thread = (HANDLE)_beginthreadex(NULL, 0,
                                                DetectorWorkerThread,
                                                worker, 0, NULL);
        if (worker->thread == NULL) {
#else
        if (pthread_create(&worker->thread, NULL,
                           DetectorWorkerThreadPthread, worker) != 0) {
#endif
            if (worker->detector != NULL) {
                sys->detector_destroy_fn(worker->detector);
                worker->detector = NULL;
            }
            continue;
        }

        worker->running = true;
        started++;
    }

    if (started == 0) {
#ifdef _WIN32
        DeleteCriticalSection(&sys->worker_lock);
#else
        pthread_mutex_destroy(&sys->worker_lock);
        pthread_cond_destroy(&sys->worker_cond);
#endif
        free(sys->workers);
        sys->workers = NULL;
        return VLC_EGENERIC;
    }

    sys->worker_count = started;
    sys->worker_running = true;
    return VLC_SUCCESS;
}

void StopDetectorWorker(filter_sys_t *sys)
{
    unsigned i;

    if (!sys || !sys->worker_running)
        return;

#ifdef _WIN32
    EnterCriticalSection(&sys->worker_lock);
    sys->worker_stop = true;
    WakeAllConditionVariable(&sys->worker_cond);
    LeaveCriticalSection(&sys->worker_lock);

    for (i = 0; i < sys->worker_count; ++i) {
        nsfw_worker_state_t *worker = &sys->workers[i];

        if (worker->thread != NULL) {
            WaitForSingleObject(worker->thread, INFINITE);
            CloseHandle(worker->thread);
            worker->thread = NULL;
        }
        if (worker->detector != NULL) {
            sys->detector_destroy_fn(worker->detector);
            worker->detector = NULL;
        }
        free(worker->rgb_buffer);
        worker->rgb_buffer = NULL;
        worker->rgb_capacity = 0;
        worker->running = false;
    }

    DeleteCriticalSection(&sys->worker_lock);
#else
    pthread_mutex_lock(&sys->worker_lock);
    sys->worker_stop = true;
    pthread_cond_broadcast(&sys->worker_cond);
    pthread_mutex_unlock(&sys->worker_lock);

    for (i = 0; i < sys->worker_count; ++i) {
        nsfw_worker_state_t *worker = &sys->workers[i];

        if (worker->running) {
            pthread_join(worker->thread, NULL);
        }
        if (worker->detector != NULL) {
            sys->detector_destroy_fn(worker->detector);
            worker->detector = NULL;
        }
        free(worker->rgb_buffer);
        worker->rgb_buffer = NULL;
        worker->rgb_capacity = 0;
        worker->running = false;
    }

    pthread_mutex_destroy(&sys->worker_lock);
    pthread_cond_destroy(&sys->worker_cond);
#endif
    free(sys->workers);
    sys->workers = NULL;
    sys->worker_count = 0;
    sys->worker_running = false;
    sys->worker_stop = false;
}
