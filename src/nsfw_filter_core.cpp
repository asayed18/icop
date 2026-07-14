/*****************************************************************************
 * nsfw_filter_core.cpp: NSFW detection core library implementation
 *****************************************************************************
 * Implements the public API from nsfw_filter_core.h.
 * The ONNX Runtime backend is compiled in only when NSFW_HAS_ONNXRUNTIME
 * is defined; otherwise nsfw_detector_create returns NULL and callers
 * must use nsfw_detector_create_with_backend.
 *****************************************************************************/

#include "nsfw_filter_core.h"

#include <cstdlib>
#include <array>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>
#include <wchar.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
# if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
# endif
#include <dlfcn.h>
#endif

#ifdef NSFW_HAS_ONNXRUNTIME
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#endif

/*****************************************************************************
 * Internal: detector state
 *****************************************************************************/

struct nsfw_detector {
    nsfw_config_t         config;
    std::string           model_path_owned; /* owns the copy of model_path   */
    nsfw_backend_vtable_t backend;
    std::vector<float>    preprocessed;     /* reusable tensor buffer        */
};

struct nsfw_model_profile_info {
    nsfw_model_profile_t profile;
    const char           *name;
    const char           *aliases[5];
    const char           *runtime_filename;
    int                   width;
    int                   height;
    float                 mean[3];
    float                 stddev[3];
    bool                  legacy_multiclass;
    size_t                nsfw_index;
};

enum class nsfw_tensor_layout {
    nhwc,
    nchw,
};

static const nsfw_model_profile_info kModelProfiles[] = {
    {
        NSFW_MODEL_PROFILE_MARQO,
        "marqo",
        { "nsfw-image-detection-384", "marqo/nsfw-image-detection-384", nullptr, nullptr, nullptr },
        "model.onnx",
        384,
        384,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        false,
        0,
    },
    {
        NSFW_MODEL_PROFILE_ADAMCODD,
        "adamcodd",
        { "vit-base-nsfw-detector", "adamcodd/vit-base-nsfw-detector", nullptr, nullptr, nullptr },
        "adamcodd.onnx",
        384,
        384,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        false,
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI,
        "falconsai",
        { "nsfw_image_detection", "falconsai/nsfw_image_detection", nullptr, nullptr, nullptr },
        "falconsai.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        false,
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL,
        "falconsai-official",
        {
            "nsfw_image_detection_26",
            "falconsai/nsfw_image_detection_26",
            "Falconsai/nsfw_image_detection_26",
            "falconsai26",
            "falconsai-26",
        },
        "quantized_model.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        false,
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI_BASE,
        "falconsai-base",
        {
            "falconsaibase",
            "falconsai_base",
            "falconsai-vit",
            "falconsai-pytorch",
            nullptr,
        },
        "falconsai_base.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        false,
        1,
    },
    {
        NSFW_MODEL_PROFILE_LEGACY,
        "legacy",
        { "gantman", "nsfw-detect-onnx", "legacy", nullptr, nullptr },
        "legacy.onnx",
        299,
        299,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        true,
        0,
    },
};

static const nsfw_model_profile_info *nsfw_get_model_profile_info(nsfw_model_profile_t profile)
{
    size_t i;

    for (i = 0; i < sizeof(kModelProfiles) / sizeof(kModelProfiles[0]); ++i) {
        if (kModelProfiles[i].profile == profile)
            return &kModelProfiles[i];
    }

    return &kModelProfiles[0];
}

static nsfw_tensor_layout nsfw_default_input_layout(nsfw_model_profile_t profile)
{
    return profile == NSFW_MODEL_PROFILE_LEGACY
        ? nsfw_tensor_layout::nhwc
        : nsfw_tensor_layout::nchw;
}

static void nsfw_repack_nhwc_to_nchw(const float *input,
                                     float       *output,
                                     int          width,
                                     int          height)
{
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            for (int c = 0; c < 3; ++c) {
                output[(c * height + y) * width + x] =
                    input[(y * width + x) * 3 + c];
            }
        }
    }
}

static std::string nsfw_ascii_fold_profile_name(const char *text)
{
    std::string value;

    if (!text)
        return value;

    value.reserve(std::strlen(text));
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c == '-' || c == '_' || c == '/' || c == '.' || c == ' ' )
            continue;
        value.push_back(static_cast<char>(std::tolower(c)));
    }

    return value;
}

/*****************************************************************************
 * Windows path helpers
 *****************************************************************************/

#ifdef _WIN32

static std::wstring nsfw_utf8_to_wide(const char *text)
{
    if (!text || text[0] == '\0')
        return std::wstring();

    const int len = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (len <= 0)
        return std::wstring();

    std::wstring wide(static_cast<size_t>(len), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, text, -1, &wide[0], len) <= 0)
        return std::wstring();
    wide.resize(static_cast<size_t>(len - 1));
    return wide;
}

static std::string nsfw_wide_to_utf8(const wchar_t *text)
{
    if (!text || text[0] == L'\0')
        return std::string();

    const int len = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0,
                                        nullptr, nullptr);
    if (len <= 0)
        return std::string();

    std::string utf8(static_cast<size_t>(len), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text, -1, &utf8[0], len, nullptr,
                            nullptr) <= 0) {
        return std::string();
    }
    utf8.resize(static_cast<size_t>(len - 1));
    return utf8;
}

static bool nsfw_get_module_directory(std::wstring *dir)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    wchar_t *slash = nullptr;

    if (!dir)
        return false;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&nsfw_get_module_directory),
                            &module)) {
        return false;
    }

    DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;

    slash = wcsrchr(path, L'\\');
    if (!slash)
        return false;

    *slash = L'\0';
    *dir = path;
    return true;
}

static std::string nsfw_module_sibling_path_utf8(const char *filename)
{
    std::wstring dir;
    std::string result;

    if (!filename || filename[0] == '\0')
        return result;

    if (!nsfw_get_module_directory(&dir))
        return result;

    std::wstring wide_name = nsfw_utf8_to_wide(filename);
    if (wide_name.empty())
        return result;

    dir += L'\\';
    dir += wide_name;
    result = nsfw_wide_to_utf8(dir.c_str());
    return result;
}

static void nsfw_preload_runtime_pattern(const std::wstring &dir,
                                         const wchar_t      *pattern,
                                         bool               *loaded_any)
{
    WIN32_FIND_DATAW find_data;
    HANDLE           find = INVALID_HANDLE_VALUE;
    std::wstring     search_path;

    if (!pattern)
        return;

    search_path = dir;
    search_path += L'\\';
    search_path += pattern;

    find = FindFirstFileW(search_path.c_str(), &find_data);
    if (find == INVALID_HANDLE_VALUE)
        return;

    do {
        if ((find_data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
            continue;

        std::wstring dll_path = dir;
        dll_path += L'\\';
        dll_path += find_data.cFileName;

        if (LoadLibraryExW(dll_path.c_str(), NULL, LOAD_WITH_ALTERED_SEARCH_PATH) != NULL &&
            loaded_any != nullptr) {
            *loaded_any = true;
        }
    } while (FindNextFileW(find, &find_data));

    FindClose(find);
}

static bool nsfw_preload_cuda_runtime_libraries(void)
{
    static bool attempted = false;
    static bool loaded_any = false;

    if (attempted)
        return loaded_any;
    attempted = true;

    std::wstring dir;
    if (!nsfw_get_module_directory(&dir))
        return false;

    const wchar_t *patterns[] = {
        L"onnxruntime_providers_shared.dll",
        L"onnxruntime_providers_cuda.dll",
        L"cublas*.dll",
        L"cudart*.dll",
        L"cudnn*.dll",
        L"cufft*.dll",
        L"curand*.dll",
        L"cusolver*.dll",
        L"cusparse*.dll",
        L"nv*.dll",
        L"zlibwapi.dll",
    };

    for (const wchar_t *pattern : patterns)
        nsfw_preload_runtime_pattern(dir, pattern, &loaded_any);

    return loaded_any;
}

static std::string nsfw_module_sibling_path_utf8(const wchar_t *filename)
{
    std::wstring dir;
    if (!filename || !nsfw_get_module_directory(&dir))
        return std::string();

    dir += L'\\';
    dir += filename;
    return nsfw_wide_to_utf8(dir.c_str());
}

static bool nsfw_file_exists_utf8(const char *path)
{
    std::wstring wide = nsfw_utf8_to_wide(path);
    if (wide.empty())
        return false;

    DWORD attrs = GetFileAttributesW(wide.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES &&
           (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

#else
static bool nsfw_get_module_directory(std::string *dir)
{
    Dl_info info;
    const char *path;
    const char *slash;

    if (!dir)
        return false;

    dir->clear();
    std::memset(&info, 0, sizeof(info));
    if (dladdr(reinterpret_cast<const void *>(&nsfw_get_module_directory),
               &info) == 0 ||
        info.dli_fname == nullptr || info.dli_fname[0] == '\0') {
        return false;
    }

    path = info.dli_fname;
    slash = std::strrchr(path, '/');
    if (!slash)
        slash = std::strrchr(path, '\\');
    if (!slash)
        return false;

    dir->assign(path, static_cast<size_t>(slash - path + 1));
    return true;
}

static std::string nsfw_module_sibling_path_utf8(const char *filename)
{
    std::string dir;
    std::string result;

    if (!filename || filename[0] == '\0')
        return result;

    if (!nsfw_get_module_directory(&dir))
        return result;

    result = dir;
    result += filename;
    return result;
}

static bool nsfw_file_exists_utf8(const char *path)
{
    struct stat st;

    if (path == nullptr || path[0] == '\0')
        return false;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static bool nsfw_preload_cuda_runtime_libraries(void)
{
    return false;
}

#endif

static std::string nsfw_resolve_default_model_path(const nsfw_model_profile_info *info)
{
    if (!info)
        return std::string();

    {
        std::string sibling = nsfw_module_sibling_path_utf8(info->runtime_filename);
        if (!sibling.empty() && nsfw_file_exists_utf8(sibling.c_str()))
            return sibling;
    }

#ifdef NSFW_MODEL_PATH_MARQO
    if (info->profile == NSFW_MODEL_PROFILE_MARQO &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_MARQO))
        return std::string(NSFW_MODEL_PATH_MARQO);
#endif
#ifdef NSFW_MODEL_PATH_ADAMCODD
    if (info->profile == NSFW_MODEL_PROFILE_ADAMCODD &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_ADAMCODD))
        return std::string(NSFW_MODEL_PATH_ADAMCODD);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_FALCONSAI))
        return std::string(NSFW_MODEL_PATH_FALCONSAI);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI_OFFICIAL
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_FALCONSAI_OFFICIAL))
        return std::string(NSFW_MODEL_PATH_FALCONSAI_OFFICIAL);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI_BASE
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI_BASE &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_FALCONSAI_BASE))
        return std::string(NSFW_MODEL_PATH_FALCONSAI_BASE);
#endif
#ifdef NSFW_MODEL_PATH_LEGACY
    if (info->profile == NSFW_MODEL_PROFILE_LEGACY &&
        nsfw_file_exists_utf8(NSFW_MODEL_PATH_LEGACY))
        return std::string(NSFW_MODEL_PATH_LEGACY);
#endif

#ifdef NSFW_DEFAULT_MODEL_PATH
    if (info->profile == NSFW_MODEL_PROFILE_MARQO &&
        nsfw_file_exists_utf8(NSFW_DEFAULT_MODEL_PATH))
        return std::string(NSFW_DEFAULT_MODEL_PATH);
#endif

    return std::string();
}

static std::string nsfw_resolve_model_path(const nsfw_config_t *config)
{
    const nsfw_model_profile_info *info;

    if (config && config->model_path && config->model_path[0] != '\0')
        return std::string(config->model_path);

    info = nsfw_get_model_profile_info(config ? config->model_profile
                                              : NSFW_MODEL_PROFILE_MARQO);
    {
        std::string sibling = nsfw_module_sibling_path_utf8(info->runtime_filename);
        if (!sibling.empty() && nsfw_file_exists_utf8(sibling.c_str()))
            return sibling;
    }

    return nsfw_resolve_default_model_path(info);
}

/*****************************************************************************
 * Preprocessing
 *****************************************************************************/

static int nsfw_preprocess_frame_internal(const uint8_t *frame_data,
                                          int            width,
                                          int            height,
                                          int            channels,
                                          float         *output,
                                          int            model_width,
                                          int            model_height,
                                          const float    *mean,
                                          const float    *stddev)
{
    if (!frame_data || !output || width <= 0 || height <= 0 ||
        channels < 3 || model_width <= 0 || model_height <= 0)
        return -1;

    const float x_scale = static_cast<float>(width)  / static_cast<float>(model_width);
    const float y_scale = static_cast<float>(height) / static_cast<float>(model_height);

    for (int y = 0; y < model_height; y++) {
        float src_y = y * y_scale;
        int   y0    = static_cast<int>(src_y);
        int   y1    = std::min(y0 + 1, height - 1);
        float yf    = src_y - static_cast<float>(y0);

        for (int x = 0; x < model_width; x++) {
            float src_x = x * x_scale;
            int   x0    = static_cast<int>(src_x);
            int   x1    = std::min(x0 + 1, width - 1);
            float xf    = src_x - static_cast<float>(x0);

            /* Bilinear interpolation for each colour channel. */
            for (int c = 0; c < 3; c++) {
                float v00 = static_cast<float>(
                    frame_data[(y0 * width + x0) * channels + c]);
                float v10 = static_cast<float>(
                    frame_data[(y0 * width + x1) * channels + c]);
                float v01 = static_cast<float>(
                    frame_data[(y1 * width + x0) * channels + c]);
                float v11 = static_cast<float>(
                    frame_data[(y1 * width + x1) * channels + c]);

                float v = v00 * (1.0f - xf) * (1.0f - yf)
                        + v10 * xf           * (1.0f - yf)
                        + v01 * (1.0f - xf) * yf
                        + v11 * xf           * yf;

                /* NHWC layout: interleaved channels. */
                v /= 255.0f;
                if (mean != nullptr && stddev != nullptr) {
                    float scale = stddev[c] != 0.0f ? stddev[c] : 1.0f;
                    v = (v - mean[c]) / scale;
                }
                output[(y * model_width + x) * 3 + c] = v;
            }
        }
    }

    return 0;
}

int nsfw_preprocess_frame(const uint8_t *frame_data,
                          int            width,
                          int            height,
                          int            channels,
                          float         *output,
                          int            model_width,
                          int            model_height)
{
    return nsfw_preprocess_frame_internal(frame_data, width, height,
                                          channels, output, model_width,
                                          model_height, nullptr, nullptr);
}

/*****************************************************************************
 * Threshold mapping
 *****************************************************************************/

float nsfw_sensitivity_to_threshold(nsfw_sensitivity_t sensitivity)
{
    switch (sensitivity) {
        case NSFW_SENSITIVITY_LOW:    return 0.70f;
        case NSFW_SENSITIVITY_MEDIUM: return 0.50f;
        case NSFW_SENSITIVITY_HIGH:   return 0.30f;
        default:                       return 0.50f;
    }
}

float nsfw_model_profile_normalize_score(nsfw_model_profile_t profile,
                                         float raw_score)
{
    float raw_midpoint = 0.50f;
    float numerator;
    float denominator;

    if (raw_score < 0.0f)
        raw_score = 0.0f;
    if (raw_score > 1.0f)
        raw_score = 1.0f;

    switch (profile) {
        case NSFW_MODEL_PROFILE_FALCONSAI:
        case NSFW_MODEL_PROFILE_FALCONSAI_BASE:
        case NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL:
            raw_midpoint = 0.02f;
            break;
        case NSFW_MODEL_PROFILE_MARQO:
        case NSFW_MODEL_PROFILE_ADAMCODD:
        case NSFW_MODEL_PROFILE_LEGACY:
        default:
            break;
    }

    /* Preserve score ordering while mapping each profile's operating
     * midpoint onto the shared UI midpoint of 0.5. */
    numerator = raw_score * (1.0f - raw_midpoint);
    denominator = numerator + (1.0f - raw_score) * raw_midpoint;
    return denominator > 0.0f ? numerator / denominator : 0.0f;
}

const char *nsfw_model_profile_name(nsfw_model_profile_t profile)
{
    return nsfw_get_model_profile_info(profile)->name;
}

int nsfw_model_profile_parse(const char *text, nsfw_model_profile_t *profile)
{
    std::string folded;
    size_t i;

    if (!profile)
        return 0;

    folded = nsfw_ascii_fold_profile_name(text);
    if (folded.empty()) {
        *profile = NSFW_MODEL_PROFILE_MARQO;
        return 1;
    }

    for (i = 0; i < sizeof(kModelProfiles) / sizeof(kModelProfiles[0]); ++i) {
        const nsfw_model_profile_info &info = kModelProfiles[i];
        size_t alias_index;

        if (folded == nsfw_ascii_fold_profile_name(info.name)) {
            *profile = info.profile;
            return 1;
        }

        for (alias_index = 0; alias_index < sizeof(info.aliases) / sizeof(info.aliases[0]); ++alias_index) {
            const char *alias = info.aliases[alias_index];
            if (!alias)
                continue;
            if (folded == nsfw_ascii_fold_profile_name(alias)) {
                *profile = info.profile;
                return 1;
            }
        }
    }

    *profile = NSFW_MODEL_PROFILE_MARQO;
    return 0;
}

void nsfw_config_set_model_profile(nsfw_config_t *config,
                                   nsfw_model_profile_t profile)
{
    const nsfw_model_profile_info *info;

    if (!config)
        return;

    info = nsfw_get_model_profile_info(profile);
    config->model_profile = info->profile;
    config->model_width = info->width;
    config->model_height = info->height;
}

/*****************************************************************************
 * Default config
 *****************************************************************************/

nsfw_config_t nsfw_config_default(void)
{
    nsfw_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.threshold    = 0.50f;
    nsfw_config_set_model_profile(&cfg, NSFW_MODEL_PROFILE_MARQO);
    cfg.model_path   = nullptr;
    return cfg;
}

/*****************************************************************************
 * ONNX Runtime backend (conditionally compiled)
 *****************************************************************************/

#ifdef NSFW_HAS_ONNXRUNTIME

typedef const OrtApiBase *(ORT_API_CALL *nsfw_ort_get_api_base_fn)(void);

static bool nsfw_onnxruntime_initialized(void)
{
#ifdef _WIN32
    static HMODULE s_module = NULL;
    static bool    s_ready = false;
    static bool    s_attempted = false;

    if (s_attempted)
        return s_ready;
    s_attempted = true;

    std::string sibling_dll_path = nsfw_module_sibling_path_utf8(L"onnxruntime.dll");
    if (!sibling_dll_path.empty()) {
        std::wstring sibling_dll_path_w = nsfw_utf8_to_wide(sibling_dll_path.c_str());
        if (!sibling_dll_path_w.empty())
            s_module = LoadLibraryExW(sibling_dll_path_w.c_str(), NULL,
                                      LOAD_WITH_ALTERED_SEARCH_PATH);
    }

    const char *dll_path = nullptr;
#ifdef NSFW_ONNXRUNTIME_DLL_PATH
    dll_path = NSFW_ONNXRUNTIME_DLL_PATH;
#endif

    if (!s_module && dll_path != nullptr && dll_path[0] != '\0')
        s_module = LoadLibraryExA(dll_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);

    if (!s_module)
        s_module = LoadLibraryA("onnxruntime.dll");
    if (!s_module)
        s_module = LoadLibraryW(L"C:\\Windows\\System32\\onnxruntime.dll");
    if (!s_module)
        return false;

    auto get_api_base = reinterpret_cast<nsfw_ort_get_api_base_fn>(
        GetProcAddress(s_module, "OrtGetApiBase"));
    if (!get_api_base) {
        FreeLibrary(s_module);
        s_module = NULL;
        return false;
    }

    const OrtApiBase *api_base = get_api_base();
    if (!api_base) {
        FreeLibrary(s_module);
        s_module = NULL;
        return false;
    }

    const OrtApi *api = nullptr;
    constexpr int kMaxBundledOrtApiVersion = 27;
    const int start_version = std::min(static_cast<int>(ORT_API_VERSION),
                                       kMaxBundledOrtApiVersion);
    for (int version = start_version; version >= 1; --version) {
        api = api_base->GetApi(static_cast<uint32_t>(version));
        if (api != nullptr)
            break;
    }
    if (!api) {
        FreeLibrary(s_module);
        s_module = NULL;
        return false;
    }

    Ort::InitApi(api);
    s_ready = true;
    return true;
#else
    static void *s_module = NULL;
    static bool s_ready = false;
    static bool s_attempted = false;
    static const char *const kCandidateNames[] = {
        "libonnxruntime.so",
        "libonnxruntime.so.1",
        "libonnxruntime.dylib",
        "libonnxruntime.1.dylib",
        "onnxruntime.so",
        "onnxruntime.dylib",
        NULL,
    };

    if (s_attempted)
        return s_ready;
    s_attempted = true;

    {
        std::string sibling_paths[] = {
            nsfw_module_sibling_path_utf8("libonnxruntime.so"),
            nsfw_module_sibling_path_utf8("libonnxruntime.so.1"),
            nsfw_module_sibling_path_utf8("libonnxruntime.dylib"),
            nsfw_module_sibling_path_utf8("libonnxruntime.1.dylib"),
        };

        for (const std::string &candidate : sibling_paths) {
            if (!candidate.empty())
                s_module = dlopen(candidate.c_str(), RTLD_NOW | RTLD_LOCAL);
            if (s_module != NULL)
                break;
        }
    }

#ifdef NSFW_ONNXRUNTIME_DLL_PATH
    if (!s_module && NSFW_ONNXRUNTIME_DLL_PATH[0] != '\0')
        s_module = dlopen(NSFW_ONNXRUNTIME_DLL_PATH, RTLD_NOW | RTLD_LOCAL);
#endif

    for (const char *const *name = kCandidateNames; !s_module && *name != NULL; ++name) {
        s_module = dlopen(*name, RTLD_NOW | RTLD_LOCAL);
    }

    if (!s_module)
        return false;

    auto get_api_base = reinterpret_cast<nsfw_ort_get_api_base_fn>(
        dlsym(s_module, "OrtGetApiBase"));
    if (!get_api_base) {
        dlclose(s_module);
        s_module = NULL;
        return false;
    }

    const OrtApiBase *api_base = get_api_base();
    if (!api_base) {
        dlclose(s_module);
        s_module = NULL;
        return false;
    }

    const OrtApi *api = nullptr;
    constexpr int kMaxBundledOrtApiVersion = 27;
    const int start_version = std::min(static_cast<int>(ORT_API_VERSION),
                                       kMaxBundledOrtApiVersion);
    for (int version = start_version; version >= 1; --version) {
        api = api_base->GetApi(static_cast<uint32_t>(version));
        if (api != nullptr)
            break;
    }
    if (!api) {
        dlclose(s_module);
        s_module = NULL;
        return false;
    }

    Ort::InitApi(api);
    s_ready = true;
    return true;
#endif
}

struct onnx_context {
    std::unique_ptr<Ort::Env>         env;
    std::unique_ptr<Ort::Session>     session;
    nsfw_model_profile_t              profile;
    int                               model_width;
    int                               model_height;
    nsfw_tensor_layout                input_layout;
    std::string                       provider_name;
    std::vector<float>                session_input_buffer;
    std::vector<std::string>          input_name_strings;
    std::vector<std::string>          output_name_strings;
    std::vector<const char *>         input_names;
    std::vector<const char *>         output_names;

    onnx_context()
        : env(nullptr)
        , session(nullptr)
        , profile(NSFW_MODEL_PROFILE_MARQO)
        , model_width(0)
        , model_height(0)
        , input_layout(nsfw_tensor_layout::nhwc)
        , provider_name("cpu")
    {}
};

enum class nsfw_onnx_provider_preference {
    auto_detect,
    cpu,
    cuda,
};

static std::string nsfw_get_env_string(const char *name)
{
    const char *value = std::getenv(name);
    if (!value)
        return std::string();
    return std::string(value);
}

static std::string nsfw_ascii_lower(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

static nsfw_onnx_provider_preference nsfw_get_provider_preference(void)
{
    std::string value = nsfw_ascii_lower(nsfw_get_env_string("NSFW_ONNX_PROVIDER"));

    if (value.empty())
        return nsfw_onnx_provider_preference::cpu;
    if (value == "auto")
        return nsfw_onnx_provider_preference::auto_detect;
    if (value == "cuda" || value == "gpu")
        return nsfw_onnx_provider_preference::cuda;
    return nsfw_onnx_provider_preference::cpu;
}

static int nsfw_get_cuda_device_id(void)
{
    std::string value = nsfw_get_env_string("NSFW_ONNX_CUDA_DEVICE_ID");
    char *end = nullptr;
    long parsed = 0;

    if (value.empty())
        return 0;

    parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || *end != '\0' || parsed < 0 || parsed > INT32_MAX)
        return 0;

    return static_cast<int>(parsed);
}

static bool nsfw_preload_cuda_runtime_libraries(void);

static bool onnx_try_enable_cuda(Ort::SessionOptions *opts, int device_id)
{
    if (!opts)
        return false;

    nsfw_preload_cuda_runtime_libraries();

    try {
        Ort::CUDAProviderOptions cuda_options;
        cuda_options.Update({
            {"device_id", std::to_string(device_id)},
        });
        opts->AppendExecutionProvider_CUDA_V2(*cuda_options);
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static const char *onnx_configure_execution_provider(Ort::SessionOptions *opts)
{
    nsfw_onnx_provider_preference preference = nsfw_get_provider_preference();
    bool try_cuda;

    if (!opts)
        return "cpu";

    if (preference == nsfw_onnx_provider_preference::cpu)
        return "cpu";

    try_cuda = preference == nsfw_onnx_provider_preference::cuda ||
               preference == nsfw_onnx_provider_preference::auto_detect;
    if (try_cuda && onnx_try_enable_cuda(opts, nsfw_get_cuda_device_id()))
        return "cuda";

    if (preference == nsfw_onnx_provider_preference::cuda) {
        std::fprintf(stderr,
                     "icop_core: CUDA execution provider unavailable, falling back to CPU\n");
    }

    return "cpu";
}

static nsfw_tensor_layout onnx_detect_input_layout(const onnx_context *oc)
{
    if (!oc || !oc->session)
        return nsfw_tensor_layout::nhwc;

    try {
        auto input_info = oc->session->GetInputTypeInfo(0);
        auto tensor_info = input_info.GetTensorTypeAndShapeInfo();
        auto dims = tensor_info.GetShape();

        if (dims.size() == 4) {
            if (dims[1] == 3)
                return nsfw_tensor_layout::nchw;
            if (dims[3] == 3)
                return nsfw_tensor_layout::nhwc;
        }
    } catch (const Ort::Exception &) {
    }

    return nsfw_default_input_layout(oc->profile);
}

static int onnx_load_model(void *ctx, const char *model_path)
{
    auto *oc = static_cast<onnx_context *>(ctx);
    if (!oc || !model_path || !nsfw_onnxruntime_initialized())
        return -1;

    try {
        oc->env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "icop");
    } catch (...) {
        return -1;
    }

    Ort::SessionOptions opts;
    opts.SetIntraOpNumThreads(1);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    oc->provider_name = onnx_configure_execution_provider(&opts);

    try {
#ifdef _WIN32
        /* Wide-char path required on Windows. */
        int len = MultiByteToWideChar(CP_UTF8, 0, model_path, -1, nullptr, 0);
        if (len <= 0) return -1;
        std::wstring wpath(static_cast<size_t>(len), 0);
        MultiByteToWideChar(CP_UTF8, 0, model_path, -1, &wpath[0], len);
        oc->session = std::make_unique<Ort::Session>(
            *oc->env, wpath.c_str(), opts);
#else
        oc->session = std::make_unique<Ort::Session>(
            *oc->env, model_path, opts);
#endif
    } catch (const Ort::Exception &) {
        return -1;
    }

    std::fprintf(stderr,
                 "icop_core: ONNX session ready with %s provider\n",
                 oc->provider_name.c_str());

    oc->input_layout = onnx_detect_input_layout(oc);

    /* Cache input/output names. */
    Ort::AllocatorWithDefaultOptions alloc;

    size_t n_in = oc->session->GetInputCount();
    oc->input_name_strings.resize(n_in);
    oc->input_names.resize(n_in);
    for (size_t i = 0; i < n_in; i++) {
        auto name = oc->session->GetInputNameAllocated(i, alloc);
        oc->input_name_strings[i] = name.get();
        oc->input_names[i] = oc->input_name_strings[i].c_str();
    }

    size_t n_out = oc->session->GetOutputCount();
    oc->output_name_strings.resize(n_out);
    oc->output_names.resize(n_out);
    for (size_t i = 0; i < n_out; i++) {
        auto name = oc->session->GetOutputNameAllocated(i, alloc);
        oc->output_name_strings[i] = name.get();
        oc->output_names[i] = oc->output_name_strings[i].c_str();
    }

    return 0;
}

static float nsfw_softmax_probability(const float *values, size_t count,
                                      size_t index)
{
    float max_value = 0.0f;
    float sum = 0.0f;
    size_t i;

    if (!values || count == 0 || index >= count)
        return 0.0f;

    max_value = values[0];
    for (i = 1; i < count; ++i) {
        if (values[i] > max_value)
            max_value = values[i];
    }

    for (i = 0; i < count; ++i)
        sum += std::exp(values[i] - max_value);

    if (sum <= 0.0f)
        return 0.0f;

    return std::exp(values[index] - max_value) / sum;
}

static int onnx_infer(void *ctx, const float *input, int input_size, float *output)
{
    auto *oc = static_cast<onnx_context *>(ctx);
    const float *tensor_input = input;
    std::array<int64_t, 4> input_shape = {};

    if (!oc->session) return -1;
    if (!input || !output || input_size != 3 * oc->model_width * oc->model_height)
        return -1;

    if (oc->input_layout == nsfw_tensor_layout::nchw) {
        input_shape = {
            1,
            3,
            static_cast<int64_t>(oc->model_height),
            static_cast<int64_t>(oc->model_width)
        };
        if (oc->session_input_buffer.size() != static_cast<size_t>(input_size))
            oc->session_input_buffer.resize(static_cast<size_t>(input_size));
        nsfw_repack_nhwc_to_nchw(input, oc->session_input_buffer.data(),
                                 oc->model_width, oc->model_height);
        tensor_input = oc->session_input_buffer.data();
    } else {
        input_shape = {
            1,
            static_cast<int64_t>(oc->model_height),
            static_cast<int64_t>(oc->model_width),
            3
        };
    }

    auto mem_info = Ort::MemoryInfo::CreateCpu(
        OrtArenaAllocator, OrtMemTypeDefault);

    auto input_tensor = Ort::Value::CreateTensor<float>(
        mem_info, const_cast<float *>(tensor_input),
        static_cast<size_t>(input_size),
        input_shape.data(), input_shape.size());

    try {
        auto outputs = oc->session->Run(
            Ort::RunOptions{nullptr},
            oc->input_names.data(), &input_tensor, 1,
            oc->output_names.data(),
            static_cast<size_t>(oc->output_names.size()));

        if (outputs.empty())
            return -1;

        auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
        size_t output_size = output_info.GetElementCount();
        const float *probs = outputs[0].GetTensorData<float>();
        const nsfw_model_profile_info *info = nsfw_get_model_profile_info(oc->profile);
        if (!probs || output_size == 0)
            return -1;

        float score = 0.0f;
        if (info->legacy_multiclass) {
            if (output_size < 5)
                return -1;
            score = nsfw_softmax_probability(probs, output_size, 1) +
                    nsfw_softmax_probability(probs, output_size, 3) +
                    nsfw_softmax_probability(probs, output_size, 4);
        } else {
            if (info->nsfw_index >= output_size)
                return -1;
            score = nsfw_softmax_probability(probs, output_size, info->nsfw_index);
        }

        if (score < 0.0f) score = 0.0f;
        if (score > 1.0f) score = 1.0f;
        *output = score;
    } catch (const Ort::Exception &) {
        return -1;
    }

    return 0;
}

static void onnx_destroy(void *ctx)
{
    delete static_cast<onnx_context *>(ctx);
}

#endif /* NSFW_HAS_ONNXRUNTIME */

/*****************************************************************************
 * Construction helpers
 *****************************************************************************/

nsfw_detector_t *nsfw_detector_create_with_backend(
    const nsfw_config_t         *config,
    const nsfw_backend_vtable_t *vtable)
{
    if (!config || !vtable || !vtable->load_model || !vtable->infer)
        return nullptr;

    /* Validate threshold range. */
    if (config->threshold < 0.0f || config->threshold > 1.0f)
        return nullptr;
    if (config->model_width <= 0 || config->model_height <= 0)
        return nullptr;

    auto *det = new (std::nothrow) nsfw_detector();
    if (!det) return nullptr;

    det->config = *config;
    if (config->model_path) {
        det->model_path_owned = config->model_path;
        det->config.model_path = det->model_path_owned.c_str();
    }

    det->backend = *vtable;
    det->preprocessed.resize(
        static_cast<size_t>(3 * config->model_width * config->model_height), 0.0f);

    /* Attempt to load the model through the backend. */
    if (vtable->load_model(vtable->ctx, det->config.model_path) != 0) {
        /* Backend load failed – clean up. */
        if (vtable->destroy) vtable->destroy(vtable->ctx);
        delete det;
        return nullptr;
    }

    return det;
}

nsfw_detector_t *nsfw_detector_create(const nsfw_config_t *config)
{
    std::string model_path = nsfw_resolve_model_path(config);
    if (!config || model_path.empty())
        return nullptr;

    nsfw_config_t resolved = *config;
    resolved.model_path = model_path.c_str();

#ifdef NSFW_HAS_ONNXRUNTIME
    auto *oc = new (std::nothrow) onnx_context();
    if (!oc) return nullptr;

    oc->profile      = resolved.model_profile;
    oc->model_width  = resolved.model_width;
    oc->model_height = resolved.model_height;

    nsfw_backend_vtable_t vtable;
    vtable.ctx        = oc;
    vtable.load_model = onnx_load_model;
    vtable.infer      = onnx_infer;
    vtable.destroy    = onnx_destroy;

    return nsfw_detector_create_with_backend(&resolved, &vtable);
#else
    /* ONNX Runtime not available – caller must use create_with_backend. */
    (void)config;
    return nullptr;
#endif
}

void nsfw_detector_destroy(nsfw_detector_t *detector)
{
    if (!detector) return;

    if (detector->backend.destroy)
        detector->backend.destroy(detector->backend.ctx);

    delete detector;
}

/*****************************************************************************
 * Classification
 *****************************************************************************/

nsfw_result_t nsfw_detector_classify(nsfw_detector_t *detector,
                                     const uint8_t   *frame_data,
                                     int              width,
                                     int              height,
                                     int              channels)
{
    nsfw_result_t result;
    std::memset(&result, 0, sizeof(result));

    if (!detector || !frame_data || width <= 0 || height <= 0 || channels < 3)
        return result;

    /* Preprocess the frame into the reusable buffer. */
    int preproc_sz = 3 * detector->config.model_width * detector->config.model_height;
    {
        const nsfw_model_profile_info *info =
            nsfw_get_model_profile_info(detector->config.model_profile);
        if (nsfw_preprocess_frame_internal(frame_data, width, height, channels,
                                           detector->preprocessed.data(),
                                           detector->config.model_width,
                                           detector->config.model_height,
                                           info->mean, info->stddev) != 0) {
            return result;
        }
    }

    /* Run inference. */
    float score = 0.0f;
    if (detector->backend.infer(detector->backend.ctx,
                                detector->preprocessed.data(),
                                preproc_sz, &score) != 0)
        return result;

    score = nsfw_model_profile_normalize_score(detector->config.model_profile,
                                               score);

    result.score     = score;
    result.threshold = detector->config.threshold;
    result.is_nsfw   = (score >= detector->config.threshold) ? 1 : 0;

    return result;
}
