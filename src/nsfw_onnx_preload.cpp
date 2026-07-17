#include "nsfw_onnx_preload.h"
#include "nsfw_platform_utils.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*****************************************************************************
 * Internal platform helpers (unique to the preloader)
 *****************************************************************************/

#ifdef _WIN32

static bool nsfw_plat_get_module_directory(std::wstring *dir)
{
    HMODULE module = NULL;
    wchar_t path[MAX_PATH];
    wchar_t *slash = nullptr;

    if (!dir)
        return false;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&nsfw_plat_get_module_directory),
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

static void nsfw_plat_preload_runtime_pattern(const std::wstring &dir,
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

#else /* !_WIN32 */

static bool nsfw_plat_get_module_directory(std::string *dir)
{
    Dl_info info;
    const char *path;
    const char *slash;

    if (!dir)
        return false;

    dir->clear();
    std::memset(&info, 0, sizeof(info));
    if (dladdr(reinterpret_cast<const void *>(&nsfw_plat_get_module_directory),
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

static bool nsfw_plat_preload_one_library(const char *path)
{
    if (!path || path[0] == '\0')
        return false;

    void *handle = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (handle != nullptr) {
        std::fprintf(stderr, "icop_core: preloaded %s\n", path);
        return true;
    }
    return false;
}

static std::string nsfw_plat_find_ort_library_directory(void)
{
    static const char *kCandidateNames[] = {
        "libonnxruntime.so",
        "libonnxruntime.so.1",
        nullptr,
    };

    std::string dir = nsfw_platform_get_module_directory();

    if (!dir.empty()) {
        for (const char *const *name = kCandidateNames; *name != nullptr; ++name) {
            std::string candidate = dir + *name;
            struct stat st;
            if (stat(candidate.c_str(), &st) == 0 && S_ISREG(st.st_mode))
                return dir;
        }
    }

#ifdef NSFW_ONNXRUNTIME_DLL_PATH
    if (NSFW_ONNXRUNTIME_DLL_PATH[0] != '\0') {
        std::string path(NSFW_ONNXRUNTIME_DLL_PATH);
        auto slash = path.rfind('/');
        if (slash != std::string::npos)
            return path.substr(0, slash + 1);
    }
#endif

    for (const char *const *name = kCandidateNames; *name != nullptr; ++name) {
        void *handle = dlopen(*name, RTLD_NOW | RTLD_GLOBAL);
        if (handle != nullptr) {
            void *sym = dlsym(handle, "OrtGetApiBase");
            if (sym != nullptr) {
                Dl_info info;
                if (dladdr(sym, &info) != 0 && info.dli_fname != nullptr) {
                    std::string path(info.dli_fname);
                    auto slash = path.rfind('/');
                    if (slash != std::string::npos) {
                        return path.substr(0, slash + 1);
                    }
                }
            }
            dlclose(handle);
        }
    }

    return std::string();
}

#endif

/*****************************************************************************
 * CUDA / ROCm runtime preloaders
 *****************************************************************************/

std::string s_gpu_ort_directory;

#ifdef _WIN32

bool nsfw_plat_preload_cuda_runtime_libraries(void)
{
    static bool attempted = false;
    static bool loaded_any = false;

    if (attempted)
        return loaded_any;
    attempted = true;

    std::wstring dir;
    if (!nsfw_plat_get_module_directory(&dir))
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
        nsfw_plat_preload_runtime_pattern(dir, pattern, &loaded_any);

    return loaded_any;
}

bool nsfw_plat_preload_rocm_runtime_libraries(void)
{
    (void)0;
    return false;
}

#else /* !_WIN32 */

bool nsfw_plat_preload_cuda_runtime_libraries(void)
{
    static bool attempted = false;
    static bool loaded_any = false;

    if (attempted)
        return loaded_any;
    attempted = true;

    static const char *libs[] = {
        "libonnxruntime_providers_shared.so",
        "libcudart.so.13",
        "libcublas.so.13",
        "libcublasLt.so.13",
        "libcudnn.so.9",
        "libcufft.so.12",
        "libcurand.so.10",
        "libcusolver.so.12",
        "libcusparse.so.12",
        "libnvrtc.so.13",
        nullptr,
    };

    std::string dir;
    nsfw_plat_get_module_directory(&dir);

    std::string ort_dir = nsfw_plat_find_ort_library_directory();
    std::string provider_ort_dir = s_gpu_ort_directory;

    for (const char **lib = libs; *lib != nullptr; ++lib) {
        bool found = false;

        if (!dir.empty()) {
            std::string sibling = dir + *lib;
            if (nsfw_plat_preload_one_library(sibling.c_str())) {
                loaded_any = true;
                found = true;
            }
        }

        bool is_provider = (std::strncmp(*lib, "libonnxruntime_providers_", 25) == 0);
        if (!found && is_provider && !provider_ort_dir.empty()) {
            std::string candidate = provider_ort_dir + *lib;
            if (nsfw_plat_preload_one_library(candidate.c_str())) {
                loaded_any = true;
                found = true;
            }
        }

        if (!found && !ort_dir.empty()) {
            std::string ort_path = ort_dir + *lib;
            if (nsfw_plat_preload_one_library(ort_path.c_str())) {
                loaded_any = true;
                found = true;
            }
        }

        if (!found) {
            static const char *search_paths[] = {
                "/usr/local/cuda/lib64/",
                "/usr/local/cuda/targets/x86_64-linux/lib/",
                "/usr/lib/x86_64-linux-gnu/",
                "/usr/lib64/",
                nullptr,
            };

            for (const char **sp = search_paths; *sp != nullptr; ++sp) {
                std::string full = std::string(*sp) + *lib;
                if (nsfw_plat_preload_one_library(full.c_str())) {
                    loaded_any = true;
                    found = true;
                    break;
                }
            }
        }
    }

    std::fprintf(stderr, "icop_core: preloader loop done, loaded_any=%d\n", (int)loaded_any);

    {
        static const char *cuda_paths[] = {
            "/usr/local/cuda/lib64/",
            "/usr/local/cuda/targets/x86_64-linux/lib/",
            nullptr,
        };
        std::string inject;
        const char *existing = getenv("LD_LIBRARY_PATH");
        std::string existing_str = existing ? existing : "";

        for (const char **cp = cuda_paths; *cp; ++cp) {
            if (existing_str.find(*cp) != std::string::npos)
                continue;
            if (!inject.empty())
                inject += ":";
            inject += *cp;
        }
        if (!inject.empty()) {
            std::string new_ldpath = inject;
            if (!existing_str.empty()) {
                new_ldpath += ":";
                new_ldpath += existing_str;
            }
            setenv("LD_LIBRARY_PATH", new_ldpath.c_str(), 1);
            std::fprintf(stderr, "icop_core: injected CUDA paths into LD_LIBRARY_PATH\n");
        }
    }

    {
        static const char *verify_libs[] = {
            "libcublasLt.so.13",
            "libcublas.so.13",
            "libcudnn.so.9",
            nullptr,
        };
        for (const char **lib = verify_libs; *lib; ++lib) {
            dlerror();
            void *h = dlopen(*lib, RTLD_NOW);
            char *err = dlerror();
            if (h) {
                std::fprintf(stderr, "icop_core: soname verify dlopen(%s) OK\n", *lib);
                dlclose(h);
            } else {
                std::fprintf(stderr, "icop_core: soname verify dlopen(%s) FAILED: %s\n",
                             *lib, err ? err : "unknown error");
            }
        }
    }

    return loaded_any;
}

bool nsfw_plat_preload_rocm_runtime_libraries(void)
{
    static bool attempted = false;
    static bool loaded_any = false;

    if (attempted)
        return loaded_any;
    attempted = true;

    static const char *libs[] = {
        "libonnxruntime_providers_migraphx.so",
        "librocblas.so.4",
        "libhipblas.so.2",
        "librocsolver.so.3",
        "librocsparse.so.1",
        "librocfft.so.3",
        "libamdhip64.so.6",
        nullptr,
    };

    std::string dir;
    nsfw_plat_get_module_directory(&dir);

    std::string ort_dir = nsfw_plat_find_ort_library_directory();

    for (const char **lib = libs; *lib != nullptr; ++lib) {
        if (!dir.empty()) {
            std::string sibling = dir + *lib;
            if (nsfw_plat_preload_one_library(sibling.c_str())) {
                loaded_any = true;
                continue;
            }
        }

        if (!ort_dir.empty()) {
            std::string ort_path = ort_dir + *lib;
            if (nsfw_plat_preload_one_library(ort_path.c_str())) {
                loaded_any = true;
                continue;
            }
        }

        static const char *search_paths[] = {
            "/opt/rocm/lib/",
            "/usr/lib/x86_64-linux-gnu/",
            nullptr,
        };

        for (const char **sp = search_paths; *sp != nullptr; ++sp) {
            std::string full = std::string(*sp) + *lib;
            if (nsfw_plat_preload_one_library(full.c_str())) {
                loaded_any = true;
                break;
            }
        }
    }

    return loaded_any;
}

#endif
