/*****************************************************************************
 * nsfw_filter_config.c: extracted from nsfw_filter.c
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

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
#include <vlc_plugin.h>
#include <vlc_variables.h>

#include "nsfw_filter.h"
#include "nsfw_filter_internal.h"
#include "platform_abstraction.h"
#include "nsfw_filter_d3d11.h"
#include "frame_processor.h"

static const char *NormalizeRetiredModelProfile(const char *profile);
static bool StringEquals(const char *left, const char *right);
static bool ParseUnsignedEnv(const char *name, unsigned long *value);

static const char *NormalizeRetiredModelProfile(const char *profile)
{
    if (StringEquals(profile, "falconsai-base") ||
        StringEquals(profile, "falconsai-official")) {
        return "falconsai";
    }
    return profile;
}

bool ProviderEnvWantsGpu(void)
{
    const char *provider = getenv("NSFW_ONNX_PROVIDER");
    return provider == NULL ||    /* default is gpu */
           strcmp(provider, "gpu") == 0;
}

static const char *ModelProfileRuntimeFilenameUtf8(nsfw_model_profile_t profile)
{
    switch (profile) {
        case NSFW_MODEL_PROFILE_MARQO:
            return "model.onnx";
        case NSFW_MODEL_PROFILE_ADAMCODD:
            return "adamcodd.onnx";
        case NSFW_MODEL_PROFILE_FALCONSAI:
            return "falconsai.onnx";
        case NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL:
            return "quantized_model.onnx";
        case NSFW_MODEL_PROFILE_FALCONSAI_BASE:
            return "falconsai_base.onnx";
        case NSFW_MODEL_PROFILE_LEGACY:
            return "legacy.onnx";
        default:
            return "model.onnx";
    }
}

bool RuntimeSiblingFileExists(const char *filename)
{
    char path[1024];

    if (filename == NULL || filename[0] == '\0')
        return false;
    if (!nsfw_plat_get_plugin_dir(path, sizeof(path)))
        return false;
    if (strlen(path) + strlen(filename) + 1 >= sizeof(path))
        return false;
    if (snprintf(path + strlen(path), sizeof(path) - strlen(path), "%s",
                 filename) < 0)
        return false;
    return nsfw_plat_file_exists(path);
}

nsfw_model_profile_t ResolveUsableModelProfile(nsfw_model_profile_t preferred)
{
    static const nsfw_model_profile_t fallback_order[] = {
        NSFW_MODEL_PROFILE_MARQO,
        NSFW_MODEL_PROFILE_FALCONSAI,
        NSFW_MODEL_PROFILE_ADAMCODD,
        NSFW_MODEL_PROFILE_LEGACY,
    };
    size_t i;

    if (preferred >= NSFW_MODEL_PROFILE_MARQO &&
        preferred <= NSFW_MODEL_PROFILE_LEGACY) {
        if (RuntimeSiblingFileExists(ModelProfileRuntimeFilenameUtf8(preferred)))
            return preferred;
    }

    for (i = 0; i < ARRAY_SIZE(fallback_order); ++i) {
        if (fallback_order[i] == preferred)
            continue;
        if (RuntimeSiblingFileExists(
                ModelProfileRuntimeFilenameUtf8(fallback_order[i])))
            return fallback_order[i];
    }

    return preferred;
}

void ReleasePicture(picture_t *pic)
{
    typedef void (*picture_release_fn)(picture_t *);
    static picture_release_fn release_fn = NULL;
    static bool loaded = false;

    if (pic == NULL)
        return;

    if (!loaded) {
        release_fn = (picture_release_fn)nsfw_plat_lookup_vlc_sym("picture_Release");
        loaded = true;
    }

    if (release_fn != NULL)
        release_fn(pic);
}

char *DuplicateString(const char *src)
{
    size_t len;
    char *copy;

    if (src == NULL)
        return NULL;

    len = strlen(src) + 1;
    copy = (char *)malloc(len);
    if (copy == NULL)
        return NULL;

    memcpy(copy, src, len);
    return copy;
}
void SetProcessEnvOptionalUnsigned(const char *name, int value)
{
    if (value <= 0) {
        nsfw_plat_set_env(name, "");
        return;
    }

    nsfw_plat_set_env_unsigned(name, (unsigned)value);
}

void ParseVlcFilterOptions(filter_t *filter)
{
    vlc_config_chain_parse_fn chain_parse = NULL;

    if (filter == NULL)
        return;

    if (!LoadVlcOptionAccessors(&chain_parse, NULL, NULL) ||
        chain_parse == NULL) {
        return;
    }

    chain_parse((vlc_object_t *)filter, NSFW_CFG_PREFIX, kNsfwFilterOptions,
                filter->p_cfg);
}

char *GetVlcConfigString(filter_t *filter, const char *name)
{
    vlc_var_create_fn var_create = NULL;
    vlc_var_get_checked_fn var_get_checked = NULL;
    vlc_value_t value;

    if (filter == NULL || name == NULL)
        return NULL;

    if (!LoadVlcOptionAccessors(NULL, &var_create, &var_get_checked) ||
        var_create == NULL || var_get_checked == NULL) {
        return NULL;
    }

    if (var_create((vlc_object_t *)filter, name,
                   VLC_VAR_STRING | VLC_VAR_DOINHERIT |
                   VLC_VAR_ISCOMMAND) != VLC_SUCCESS) {
        return NULL;
    }

    if (var_get_checked((vlc_object_t *)filter, name, VLC_VAR_STRING,
                        &value) != VLC_SUCCESS) {
        return NULL;
    }

    return value.psz_string;
}

int GetVlcConfigInteger(filter_t *filter, const char *name, int fallback)
{
    vlc_var_create_fn var_create = NULL;
    vlc_var_get_checked_fn var_get_checked = NULL;
    int64_t value;
    vlc_value_t result;

    if (filter == NULL || name == NULL)
        return fallback;

    if (!LoadVlcOptionAccessors(NULL, &var_create, &var_get_checked) ||
        var_create == NULL || var_get_checked == NULL) {
        return fallback;
    }

    if (var_create((vlc_object_t *)filter, name,
                   VLC_VAR_INTEGER | VLC_VAR_DOINHERIT |
                   VLC_VAR_ISCOMMAND) != VLC_SUCCESS) {
        return fallback;
    }

    if (var_get_checked((vlc_object_t *)filter, name, VLC_VAR_INTEGER,
                        &result) != VLC_SUCCESS) {
        return fallback;
    }

    value = result.i_int;
    if (value < INT32_MIN)
        return INT32_MIN;
    if (value > INT32_MAX)
        return INT32_MAX;
    return (int)value;
}

float GetVlcConfigFloat(filter_t *filter, const char *name, float fallback)
{
    vlc_var_create_fn var_create = NULL;
    vlc_var_get_checked_fn var_get_checked = NULL;
    vlc_value_t value;

    if (filter == NULL || name == NULL)
        return fallback;

    if (!LoadVlcOptionAccessors(NULL, &var_create, &var_get_checked) ||
        var_create == NULL || var_get_checked == NULL) {
        return fallback;
    }

    if (var_create((vlc_object_t *)filter, name,
                   VLC_VAR_FLOAT | VLC_VAR_DOINHERIT |
                   VLC_VAR_ISCOMMAND) != VLC_SUCCESS) {
        return fallback;
    }

    if (var_get_checked((vlc_object_t *)filter, name, VLC_VAR_FLOAT,
                        &value) != VLC_SUCCESS) {
        return fallback;
    }

    return value.f_float;
}

void SyncVlcOptionsToEnv(filter_t *filter)
{
    char *value;
    const char *model_profile;
    int numeric;

    if (filter == NULL)
        return;

    /* VLC owns these strings and may allocate them with the host CRT.
     * Do not free them from the plugin on Windows. */
    value = GetVlcConfigString(filter, "nsfw-model-profile");
    model_profile = NormalizeRetiredModelProfile(value);
    nsfw_plat_set_env("NSFW_MODEL_PROFILE",
                       model_profile != NULL ? model_profile : "marqo");

    value = GetVlcConfigString(filter, "nsfw-model-path");
    nsfw_plat_set_env("NSFW_MODEL_PATH", value);

    value = GetVlcConfigString(filter, "nsfw-provider");
    nsfw_plat_set_env("NSFW_ONNX_PROVIDER", value != NULL ? value : "cpu");

    numeric = GetVlcConfigInteger(filter, "nsfw-cuda-device-id", 0);
    nsfw_plat_set_env_unsigned("NSFW_ONNX_CUDA_DEVICE_ID", (unsigned)((numeric < 0) ? 0 : numeric));

    numeric = GetVlcConfigInteger(filter, "nsfw-analysis-stride", 0);
    SetProcessEnvOptionalUnsigned("NSFW_ANALYSIS_STRIDE", numeric);

    numeric = GetVlcConfigInteger(filter, "nsfw-block-padding-frames", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BLOCK_PADDING_FRAMES", numeric);

    numeric = GetVlcConfigInteger(filter, "nsfw-buffered-frames", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BUFFERED_FRAMES", numeric);

    numeric = GetVlcConfigInteger(filter, "nsfw-worker-threads", 0);
    SetProcessEnvOptionalUnsigned("NSFW_WORKER_THREADS", numeric);

    numeric = GetVlcConfigInteger(filter, "nsfw-decision-reload-frames", 0);
    SetProcessEnvOptionalUnsigned("NSFW_DECISION_RELOAD_FRAMES", numeric);

    value = GetVlcConfigString(filter, "nsfw-decision-map-path");
    nsfw_plat_set_env("NSFW_DECISION_MAP_PATH", value);

    value = GetVlcConfigString(filter, "nsfw-scan-status-path");
    nsfw_plat_set_env("NSFW_SCAN_STATUS_PATH", value);
}
nsfw_block_style_t ParseBlockStyle(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return NSFW_BLOCK_STYLE_BLACK;

    if (strcmp(text, "blur") == 0)
        return NSFW_BLOCK_STYLE_BLUR;
    if (strcmp(text, "warning") == 0 || strcmp(text, "red") == 0)
        return NSFW_BLOCK_STYLE_WARNING;

    return NSFW_BLOCK_STYLE_BLACK;
}

const char *BlockStyleName(nsfw_block_style_t style)
{
    switch (style) {
        case NSFW_BLOCK_STYLE_BLUR:
            return "blur";
        case NSFW_BLOCK_STYLE_WARNING:
            return "warning";
        case NSFW_BLOCK_STYLE_BLACK:
        default:
            return "black";
    }
}

bool MarkBackendFailureLogged(filter_sys_t *sys)
{
#ifdef _WIN32
    return InterlockedCompareExchange(&sys->backend_failure_logged, 1, 0) == 0;
#else
    if (sys->backend_failure_logged)
        return false;
    sys->backend_failure_logged = true;
    return true;
#endif
}

int EnsureRgbBuffer(filter_sys_t *sys, size_t required)
{
    if (!sys)
        return -1;
    if (required <= sys->rgb_capacity)
        return 0;

    uint8_t *buf = (uint8_t *)realloc(sys->rgb_buffer, required);
    if (!buf)
        return -1;

    sys->rgb_buffer = buf;
    sys->rgb_capacity = required;
    return 0;
}
unsigned DefaultDecisionReloadStride(void)
{
    return 12;
}

unsigned DefaultAnalysisStride(const video_format_t *fmt)
{
    uint64_t pixels;

    if (!fmt)
        return 3;

    pixels = (uint64_t)nsfw_fp_visible_width(fmt) * (uint64_t)nsfw_fp_visible_height(fmt);
    if (pixels >= (uint64_t)3840 * 2160)
        return 6;
    if (pixels >= (uint64_t)2560 * 1440)
        return 5;
    return 3;
}

unsigned ResolveAnalysisStride(const video_format_t *fmt)
{
    unsigned long parsed = 0;

    if (ParseUnsignedEnv("NSFW_ANALYSIS_STRIDE", &parsed)) {
        if (parsed == 0)
            return DefaultAnalysisStride(fmt);
        if (parsed > 32)
            return 32;
        return (unsigned)parsed;
    }

    return DefaultAnalysisStride(fmt);
}

unsigned DefaultBlockPaddingFrames(unsigned analysis_stride)
{
    unsigned padding;

    if (analysis_stride == 0)
        return 0;

    padding = analysis_stride + 2;
    if (padding > 32)
        padding = 32;
    return padding;
}

unsigned ResolveBlockPaddingFrames(unsigned analysis_stride)
{
    unsigned long parsed = 0;

    if (ParseUnsignedEnv("NSFW_BLOCK_PADDING_FRAMES", &parsed)) {
        if (parsed == 0)
            return DefaultBlockPaddingFrames(analysis_stride);
        if (parsed > 32)
            return 32;
        return (unsigned)parsed;
    }

    return DefaultBlockPaddingFrames(analysis_stride);
}

unsigned DefaultPrebufferFrames(const video_format_t *fmt)
{
    uint64_t pixels;

    if (!fmt)
        return 3;

    pixels = (uint64_t)nsfw_fp_visible_width(fmt) * (uint64_t)nsfw_fp_visible_height(fmt);
    if (pixels >= (uint64_t)3840 * 2160)
        return 6;
    if (pixels >= (uint64_t)1920 * 1080)
        return 4;
    return 3;
}

vlc_tick_t EstimatedFrameInterval(const video_format_t *fmt)
{
    if (fmt != NULL &&
        fmt->i_frame_rate > 0 &&
        fmt->i_frame_rate_base > 0) {
        return ((vlc_tick_t)CLOCK_FREQ * fmt->i_frame_rate_base +
                fmt->i_frame_rate - 1) / fmt->i_frame_rate;
    }

    return CLOCK_FREQ / 24;
}

bool ParseUnsignedEnv(const char *name, unsigned long *value)
{
    const char *raw = getenv(name);
    char *end = NULL;

    if (value == NULL || raw == NULL || raw[0] == '\0')
        return false;

    *value = strtoul(raw, &end, 10);
    return end != raw && end != NULL && *end == '\0';
}

unsigned ResolvePrebufferFrames(const video_format_t *fmt)
{
    unsigned fallback = DefaultPrebufferFrames(fmt);
    unsigned long buffer_ms = 0;
    unsigned long buffer_seconds = 0;

    if (fallback == 0)
        fallback = 1;
    if (fallback > NSFW_MAX_BUFFER_FRAMES)
        fallback = NSFW_MAX_BUFFER_FRAMES;

    if (ParseUnsignedEnv("NSFW_BUFFERED_FRAMES", &buffer_ms)) {
        if (buffer_ms == 0)
            return fallback;
        if (buffer_ms > NSFW_MAX_BUFFER_FRAMES)
            return NSFW_MAX_BUFFER_FRAMES;
        return (unsigned)buffer_ms;
    }

    if (ParseUnsignedEnv("NSFW_BUFFER_MS", &buffer_ms) ||
        ParseUnsignedEnv("NSFW_BUFFER_SECONDS", &buffer_seconds)) {
        vlc_tick_t frame_interval = EstimatedFrameInterval(fmt);
        uint64_t target_ticks = buffer_ms > 0
            ? (uint64_t)buffer_ms * CLOCK_FREQ / 1000
            : (uint64_t)buffer_seconds * CLOCK_FREQ;
        uint64_t required_frames;

        if (buffer_ms == 0 && buffer_seconds == 0)
            return fallback;

        if (frame_interval <= 0)
            frame_interval = CLOCK_FREQ / 24;

        required_frames = target_ticks / (uint64_t)frame_interval;
        if ((target_ticks % (uint64_t)frame_interval) != 0)
            required_frames++;
        required_frames++;

        if (required_frames == 0)
            return 1;
        if (required_frames > NSFW_MAX_BUFFER_FRAMES)
            return NSFW_MAX_BUFFER_FRAMES;
        return (unsigned)required_frames;
    }

    return fallback;
}

unsigned ResolveDecisionReloadStride(void)
{
    unsigned long parsed = 0;

    if (ParseUnsignedEnv("NSFW_DECISION_RELOAD_FRAMES", &parsed)) {
        if (parsed == 0)
            return DefaultDecisionReloadStride();
        if (parsed > 240)
            return 240;
        return (unsigned)parsed;
    }

    return DefaultDecisionReloadStride();
}

unsigned DefaultWorkerCount(void)
{
    unsigned workers;

    /*
     * GPU mode uses 1 worker because each worker creates a separate ONNX
     * session which does not scale well on GPU. CPU mode uses the available
     * core count.
     */
    if (ProviderEnvWantsGpu())
        return 1;

    workers = nsfw_plat_cpu_count();
    if (workers == 0)
        workers = 4;
    if (workers > NSFW_MAX_WORKER_THREADS)
        workers = NSFW_MAX_WORKER_THREADS;
    return workers > 0 ? workers : 1;
}

unsigned ResolveWorkerCount(void)
{
    unsigned long parsed = 0;
    unsigned fallback = DefaultWorkerCount();

    if (ParseUnsignedEnv("NSFW_WORKER_THREADS", &parsed)) {
        if (parsed == 0)
            return fallback;
        if (ProviderEnvWantsGpu()) {
            if (parsed > 1) {
                fprintf(stderr,
                        "icop: capped GPU detector workers at 1 (requested %lu) to avoid duplicate GPU sessions\n",
                        parsed);
            }
            return 1;
        }
        if (parsed > NSFW_MAX_WORKER_THREADS)
            return NSFW_MAX_WORKER_THREADS;
        return (unsigned)parsed;
    }

    return fallback;
}

unsigned MinimumPrebufferFrames(unsigned analysis_stride,
                                       unsigned block_padding_frames)
{
    unsigned minimum = 1;
    unsigned padding_window = block_padding_frames * 2 + 1;
    unsigned parallel_window = analysis_stride * 4;

    if (padding_window > minimum)
        minimum = padding_window;
    if (parallel_window > minimum)
        minimum = parallel_window;
    if (minimum > NSFW_MAX_BUFFER_FRAMES)
        minimum = NSFW_MAX_BUFFER_FRAMES;
    return minimum;
}

void ConstrainDecoderQueue(filter_sys_t *sys, unsigned surface_count)
{
    unsigned queue_limit;
    unsigned requested_stride;
    unsigned requested_prebuffer;

    if (sys == NULL)
        return;

    requested_stride = sys->analysis_stride;
    requested_prebuffer = sys->prebuffer_frames;
    queue_limit = surface_count > NSFW_D3D11_RESERVED_DECODER_SURFACES
        ? surface_count - NSFW_D3D11_RESERVED_DECODER_SURFACES
        : 1;
    if (queue_limit > NSFW_D3D11_MAX_BUFFERED_FRAMES)
        queue_limit = NSFW_D3D11_MAX_BUFFERED_FRAMES;
    if (sys->analysis_stride > queue_limit)
        sys->analysis_stride = queue_limit;
    if (sys->prebuffer_frames > queue_limit)
        sys->prebuffer_frames = queue_limit;
    if (sys->prebuffer_frames < sys->analysis_stride)
        sys->prebuffer_frames = sys->analysis_stride;

    if (requested_stride != sys->analysis_stride ||
        requested_prebuffer != sys->prebuffer_frames) {
        fprintf(stderr,
                "icop: capped D3D11 opaque queue at %u frames and analysis stride at %u (decoder surfaces=%u, requested queue=%u stride=%u)\n",
                sys->prebuffer_frames, sys->analysis_stride,
                surface_count, requested_prebuffer, requested_stride);
    }
}

bool IsNullOrEmpty(const char *value)
{
    return value == NULL || value[0] == '\0';
}

bool StringEquals(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}
void PersistModernDefaultSettings(filter_t *filter)
{
    vlc_config_put_psz_fn put_psz = NULL;
    vlc_config_put_int_fn put_int = NULL;
    vlc_config_put_float_fn put_float = NULL;
    vlc_config_save_file_fn save_file = NULL;
    int save_result;

    if (filter == NULL)
        return;

    if (!LoadVlcConfigWriteAccessors(&put_psz, &put_int, &put_float,
                                     &save_file) ||
        put_psz == NULL || put_int == NULL || put_float == NULL ||
        save_file == NULL) {
        fprintf(stderr,
                "icop: applied runtime defaults, but VLC config save accessors are unavailable so the old preset was not rewritten\n");
        return;
    }

    put_psz((vlc_object_t *)filter, "nsfw-model-profile", "marqo");
    put_psz((vlc_object_t *)filter, "nsfw-model-path", "");
    put_psz((vlc_object_t *)filter, "nsfw-provider", "cpu");
    put_psz((vlc_object_t *)filter, "nsfw-block-style", "black");
    put_float((vlc_object_t *)filter, "nsfw-threshold",
              NSFW_DEFAULT_THRESHOLD);
    put_int((vlc_object_t *)filter, "nsfw-mute-audio-on-blocked", 0);
    put_int((vlc_object_t *)filter, "nsfw-analysis-stride", 0);
    put_int((vlc_object_t *)filter, "nsfw-block-padding-frames", 0);
    put_int((vlc_object_t *)filter, "nsfw-buffered-frames", 0);
    put_int((vlc_object_t *)filter, "nsfw-worker-threads", 0);
    put_int((vlc_object_t *)filter, "nsfw-cuda-device-id", 0);
    put_int((vlc_object_t *)filter, "nsfw-decision-reload-frames", 0);
    put_psz((vlc_object_t *)filter, "nsfw-decision-map-path", "");
    put_psz((vlc_object_t *)filter, "nsfw-scan-status-path", "");
    put_int((vlc_object_t *)filter, "nsfw-debug-overlay", 0);
    put_int((vlc_object_t *)filter, "nsfw-settings-version",
            NSFW_SETTINGS_VERSION_CURRENT);

    save_result = save_file((vlc_object_t *)filter);
    if (save_result == VLC_SUCCESS) {
        fprintf(stderr,
                "icop: rewrote the older saved preset to current defaults\n");
    } else {
        fprintf(stderr,
                "icop: applied runtime defaults, but failed to save the updated preset (error %d)\n",
                save_result);
    }
}

void MaybeReplaceLegacyPreset(filter_t *filter)
{
    filter_sys_t *sys;
    char *model_profile;
    char *provider;
    char *block_style;
    char *decision_map_path;
    char *scan_status_path;
    int settings_version;
    int analysis_stride;
    int block_padding_frames;
    int buffered_frames;
    int worker_threads;
    int decision_reload_frames;
    int mute_audio_on_blocked;
    float threshold;

    if (filter == NULL || filter->p_sys == NULL)
        return;

    settings_version = GetVlcConfigInteger(filter, "nsfw-settings-version", 0);
    if (settings_version >= NSFW_SETTINGS_VERSION_CURRENT)
        return;

    model_profile = GetVlcConfigString(filter, "nsfw-model-profile");
    provider = GetVlcConfigString(filter, "nsfw-provider");
    block_style = GetVlcConfigString(filter, "nsfw-block-style");
    decision_map_path = GetVlcConfigString(filter, "nsfw-decision-map-path");
    scan_status_path = GetVlcConfigString(filter, "nsfw-scan-status-path");
    threshold = GetVlcConfigFloat(filter, "nsfw-threshold",
                                  NSFW_DEFAULT_THRESHOLD);
    mute_audio_on_blocked =
        GetVlcConfigInteger(filter, "nsfw-mute-audio-on-blocked", 0);
    analysis_stride = GetVlcConfigInteger(filter, "nsfw-analysis-stride", 0);
    block_padding_frames =
        GetVlcConfigInteger(filter, "nsfw-block-padding-frames", 0);
    buffered_frames = GetVlcConfigInteger(filter, "nsfw-buffered-frames", 0);
    worker_threads = GetVlcConfigInteger(filter, "nsfw-worker-threads", 0);
    decision_reload_frames =
        GetVlcConfigInteger(filter, "nsfw-decision-reload-frames", 0);

    if (!StringEquals(model_profile, "legacy") ||
        !StringEquals(provider, "auto") ||
        !StringEquals(block_style, "blur") ||
        threshold < 0.49f || threshold > 0.51f ||
        mute_audio_on_blocked != 0 ||
        analysis_stride != 5 ||
        block_padding_frames != 5 ||
        buffered_frames != NSFW_MAX_BUFFER_FRAMES ||
        worker_threads != 4 ||
        decision_reload_frames != 0 ||
        !IsNullOrEmpty(decision_map_path) ||
        !IsNullOrEmpty(scan_status_path)) {
        return;
    }

    sys = filter->p_sys;
    sys->threshold = NSFW_DEFAULT_THRESHOLD;
    sys->block_style = NSFW_BLOCK_STYLE_BLACK;
    sys->mute_audio_on_blocked = false;
    sys->analysis_stride = DefaultAnalysisStride(&filter->fmt_in.video);
    sys->decision_reload_stride = DefaultDecisionReloadStride();
    sys->prebuffer_frames = DefaultPrebufferFrames(&filter->fmt_in.video);
    sys->block_padding_frames =
        DefaultBlockPaddingFrames(sys->analysis_stride);
    if (sys->prebuffer_frames <
        MinimumPrebufferFrames(sys->analysis_stride,
                               sys->block_padding_frames)) {
        sys->prebuffer_frames =
            MinimumPrebufferFrames(sys->analysis_stride,
                                   sys->block_padding_frames);
    }

    nsfw_plat_set_env("NSFW_MODEL_PROFILE", "marqo");
    nsfw_plat_set_env("NSFW_MODEL_PATH", "");
    nsfw_plat_set_env("NSFW_ONNX_PROVIDER", "cpu");
    SetProcessEnvOptionalUnsigned("NSFW_ANALYSIS_STRIDE", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BLOCK_PADDING_FRAMES", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BUFFERED_FRAMES", 0);
    SetProcessEnvOptionalUnsigned("NSFW_WORKER_THREADS", 0);
    SetProcessEnvOptionalUnsigned("NSFW_DECISION_RELOAD_FRAMES", 0);
    nsfw_plat_set_env("NSFW_DECISION_MAP_PATH", "");
    nsfw_plat_set_env("NSFW_SCAN_STATUS_PATH", "");

    fprintf(stderr,
            "icop: replaced an older saved legacy preset with current defaults (marqo, black, automatic buffering)\n");
    PersistModernDefaultSettings(filter);
}
