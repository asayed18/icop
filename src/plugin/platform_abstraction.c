/*****************************************************************************
 * platform_abstraction.c: OS platform abstraction layer implementation
 *
 * Hides Windows / Linux / macOS differences behind a uniform C API.
 * Every function has a single definition that compiles on all supported
 * platforms – no #ifdef leaks into the callers.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include "platform_abstraction.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
# include <windows.h>
# include <process.h>
#else
# if defined(__linux__) && !defined(_GNU_SOURCE)
#  define _GNU_SOURCE
# endif
# include <dlfcn.h>
# include <unistd.h>
#endif

/* ================================================================== */
/*  Plugin directory                                                    */
/* ================================================================== */

bool nsfw_plat_get_plugin_dir(char *path, size_t capacity)
{
#ifdef _WIN32
    HMODULE module = NULL;
    wchar_t wide[MAX_PATH];
    wchar_t *slash;

    if (!path || capacity == 0)
        return false;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&nsfw_plat_get_plugin_dir, &module))
        return false;

    if (GetModuleFileNameW(module, wide, MAX_PATH) == 0)
        return false;

    slash = wcsrchr(wide, L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';

    /* Convert wide char to UTF-8 */
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, -1, NULL, 0,
                                  NULL, NULL);
    if (len <= 0 || (size_t)len > capacity)
        return false;

    return WideCharToMultiByte(CP_UTF8, 0, wide, -1, path, (int)capacity,
                                NULL, NULL) > 0;
#else
    Dl_info info;
    const char *slash;

    if (!path || capacity == 0)
        return false;

    memset(&info, 0, sizeof(info));
    if (dladdr((const void *)&nsfw_plat_get_plugin_dir, &info) == 0 ||
        info.dli_fname == NULL || info.dli_fname[0] == '\0')
        return false;

    slash = strrchr(info.dli_fname, '/');
    if (!slash)
        slash = strrchr(info.dli_fname, '\\');
    if (!slash)
        return false;

    size_t len = (size_t)(slash - info.dli_fname + 1);
    if (len + 1 > capacity)
        return false;

    memcpy(path, info.dli_fname, len);
    path[len] = '\0';
    return true;
#endif
}

/* ================================================================== */
/*  Dynamic library loading                                             */
/* ================================================================== */

void *nsfw_plat_dlopen(const char *path)
{
#ifdef _WIN32
    wchar_t wide[MAX_PATH];
    int len;

    if (!path || path[0] == '\0')
        return NULL;

    len = MultiByteToWideChar(CP_UTF8, 0, path, -1, NULL, 0);
    if (len <= 0 || len > MAX_PATH)
        return NULL;

    MultiByteToWideChar(CP_UTF8, 0, path, -1, wide, MAX_PATH);
    return (void *)LoadLibraryExW(wide, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
#else
    if (!path || path[0] == '\0')
        return NULL;
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void *nsfw_plat_dlsym(void *module, const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

#ifdef _WIN32
    return (void *)GetProcAddress((HMODULE)module, name);
#else
    return dlsym(module ? module : RTLD_DEFAULT, name);
#endif
}

void nsfw_plat_dlclose(void *module)
{
    if (!module)
        return;

#ifdef _WIN32
    FreeLibrary((HMODULE)module);
#else
    dlclose(module);
#endif
}

void *nsfw_plat_lookup_vlc_sym(const char *name)
{
    if (!name || name[0] == '\0')
        return NULL;

#ifdef _WIN32
    HMODULE core = GetModuleHandleW(L"libvlccore.dll");
    if (!core)
        return NULL;
    return (void *)GetProcAddress(core, name);
#else
    return dlsym(RTLD_DEFAULT, name);
#endif
}

/* ================================================================== */
/*  Environment variables                                               */
/* ================================================================== */

void nsfw_plat_set_env(const char *name, const char *value)
{
    if (!name)
        return;

    if (!value)
        value = "";

#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void nsfw_plat_set_env_unsigned(const char *name, unsigned value)
{
    char buffer[32];
    snprintf(buffer, sizeof(buffer), "%u", value);
    nsfw_plat_set_env(name, buffer);
}

/* ================================================================== */
/*  File system helpers                                                 */
/* ================================================================== */

bool nsfw_plat_file_exists(const char *path)
{
    struct stat st;

    if (!path || path[0] == '\0')
        return false;

    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

uint64_t nsfw_plat_file_signature(const char *path)
{
    struct stat st;

    if (!path || path[0] == '\0')
        return 0;

    if (stat(path, &st) != 0)
        return 0;

    return ((uint64_t)(uint32_t)st.st_mtime << 32) ^
           (uint64_t)(uint32_t)(st.st_size & 0xffffffffu);
}

/* ================================================================== */
/*  System information                                                  */
/* ================================================================== */

unsigned nsfw_plat_cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    unsigned count = info.dwNumberOfProcessors;
    return count > 0 ? count : 1;
#else
    long count = sysconf(_SC_NPROCESSORS_ONLN);
    return count > 0 ? (unsigned)count : 1;
#endif
}

/* ================================================================== */
/*  Thread-safe one-shot flag                                           */
/* ================================================================== */

bool nsfw_plat_mark_once(volatile long *flag)
{
#ifdef _WIN32
    return InterlockedCompareExchange(flag, 1, 0) == 0;
#else
    /* Not a true atomic but good enough for diagnostic flags where a
     * double-print on thread contention is acceptable. */
    if (*flag)
        return false;
    *flag = 1;
    return true;
#endif
}


