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
#include "nsfw_cuda_host.h"
#include "platform_abstraction.h"
#include "frame_processor.h"

#ifdef _WIN32
typedef enum nsfw_inference_host_kind_t {
    NSFW_INFERENCE_HOST_NONE = 0,
    NSFW_INFERENCE_HOST_CUDA,
    NSFW_INFERENCE_HOST_DML,
} nsfw_inference_host_kind_t;
#endif

struct nsfw_worker_state_t {
#ifdef _WIN32
    HANDLE          thread;
#else
    pthread_t       thread;
#endif
    filter_sys_t   *sys;
    nsfw_detector_t *detector;
    nsfw_cuda_host_t *cuda_host;
    nsfw_config_t   config;
    char           *model_path;
    uint8_t        *rgb_buffer;
    size_t          rgb_capacity;
    bool            create_detector_on_thread;
    bool            use_cuda_host;
#ifdef _WIN32
    nsfw_inference_host_kind_t host_kind;
    bool            gpu_batch_supported;
#endif
    bool            running;
    bool            stop;
};

static nsfw_frame_slot_t *GetFrameSlotLocked(filter_sys_t *sys,
                                             unsigned offset);
static nsfw_frame_slot_t *FindNextPendingFrameLocked(filter_sys_t *sys);

/* The main Windows core is paired with the CUDA ONNX Runtime.  When the
 * isolated GPU hosts are unavailable, create a CPU session in that core
 * without changing the user's provider setting permanently. */
nsfw_detector_t *CreateCpuFallbackDetector(filter_sys_t *sys,
                                           const nsfw_config_t *config)
{
    const char *current_provider;
    char *saved_provider;
    nsfw_detector_t *detector;

    if (sys == NULL || config == NULL || sys->detector_create_fn == NULL)
        return NULL;

    current_provider = getenv("NSFW_ONNX_PROVIDER");
    saved_provider = current_provider != NULL ? DuplicateString(current_provider)
                                             : NULL;
    if (current_provider != NULL && saved_provider == NULL)
        return NULL;

    nsfw_plat_set_env("NSFW_ONNX_PROVIDER", "cpu");
    detector = sys->detector_create_fn(config);
    nsfw_plat_set_env("NSFW_ONNX_PROVIDER",
                      saved_provider != NULL ? saved_provider : "");
    free(saved_provider);
    return detector;
}

int ClassifyDetector(filter_sys_t *sys, nsfw_detector_t *detector,
                     const uint8_t *frame_data, int width, int height,
                     int channels, nsfw_result_t *result)
{
    if (sys == NULL || detector == NULL || frame_data == NULL ||
        result == NULL || sys->detector_classify_checked_fn == NULL) {
        return -1;
    }

    return sys->detector_classify_checked_fn(detector, frame_data, width,
                                             height, channels, result);
}

#ifdef _WIN32
static int StartWindowsInferenceEndpoint(nsfw_worker_state_t *worker)
{
    if (worker == NULL)
        return -1;

    if (nsfw_cuda_host_start(&worker->cuda_host, &worker->config) == 0) {
        worker->host_kind = NSFW_INFERENCE_HOST_CUDA;
        worker->use_cuda_host = true;
        fprintf(stderr, "icop: using isolated CUDA inference host\n");
        return 0;
    }

    fprintf(stderr, "icop: CUDA host unavailable; trying DirectML\n");
    if (nsfw_dml_host_start(&worker->cuda_host, &worker->config) == 0) {
        worker->host_kind = NSFW_INFERENCE_HOST_DML;
        worker->use_cuda_host = true;
        fprintf(stderr, "icop: using isolated DirectML inference host\n");
        return 0;
    }

    fprintf(stderr, "icop: DirectML host unavailable; falling back to CPU\n");
    worker->use_cuda_host = false;
    worker->host_kind = NSFW_INFERENCE_HOST_NONE;
    worker->detector = CreateCpuFallbackDetector(worker->sys, &worker->config);
    if (worker->detector == NULL)
        return -1;

    fprintf(stderr, "icop: using CPU inference fallback\n");
    return 0;
}

static int ClassifyBatchWithWindowsFallback(nsfw_worker_state_t *worker,
                                            const uint8_t *frame_data,
                                            unsigned frame_count,
                                            int width, int height,
                                            nsfw_result_t *results);

static int ClassifyWithWindowsFallback(nsfw_worker_state_t *worker,
                                       const uint8_t *frame_data,
                                       int width, int height,
                                       nsfw_result_t *result)
{
    return ClassifyBatchWithWindowsFallback(worker, frame_data, 1, width,
                                            height, result);
}

static int ClassifyBatchWithWindowsFallback(nsfw_worker_state_t *worker,
                                            const uint8_t *frame_data,
                                            unsigned frame_count,
                                            int width, int height,
                                            nsfw_result_t *results)
{
    size_t frame_byte_count;
    unsigned i;

    if (worker == NULL || frame_data == NULL || results == NULL ||
        frame_count == 0 || frame_count > NSFW_MAX_GPU_BATCH_FRAMES) {
        return -1;
    }

    frame_byte_count = (size_t)width * (size_t)height * 3;
    if (frame_byte_count == 0)
        return -1;

    for (;;) {
        if (worker->use_cuda_host) {
            int batch_status = nsfw_cuda_host_classify_batch(
                worker->cuda_host, frame_data, frame_count, width, height, 3,
                results);
            if (batch_status == 0) {
                return 0;
            }
            if (batch_status == NSFW_BATCH_UNSUPPORTED) {
                /* The graph itself has a fixed batch axis.  Remember that
                 * result so later frames take the normal single-frame path
                 * on this same already-loaded GPU session. */
                worker->gpu_batch_supported = false;
                for (i = 0; i < frame_count; ++i) {
                    if (nsfw_cuda_host_classify(
                            worker->cuda_host,
                            frame_data + (size_t)i * frame_byte_count,
                            width, height, 3, &results[i]) != 0) {
                        break;
                    }
                }
                if (i == frame_count)
                    return 0;
            }

            nsfw_cuda_host_stop(&worker->cuda_host);
            if (worker->host_kind == NSFW_INFERENCE_HOST_CUDA) {
                fprintf(stderr,
                        "icop: CUDA host stopped responding; trying DirectML\n");
                if (nsfw_dml_host_start(&worker->cuda_host,
                                        &worker->config) == 0) {
                    worker->host_kind = NSFW_INFERENCE_HOST_DML;
                    fprintf(stderr,
                            "icop: using isolated DirectML inference host\n");
                    continue;
                }
                fprintf(stderr,
                        "icop: DirectML host unavailable; falling back to CPU\n");
            } else {
                fprintf(stderr,
                        "icop: DirectML host stopped responding; falling back to CPU\n");
            }

            worker->use_cuda_host = false;
            worker->host_kind = NSFW_INFERENCE_HOST_NONE;
            worker->detector = CreateCpuFallbackDetector(worker->sys,
                                                          &worker->config);
            if (worker->detector == NULL)
                return -1;
            fprintf(stderr, "icop: using CPU inference fallback\n");
        }

        if (worker->detector != NULL) {
            for (i = 0; i < frame_count; ++i) {
                if (ClassifyDetector(worker->sys, worker->detector,
                                     frame_data + (size_t)i * frame_byte_count,
                                     width, height, 3, &results[i]) != 0) {
                    return -1;
                }
            }
            return 0;
        }
        return -1;
    }
}

static bool WorkerUsesGpuBatch(const nsfw_worker_state_t *worker)
{
    return worker != NULL && worker->sys != NULL && worker->use_cuda_host &&
           worker->gpu_batch_supported && worker->sys->gpu_batch_size > 1;
}

/* Return 1 after a batch was processed, 0 when the earliest pending frame
 * should use the ordinary single-frame path, and -1 when stopping. */
static int ProcessGpuBatch(nsfw_worker_state_t *worker)
{
    filter_sys_t *sys;
    nsfw_frame_slot_t *slots[NSFW_MAX_GPU_BATCH_FRAMES] = { 0 };
    picture_t *pictures[NSFW_MAX_GPU_BATCH_FRAMES] = { 0 };
    nsfw_result_t results[NSFW_MAX_GPU_BATCH_FRAMES] = { 0 };
    unsigned batch_limit;
    unsigned batch_count = 0;
    unsigned i;
    int batch_width = 0;
    int batch_height = 0;
    size_t frame_byte_count = 0;
    bool packed = true;
    bool gpu_readback_failed = false;
    bool inference_failed = false;

    if (!WorkerUsesGpuBatch(worker))
        return 0;
    sys = worker->sys;
    batch_limit = sys->gpu_batch_size;
    if (batch_limit > NSFW_MAX_GPU_BATCH_FRAMES)
        batch_limit = NSFW_MAX_GPU_BATCH_FRAMES;

    EnterCriticalSection(&sys->worker_lock);
    for (;;) {
        nsfw_frame_slot_t *first = FindNextPendingFrameLocked(sys);

        if (sys->worker_stop) {
            LeaveCriticalSection(&sys->worker_lock);
            return -1;
        }
        if (first == NULL) {
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
            continue;
        }
        if (!first->analyze) {
            LeaveCriticalSection(&sys->worker_lock);
            return 0;
        }
        if (sys->queue_count < sys->prebuffer_frames) {
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);
            continue;
        }
        break;
    }

    for (i = 0; i < sys->queue_count && batch_count < batch_limit; ++i) {
        nsfw_frame_slot_t *slot = GetFrameSlotLocked(sys, i);

        if (slot != NULL && slot->picture != NULL && slot->analyze &&
            !slot->decision_ready && !slot->processing) {
            slot->processing = true;
            slots[batch_count] = slot;
            pictures[batch_count] = slot->picture;
            batch_count++;
        }
    }
    LeaveCriticalSection(&sys->worker_lock);

    if (batch_count == 0)
        return 0;

    for (i = 0; i < batch_count; ++i) {
        picture_t *picture = pictures[i];
        size_t needed;
        int width = 0;
        int height = 0;
        bool frame_packed = false;

        needed = (size_t)nsfw_fp_clamp_dimension(
                     sys->analysis_width,
                     nsfw_fp_visible_width(&picture->format)) *
                 (size_t)nsfw_fp_clamp_dimension(
                     sys->analysis_height,
                     nsfw_fp_visible_height(&picture->format)) * 3;
        if (i == 0) {
            if (needed == 0 || needed > SIZE_MAX / batch_count) {
                packed = false;
                break;
            }
            if (needed * batch_count > worker->rgb_capacity) {
                uint8_t *replacement = (uint8_t *)realloc(
                    worker->rgb_buffer, needed * batch_count);
                if (replacement == NULL) {
                    packed = false;
                    break;
                }
                worker->rgb_buffer = replacement;
                worker->rgb_capacity = needed * batch_count;
            }
        }

        if (worker->rgb_buffer == NULL || worker->rgb_capacity < needed * batch_count) {
            packed = false;
            break;
        }

        if (sys->backend_ops != NULL) {
            frame_packed = sys->backend_ops->readback_rgb(
                sys->backend_data, picture,
                worker->rgb_buffer + (size_t)i * needed, needed,
                &width, &height) == 0;
            if (!frame_packed)
                gpu_readback_failed = true;
        } else {
            frame_packed = nsfw_fp_pack_to_rgb(
                picture, worker->rgb_buffer + (size_t)i * needed, needed,
                sys->analysis_width, sys->analysis_height, &width, &height) == 0;
        }
        if (!frame_packed || width <= 0 || height <= 0) {
            packed = false;
            break;
        }

        if (i == 0) {
            batch_width = width;
            batch_height = height;
            frame_byte_count = (size_t)width * (size_t)height * 3;
            if (frame_byte_count == 0 || frame_byte_count != needed) {
                packed = false;
                break;
            }
        } else if (width != batch_width || height != batch_height ||
                   frame_byte_count != needed) {
            packed = false;
            break;
        }
    }

    if (packed) {
        if (ClassifyBatchWithWindowsFallback(worker, worker->rgb_buffer,
                                             batch_count, batch_width,
                                             batch_height, results) != 0) {
            inference_failed = true;
            for (i = 0; i < batch_count; ++i) {
                results[i].is_nsfw = 1;
                results[i].score = 1.0f;
                results[i].threshold = sys->threshold;
            }
        }
    } else {
        for (i = 0; i < batch_count; ++i) {
            results[i].is_nsfw = 1;
            results[i].score = 1.0f;
            results[i].threshold = sys->threshold;
        }
        if (gpu_readback_failed && MarkBackendFailureLogged(sys)) {
            fprintf(stderr,
                    "icop: hardware backend batch readback failed; blocking affected output fail-closed\n");
        }
    }

    EnterCriticalSection(&sys->worker_lock);
    if (inference_failed) {
        sys->cuda_host_failed = true;
        if (MarkBackendFailureLogged(sys)) {
            fprintf(stderr,
                    "icop: GPU batch inference failed; blocking output fail-closed\n");
        }
    }
    for (i = 0; i < batch_count; ++i) {
        bool blocked = sys->cuda_host_failed || slots[i]->blocked ||
                       TimeInBlockedRangeLocked(sys, slots[i]->timestamp_ms);

        if (results[i].is_nsfw) {
            RegisterPositiveDetection(sys, &results[i], slots[i]->timestamp_ms,
                                      &blocked);
        }
        slots[i]->result = results[i];
        slots[i]->blocked = blocked;
        slots[i]->decision_ready = true;
        slots[i]->processing = false;
    }
    WakeAllConditionVariable(&sys->worker_cond);
    LeaveCriticalSection(&sys->worker_lock);
    return 1;
}
#endif

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

static bool BlockRangeEndAtLocked(const filter_sys_t *sys,
                                  uint64_t timestamp_ms,
                                  uint64_t *end_ms)
{
    size_t i;

    if (sys == NULL || end_ms == NULL)
        return false;

    for (i = 0; i < sys->time_block_range_count; ++i) {
        const nsfw_time_block_range_t *range = &sys->time_block_ranges[i];

        if (timestamp_ms < range->start_ms)
            return false;
        if (timestamp_ms <= range->end_ms) {
            *end_ms = range->end_ms;
            return true;
        }
    }

    return false;
}

bool ShouldScheduleAnalysisLocked(filter_sys_t *sys, uint64_t sequence,
                                  uint64_t timestamp_ms)
{
    uint64_t block_end_ms;
    uint64_t renewal_lead_ms;

    if (sys == NULL || sys->analysis_stride == 0)
        return false;
    if (((sequence - 1) % sys->analysis_stride) != 0)
        return false;

    if (!BlockRangeEndAtLocked(sys, timestamp_ms, &block_end_ms))
        return true;

    /*
     * A positive result creates a padding window.  Do not spend inference on
     * every normal stride while that window is already masked; reserve one
     * stride-aligned sample immediately before it expires.  This renewal
     * sample extends a continuing block before raw output can reappear.
     */
    if (sys->renewal_block_end_ms == block_end_ms)
        return false;

    renewal_lead_ms = (uint64_t)sys->analysis_stride *
                      (sys->frame_interval_ms > 0 ?
                       sys->frame_interval_ms : 41);
    if (timestamp_ms < block_end_ms &&
        block_end_ms - timestamp_ms > renewal_lead_ms)
        return false;

    sys->renewal_block_end_ms = block_end_ms;
    return true;
}

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
    uint64_t end_ms;

    return BlockRangeEndAtLocked(sys, timestamp_ms, &end_ms);
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
    *blocked = sys->cuda_host_failed || slot->blocked ||
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
    uint64_t previous_end_ms = 0;
    uint64_t updated_end_ms = 0;
    bool had_active_range;

    if (!sys || !result || !blocked || !result->is_nsfw)
        return;

    *blocked = true;
    sys->audio_mute_requested = true;
    padding_ms = (uint64_t)sys->block_padding_frames *
                 (sys->frame_interval_ms > 0 ? sys->frame_interval_ms : 41);
    start_ms = timestamp_ms > padding_ms ? timestamp_ms - padding_ms : 0;
    end_ms = timestamp_ms + padding_ms;
    had_active_range = BlockRangeEndAtLocked(sys, timestamp_ms,
                                             &previous_end_ms);
    ApplyBlockWindowLocked(sys, start_ms, end_ms);
    if (BlockRangeEndAtLocked(sys, timestamp_ms, &updated_end_ms) &&
        (!had_active_range || updated_end_ms > previous_end_ms)) {
        /* A positive renewal moved the deadline, so schedule the next one. */
        sys->renewal_block_end_ms = 0;
    }
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

    if (worker->create_detector_on_thread) {
        worker->detector = sys->detector_create_fn(&worker->config);
        if (worker->detector == NULL) {
            fprintf(stderr,
                    "icop: GPU detector initialization failed on its worker thread; blocking analyzed frames fail-closed\n");
        }
    }

    for (;;) {
        if (WorkerUsesGpuBatch(worker)) {
            int batch_status = ProcessGpuBatch(worker);

            if (batch_status < 0)
                break;
            if (batch_status > 0)
                continue;
        }

        nsfw_frame_slot_t *slot;
        picture_t *picture;
        nsfw_result_t result = { 0, 0.0f, 0.0f };
        int width = 0;
        int height = 0;
        bool blocked = false;
        bool packed = false;
        bool gpu_readback_failed = false;
        bool inference_failed = false;

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

            if (packed && worker->use_cuda_host) {
                if (ClassifyWithWindowsFallback(worker,
                                                 worker->rgb_buffer,
                                                 width, height,
                                                 &result) != 0) {
                    inference_failed = true;
                    result.is_nsfw = 1;
                    result.score = 1.0f;
                    result.threshold = sys->threshold;
                    if (MarkBackendFailureLogged(sys)) {
                        fprintf(stderr,
                                "icop: CUDA, DirectML, and CPU inference are unavailable; blocking analyzed frames fail-closed\n");
                    }
                }
            } else if (packed && worker->detector != NULL) {
                if (ClassifyDetector(sys, worker->detector,
                                     worker->rgb_buffer, width, height, 3,
                                     &result) != 0) {
                    inference_failed = true;
                    result.is_nsfw = 1;
                    result.score = 1.0f;
                    result.threshold = sys->threshold;
                    if (MarkBackendFailureLogged(sys)) {
                        fprintf(stderr,
                                "icop: CPU inference failed; blocking analyzed frames fail-closed\n");
                    }
                }
            } else if (packed) {
                result.is_nsfw = 1;
                result.score = 1.0f;
                result.threshold = sys->threshold;
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
        if (inference_failed)
            sys->cuda_host_failed = true;
        blocked = sys->cuda_host_failed || slot->blocked ||
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

    if (worker->create_detector_on_thread) {
        worker->detector = sys->detector_create_fn(&worker->config);
        if (worker->detector == NULL) {
            fprintf(stderr,
                    "icop: GPU detector initialization failed on its worker thread; blocking analyzed frames fail-closed\n");
        }
    }

    for (;;) {
        nsfw_frame_slot_t *slot;
        picture_t *picture;
        nsfw_result_t result = { 0, 0.0f, 0.0f };
        int width = 0;
        int height = 0;
        bool blocked = false;
        bool packed = false;
        bool gpu_readback_failed = false;
        bool inference_failed = false;

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

            if (packed && worker->use_cuda_host) {
                if (nsfw_cuda_host_classify(worker->cuda_host,
                                             worker->rgb_buffer,
                                             width, height, 3,
                                             &result) != 0) {
                    inference_failed = true;
                    result.is_nsfw = 1;
                    result.score = 1.0f;
                    result.threshold = sys->threshold;
                    if (MarkBackendFailureLogged(sys)) {
                        fprintf(stderr,
                                "icop: CUDA host unavailable; blocking analyzed frames fail-closed\n");
                    }
                }
            } else if (packed && worker->detector != NULL) {
                if (ClassifyDetector(sys, worker->detector,
                                     worker->rgb_buffer, width, height, 3,
                                     &result) != 0) {
                    inference_failed = true;
                    result.is_nsfw = 1;
                    result.score = 1.0f;
                    result.threshold = sys->threshold;
                    if (MarkBackendFailureLogged(sys)) {
                        fprintf(stderr,
                                "icop: inference failed; blocking analyzed frames fail-closed\n");
                    }
                }
            } else if (packed) {
                result.is_nsfw = 1;
                result.score = 1.0f;
                result.threshold = sys->threshold;
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

        pthread_mutex_lock(&sys->worker_lock);
        if (inference_failed)
            sys->cuda_host_failed = true;
        blocked = sys->cuda_host_failed || slot->blocked ||
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
    bool create_detector_on_thread;
    bool use_cuda_host = false;

    if (!sys || !cfg || sys->detector_classify_checked_fn == NULL)
        return VLC_EGENERIC;

    /* A hardware backend has one shared readback pipeline; serialize it. */
    desired_workers = sys->backend_ops != NULL ? 1 : ResolveWorkerCount();
    if (desired_workers == 0)
        desired_workers = 1;
    create_detector_on_thread = ProviderEnvWantsGpu();
#ifdef _WIN32
    use_cuda_host = ProviderEnvWantsGpu();
    if (use_cuda_host)
        create_detector_on_thread = false;
#endif

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
        worker->config = *cfg;
        worker->create_detector_on_thread = create_detector_on_thread;
        worker->use_cuda_host = use_cuda_host;
#ifdef _WIN32
        /* Optimistically batch until the loaded graph tells us it has a
         * fixed batch dimension. */
        worker->gpu_batch_supported = true;
#endif
        if (cfg->model_path != NULL && cfg->model_path[0] != '\0') {
            size_t length = strlen(cfg->model_path) + 1;
            worker->model_path = (char *)malloc(length);
            if (worker->model_path == NULL)
                continue;
            memcpy(worker->model_path, cfg->model_path, length);
            worker->config.model_path = worker->model_path;
        }

        if (worker->use_cuda_host) {
#ifdef _WIN32
            if (StartWindowsInferenceEndpoint(worker) != 0) {
                fprintf(stderr,
                        "icop: CUDA, DirectML, and CPU inference are unavailable\n");
                free(worker->model_path);
                worker->model_path = NULL;
                continue;
            }
#endif
        } else if (!worker->create_detector_on_thread) {
            worker->detector = sys->detector_create_fn(&worker->config);
            if (worker->detector == NULL) {
                free(worker->model_path);
                worker->model_path = NULL;
                continue;
            }
        }

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
            nsfw_cuda_host_stop(&worker->cuda_host);
            free(worker->model_path);
            worker->model_path = NULL;
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
        nsfw_cuda_host_stop(&worker->cuda_host);
        free(worker->model_path);
        worker->model_path = NULL;
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
        nsfw_cuda_host_stop(&worker->cuda_host);
        free(worker->model_path);
        worker->model_path = NULL;
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
