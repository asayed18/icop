/*****************************************************************************
 * nsfw_filter_loader.c: tiny VLC-facing loader for the icop filter module
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <windows.h>
#include <wchar.h>

#define NSFW_IMPL_DLL_NAME L"icop_impl.dll"
#define NSFW_VLC_ENTRY_NAME "vlc_entry__3_0_0f"

#if defined(__GNUC__)
# define NSFW_LOADER_EXPORT __attribute__((dllexport))
# define NSFW_LOADER_CDECL  __cdecl
#else
# define NSFW_LOADER_EXPORT __declspec(dllexport)
# define NSFW_LOADER_CDECL  __cdecl
#endif

typedef int (NSFW_LOADER_CDECL *vlc_set_cb)(void *, void *, int, ...);
typedef int (NSFW_LOADER_CDECL *nsfw_vlc_entry_fn)(vlc_set_cb, void *);

static INIT_ONCE        g_init_once = INIT_ONCE_STATIC_INIT;
static HMODULE          g_impl_module = NULL;
static nsfw_vlc_entry_fn g_impl_entry = NULL;

static BOOL nsfw_get_loader_path(wchar_t *path, DWORD path_capacity)
{
    HMODULE self = NULL;

    if (!path || path_capacity == 0)
        return FALSE;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&nsfw_get_loader_path, &self)) {
        return FALSE;
    }

    return GetModuleFileNameW(self, path, path_capacity) > 0;
}

static BOOL nsfw_build_impl_path(wchar_t *path, DWORD path_capacity)
{
    wchar_t *slash = NULL;

    if (!nsfw_get_loader_path(path, path_capacity))
        return FALSE;

    slash = wcsrchr(path, L'\\');
    if (!slash)
        return FALSE;

    slash[1] = L'\0';

    if (wcslen(path) + wcslen(NSFW_IMPL_DLL_NAME) + 1 > path_capacity)
        return FALSE;

    return wcscat_s(path, path_capacity, NSFW_IMPL_DLL_NAME) == 0;
}

static BOOL CALLBACK nsfw_init_impl(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    wchar_t impl_path[MAX_PATH];

    (void)once;
    (void)param;
    (void)ctx;

    if (!nsfw_build_impl_path(impl_path, MAX_PATH))
        return FALSE;

    g_impl_module = LoadLibraryExW(impl_path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_impl_module)
        return FALSE;

    g_impl_entry = (nsfw_vlc_entry_fn)GetProcAddress(g_impl_module,
                                                     NSFW_VLC_ENTRY_NAME);
    return g_impl_entry != NULL;
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reason;
    (void)reserved;
    return TRUE;
}

NSFW_LOADER_EXPORT
int NSFW_LOADER_CDECL vlc_entry__3_0_0f(vlc_set_cb vlc_set, void *opaque)
{
    PVOID unused = NULL;

    if (!InitOnceExecuteOnce(&g_init_once, nsfw_init_impl, NULL, &unused))
        return -1;
    if (g_impl_entry == NULL)
        return -1;

    return g_impl_entry(vlc_set, opaque);
}
