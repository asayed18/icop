/*****************************************************************************
 * nsfw_window_icon.c: VLC window icon management (Windows only)
 *
 * Applies the ICOP active-window icon to visible VLC windows on Windows;
 * compiles to no-ops on Linux / macOS.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
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

#ifdef _WIN32
# include <vlc_common.h>
#endif

/* ================================================================== */
/*  VLC window icon  (Windows only, no-ops elsewhere)                   */
/* ================================================================== */

#ifdef _WIN32

/* ---- Windows implementation ---- */

#define NSFW_FILTER_ICON_RESOURCE_ID 101
#define NSFW_MAX_ICON_WINDOWS 16
#define NSFW_ICON_REFRESH_FRAMES 120

typedef struct nsfw_window_icon_entry_t
{
    HWND hwnd;
    HICON previous_big;
    HICON previous_small;
    bool big_changed;
    bool small_changed;
} nsfw_window_icon_entry_t;

static SRWLOCK g_window_icon_lock = SRWLOCK_INIT;
static unsigned g_window_icon_users = 0;
static HICON g_window_icon_big = NULL;
static HICON g_window_icon_small = NULL;
static nsfw_window_icon_entry_t
    g_window_icon_entries[NSFW_MAX_ICON_WINDOWS];
static size_t g_window_icon_entry_count = 0;

static bool NsfwSendWindowIcon(HWND hwnd, WPARAM size, HICON icon,
                                HICON *previous)
{
    DWORD_PTR result = 0;

    if (!IsWindow(hwnd))
        return false;

    if (!SendMessageTimeoutW(hwnd, WM_SETICON, size,
                             (LPARAM)(INT_PTR)icon,
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 200,
                             &result))
        return false;

    if (previous != NULL)
        *previous = (HICON)(INT_PTR)result;
    return true;
}

static nsfw_window_icon_entry_t *NsfwFindWindowIconEntry(HWND hwnd)
{
    size_t i;
    for (i = 0; i < g_window_icon_entry_count; ++i) {
        if (g_window_icon_entries[i].hwnd == hwnd)
            return &g_window_icon_entries[i];
    }
    return NULL;
}

static bool NsfwWindowAcceptsIcon(HWND hwnd)
{
    DWORD process_id = 0;
    LONG_PTR style;

    if (!IsWindowVisible(hwnd))
        return false;

    GetWindowThreadProcessId(hwnd, &process_id);
    if (process_id != GetCurrentProcessId())
        return false;

    style = GetWindowLongPtrW(hwnd, GWL_STYLE);
    return (style & WS_CAPTION) != 0;
}

static void NsfwApplyWindowIcon(HWND hwnd)
{
    nsfw_window_icon_entry_t *entry = NsfwFindWindowIconEntry(hwnd);
    bool new_entry = false;

    if (entry == NULL) {
        if (g_window_icon_entry_count >= NSFW_MAX_ICON_WINDOWS)
            return;
        entry = &g_window_icon_entries[g_window_icon_entry_count];
        memset(entry, 0, sizeof(*entry));
        entry->hwnd = hwnd;
        new_entry = true;
    }

    if (entry->big_changed) {
        NsfwSendWindowIcon(hwnd, ICON_BIG, g_window_icon_big, NULL);
    } else {
        entry->big_changed = NsfwSendWindowIcon(
            hwnd, ICON_BIG, g_window_icon_big, &entry->previous_big);
    }

    if (entry->small_changed) {
        NsfwSendWindowIcon(hwnd, ICON_SMALL, g_window_icon_small, NULL);
    } else {
        entry->small_changed = NsfwSendWindowIcon(
            hwnd, ICON_SMALL, g_window_icon_small, &entry->previous_small);
    }

    if (new_entry && (entry->big_changed || entry->small_changed)) {
        ++g_window_icon_entry_count;
        RedrawWindow(hwnd, NULL, NULL,
                     RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    }
}

static BOOL CALLBACK NsfwApplyWindowIconCallback(HWND hwnd, LPARAM data)
{
    VLC_UNUSED(data);
    if (NsfwWindowAcceptsIcon(hwnd))
        NsfwApplyWindowIcon(hwnd);
    return TRUE;
}

static bool NsfwLoadWindowIcons(void)
{
    HMODULE module = NULL;
    int big_width = GetSystemMetrics(SM_CXICON);
    int big_height = GetSystemMetrics(SM_CYICON);
    int small_width = GetSystemMetrics(SM_CXSMICON);
    int small_height = GetSystemMetrics(SM_CYSMICON);

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&NsfwLoadWindowIcons, &module))
        return false;

    g_window_icon_big = (HICON)LoadImageW(
        module, MAKEINTRESOURCEW(NSFW_FILTER_ICON_RESOURCE_ID), IMAGE_ICON,
        big_width, big_height, LR_DEFAULTCOLOR);
    g_window_icon_small = (HICON)LoadImageW(
        module, MAKEINTRESOURCEW(NSFW_FILTER_ICON_RESOURCE_ID), IMAGE_ICON,
        small_width, small_height, LR_DEFAULTCOLOR);

    if (g_window_icon_big == NULL || g_window_icon_small == NULL) {
        if (g_window_icon_big != NULL) DestroyIcon(g_window_icon_big);
        if (g_window_icon_small != NULL) DestroyIcon(g_window_icon_small);
        g_window_icon_big = NULL;
        g_window_icon_small = NULL;
        return false;
    }
    return true;
}

bool nsfw_plat_window_icon_enable(void)
{
    bool enabled = false;

    AcquireSRWLockExclusive(&g_window_icon_lock);
    if (g_window_icon_users > 0) {
        ++g_window_icon_users;
        enabled = true;
    } else if (NsfwLoadWindowIcons()) {
        g_window_icon_users = 1;
        EnumWindows(NsfwApplyWindowIconCallback, 0);
        enabled = true;
        fprintf(stderr,
                "icop: applied active icon to %zu VLC window(s)\n",
                g_window_icon_entry_count);
    } else {
        fprintf(stderr,
                "icop: unable to load embedded active-window icon\n");
    }
    ReleaseSRWLockExclusive(&g_window_icon_lock);
    return enabled;
}

void nsfw_plat_window_icon_refresh(void)
{
    AcquireSRWLockExclusive(&g_window_icon_lock);
    if (g_window_icon_users > 0)
        EnumWindows(NsfwApplyWindowIconCallback, 0);
    ReleaseSRWLockExclusive(&g_window_icon_lock);
}

void nsfw_plat_window_icon_disable(void)
{
    size_t i;

    AcquireSRWLockExclusive(&g_window_icon_lock);
    if (g_window_icon_users == 0) {
        ReleaseSRWLockExclusive(&g_window_icon_lock);
        return;
    }

    --g_window_icon_users;
    if (g_window_icon_users > 0) {
        ReleaseSRWLockExclusive(&g_window_icon_lock);
        return;
    }

    for (i = 0; i < g_window_icon_entry_count; ++i) {
        nsfw_window_icon_entry_t *entry = &g_window_icon_entries[i];

        if (entry->big_changed)
            NsfwSendWindowIcon(entry->hwnd, ICON_BIG,
                               entry->previous_big, NULL);
        if (entry->small_changed)
            NsfwSendWindowIcon(entry->hwnd, ICON_SMALL,
                               entry->previous_small, NULL);
        if (IsWindow(entry->hwnd))
            RedrawWindow(entry->hwnd, NULL, NULL,
                         RDW_FRAME | RDW_INVALIDATE | RDW_UPDATENOW);
    }

    memset(g_window_icon_entries, 0, sizeof(g_window_icon_entries));
    g_window_icon_entry_count = 0;
    DestroyIcon(g_window_icon_big);
    DestroyIcon(g_window_icon_small);
    g_window_icon_big = NULL;
    g_window_icon_small = NULL;
    fprintf(stderr, "icop: restored VLC window icon\n");
    ReleaseSRWLockExclusive(&g_window_icon_lock);
}

#else  /* !_WIN32 */

bool nsfw_plat_window_icon_enable(void)  { return false; }
void nsfw_plat_window_icon_disable(void) {}
void nsfw_plat_window_icon_refresh(void) {}

#endif /* _WIN32 */
