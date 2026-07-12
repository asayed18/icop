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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
# include <wchar.h>
#endif

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_input.h>
#include <vlc_aout.h>
#include <vlc_picture.h>
#include <vlc_plugin.h>
#include <vlc_fourcc.h>
#include <vlc_variables.h>

#include "nsfw_filter.h"

/*****************************************************************************
 * Worker state
 *****************************************************************************/

struct nsfw_worker_state_t {
#ifdef _WIN32
    HANDLE          thread;
#endif
    filter_sys_t   *sys;
    nsfw_detector_t *detector;
    uint8_t        *rgb_buffer;
    size_t          rgb_capacity;
    bool            running;
    bool            stop;
};

/*****************************************************************************
 * Module option labels
 *****************************************************************************/

static const char *const kModelProfileValues[] = {
    "marqo",
    "adamcodd",
    "falconsai",
    "falconsai-base",
    "falconsai-official",
    "legacy",
};

static const char *const kModelProfileLabels[] = {
    "Marqo / nsfw-image-detection-384",
    "AdamCodd / vit-base-nsfw-detector",
    "Falconsai / nsfw_image_detection",
    "Falconsai / nsfw_image_detection (base ONNX)",
    "Falconsai / nsfw_image_detection_26",
    "Legacy / GantMan",
};

    static const char *const kProviderValues[] = {
        "auto",
        "cpu",
        "cuda",
    };

static const char *const kProviderLabels[] = {
    "Auto",
    "CPU",
    "CUDA",
};

static const char *const kBlockStyleValues[] = {
    "black",
    "blur",
    "warning",
};

static const char *const kBlockStyleLabels[] = {
    "Black out",
    "Blur",
    "Red warning",
};

#define NSFW_CFG_PREFIX "nsfw-"
#define NSFW_SETTINGS_VERSION_CURRENT 1

static const char *const kNsfwFilterOptions[] = {
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
    "settings-version",
    NULL
};

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open(vlc_object_t *);
static void Close(vlc_object_t *);
static void Flush(filter_t *);
static picture_t *Filter(filter_t *, picture_t *);
#ifdef _WIN32
static unsigned __stdcall DetectorWorkerThread(void *);
#endif

vlc_module_begin()
    set_description("NSFW content filter")
    set_shortname("NSFW Filter")
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
    add_string("nsfw-provider", "cpu",
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
    add_integer("nsfw-settings-version", 0,
                "Settings version",
                "Internal version for one-time defaults migration.", false)
        change_integer_range(0, NSFW_SETTINGS_VERSION_CURRENT)
        change_private()
    add_shortcut("nsfw")
vlc_module_end()

/*****************************************************************************
 * Runtime-loaded core helpers
 *****************************************************************************/

#ifdef _WIN32

#define NSFW_CORE_DLL_NAME L"nsfw_filter_core.dll"

static bool GetPluginDirectory(wchar_t *path, DWORD path_capacity)
{
    HMODULE module = NULL;
    wchar_t *slash = NULL;

    if (!path || path_capacity == 0)
        return false;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&GetPluginDirectory, &module)) {
        return false;
    }

    if (GetModuleFileNameW(module, path, path_capacity) == 0)
        return false;

    slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;

    slash[1] = L'\0';
    return true;
}

static void ReleasePicture(picture_t *pic)
{
    typedef void (*picture_release_fn)(picture_t *);
    static picture_release_fn release_fn = NULL;
    static bool loaded = false;

    if (pic == NULL)
        return;

    if (!loaded) {
        HMODULE core = GetModuleHandleW(L"libvlccore.dll");

        if (core != NULL) {
            release_fn = (picture_release_fn)GetProcAddress(core,
                                                            "picture_Release");
        }
        loaded = true;
    }

    if (release_fn != NULL)
        release_fn(pic);
}

static char *DuplicateString(const char *src)
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

static uint64_t FileSignature(const char *path)
{
    struct _stat64 st;

    if (path == NULL || path[0] == '\0')
        return 0;
    if (_stat64(path, &st) != 0)
        return 0;

    return ((uint64_t)(uint32_t)st.st_mtime << 32) ^
           (uint64_t)(uint32_t)(st.st_size & 0xffffffffu);
}

static bool LoadCoreModule(filter_sys_t *sys)
{
    wchar_t path[MAX_PATH];

    if (!sys || sys->core_module != NULL)
        return sys && sys->core_module != NULL;

    if (!GetPluginDirectory(path, MAX_PATH))
        return false;

    if (wcslen(path) + wcslen(NSFW_CORE_DLL_NAME) + 1 > MAX_PATH)
        return false;
    if (wcscat_s(path, MAX_PATH, NSFW_CORE_DLL_NAME) != 0)
        return false;

    sys->core_module = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!sys->core_module)
        return false;

    sys->config_default_fn = (nsfw_config_t (*)(void))GetProcAddress(
        (HMODULE)sys->core_module, "nsfw_config_default");
    sys->model_profile_name_fn = (const char *(*)(nsfw_model_profile_t))
        GetProcAddress((HMODULE)sys->core_module, "nsfw_model_profile_name");
    sys->model_profile_parse_fn = (int (*)(const char *,
                                           nsfw_model_profile_t *))
        GetProcAddress((HMODULE)sys->core_module, "nsfw_model_profile_parse");
    sys->config_set_model_profile_fn = (void (*)(nsfw_config_t *,
                                                 nsfw_model_profile_t))
        GetProcAddress((HMODULE)sys->core_module,
                       "nsfw_config_set_model_profile");
    sys->detector_create_fn = (nsfw_detector_t *(*)(const nsfw_config_t *))
        GetProcAddress((HMODULE)sys->core_module, "nsfw_detector_create");
    sys->detector_destroy_fn = (void (*)(nsfw_detector_t *))
        GetProcAddress((HMODULE)sys->core_module, "nsfw_detector_destroy");
    sys->detector_classify_fn = (nsfw_result_t (*)(nsfw_detector_t *,
                                                   const uint8_t *,
                                                   int, int, int))
        GetProcAddress((HMODULE)sys->core_module, "nsfw_detector_classify");

    if (!sys->config_default_fn || !sys->model_profile_name_fn ||
        !sys->model_profile_parse_fn || !sys->config_set_model_profile_fn ||
        !sys->detector_create_fn ||
        !sys->detector_destroy_fn || !sys->detector_classify_fn) {
        FreeLibrary((HMODULE)sys->core_module);
        sys->core_module = NULL;
        sys->config_default_fn = NULL;
        sys->model_profile_name_fn = NULL;
        sys->model_profile_parse_fn = NULL;
        sys->config_set_model_profile_fn = NULL;
        sys->detector_create_fn = NULL;
        sys->detector_destroy_fn = NULL;
        sys->detector_classify_fn = NULL;
        return false;
    }

    return true;
}

static void UnloadCoreModule(filter_sys_t *sys)
{
    if (!sys)
        return;

    if (sys->core_module != NULL)
        FreeLibrary((HMODULE)sys->core_module);

    sys->core_module = NULL;
    sys->config_default_fn = NULL;
    sys->model_profile_name_fn = NULL;
    sys->model_profile_parse_fn = NULL;
    sys->config_set_model_profile_fn = NULL;
    sys->detector_create_fn = NULL;
    sys->detector_destroy_fn = NULL;
    sys->detector_classify_fn = NULL;
}

#else

static bool LoadCoreModule(filter_sys_t *sys)
{
    VLC_UNUSED(sys);
    return false;
}

static void UnloadCoreModule(filter_sys_t *sys)
{
    VLC_UNUSED(sys);
}

#endif

/*****************************************************************************
 * VLC option helpers
 *****************************************************************************/

static void SetProcessEnvValue(const char *name, const char *value)
{
    if (name == NULL)
        return;

#ifdef _WIN32
    _putenv_s(name, value != NULL ? value : "");
#else
    if (value == NULL)
        value = "";
    setenv(name, value, 1);
#endif
}

static void SetProcessEnvUnsigned(const char *name, unsigned value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%u", value);
    SetProcessEnvValue(name, buffer);
}

static void SetProcessEnvOptionalUnsigned(const char *name, int value)
{
    if (value <= 0) {
        SetProcessEnvValue(name, "");
        return;
    }

    SetProcessEnvUnsigned(name, (unsigned)value);
}

typedef void (*vlc_config_chain_parse_fn)(vlc_object_t *, const char *,
                                          const char *const *,
                                          config_chain_t *);
typedef int (*vlc_var_create_fn)(vlc_object_t *, const char *, int);
typedef int (*vlc_var_get_checked_fn)(vlc_object_t *, const char *, int,
                                      vlc_value_t *);
typedef void (*vlc_config_put_psz_fn)(vlc_object_t *, const char *,
                                      const char *);
typedef void (*vlc_config_put_int_fn)(vlc_object_t *, const char *, int64_t);
typedef void (*vlc_config_put_float_fn)(vlc_object_t *, const char *, float);
typedef int (*vlc_config_save_file_fn)(vlc_object_t *);

static bool LoadVlcOptionAccessors(vlc_config_chain_parse_fn *chain_parse,
                                   vlc_var_create_fn *var_create,
                                   vlc_var_get_checked_fn *var_get_checked)
{
    static bool loaded = false;
    static vlc_config_chain_parse_fn cached_chain_parse = NULL;
    static vlc_var_create_fn cached_var_create = NULL;
    static vlc_var_get_checked_fn cached_var_get_checked = NULL;
    HMODULE core = NULL;

    if (!loaded) {
        core = GetModuleHandleW(L"libvlccore.dll");
        if (core != NULL) {
            cached_chain_parse = (vlc_config_chain_parse_fn)GetProcAddress(
                core, "config_ChainParse");
            cached_var_create = (vlc_var_create_fn)GetProcAddress(
                core, "var_Create");
            cached_var_get_checked = (vlc_var_get_checked_fn)GetProcAddress(
                core, "var_GetChecked");
        }
        loaded = true;
    }

    if (chain_parse != NULL)
        *chain_parse = cached_chain_parse;
    if (var_create != NULL)
        *var_create = cached_var_create;
    if (var_get_checked != NULL)
        *var_get_checked = cached_var_get_checked;

    return cached_chain_parse != NULL &&
           cached_var_create != NULL &&
           cached_var_get_checked != NULL;
}

static bool LoadVlcConfigWriteAccessors(vlc_config_put_psz_fn *put_psz,
                                        vlc_config_put_int_fn *put_int,
                                        vlc_config_put_float_fn *put_float,
                                        vlc_config_save_file_fn *save_file)
{
    static bool loaded = false;
    static vlc_config_put_psz_fn cached_put_psz = NULL;
    static vlc_config_put_int_fn cached_put_int = NULL;
    static vlc_config_put_float_fn cached_put_float = NULL;
    static vlc_config_save_file_fn cached_save_file = NULL;
    HMODULE core = NULL;

    if (!loaded) {
        core = GetModuleHandleW(L"libvlccore.dll");
        if (core != NULL) {
            cached_put_psz = (vlc_config_put_psz_fn)GetProcAddress(
                core, "config_PutPsz");
            cached_put_int = (vlc_config_put_int_fn)GetProcAddress(
                core, "config_PutInt");
            cached_put_float = (vlc_config_put_float_fn)GetProcAddress(
                core, "config_PutFloat");
            cached_save_file = (vlc_config_save_file_fn)GetProcAddress(
                core, "config_SaveConfigFile");
        }
        loaded = true;
    }

    if (put_psz != NULL)
        *put_psz = cached_put_psz;
    if (put_int != NULL)
        *put_int = cached_put_int;
    if (put_float != NULL)
        *put_float = cached_put_float;
    if (save_file != NULL)
        *save_file = cached_save_file;

    return cached_put_psz != NULL &&
           cached_put_int != NULL &&
           cached_put_float != NULL &&
           cached_save_file != NULL;
}

static void ParseVlcFilterOptions(filter_t *filter)
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

static char *GetVlcConfigString(filter_t *filter, const char *name)
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

static int GetVlcConfigInteger(filter_t *filter, const char *name, int fallback)
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

static float GetVlcConfigFloat(filter_t *filter, const char *name, float fallback)
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

static void SyncVlcOptionsToEnv(filter_t *filter)
{
    char *value;
    int numeric;

    if (filter == NULL)
        return;

    /* VLC owns these strings and may allocate them with the host CRT.
     * Do not free them from the plugin on Windows. */
    value = GetVlcConfigString(filter, "nsfw-model-profile");
    SetProcessEnvValue("NSFW_MODEL_PROFILE", value != NULL ? value : "marqo");

    value = GetVlcConfigString(filter, "nsfw-model-path");
    SetProcessEnvValue("NSFW_MODEL_PATH", value);

    value = GetVlcConfigString(filter, "nsfw-provider");
    SetProcessEnvValue("NSFW_ONNX_PROVIDER", value != NULL ? value : "cpu");

    numeric = GetVlcConfigInteger(filter, "nsfw-cuda-device-id", 0);
    SetProcessEnvUnsigned("NSFW_ONNX_CUDA_DEVICE_ID", (unsigned)((numeric < 0) ? 0 : numeric));

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
    SetProcessEnvValue("NSFW_DECISION_MAP_PATH", value);

    value = GetVlcConfigString(filter, "nsfw-scan-status-path");
    SetProcessEnvValue("NSFW_SCAN_STATUS_PATH", value);
}

static nsfw_block_style_t ParseBlockStyle(const char *text)
{
    if (text == NULL || text[0] == '\0')
        return NSFW_BLOCK_STYLE_BLACK;

    if (strcmp(text, "blur") == 0)
        return NSFW_BLOCK_STYLE_BLUR;
    if (strcmp(text, "warning") == 0 || strcmp(text, "red") == 0)
        return NSFW_BLOCK_STYLE_WARNING;

    return NSFW_BLOCK_STYLE_BLACK;
}

static const char *BlockStyleName(nsfw_block_style_t style)
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

static void FourccToString(vlc_fourcc_t chroma, char out[5])
{
    unsigned i;

    if (out == NULL)
        return;

    out[0] = (char)(chroma & 0xFFu);
    out[1] = (char)((chroma >> 8) & 0xFFu);
    out[2] = (char)((chroma >> 16) & 0xFFu);
    out[3] = (char)((chroma >> 24) & 0xFFu);
    out[4] = '\0';

    for (i = 0; i < 4; ++i) {
        if (!isprint((unsigned char)out[i]))
            out[i] = '.';
    }
}

typedef int (*vlc_input_control_fn)(input_thread_t *, int, ...);
typedef int (*vlc_aout_mute_get_fn)(audio_output_t *);
typedef int (*vlc_aout_mute_set_fn)(audio_output_t *, bool);
typedef void (*vlc_object_release_fn)(vlc_object_t *);

static bool LoadVlcPlaybackAccessors(vlc_input_control_fn *input_control,
                                     vlc_aout_mute_get_fn *mute_get,
                                     vlc_aout_mute_set_fn *mute_set,
                                     vlc_object_release_fn *object_release)
{
    static bool loaded = false;
    static vlc_input_control_fn cached_input_control = NULL;
    static vlc_aout_mute_get_fn cached_mute_get = NULL;
    static vlc_aout_mute_set_fn cached_mute_set = NULL;
    static vlc_object_release_fn cached_object_release = NULL;
    HMODULE core = NULL;

    if (!loaded) {
        core = GetModuleHandleW(L"libvlccore.dll");
        if (core != NULL) {
            cached_input_control = (vlc_input_control_fn)GetProcAddress(core, "input_Control");
            cached_mute_get = (vlc_aout_mute_get_fn)GetProcAddress(core, "aout_MuteGet");
            cached_mute_set = (vlc_aout_mute_set_fn)GetProcAddress(core, "aout_MuteSet");
            cached_object_release = (vlc_object_release_fn)GetProcAddress(core, "vlc_object_release");
        }
        loaded = true;
    }

    if (input_control)
        *input_control = cached_input_control;
    if (mute_get)
        *mute_get = cached_mute_get;
    if (mute_set)
        *mute_set = cached_mute_set;
    if (object_release)
        *object_release = cached_object_release;

    return cached_input_control != NULL &&
           cached_mute_get != NULL &&
           cached_mute_set != NULL &&
           cached_object_release != NULL;
}

static input_thread_t *FindInputThread(vlc_object_t *obj)
{
    while (obj != NULL) {
        const struct vlc_common_members *members =
            (const struct vlc_common_members *)obj;

        if (members->object_type != NULL &&
            strncmp(members->object_type, "input", 5) == 0) {
            return (input_thread_t *)obj;
        }
        obj = members->parent;
    }

    return NULL;
}

static void SetAudioMutedForBlockedFrame(filter_t *filter, bool blocked)
{
    filter_sys_t *sys;
    input_thread_t *input;
    audio_output_t *aout;
    int current_mute;
    vlc_input_control_fn input_control = NULL;
    vlc_aout_mute_get_fn mute_get = NULL;
    vlc_aout_mute_set_fn mute_set = NULL;
    vlc_object_release_fn object_release = NULL;

    if (filter == NULL || filter->p_sys == NULL)
        return;

    sys = filter->p_sys;
    if (!sys->mute_audio_on_blocked)
        return;

    if (blocked && sys->audio_muted_by_filter)
        return;
    if (!blocked && !sys->audio_muted_by_filter)
        return;

    input = FindInputThread((vlc_object_t *)filter);
    if (input == NULL) {
        if (blocked && !sys->audio_mute_warning_logged) {
            fprintf(stderr,
                    "nsfw_filter: could not locate the current VLC input to mute audio\n");
            sys->audio_mute_warning_logged = true;
        }
        return;
    }

    if (!LoadVlcPlaybackAccessors(&input_control, &mute_get, &mute_set, &object_release)) {
        if (blocked && !sys->audio_mute_warning_logged) {
            fprintf(stderr,
                    "nsfw_filter: VLC playback accessors are unavailable for mute control\n");
            sys->audio_mute_warning_logged = true;
        }
        return;
    }

    if (input_control(input, INPUT_GET_AOUT, &aout) != VLC_SUCCESS || aout == NULL) {
        if (blocked && !sys->audio_mute_warning_logged) {
            fprintf(stderr,
                    "nsfw_filter: could not access the VLC audio output for mute control\n");
            sys->audio_mute_warning_logged = true;
        }
        return;
    }

    current_mute = mute_get(aout);
    if (blocked) {
        sys->audio_previous_mute = current_mute > 0;
        sys->audio_muted_by_filter = true;
        if (current_mute == 0)
            mute_set(aout, true);
    } else {
        if (!sys->audio_previous_mute)
            mute_set(aout, false);
        sys->audio_muted_by_filter = false;
        sys->audio_previous_mute = false;
    }

    object_release((vlc_object_t *)aout);
}

/*****************************************************************************
 * Heuristic scoring helpers
 *****************************************************************************/

typedef struct
{
    bool little_endian;
    unsigned bits;
    unsigned storage_shift;
} nsfw_sample16_desc_t;

static inline int VisibleWidth(const video_format_t *fmt)
{
    return fmt->i_visible_width > 0 ? fmt->i_visible_width : fmt->i_width;
}

static inline int VisibleHeight(const video_format_t *fmt)
{
    return fmt->i_visible_height > 0 ? fmt->i_visible_height : fmt->i_height;
}

static inline int ClampDimension(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

static bool IsOpaqueHardwareChroma(vlc_fourcc_t chroma)
{
    switch (chroma) {
        case VLC_CODEC_D3D9_OPAQUE:
        case VLC_CODEC_D3D9_OPAQUE_10B:
        case VLC_CODEC_D3D11_OPAQUE:
        case VLC_CODEC_D3D11_OPAQUE_10B:
        case VLC_CODEC_VAAPI_420:
        case VLC_CODEC_VAAPI_420_10BPP:
        case VLC_CODEC_CVPX_NV12:
        case VLC_CODEC_CVPX_I420:
        case VLC_CODEC_CVPX_BGRA:
        case VLC_CODEC_CVPX_P010:
            return true;
        default:
            return false;
    }
}

static inline unsigned SampleMask(unsigned bits)
{
    if (bits >= 16)
        return 0xFFFFu;
    return (1u << bits) - 1u;
}

static inline unsigned NeutralChromaSample(unsigned bits)
{
    if (bits == 0)
        return 0;
    return 1u << (bits - 1u);
}

static inline uint16_t ReadWord16(const uint8_t *ptr, bool little_endian)
{
    if (little_endian)
        return (uint16_t)(ptr[0] | ((uint16_t)ptr[1] << 8));
    return (uint16_t)(((uint16_t)ptr[0] << 8) | ptr[1]);
}

static inline void WriteWord16(uint8_t *ptr, uint16_t value, bool little_endian)
{
    if (little_endian) {
        ptr[0] = (uint8_t)(value & 0xFFu);
        ptr[1] = (uint8_t)(value >> 8);
    } else {
        ptr[0] = (uint8_t)(value >> 8);
        ptr[1] = (uint8_t)(value & 0xFFu);
    }
}

static inline unsigned DecodeSample16(const uint8_t *ptr, nsfw_sample16_desc_t desc)
{
    unsigned value = ReadWord16(ptr, desc.little_endian);
    value >>= desc.storage_shift;
    value &= SampleMask(desc.bits);
    return value;
}

static inline uint16_t EncodeSample16(unsigned sample, nsfw_sample16_desc_t desc)
{
    unsigned value = sample & SampleMask(desc.bits);
    return (uint16_t)(value << desc.storage_shift);
}

static inline uint8_t ScaleSampleToByte(unsigned sample, unsigned bits)
{
    unsigned max_value = SampleMask(bits);

    if (max_value == 0)
        return 0;

    return (uint8_t)((sample * 255u + (max_value / 2u)) / max_value);
}

static inline uint8_t DecodeSample16ToByte(const uint8_t *ptr,
                                           nsfw_sample16_desc_t desc)
{
    return ScaleSampleToByte(DecodeSample16(ptr, desc), desc.bits);
}

static bool GetPlanar16Layout(vlc_fourcc_t chroma,
                              nsfw_sample16_desc_t *desc,
                              bool *swap_uv,
                              unsigned *u_step_x,
                              unsigned *u_step_y,
                              bool *has_alpha)
{
    if (!desc || !swap_uv || !u_step_x || !u_step_y || !has_alpha)
        return false;

    *swap_uv = false;
    *has_alpha = false;

    switch (chroma) {
        case VLC_CODEC_I420_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_16L:
            *desc = (nsfw_sample16_desc_t){ true, 16, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I420_16B:
            *desc = (nsfw_sample16_desc_t){ false, 16, 0 };
            *u_step_x = 1;
            *u_step_y = 1;
            return true;
        case VLC_CODEC_I422_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I422_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I422_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I422_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I422_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I422_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 1;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_16L:
            *desc = (nsfw_sample16_desc_t){ true, 16, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_I444_16B:
            *desc = (nsfw_sample16_desc_t){ false, 16, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            return true;
        case VLC_CODEC_YUVA_444_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            *has_alpha = true;
            return true;
        case VLC_CODEC_YUVA_444_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 0;
            *u_step_y = 0;
            *has_alpha = true;
            return true;
        default:
            return false;
    }
}

static inline bool IsSkinYCbCr(unsigned y, unsigned cb, unsigned cr)
{
    return y > 80 && cb >= 85 && cb <= 135 && cr >= 135 && cr <= 180;
}

static float ScorePlanarYCbCr(const picture_t *pic, bool swap_uv,
                              unsigned u_step_x, unsigned u_step_y)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];

    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < y_plane->i_visible_lines; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < y_plane->i_visible_pitch; x += 2) {
            unsigned yy = y_row[x];
            unsigned uu = u_row[x >> u_step_x];
            unsigned vv = v_row[x >> u_step_x];

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScorePlanarYCbCr16(const filter_t *p_filter, const picture_t *pic,
                                bool swap_uv, unsigned u_step_x,
                                unsigned u_step_y, nsfw_sample16_desc_t desc)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];
    const int width = VisibleWidth(&p_filter->fmt_in.video);
    const int height = VisibleHeight(&p_filter->fmt_in.video);

    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)x * 2), desc);
            unsigned uu = DecodeSample16ToByte(
                u_row + ((size_t)(x >> u_step_x) * 2), desc);
            unsigned vv = DecodeSample16ToByte(
                v_row + ((size_t)(x >> u_step_x) * 2), desc);

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScoreP010(const filter_t *p_filter, const picture_t *pic)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const int width = VisibleWidth(&pic->format);
    const int height = VisibleHeight(&pic->format);
    const nsfw_sample16_desc_t desc = { true, 10, 6 };

    unsigned hits = 0;
    unsigned samples = 0;

    VLC_UNUSED(p_filter);

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            size_t chroma = (size_t)(x >> 1) * 4;
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)x * 2), desc);
            unsigned uu = DecodeSample16ToByte(uv_row + chroma, desc);
            unsigned vv = DecodeSample16ToByte(uv_row + chroma + 2, desc);

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScoreSemiPlanarYCbCr(const picture_t *pic, bool swap_uv)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const int width = pic->p[Y_PLANE].i_visible_pitch;
    const int height = pic->p[Y_PLANE].i_visible_lines;
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            size_t chroma = (size_t)(x >> 1) * 2;
            unsigned yy = y_row[x];
            unsigned uu = uv_row[chroma + (swap_uv ? 1 : 0)];
            unsigned vv = uv_row[chroma + (swap_uv ? 0 : 1)];

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScorePackedRGB(const picture_t *pic, int pixel_size,
                            int r_offset, int g_offset, int b_offset)
{
    const plane_t *plane = &pic->p[0];
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < plane->i_visible_lines; y += 2) {
        const uint8_t *row = plane->p_pixels + y * plane->i_pitch;
        for (int x = 0; x < plane->i_visible_pitch; x += pixel_size * 2) {
            const uint8_t *px = row + x;
            unsigned r = px[r_offset];
            unsigned g = px[g_offset];
            unsigned b = px[b_offset];

            unsigned yy = (77u * r + 150u * g + 29u * b) >> 8;
            unsigned cb = 128u + ((-43 * (int)r - 85 * (int)g + 128 * (int)b) >> 8);
            unsigned cr = 128u + ((128 * (int)r - 107 * (int)g - 21 * (int)b) >> 8);

            if (IsSkinYCbCr(yy, cb, cr))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScoreFrame(const filter_t *p_filter, const picture_t *pic)
{
    vlc_fourcc_t chroma;
    nsfw_sample16_desc_t desc;
    bool swap_uv;
    unsigned u_step_x;
    unsigned u_step_y;
    bool has_alpha;

    VLC_UNUSED(p_filter);
    chroma = pic->format.i_chroma;

    if (GetPlanar16Layout(chroma, &desc, &swap_uv,
                          &u_step_x, &u_step_y, &has_alpha)) {
        return ScorePlanarYCbCr16(p_filter, pic, swap_uv, u_step_x, u_step_y,
                                  desc);
    }

    switch (chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
            return ScorePlanarYCbCr(pic, false, 1, 1);
        case VLC_CODEC_YV12:
            return ScorePlanarYCbCr(pic, true, 1, 1);
        case VLC_CODEC_I422:
            return ScorePlanarYCbCr(pic, false, 1, 0);
        case VLC_CODEC_I444:
            return ScorePlanarYCbCr(pic, false, 0, 0);
        case VLC_CODEC_YUVA:
            return ScorePlanarYCbCr(pic, false, 1, 1);
        case VLC_CODEC_RGB24:
            return ScorePackedRGB(pic, 3, 2, 1, 0);
        case VLC_CODEC_RGB32:
            return ScorePackedRGB(pic, 4, 2, 1, 0);
        case VLC_CODEC_RGBA:
            return ScorePackedRGB(pic, 4, 0, 1, 2);
        case VLC_CODEC_ARGB:
            return ScorePackedRGB(pic, 4, 1, 2, 3);
        case VLC_CODEC_BGRA:
            return ScorePackedRGB(pic, 4, 2, 1, 0);
        case VLC_CODEC_NV12:
            return ScoreSemiPlanarYCbCr(pic, false);
        case VLC_CODEC_NV21:
            return ScoreSemiPlanarYCbCr(pic, true);
        case VLC_CODEC_P010:
            return ScoreP010(p_filter, pic);
        default:
            return 0.0f;
    }
}

static void BlackoutPlane(plane_t *plane, uint8_t value)
{
    for (int y = 0; y < plane->i_visible_lines; y++) {
        memset(plane->p_pixels + y * plane->i_pitch, value,
               (size_t)plane->i_visible_pitch);
    }
}

static void BlackoutPlane16(plane_t *plane, uint16_t value, bool little_endian)
{
    for (int y = 0; y < plane->i_visible_lines; y++) {
        uint8_t *row = plane->p_pixels + y * plane->i_pitch;
        for (int x = 0; x + 1 < plane->i_visible_pitch; x += 2)
            WriteWord16(row + x, value, little_endian);
    }
}

static void BlackoutPlanarFrame16(picture_t *pic, nsfw_sample16_desc_t desc,
                                  bool has_alpha)
{
    uint16_t chroma = EncodeSample16(NeutralChromaSample(desc.bits), desc);

    BlackoutPlane16(&pic->p[Y_PLANE], EncodeSample16(0, desc), desc.little_endian);
    BlackoutPlane16(&pic->p[U_PLANE], chroma, desc.little_endian);
    BlackoutPlane16(&pic->p[V_PLANE], chroma, desc.little_endian);

    if (has_alpha) {
        BlackoutPlane16(&pic->p[A_PLANE],
                        EncodeSample16(SampleMask(desc.bits), desc),
                        desc.little_endian);
    }
}

static void BlackoutP010Frame(picture_t *pic)
{
    const nsfw_sample16_desc_t desc = { true, 10, 6 };
    uint16_t chroma = EncodeSample16(NeutralChromaSample(desc.bits), desc);

    BlackoutPlane16(&pic->p[Y_PLANE], EncodeSample16(0, desc), true);
    BlackoutPlane16(&pic->p[U_PLANE], chroma, true);
}

static void BlackoutSemiPlanarFrame(picture_t *pic)
{
    BlackoutPlane(&pic->p[Y_PLANE], 0x00);

    for (int y = 0; y < pic->p[U_PLANE].i_visible_lines; y++) {
        memset(pic->p[U_PLANE].p_pixels + y * pic->p[U_PLANE].i_pitch, 0x80,
               (size_t)pic->p[U_PLANE].i_visible_pitch);
    }
}

static void BlurInterleavedPlane8(plane_t *plane, unsigned pixel_stride,
                                  const int *channel_offsets,
                                  unsigned channel_count, unsigned radius)
{
    uint8_t *tmp;
    int width;
    int height;
    int pitch;
    unsigned window;

    if (plane == NULL || plane->p_pixels == NULL || pixel_stride == 0 ||
        channel_offsets == NULL || channel_count == 0 || radius == 0) {
        return;
    }

    width = plane->i_visible_pitch / (int)pixel_stride;
    height = plane->i_visible_lines;
    pitch = plane->i_pitch;
    if (width <= 0 || height <= 0)
        return;

    tmp = (uint8_t *)malloc((size_t)width * (size_t)height *
                            (size_t)channel_count);
    if (tmp == NULL)
        return;

    window = radius * 2 + 1;

    for (int y = 0; y < height; ++y) {
        const uint8_t *src = plane->p_pixels + (size_t)y * pitch;

        for (unsigned c = 0; c < channel_count; ++c) {
            int offset = channel_offsets[c];
            unsigned sum = (unsigned)(radius + 1u) * src[offset];

            for (unsigned i = 1; i <= radius; ++i) {
                int sample_x = (int)i;
                if (sample_x >= width)
                    sample_x = width - 1;
                sum += src[(size_t)sample_x * pixel_stride + offset];
            }

            tmp[((size_t)y * (size_t)width) * channel_count + c] =
                (uint8_t)(sum / window);

            for (int x = 1; x < width; ++x) {
                int add_x = x + (int)radius;
                int sub_x = x - (int)radius - 1;

                if (add_x >= width)
                    add_x = width - 1;
                if (sub_x < 0)
                    sub_x = 0;

                sum += src[(size_t)add_x * pixel_stride + offset];
                sum -= src[(size_t)sub_x * pixel_stride + offset];
                tmp[(((size_t)y * (size_t)width) + (size_t)x) *
                    channel_count + c] = (uint8_t)(sum / window);
            }
        }
    }

    for (int x = 0; x < width; ++x) {
        for (unsigned c = 0; c < channel_count; ++c) {
            int offset = channel_offsets[c];
            unsigned sum = (unsigned)(radius + 1u) *
                tmp[(size_t)x * channel_count + c];

            for (unsigned i = 1; i <= radius; ++i) {
                int sample_y = (int)i;
                if (sample_y >= height)
                    sample_y = height - 1;
                sum += tmp[(((size_t)sample_y * (size_t)width) +
                            (size_t)x) * channel_count + c];
            }

            plane->p_pixels[(size_t)x * pixel_stride + offset] =
                (uint8_t)(sum / window);

            for (int y = 1; y < height; ++y) {
                int add_y = y + (int)radius;
                int sub_y = y - (int)radius - 1;

                if (add_y >= height)
                    add_y = height - 1;
                if (sub_y < 0)
                    sub_y = 0;

                sum += tmp[(((size_t)add_y * (size_t)width) + (size_t)x) *
                           channel_count + c];
                sum -= tmp[(((size_t)sub_y * (size_t)width) + (size_t)x) *
                           channel_count + c];
                plane->p_pixels[(size_t)y * pitch +
                                (size_t)x * pixel_stride + offset] =
                    (uint8_t)(sum / window);
            }
        }
    }

    free(tmp);
}

static void BlurInterleavedPlane16(plane_t *plane,
                                   unsigned pixel_stride_bytes,
                                   const int *channel_offsets_bytes,
                                   unsigned channel_count,
                                   bool little_endian,
                                   unsigned radius)
{
    uint16_t *tmp;
    int width;
    int height;
    int pitch;
    unsigned window;

    if (plane == NULL || plane->p_pixels == NULL || pixel_stride_bytes == 0 ||
        channel_offsets_bytes == NULL || channel_count == 0 || radius == 0) {
        return;
    }

    width = plane->i_visible_pitch / (int)pixel_stride_bytes;
    height = plane->i_visible_lines;
    pitch = plane->i_pitch;
    if (width <= 0 || height <= 0)
        return;

    tmp = (uint16_t *)malloc((size_t)width * (size_t)height *
                             (size_t)channel_count * sizeof(*tmp));
    if (tmp == NULL)
        return;

    window = radius * 2 + 1;

    for (int y = 0; y < height; ++y) {
        const uint8_t *src = plane->p_pixels + (size_t)y * pitch;

        for (unsigned c = 0; c < channel_count; ++c) {
            int offset = channel_offsets_bytes[c];
            uint32_t sum = (uint32_t)(radius + 1u) *
                ReadWord16(src + offset, little_endian);

            for (unsigned i = 1; i <= radius; ++i) {
                int sample_x = (int)i;
                if (sample_x >= width)
                    sample_x = width - 1;
                sum += ReadWord16(src + (size_t)sample_x *
                                  pixel_stride_bytes + offset,
                                  little_endian);
            }

            tmp[((size_t)y * (size_t)width) * channel_count + c] =
                (uint16_t)(sum / window);

            for (int x = 1; x < width; ++x) {
                int add_x = x + (int)radius;
                int sub_x = x - (int)radius - 1;

                if (add_x >= width)
                    add_x = width - 1;
                if (sub_x < 0)
                    sub_x = 0;

                sum += ReadWord16(src + (size_t)add_x * pixel_stride_bytes +
                                  offset, little_endian);
                sum -= ReadWord16(src + (size_t)sub_x * pixel_stride_bytes +
                                  offset, little_endian);
                tmp[(((size_t)y * (size_t)width) + (size_t)x) *
                    channel_count + c] = (uint16_t)(sum / window);
            }
        }
    }

    for (int x = 0; x < width; ++x) {
        for (unsigned c = 0; c < channel_count; ++c) {
            int offset = channel_offsets_bytes[c];
            uint32_t sum = (uint32_t)(radius + 1u) *
                tmp[(size_t)x * channel_count + c];

            for (unsigned i = 1; i <= radius; ++i) {
                int sample_y = (int)i;
                if (sample_y >= height)
                    sample_y = height - 1;
                sum += tmp[(((size_t)sample_y * (size_t)width) +
                            (size_t)x) * channel_count + c];
            }

            WriteWord16(plane->p_pixels + (size_t)x * pixel_stride_bytes +
                        offset, (uint16_t)(sum / window), little_endian);

            for (int y = 1; y < height; ++y) {
                int add_y = y + (int)radius;
                int sub_y = y - (int)radius - 1;

                if (add_y >= height)
                    add_y = height - 1;
                if (sub_y < 0)
                    sub_y = 0;

                sum += tmp[(((size_t)add_y * (size_t)width) + (size_t)x) *
                           channel_count + c];
                sum -= tmp[(((size_t)sub_y * (size_t)width) + (size_t)x) *
                           channel_count + c];
                WriteWord16(plane->p_pixels + (size_t)y * pitch +
                            (size_t)x * pixel_stride_bytes + offset,
                            (uint16_t)(sum / window), little_endian);
            }
        }
    }

    free(tmp);
}

static void BlurPlane8(plane_t *plane, unsigned radius)
{
    static const int offsets[] = { 0 };

    BlurInterleavedPlane8(plane, 1, offsets, ARRAY_SIZE(offsets), radius);
}

static void BlurSemiPlanarPlane8(plane_t *plane, unsigned radius)
{
    static const int offsets[] = { 0, 1 };

    BlurInterleavedPlane8(plane, 2, offsets, ARRAY_SIZE(offsets), radius);
}

static void BlurPlane16(plane_t *plane, bool little_endian, unsigned radius)
{
    static const int offsets[] = { 0 };

    BlurInterleavedPlane16(plane, 2, offsets, ARRAY_SIZE(offsets),
                           little_endian, radius);
}

static void BlurSemiPlanarPlane16(plane_t *plane, bool little_endian,
                                  unsigned radius)
{
    static const int offsets[] = { 0, 2 };

    BlurInterleavedPlane16(plane, 4, offsets, ARRAY_SIZE(offsets),
                           little_endian, radius);
}

static void BlurPackedFrame(picture_t *pic, unsigned pixel_stride,
                            const int *channel_offsets,
                            unsigned channel_count, unsigned radius)
{
    if (pic == NULL)
        return;

    BlurInterleavedPlane8(&pic->p[0], pixel_stride, channel_offsets,
                          channel_count, radius);
}

static void BlackoutFrame(filter_t *p_filter, picture_t *pic);

static unsigned HeavyBlurRadius(const picture_t *pic)
{
    int width;
    int height;
    int min_dim;
    unsigned radius;

    if (pic == NULL)
        return 24;

    width = VisibleWidth(&pic->format);
    height = VisibleHeight(&pic->format);
    if (width <= 0 || height <= 0)
        return 24;

    min_dim = width < height ? width : height;
    radius = (unsigned)(min_dim / 12);
    if (radius < 24)
        radius = 24;
    if (radius > 96)
        radius = 96;
    return radius;
}

static void BlurFrame(picture_t *pic)
{
    nsfw_sample16_desc_t desc;
    bool swap_uv;
    unsigned u_step_x;
    unsigned u_step_y;
    bool has_alpha;
    unsigned radius;
    static const int rgb_offsets[] = { 0, 1, 2 };
    static const int argb_offsets[] = { 1, 2, 3 };

    if (pic == NULL)
        return;

    radius = HeavyBlurRadius(pic);

    if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                          &u_step_x, &u_step_y, &has_alpha)) {
        VLC_UNUSED(swap_uv);
        VLC_UNUSED(u_step_x);
        VLC_UNUSED(u_step_y);
        BlurPlane16(&pic->p[Y_PLANE], desc.little_endian, radius);
        BlurPlane16(&pic->p[U_PLANE], desc.little_endian, radius);
        BlurPlane16(&pic->p[V_PLANE], desc.little_endian, radius);
        if (has_alpha)
            BlurPlane16(&pic->p[A_PLANE], desc.little_endian, radius);
        return;
    }

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
        case VLC_CODEC_YV12:
        case VLC_CODEC_I422:
        case VLC_CODEC_I444:
        case VLC_CODEC_YUVA:
            BlurPlane8(&pic->p[Y_PLANE], radius);
            BlurPlane8(&pic->p[U_PLANE], radius);
            BlurPlane8(&pic->p[V_PLANE], radius);
            if (pic->format.i_chroma == VLC_CODEC_YUVA)
                BlurPlane8(&pic->p[A_PLANE], radius);
            break;
        case VLC_CODEC_NV12:
        case VLC_CODEC_NV21:
            BlurPlane8(&pic->p[Y_PLANE], radius);
            BlurSemiPlanarPlane8(&pic->p[U_PLANE], radius);
            break;
        case VLC_CODEC_RGB24:
            BlurPackedFrame(pic, 3, rgb_offsets, ARRAY_SIZE(rgb_offsets),
                            radius);
            break;
        case VLC_CODEC_RGB32:
        case VLC_CODEC_RGBA:
        case VLC_CODEC_BGRA:
            BlurPackedFrame(pic, 4, rgb_offsets, ARRAY_SIZE(rgb_offsets),
                            radius);
            break;
        case VLC_CODEC_ARGB:
            BlurPackedFrame(pic, 4, argb_offsets, ARRAY_SIZE(argb_offsets),
                            radius);
            break;
        case VLC_CODEC_P010:
            BlurPlane16(&pic->p[Y_PLANE], true, radius);
            BlurSemiPlanarPlane16(&pic->p[U_PLANE], true, radius);
            break;
        default:
            BlackoutFrame(NULL, pic);
            break;
    }
}

static void RedWarningFrame(picture_t *pic)
{
    if (pic == NULL)
        return;

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
        case VLC_CODEC_YV12:
        case VLC_CODEC_I422:
        case VLC_CODEC_I444:
            BlackoutPlane(&pic->p[Y_PLANE], 76);
            BlackoutPlane(&pic->p[U_PLANE], 84);
            BlackoutPlane(&pic->p[V_PLANE], 255);
            break;
        case VLC_CODEC_YUVA:
            BlackoutPlane(&pic->p[Y_PLANE], 76);
            BlackoutPlane(&pic->p[U_PLANE], 84);
            BlackoutPlane(&pic->p[V_PLANE], 255);
            BlackoutPlane(&pic->p[A_PLANE], 255);
            break;
        case VLC_CODEC_NV12:
        case VLC_CODEC_NV21:
            for (int y = 0; y < pic->p[Y_PLANE].i_visible_lines; y++) {
                memset(pic->p[Y_PLANE].p_pixels + y * pic->p[Y_PLANE].i_pitch,
                       76, (size_t)pic->p[Y_PLANE].i_visible_pitch);
            }
            for (int y = 0; y < pic->p[U_PLANE].i_visible_lines; y++) {
                uint8_t *row = pic->p[U_PLANE].p_pixels + y * pic->p[U_PLANE].i_pitch;
                for (int x = 0; x < pic->p[U_PLANE].i_visible_pitch; x += 2) {
                    row[x + 0] = 84;
                    row[x + 1] = 255;
                }
            }
            break;
        case VLC_CODEC_RGB24:
        case VLC_CODEC_RGB32:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x +=
                        (pic->format.i_chroma == VLC_CODEC_RGB24 ? 3 : 4)) {
                    row[x + 0] = 255;
                    row[x + 1] = 0;
                    row[x + 2] = 0;
                    if (pic->format.i_chroma == VLC_CODEC_RGB32)
                        row[x + 3] = 255;
                }
            }
            break;
        case VLC_CODEC_RGBA:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x += 4) {
                    row[x + 0] = 255;
                    row[x + 1] = 0;
                    row[x + 2] = 0;
                    row[x + 3] = 255;
                }
            }
            break;
        case VLC_CODEC_ARGB:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x += 4) {
                    row[x + 0] = 255;
                    row[x + 1] = 255;
                    row[x + 2] = 0;
                    row[x + 3] = 0;
                }
            }
            break;
        case VLC_CODEC_BGRA:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x += 4) {
                    row[x + 0] = 0;
                    row[x + 1] = 0;
                    row[x + 2] = 255;
                    row[x + 3] = 255;
                }
            }
            break;
        case VLC_CODEC_P010:
        {
            const nsfw_sample16_desc_t desc = { true, 10, 6 };
            uint16_t y_red = EncodeSample16(76, desc);
            uint16_t u_red = EncodeSample16(84, desc);
            uint16_t v_red = EncodeSample16(255, desc);
            BlackoutPlane16(&pic->p[Y_PLANE], y_red, true);
            BlackoutPlane16(&pic->p[U_PLANE], u_red, true);
            BlackoutPlane16(&pic->p[V_PLANE], v_red, true);
            break;
        }
        default:
            BlackoutFrame(NULL, pic);
            break;
    }
}

static void BlackoutFrame(filter_t *p_filter, picture_t *pic)
{
    filter_sys_t *sys = p_filter != NULL ? p_filter->p_sys : NULL;

    if (sys != NULL) {
        switch (sys->block_style) {
            case NSFW_BLOCK_STYLE_BLUR:
                BlurFrame(pic);
                return;
            case NSFW_BLOCK_STYLE_WARNING:
                RedWarningFrame(pic);
                return;
            case NSFW_BLOCK_STYLE_BLACK:
            default:
                break;
        }
    }

    {
        nsfw_sample16_desc_t desc;
        bool swap_uv;
        unsigned u_step_x;
        unsigned u_step_y;
        bool has_alpha;

        if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                              &u_step_x, &u_step_y, &has_alpha)) {
            BlackoutPlanarFrame16(pic, desc, has_alpha);
            return;
        }
    }

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
        case VLC_CODEC_YV12:
        case VLC_CODEC_I422:
        case VLC_CODEC_I444:
            BlackoutPlane(&pic->p[Y_PLANE], 0x00);
            BlackoutPlane(&pic->p[U_PLANE], 0x80);
            BlackoutPlane(&pic->p[V_PLANE], 0x80);
            break;
        case VLC_CODEC_YUVA:
            BlackoutPlane(&pic->p[Y_PLANE], 0x00);
            BlackoutPlane(&pic->p[U_PLANE], 0x80);
            BlackoutPlane(&pic->p[V_PLANE], 0x80);
            BlackoutPlane(&pic->p[A_PLANE], 0xFF);
            break;
        case VLC_CODEC_NV12:
        case VLC_CODEC_NV21:
            BlackoutSemiPlanarFrame(pic);
            break;
        case VLC_CODEC_RGB24:
        case VLC_CODEC_RGB32:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                memset(pic->p[0].p_pixels + y * pic->p[0].i_pitch, 0x00,
                       (size_t)pic->p[0].i_visible_pitch);
            }
            break;
        case VLC_CODEC_RGBA:
        case VLC_CODEC_ARGB:
        case VLC_CODEC_BGRA:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x += 4) {
                    row[x + 0] = 0x00;
                    row[x + 1] = 0x00;
                    row[x + 2] = 0x00;
                    row[x + 3] = 0xFF;
                }
            }
            break;
        case VLC_CODEC_P010:
            BlackoutP010Frame(pic);
            break;
        default:
            break;
    }
}

/*****************************************************************************
 * RGB packing helpers for real ONNX inference
 *****************************************************************************/

static inline uint8_t ClampByte(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return (uint8_t)value;
}

static inline void YCbCrToRGB(unsigned y, unsigned cb, unsigned cr,
                              uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)cb - 128;
    int e = (int)cr - 128;

    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;

    *r = ClampByte(rr);
    *g = ClampByte(gg);
    *b = ClampByte(bb);
}

static void PackPlanarFrame(const picture_t *pic,
                            int src_width,
                            int src_height,
                            int dst_width,
                            int dst_height,
                            bool swap_uv,
                            unsigned u_step_x,
                            unsigned u_step_y,
                            uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (src_y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (src_y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            unsigned yy = y_row[src_x];
            unsigned uu = u_row[src_x >> u_step_x];
            unsigned vv = v_row[src_x >> u_step_x];

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * (size_t)dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackPlanarFrame16(const picture_t *pic,
                              int src_width,
                              int src_height,
                              int dst_width,
                              int dst_height,
                              bool swap_uv,
                              unsigned u_step_x,
                              unsigned u_step_y,
                              nsfw_sample16_desc_t desc,
                              uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (src_y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (src_y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)src_x * 2), desc);
            unsigned uu = DecodeSample16ToByte(
                u_row + ((size_t)(src_x >> u_step_x) * 2), desc);
            unsigned vv = DecodeSample16ToByte(
                v_row + ((size_t)(src_x >> u_step_x) * 2), desc);

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * (size_t)dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackP010Frame(const picture_t *pic,
                          int src_width,
                          int src_height,
                          int dst_width,
                          int dst_height,
                          uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const nsfw_sample16_desc_t desc = { true, 10, 6 };

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (src_y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            size_t chroma = (size_t)(src_x >> 1) * 4;
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)src_x * 2), desc);
            unsigned uu = DecodeSample16ToByte(uv_row + chroma, desc);
            unsigned vv = DecodeSample16ToByte(uv_row + chroma + 2, desc);

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * (size_t)dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackSemiPlanarFrame(const picture_t *pic,
                                int src_width,
                                int src_height,
                                int dst_width,
                                int dst_height,
                                bool swap_uv,
                                uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (src_y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            size_t chroma = (size_t)(src_x >> 1) * 2;
            unsigned yy = y_row[src_x];
            unsigned uu = uv_row[chroma + (swap_uv ? 1 : 0)];
            unsigned vv = uv_row[chroma + (swap_uv ? 0 : 1)];

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * (size_t)dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackPackedFrame(const picture_t *pic,
                            int src_width,
                            int src_height,
                            int dst_width,
                            int dst_height,
                            int pixel_size,
                            int r_offset,
                            int g_offset,
                            int b_offset,
                            uint8_t *rgb)
{
    const plane_t *plane = &pic->p[0];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *row = plane->p_pixels + src_y * plane->i_pitch;
        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            const uint8_t *px = row + ((size_t)src_x * pixel_size);
            size_t pixel = ((size_t)y * (size_t)dst_width + (size_t)x) * 3;
            rgb[pixel + 0] = px[r_offset];
            rgb[pixel + 1] = px[g_offset];
            rgb[pixel + 2] = px[b_offset];
        }
    }
}

static int PackFrameToRGB(const filter_t *p_filter,
                          const picture_t *pic,
                          uint8_t *rgb,
                          size_t rgb_capacity,
                          int target_width,
                          int target_height,
                          int *width,
                          int *height)
{
    vlc_fourcc_t chroma;
    int src_width;
    int src_height;

    if (!pic || !rgb || !width || !height)
        return -1;

    VLC_UNUSED(p_filter);
    chroma = pic->format.i_chroma;
    src_width = VisibleWidth(&pic->format);
    src_height = VisibleHeight(&pic->format);
    if (src_width <= 0 || src_height <= 0)
        return -1;

    *width = ClampDimension(target_width, src_width);
    *height = ClampDimension(target_height, src_height);
    size_t needed = (size_t)(*width) * (size_t)(*height) * 3;
    if (rgb_capacity < needed)
        return -1;

    {
        nsfw_sample16_desc_t desc;
        bool swap_uv;
        unsigned u_step_x;
        unsigned u_step_y;
        bool has_alpha;

        if (GetPlanar16Layout(chroma, &desc, &swap_uv,
                              &u_step_x, &u_step_y, &has_alpha)) {
            PackPlanarFrame16(pic, src_width, src_height, *width, *height,
                              swap_uv, u_step_x, u_step_y, desc, rgb);
            return 0;
        }
    }

    switch (chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
            PackPlanarFrame(pic, src_width, src_height, *width, *height,
                            false, 1, 1, rgb);
            return 0;
        case VLC_CODEC_YV12:
            PackPlanarFrame(pic, src_width, src_height, *width, *height,
                            true, 1, 1, rgb);
            return 0;
        case VLC_CODEC_I422:
            PackPlanarFrame(pic, src_width, src_height, *width, *height,
                            false, 1, 0, rgb);
            return 0;
        case VLC_CODEC_I444:
        case VLC_CODEC_YUVA:
            PackPlanarFrame(pic, src_width, src_height, *width, *height,
                            false, 0, 0, rgb);
            return 0;
        case VLC_CODEC_NV12:
            PackSemiPlanarFrame(pic, src_width, src_height, *width, *height,
                                false, rgb);
            return 0;
        case VLC_CODEC_NV21:
            PackSemiPlanarFrame(pic, src_width, src_height, *width, *height,
                                true, rgb);
            return 0;
        case VLC_CODEC_RGB24:
            PackPackedFrame(pic, src_width, src_height, *width, *height,
                            3, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_RGB32:
            PackPackedFrame(pic, src_width, src_height, *width, *height,
                            4, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_RGBA:
            PackPackedFrame(pic, src_width, src_height, *width, *height,
                            4, 0, 1, 2, rgb);
            return 0;
        case VLC_CODEC_ARGB:
            PackPackedFrame(pic, src_width, src_height, *width, *height,
                            4, 1, 2, 3, rgb);
            return 0;
        case VLC_CODEC_BGRA:
            PackPackedFrame(pic, src_width, src_height, *width, *height,
                            4, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_P010:
            PackP010Frame(pic, src_width, src_height, *width, *height, rgb);
            return 0;
        default:
            return -1;
    }
}

static int EnsureRgbBuffer(filter_sys_t *sys, size_t required)
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

static bool ParseUnsignedEnv(const char *name, unsigned long *value);

static unsigned DefaultDecisionReloadStride(void)
{
    return 12;
}

static unsigned DefaultAnalysisStride(const video_format_t *fmt)
{
    uint64_t pixels;

    if (!fmt)
        return 3;

    pixels = (uint64_t)VisibleWidth(fmt) * (uint64_t)VisibleHeight(fmt);
    if (pixels >= (uint64_t)3840 * 2160)
        return 6;
    if (pixels >= (uint64_t)2560 * 1440)
        return 5;
    return 3;
}

static unsigned ResolveAnalysisStride(const video_format_t *fmt)
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

static unsigned DefaultBlockPaddingFrames(unsigned analysis_stride)
{
    unsigned padding;

    if (analysis_stride == 0)
        return 0;

    padding = analysis_stride + 2;
    if (padding > 32)
        padding = 32;
    return padding;
}

static unsigned ResolveBlockPaddingFrames(unsigned analysis_stride)
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

static unsigned DefaultPrebufferFrames(const video_format_t *fmt)
{
    uint64_t pixels;

    if (!fmt)
        return 3;

    pixels = (uint64_t)VisibleWidth(fmt) * (uint64_t)VisibleHeight(fmt);
    if (pixels >= (uint64_t)3840 * 2160)
        return 6;
    if (pixels >= (uint64_t)1920 * 1080)
        return 4;
    return 3;
}

static vlc_tick_t EstimatedFrameInterval(const video_format_t *fmt)
{
    if (fmt != NULL &&
        fmt->i_frame_rate > 0 &&
        fmt->i_frame_rate_base > 0) {
        return ((vlc_tick_t)CLOCK_FREQ * fmt->i_frame_rate_base +
                fmt->i_frame_rate - 1) / fmt->i_frame_rate;
    }

    return CLOCK_FREQ / 24;
}

static bool ParseUnsignedEnv(const char *name, unsigned long *value)
{
    const char *raw = getenv(name);
    char *end = NULL;

    if (value == NULL || raw == NULL || raw[0] == '\0')
        return false;

    *value = strtoul(raw, &end, 10);
    return end != raw && end != NULL && *end == '\0';
}

static unsigned ResolvePrebufferFrames(const video_format_t *fmt)
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

static unsigned ResolveDecisionReloadStride(void)
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

static unsigned DefaultWorkerCount(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    unsigned workers;

    GetSystemInfo(&info);
    workers = info.dwNumberOfProcessors > 0 ?
              (unsigned)info.dwNumberOfProcessors : 1u;
    if (workers > NSFW_MAX_WORKER_THREADS)
        workers = NSFW_MAX_WORKER_THREADS;
    return workers > 0 ? workers : 1;
#else
    return 4;
#endif
}

static unsigned ResolveWorkerCount(void)
{
    unsigned long parsed = 0;
    unsigned fallback = DefaultWorkerCount();

    if (ParseUnsignedEnv("NSFW_WORKER_THREADS", &parsed)) {
        if (parsed == 0)
            return fallback;
        if (parsed > NSFW_MAX_WORKER_THREADS)
            return NSFW_MAX_WORKER_THREADS;
        return (unsigned)parsed;
    }

    return fallback;
}

static unsigned MinimumPrebufferFrames(unsigned analysis_stride,
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

static bool IsNullOrEmpty(const char *value)
{
    return value == NULL || value[0] == '\0';
}

static bool StringEquals(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcmp(left, right) == 0;
}

static void PersistModernDefaultSettings(filter_t *filter)
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
                "nsfw_filter: applied runtime defaults, but VLC config save accessors are unavailable so the old preset was not rewritten\n");
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
    put_int((vlc_object_t *)filter, "nsfw-settings-version",
            NSFW_SETTINGS_VERSION_CURRENT);

    save_result = save_file((vlc_object_t *)filter);
    if (save_result == VLC_SUCCESS) {
        fprintf(stderr,
                "nsfw_filter: rewrote the older saved preset to current defaults\n");
    } else {
        fprintf(stderr,
                "nsfw_filter: applied runtime defaults, but failed to save the updated preset (error %d)\n",
                save_result);
    }
}

static void MaybeReplaceLegacyPreset(filter_t *filter)
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

    SetProcessEnvValue("NSFW_MODEL_PROFILE", "marqo");
    SetProcessEnvValue("NSFW_MODEL_PATH", "");
    SetProcessEnvValue("NSFW_ONNX_PROVIDER", "cpu");
    SetProcessEnvOptionalUnsigned("NSFW_ANALYSIS_STRIDE", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BLOCK_PADDING_FRAMES", 0);
    SetProcessEnvOptionalUnsigned("NSFW_BUFFERED_FRAMES", 0);
    SetProcessEnvOptionalUnsigned("NSFW_WORKER_THREADS", 0);
    SetProcessEnvOptionalUnsigned("NSFW_DECISION_RELOAD_FRAMES", 0);
    SetProcessEnvValue("NSFW_DECISION_MAP_PATH", "");
    SetProcessEnvValue("NSFW_SCAN_STATUS_PATH", "");

    fprintf(stderr,
            "nsfw_filter: replaced an older saved legacy preset with current defaults (marqo, black, automatic buffering)\n");
    PersistModernDefaultSettings(filter);
}

static bool EnsureBlockRangeCapacity(filter_sys_t *sys, size_t required)
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

static bool EnsureTimeBlockRangeCapacity(filter_sys_t *sys, size_t required)
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

static void ClearDecisionMap(filter_sys_t *sys)
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

static void ClearTimeBlockRanges(filter_sys_t *sys)
{
    if (sys == NULL)
        return;

    free(sys->time_block_ranges);
    sys->time_block_ranges = NULL;
    sys->time_block_range_count = 0;
    sys->time_block_range_capacity = 0;
}

static void ParseScanStatus(filter_sys_t *sys)
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

static bool ParseDecisionMap(filter_sys_t *sys)
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

static void RefreshDecisionMap(filter_sys_t *sys, bool force)
{
    uint64_t map_signature;
    uint64_t status_signature;

    if (sys == NULL || !sys->decision_map_mode)
        return;

    map_signature = FileSignature(sys->decision_map_path);
    status_signature = FileSignature(sys->scan_status_path);

    if (force || map_signature != sys->decision_map_mtime) {
        if (ParseDecisionMap(sys))
            sys->decision_map_mtime = map_signature;
    }

    if (force || status_signature != sys->scan_status_mtime) {
        ParseScanStatus(sys);
        sys->scan_status_mtime = status_signature;
    }
}

static uint64_t PictureTimeMs(const filter_sys_t *sys, const picture_t *pic)
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

static void ResetOutputMaskState(filter_sys_t *sys)
{
    if (sys == NULL)
        return;

    sys->output_mask_active = false;
    sys->output_mask_frame_count = 0;
    sys->output_mask_start_ms = 0;
}

static void DumpBlockedFrameIfRequested(filter_t *filter, picture_t *pic)
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

    width = VisibleWidth(&pic->format);
    height = VisibleHeight(&pic->format);
    if (width <= 0 || height <= 0)
        return;

    needed = (size_t)width * (size_t)height * 3;
    rgb = (uint8_t *)malloc(needed);
    if (rgb == NULL)
        return;

    if (PackFrameToRGB(filter, pic, rgb, needed, width, height,
                       &packed_width, &packed_height) != 0 ||
        packed_width != width || packed_height != height) {
        free(rgb);
        return;
    }

    FourccToString(pic->format.i_chroma, chroma);
    if (snprintf(path, sizeof(path), "%s-%s-%s-%u.ppm",
                 prefix, BlockStyleName(sys->block_style), chroma,
                 sys->output_mask_frame_count + 1) <= 0) {
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
            "nsfw_filter: dumped blocked frame to %s (%dx%d, chroma %s, style %s)\n",
            path, width, height, chroma, BlockStyleName(sys->block_style));
}

static void UpdateOutputMaskState(filter_t *filter, uint64_t timestamp_ms,
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
            FourccToString(filter->fmt_in.video.i_chroma, chroma);
            fprintf(stderr,
                    "nsfw_filter: output masking started at %llu ms using %s style (filter input %s)\n",
                    (unsigned long long)timestamp_ms,
                    BlockStyleName(sys->block_style), chroma);
        }
        sys->output_mask_frame_count++;
        return;
    }

    if (!sys->output_mask_active)
        return;

    fprintf(stderr,
            "nsfw_filter: output masking ended before %llu ms after %u frame(s) starting at %llu ms\n",
            (unsigned long long)timestamp_ms,
            sys->output_mask_frame_count,
            (unsigned long long)sys->output_mask_start_ms);
    ResetOutputMaskState(sys);
}

static void ApplyBlockedOutput(filter_t *filter, picture_t *pic, bool blocked)
{
    uint64_t timestamp_ms;
    char chroma[5];

    if (filter == NULL || filter->p_sys == NULL || pic == NULL)
        return;

    timestamp_ms = PictureTimeMs(filter->p_sys, pic);
    UpdateOutputMaskState(filter, timestamp_ms, blocked);
    SetAudioMutedForBlockedFrame(filter, blocked);
    if (blocked) {
        FourccToString(pic->format.i_chroma, chroma);
        BlackoutFrame(filter, pic);
        DumpBlockedFrameIfRequested(filter, pic);
        if (filter->p_sys->output_mask_frame_count == 1) {
            fprintf(stderr,
                    "nsfw_filter: applied %s style to output picture chroma %s at %llu ms\n",
                    BlockStyleName(filter->p_sys->block_style), chroma,
                    (unsigned long long)timestamp_ms);
        }
    }
}

static bool DecisionMapContains(const filter_sys_t *sys, uint64_t pts_ms)
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

static bool DecisionMapShouldBlock(filter_t *p_filter, picture_t *p_pic)
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

static unsigned QueueIndex(const filter_sys_t *sys, unsigned offset)
{
    return (sys->queue_head + offset) % NSFW_MAX_BUFFER_FRAMES;
}

static nsfw_frame_slot_t *GetFrameSlotLocked(filter_sys_t *sys, unsigned offset)
{
    if (!sys || offset >= sys->queue_count)
        return NULL;
    return &sys->frame_queue[QueueIndex(sys, offset)];
}

static bool TimeInBlockedRangeLocked(const filter_sys_t *sys, uint64_t timestamp_ms);

static bool OldestFrameReadyLocked(filter_sys_t *sys)
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

static bool HasProcessingFramesLocked(filter_sys_t *sys)
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

static bool QueuePictureLocked(filter_sys_t *sys, picture_t *pic, bool analyze)
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

static bool TimeInBlockedRangeLocked(const filter_sys_t *sys, uint64_t timestamp_ms)
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

static void PruneExpiredTimeBlockRangesLocked(filter_sys_t *sys,
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

static void EnsureQueuedFramesInWindowLocked(filter_sys_t *sys,
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

static void ApplyBlockWindowLocked(filter_sys_t *sys, uint64_t start_ms,
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

static picture_t *TakeReadyOutputLocked(filter_sys_t *sys, bool *blocked)
{
    nsfw_frame_slot_t *slot;
    picture_t *picture;

    if (!sys || !blocked || !OldestFrameReadyLocked(sys))
        return NULL;

    slot = GetFrameSlotLocked(sys, 0);
    picture = slot->picture;
    *blocked = slot->blocked ||
               TimeInBlockedRangeLocked(sys, slot->timestamp_ms);
    memset(slot, 0, sizeof(*slot));
    sys->queue_head = (sys->queue_head + 1) % NSFW_MAX_BUFFER_FRAMES;
    sys->queue_count--;
    if (sys->queue_count == 0)
        sys->queue_head = 0;
    return picture;
}

static void ReleaseQueuedFramesLocked(filter_sys_t *sys)
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

static void RegisterPositiveDetection(filter_sys_t *sys,
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
    padding_ms = (uint64_t)sys->block_padding_frames *
                 (sys->frame_interval_ms > 0 ? sys->frame_interval_ms : 41);
    start_ms = timestamp_ms > padding_ms ? timestamp_ms - padding_ms : 0;
    end_ms = timestamp_ms + padding_ms;
    ApplyBlockWindowLocked(sys, start_ms, end_ms);
    sys->block_count++;
    if (sys->block_count == 1 || (sys->block_count % 30) == 0) {
        fprintf(stderr,
                "nsfw_filter: ONNX score %.3f >= %.3f, blacking out frames around %llu ms (+/-%u frames)\n",
                result->score, result->threshold,
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
            size_t needed = (size_t)ClampDimension(sys->analysis_width,
                                                   VisibleWidth(&picture->format)) *
                            (size_t)ClampDimension(sys->analysis_height,
                                                   VisibleHeight(&picture->format)) * 3;

            if (needed > 0) {
                uint8_t *buf = (uint8_t *)realloc(worker->rgb_buffer, needed);
                if (buf != NULL) {
                    worker->rgb_buffer = buf;
                    worker->rgb_capacity = needed;
                }
            }

            if (worker->rgb_buffer != NULL &&
                worker->rgb_capacity >= needed &&
                PackFrameToRGB(NULL, picture, worker->rgb_buffer,
                               worker->rgb_capacity,
                               sys->analysis_width, sys->analysis_height,
                               &width, &height) == 0) {
                packed = true;
            }

            if (packed) {
                result = sys->detector_classify_fn(worker->detector,
                                                   worker->rgb_buffer,
                                                   width, height, 3);
            } else {
                float score = ScoreFrame(NULL, picture);

                result.is_nsfw = score >= sys->threshold;
                result.score = score;
                result.threshold = sys->threshold;
                if (sys->block_count == 0) {
                    fprintf(stderr,
                            "nsfw_filter: unable to pack frame for ONNX inference, using heuristic fallback\n");
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
#endif

static int StartDetectorWorker(filter_sys_t *sys, const nsfw_config_t *cfg)
{
    unsigned i;
    unsigned started = 0;
    unsigned desired_workers;

    if (!sys || !cfg || sys->detector_classify_fn == NULL)
        return VLC_EGENERIC;

#ifndef _WIN32
    VLC_UNUSED(cfg);
    return VLC_EGENERIC;
#else
    desired_workers = ResolveWorkerCount();
    if (desired_workers == 0)
        desired_workers = 1;

    sys->workers = (nsfw_worker_state_t *)calloc(desired_workers,
                                                 sizeof(*sys->workers));
    if (sys->workers == NULL)
        return VLC_ENOMEM;

    InitializeCriticalSection(&sys->worker_lock);
    InitializeConditionVariable(&sys->worker_cond);
    sys->worker_stop = false;
    sys->worker_running = false;
    sys->worker_count = 0;

    for (i = 0; i < desired_workers; ++i) {
        nsfw_worker_state_t *worker = &sys->workers[started];

        worker->sys = sys;
        worker->detector = sys->detector_create_fn(cfg);
        if (worker->detector == NULL)
            continue;

        worker->thread = (HANDLE)_beginthreadex(NULL, 0,
                                                DetectorWorkerThread,
                                                worker, 0, NULL);
        if (worker->thread == NULL) {
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
        DeleteCriticalSection(&sys->worker_lock);
        free(sys->workers);
        sys->workers = NULL;
        return VLC_EGENERIC;
    }

    sys->worker_count = started;
    sys->worker_running = true;
    return VLC_SUCCESS;
#endif
}

static void StopDetectorWorker(filter_sys_t *sys)
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
#endif
    free(sys->workers);
    sys->workers = NULL;
    sys->worker_count = 0;
    sys->worker_running = false;
    sys->worker_stop = false;
}

/*****************************************************************************
 * Open: initialize the filter
 *****************************************************************************/
static int Open(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;

    if (p_filter->fmt_in.video.i_width <= 0 ||
        p_filter->fmt_in.video.i_height <= 0)
        return VLC_EGENERIC;

    if (IsOpaqueHardwareChroma(p_filter->fmt_in.video.i_chroma)) {
        fprintf(stderr,
                "nsfw_filter: rejecting opaque hardware chroma %4.4s so VLC can build a software converter chain\n",
                (const char *)&p_filter->fmt_in.video.i_chroma);
        return VLC_EGENERIC;
    }

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
    p_filter->p_sys->mute_audio_on_blocked =
        GetVlcConfigInteger(p_filter, "nsfw-mute-audio-on-blocked", 0) != 0;
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
                    "nsfw_filter: decision-map mode enabled with %zu blocked ranges\n",
                    p_filter->p_sys->block_range_count);
            p_filter->pf_video_filter = Filter;
            p_filter->pf_flush = Flush;
            return VLC_SUCCESS;
        }
    }

    if (LoadCoreModule(p_filter->p_sys)) {
        nsfw_config_t cfg = p_filter->p_sys->config_default_fn();
        const char *model_profile = getenv("NSFW_MODEL_PROFILE");
        nsfw_model_profile_t profile = cfg.model_profile;
        cfg.threshold = p_filter->p_sys->threshold;
        if (model_profile != NULL && model_profile[0] != '\0') {
            if (!p_filter->p_sys->model_profile_parse_fn(model_profile, &profile)) {
                fprintf(stderr,
                        "nsfw_filter: unrecognized NSFW_MODEL_PROFILE=%s, defaulting to %s\n",
                        model_profile,
                        p_filter->p_sys->model_profile_name_fn(profile));
            }
        }

        p_filter->p_sys->config_set_model_profile_fn(&cfg, profile);
        p_filter->p_sys->analysis_width = cfg.model_width;
        p_filter->p_sys->analysis_height = cfg.model_height;

        const char *model_path = getenv("NSFW_MODEL_PATH");
        if (model_path != NULL && model_path[0] != '\0')
            cfg.model_path = model_path;

        fprintf(stderr,
                "nsfw_filter: using %s model profile (%dx%d)\n",
                p_filter->p_sys->model_profile_name_fn(cfg.model_profile),
                cfg.model_width, cfg.model_height);

        if (StartDetectorWorker(p_filter->p_sys, &cfg) == VLC_SUCCESS) {
            fprintf(stderr,
                    "nsfw_filter: started %u parallel detector worker(s)\n",
                    p_filter->p_sys->worker_count);
        } else {
            p_filter->p_sys->detector = p_filter->p_sys->detector_create_fn(&cfg);
            if (p_filter->p_sys->detector == NULL) {
                fprintf(stderr,
                        "nsfw_filter: ONNX detector unavailable, using heuristic fallback\n");
            } else {
                fprintf(stderr,
                        "nsfw_filter: unable to start detector worker, using synchronous inference\n");
            }
        }
    } else {
        fprintf(stderr,
                "nsfw_filter: unable to load core DLL, using heuristic fallback\n");
    }

    p_filter->pf_video_filter = Filter;
    p_filter->pf_flush = Flush;
    fprintf(stderr,
            "nsfw_filter: holding %u processed frames before playback\n",
            p_filter->p_sys->prebuffer_frames);
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Flush: release delayed frames and reset buffered state
 *****************************************************************************/
static void Flush(filter_t *p_filter)
{
    filter_sys_t *sys;

    if (p_filter == NULL || p_filter->p_sys == NULL)
        return;

    sys = p_filter->p_sys;

    if (sys->decision_map_mode) {
        SetAudioMutedForBlockedFrame(p_filter, false);
        ResetOutputMaskState(sys);
        sys->frame_count = 0;
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
#endif
    }

    SetAudioMutedForBlockedFrame(p_filter, false);
    ResetOutputMaskState(sys);
    ClearTimeBlockRanges(sys);
    sys->frame_count = 0;
}

/*****************************************************************************
 * Close: clean up the filter
 *****************************************************************************/
static void Close(vlc_object_t *p_this)
{
    filter_t *p_filter = (filter_t *)p_this;
    if (p_filter->p_sys != NULL) {
        Flush(p_filter);
        StopDetectorWorker(p_filter->p_sys);
        if (p_filter->p_sys->detector_destroy_fn != NULL)
            p_filter->p_sys->detector_destroy_fn(p_filter->p_sys->detector);
        SetAudioMutedForBlockedFrame(p_filter, false);
        ClearDecisionMap(p_filter->p_sys);
        ClearTimeBlockRanges(p_filter->p_sys);
        free(p_filter->p_sys->decision_map_path);
        free(p_filter->p_sys->scan_status_path);
        free(p_filter->p_sys->rgb_buffer);
        UnloadCoreModule(p_filter->p_sys);
    }
    free(p_filter->p_sys);
    p_filter->p_sys = NULL;
}

/*****************************************************************************
 * Filter: process a video frame
 *****************************************************************************/
static picture_t *Filter(filter_t *p_filter, picture_t *p_pic)
{
    filter_sys_t *sys = p_filter->p_sys;
    bool blocked = false;
    bool should_analyze;
    picture_t *output = NULL;
    size_t needed;
    int width = 0;
    int height = 0;
    uint64_t sequence;
    uint64_t timestamp_ms;

    if (p_pic == NULL)
        return NULL;

    sys->frame_count++;
    sequence = sys->frame_count;
    timestamp_ms = PictureTimeMs(sys, p_pic);

    if (sys->decision_map_mode) {
        blocked = DecisionMapShouldBlock(p_filter, p_pic);
        if (blocked) {
            sys->block_count++;
            if (sys->block_count == 1 || (sys->block_count % 60) == 0) {
                fprintf(stderr,
                        "nsfw_filter: decision map blocked frame %llu ms (scanned %llu ms)%s\n",
                        (unsigned long long)PictureTimeMs(sys, p_pic),
                        (unsigned long long)sys->last_scanned_ms,
                        sys->scan_done ? "" : ", pending scan");
            }
        }
        ApplyBlockedOutput(p_filter, p_pic, blocked);
        return p_pic;
    }

    if (sys->worker_running) {
        EnterCriticalSection(&sys->worker_lock);
        blocked = TimeInBlockedRangeLocked(sys, timestamp_ms);
        LeaveCriticalSection(&sys->worker_lock);

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

        if (sys->queue_count >= NSFW_MAX_BUFFER_FRAMES)
            output = TakeReadyOutputLocked(sys, &blocked);

        if (!QueuePictureLocked(sys, p_pic, should_analyze)) {
            LeaveCriticalSection(&sys->worker_lock);
            ReleasePicture(p_pic);
            return NULL;
        }

        WakeAllConditionVariable(&sys->worker_cond);

        if (sys->queue_count < sys->prebuffer_frames) {
            LeaveCriticalSection(&sys->worker_lock);
            if (output != NULL) {
                ApplyBlockedOutput(p_filter, output, blocked);
                return output;
            }
            return NULL;
        }

        while (output == NULL && !OldestFrameReadyLocked(sys))
            SleepConditionVariableCS(&sys->worker_cond, &sys->worker_lock,
                                     INFINITE);

        if (output == NULL)
            output = TakeReadyOutputLocked(sys, &blocked);
        LeaveCriticalSection(&sys->worker_lock);
#endif

        if (output == NULL)
            return NULL;
        ApplyBlockedOutput(p_filter, output, blocked);
        return output;
    } else if (sys->detector != NULL) {
        nsfw_result_t result;

        blocked = TimeInBlockedRangeLocked(sys, timestamp_ms);
        if (blocked) {
            ApplyBlockedOutput(p_filter, p_pic, true);
            return p_pic;
        }

        should_analyze = ((sys->frame_count - 1) % sys->analysis_stride) == 0;
        needed = (size_t)ClampDimension(sys->analysis_width,
                                        VisibleWidth(&p_pic->format)) *
                 (size_t)ClampDimension(sys->analysis_height,
                                        VisibleHeight(&p_pic->format)) * 3;
        if (should_analyze &&
            needed > 0 &&
            EnsureRgbBuffer(sys, needed) == 0 &&
            PackFrameToRGB(p_filter, p_pic, sys->rgb_buffer, sys->rgb_capacity,
                           sys->analysis_width, sys->analysis_height,
                           &width, &height) == 0) {
            result = sys->detector_classify_fn(sys->detector, sys->rgb_buffer,
                                               width, height, 3);
            RegisterPositiveDetection(sys, &result, timestamp_ms, &blocked);
        } else if (should_analyze && sys->block_count == 0) {
            fprintf(stderr,
                    "nsfw_filter: unable to pack frame for ONNX inference, using heuristic fallback\n");
        }

        if (blocked) {
            ApplyBlockedOutput(p_filter, p_pic, true);
            return p_pic;
        }

        if (!should_analyze) {
            ApplyBlockedOutput(p_filter, p_pic, false);
            return p_pic;
        }

        if (needed > 0 && width > 0 && height > 0) {
            ApplyBlockedOutput(p_filter, p_pic, false);
            return p_pic;
        }
    }

        {
            float score = ScoreFrame(p_filter, p_pic);
            blocked = score >= sys->threshold;
            if (blocked) {
                sys->block_count++;
                if (sys->block_count == 1 || (sys->block_count % 30) == 0) {
                    fprintf(stderr,
                            "nsfw_filter: heuristic score %.3f >= %.3f, blacking out frame\n",
                            score, sys->threshold);
                }
            }
            ApplyBlockedOutput(p_filter, p_pic, blocked);
        }

    return p_pic;
}
