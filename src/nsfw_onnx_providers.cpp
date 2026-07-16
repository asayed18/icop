#include "nsfw_onnx_providers.h"
#include "nsfw_onnx_preload.h"
#include "nsfw_platform_utils.h"

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
#include <set>
#include <wchar.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
# if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
# endif
#include <dlfcn.h>
#include <dirent.h>
#endif

#ifdef NSFW_HAS_ONNXRUNTIME
#define ORT_API_MANUAL_INIT
#include <onnxruntime_cxx_api.h>
#endif

/*****************************************************************************
 * ONNX Runtime backend implementation
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

    std::string sibling_dll_path = nsfw_platform_module_sibling_path("onnxruntime.dll");
    if (!sibling_dll_path.empty()) {
        std::wstring sibling_dll_path_w = nsfw_platform_utf8_to_wide(sibling_dll_path.c_str());
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
            nsfw_platform_module_sibling_path("libonnxruntime.so"),
            nsfw_platform_module_sibling_path("libonnxruntime.so.1"),
            nsfw_platform_module_sibling_path("libonnxruntime.dylib"),
            nsfw_platform_module_sibling_path("libonnxruntime.1.dylib"),
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

    if (!s_module && !s_gpu_ort_directory.empty()) {
        static const char *gpu_ort_names[] = {
            "libonnxruntime.so", "libonnxruntime.so.1", nullptr
        };
        for (const char **n = gpu_ort_names; *n && !s_module; ++n) {
            std::string full = s_gpu_ort_directory + *n;
            s_module = dlopen(full.c_str(), RTLD_NOW | RTLD_LOCAL);
        }
    }

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
        , input_layout(NSFW_TENSOR_LAYOUT_NHWC)
        , provider_name("cpu")
    {}
};

/* ------------------------------------------------------------------ */
/*  Utilities used by provider configuration                           */
/* ------------------------------------------------------------------ */

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

static void nsfw_set_env_string(const char *name, const char *value)
{
    if (!name || !value)
        return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static bool nsfw_get_provider_preference_is_gpu(void)
{
    std::string value = nsfw_ascii_lower(nsfw_get_env_string("NSFW_ONNX_PROVIDER"));
    if (value.empty())
        return true;
    return value == "gpu";
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

/* ------------------------------------------------------------------ */
/*  Per-EP registration helpers                                        */
/* ------------------------------------------------------------------ */

static bool onnx_try_enable_cuda(Ort::SessionOptions *opts)
{
    if (!opts) return false;

    nsfw_plat_preload_cuda_runtime_libraries();

    try {
        Ort::CUDAProviderOptions cuda_options;
        cuda_options.Update({
            {"device_id", std::to_string(nsfw_get_cuda_device_id())},
        });
        opts->AppendExecutionProvider_CUDA_V2(*cuda_options);
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static bool onnx_try_enable_rocm(Ort::SessionOptions *opts)
{
    if (!opts) return false;

    nsfw_plat_preload_rocm_runtime_libraries();

    try {
        int device_id = nsfw_get_cuda_device_id();
        opts->AppendExecutionProvider("ROCM", {
            {"device_id", std::to_string(device_id)},
        });
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static bool onnx_try_enable_tensorrt(Ort::SessionOptions *opts)
{
    if (!opts) return false;

    try {
        int device_id = nsfw_get_cuda_device_id();
        opts->AppendExecutionProvider("TensorRT", {
            {"device_id", std::to_string(device_id)},
            {"trt_max_workspace_size", "1073741824"},
            {"trt_fp16_enable", "1"},
        });
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static bool onnx_try_enable_dml(Ort::SessionOptions *opts)
{
    if (!opts) return false;
    try {
        opts->AppendExecutionProvider("DML", {});
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static bool onnx_try_enable_coreml(Ort::SessionOptions *opts)
{
    if (!opts) return false;
    try {
        opts->AppendExecutionProvider("CoreML", {});
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

static bool onnx_try_enable_xnnpack(Ort::SessionOptions *opts)
{
    if (!opts) return false;
    try {
        opts->AppendExecutionProvider("XNNPACK", {});
        return true;
    } catch (const Ort::Exception &) {
        return false;
    }
}

/* ------------------------------------------------------------------ */
/*  Get available provider names from the loaded ORT build             */
/* ------------------------------------------------------------------ */

static std::set<std::string> onnx_get_available_providers(void)
{
    std::set<std::string> result;
    try {
        auto providers = Ort::GetAvailableProviders();
        for (const auto &p : providers) {
            std::string lower;
            lower.resize(p.size());
            std::transform(p.begin(), p.end(), lower.begin(),
                           [](unsigned char c) {
                               return static_cast<char>(std::tolower(c));
                           });
            result.insert(std::move(lower));
        }
    } catch (...) {}
    return result;
}

/* ------------------------------------------------------------------ */
/*  Multi-EP priority-ordered provider configuration                  */
/* ------------------------------------------------------------------ */

struct EpEntry {
    const char *name;
    const char *api_name;
    bool      (*try_enable)(Ort::SessionOptions *);
};

static const EpEntry kGpuPriority[] = {
#ifdef _WIN32
    {"dml",     "dmlexecutionprovider",        onnx_try_enable_dml},
    {"cuda",    "cudaexecutionprovider",       onnx_try_enable_cuda},
#elif defined(__APPLE__)
    {"coreml",  "coremlexecutionprovider",     onnx_try_enable_coreml},
#else
    {"tensorrt","tensorrtexecutionprovider",   onnx_try_enable_tensorrt},
    {"cuda",    "cudaexecutionprovider",       onnx_try_enable_cuda},
    {"rocm",    "rocmexecutionprovider",       onnx_try_enable_rocm},
#endif
    {"xnnpack", "xnnpackexecutionprovider",    onnx_try_enable_xnnpack},
};
static const size_t kGpuPriorityCount =
    sizeof(kGpuPriority) / sizeof(kGpuPriority[0]);

static std::string onnx_configure_providers(Ort::SessionOptions *opts)
{
    if (!opts)
        return "cpu";

    if (!nsfw_get_provider_preference_is_gpu()) {
        return "cpu";
    }

    std::set<std::string> available = onnx_get_available_providers();

    std::vector<std::string> registered;
    for (size_t i = 0; i < kGpuPriorityCount; ++i) {
        const auto &ep = kGpuPriority[i];
        if (available.find(ep.api_name) == available.end())
            continue;
        if (ep.try_enable && ep.try_enable(opts))
            registered.push_back(ep.name);
    }

    if (registered.empty()) {
        std::fprintf(stderr,
                     "icop_core: no GPU execution provider available, "
                     "falling back to CPU\n");
        nsfw_set_env_string("NSFW_ONNX_PROVIDER", "cpu");
        return "cpu";
    }

    registered.push_back("cpu");

    std::string joined = registered[0];
    for (size_t i = 1; i < registered.size(); ++i) {
        joined += ",";
        joined += registered[i];
    }

    std::fprintf(stderr, "icop_core: registered providers: %s\n",
                 joined.c_str());
    return joined;
}

/* ------------------------------------------------------------------ */
/*  Input layout detection                                             */
/* ------------------------------------------------------------------ */

static nsfw_tensor_layout onnx_detect_input_layout(const onnx_context *oc)
{
    if (!oc || !oc->session)
        return NSFW_TENSOR_LAYOUT_NHWC;

    try {
        auto input_info = oc->session->GetInputTypeInfo(0);
        auto tensor_info = input_info.GetTensorTypeAndShapeInfo();
        auto dims = tensor_info.GetShape();

        if (dims.size() == 4) {
            if (dims[1] == 3)
                return NSFW_TENSOR_LAYOUT_NCHW;
            if (dims[3] == 3)
                return NSFW_TENSOR_LAYOUT_NHWC;
        }
    } catch (const Ort::Exception &) {
    }

    return oc->profile == NSFW_MODEL_PROFILE_LEGACY
        ? NSFW_TENSOR_LAYOUT_NHWC
        : NSFW_TENSOR_LAYOUT_NCHW;
}

/* ------------------------------------------------------------------ */
/*  Softmax helper                                                     */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/*  Exported C API                                                     */
/* ------------------------------------------------------------------ */

void *nsfw_onnx_create_context(nsfw_model_profile_t profile,
                                int model_width, int model_height)
{
    auto *oc = new (std::nothrow) onnx_context();
    if (!oc)
        return nullptr;

    oc->profile = profile;
    oc->model_width = model_width;
    oc->model_height = model_height;

    return oc;
}

int nsfw_onnx_load_model(void *ctx, const char *model_path)
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
    oc->provider_name = onnx_configure_providers(&opts);

    try {
#ifdef _WIN32
        std::wstring wmodel_path = nsfw_platform_utf8_to_wide(model_path);
        oc->session = std::make_unique<Ort::Session>(
            *oc->env, wmodel_path.c_str(), opts);
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

int nsfw_onnx_infer(void *ctx, const float *input, int input_size, float *output)
{
    auto *oc = static_cast<onnx_context *>(ctx);
    const float *tensor_input = input;
    std::array<int64_t, 4> input_shape = {};

    if (!oc->session) return -1;
    if (!input || !output || input_size != 3 * oc->model_width * oc->model_height)
        return -1;

    if (oc->input_layout == NSFW_TENSOR_LAYOUT_NCHW) {
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

        const nsfw_model_profile_t &p = oc->profile;
        bool legacy = (p == NSFW_MODEL_PROFILE_LEGACY);

        if (!probs || output_size == 0)
            return -1;

        float score = 0.0f;
        if (legacy) {
            if (output_size < 5)
                return -1;
            score = nsfw_softmax_probability(probs, output_size, 1) +
                    nsfw_softmax_probability(probs, output_size, 3) +
                    nsfw_softmax_probability(probs, output_size, 4);
        } else {
            size_t nsfw_idx = (p == NSFW_MODEL_PROFILE_MARQO) ? 0 : 1;
            if (nsfw_idx >= output_size)
                return -1;
            score = nsfw_softmax_probability(probs, output_size, nsfw_idx);
        }

        if (score < 0.0f) score = 0.0f;
        if (score > 1.0f) score = 1.0f;
        *output = score;
    } catch (const Ort::Exception &) {
        return -1;
    }

    return 0;
}

void nsfw_onnx_destroy(void *ctx)
{
    delete static_cast<onnx_context *>(ctx);
}

int nsfw_onnx_has_provider(const char *provider_name)
{
    if (!provider_name || provider_name[0] == '\0')
        return 0;

    nsfw_plat_preload_cuda_runtime_libraries();
    nsfw_plat_preload_rocm_runtime_libraries();

    if (!nsfw_onnxruntime_initialized())
        return 0;

    try {
        std::vector<std::string> providers = Ort::GetAvailableProviders();
        std::string target = nsfw_ascii_lower(std::string(provider_name));
        for (const auto &p : providers) {
            std::string lower = nsfw_ascii_lower(p);
            if (lower == target)
                return 1;
            if (target == "cuda" && lower == "cudaexecutionprovider")
                return 1;
            if (target == "rocm" && lower == "rocmexecutionprovider")
                return 1;
            if (target == "cpu" && lower == "cpuexecutionprovider")
                return 1;
            if (target == "tensorrt" && lower == "tensorrtexecutionprovider")
                return 1;
        }
    } catch (...) {
    }
    return 0;
}

#else /* !NSFW_HAS_ONNXRUNTIME */

void *nsfw_onnx_create_context(nsfw_model_profile_t profile,
                                int model_width, int model_height)
{
    (void)profile;
    (void)model_width;
    (void)model_height;
    return nullptr;
}

int nsfw_onnx_load_model(void *ctx, const char *model_path)
{
    (void)ctx;
    (void)model_path;
    return -1;
}

int nsfw_onnx_infer(void *ctx, const float *input, int input_size, float *output)
{
    (void)ctx;
    (void)input;
    (void)input_size;
    (void)output;
    return -1;
}

void nsfw_onnx_destroy(void *ctx)
{
    (void)ctx;
}

int nsfw_onnx_has_provider(const char *provider_name)
{
    (void)provider_name;
    return 0;
}

#endif
