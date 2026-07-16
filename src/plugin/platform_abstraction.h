/*****************************************************************************
 * platform_abstraction.h: OS platform abstraction layer
 *
 * Hides Windows / Linux / macOS differences behind a uniform C API.
 * No VLC types are used here – this layer only depends on the C runtime.
 * Callers include nsfw_filter.c and any other module that needs OS services.
 *****************************************************************************/

#ifndef VLC_NSFW_PLATFORM_ABSTRACTION_H
#define VLC_NSFW_PLATFORM_ABSTRACTION_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Plugin directory                                                   */
/* ------------------------------------------------------------------ */

/* Full path (with trailing separator) of the directory that contains
 * the running plugin shared library.  Returns false on failure. */
bool nsfw_plat_get_plugin_dir(char *path, size_t capacity);

/* ------------------------------------------------------------------ */
/*  Dynamic library loading  (dlopen / LoadLibrary)                    */
/* ------------------------------------------------------------------ */

/* Load a shared library by absolute path (UTF-8).  Returns NULL on failure. */
void *nsfw_plat_dlopen(const char *path);

/* Look up an exported symbol.  module may be NULL to search the global
 * symbol table (useful for VLC host symbols on Linux). */
void *nsfw_plat_dlsym(void *module, const char *name);

/* Unload a library loaded with nsfw_plat_dlopen. */
void  nsfw_plat_dlclose(void *module);

/* Shortcut: look up a symbol exported by the VLC host process. */
void *nsfw_plat_lookup_vlc_sym(const char *name);

/* ------------------------------------------------------------------ */
/*  Environment variables                                              */
/* ------------------------------------------------------------------ */

void nsfw_plat_set_env(const char *name, const char *value);
void nsfw_plat_set_env_unsigned(const char *name, unsigned value);

/* ------------------------------------------------------------------ */
/*  File system helpers                                                */
/* ------------------------------------------------------------------ */

/* True if path points to a regular file. */
bool     nsfw_plat_file_exists(const char *path);

/* Combine mtime and size into a single uint64_t for change detection. */
uint64_t nsfw_plat_file_signature(const char *path);

/* ------------------------------------------------------------------ */
/*  System information                                                 */
/* ------------------------------------------------------------------ */

/* Number of logical processors (0 on failure). */
unsigned nsfw_plat_cpu_count(void);

/* ------------------------------------------------------------------ */
/*  Thread-safe one-shot flag                                          */
/* ------------------------------------------------------------------ */

/* Returns true the first time it is called for a given flag,
 * false on every subsequent call.  Thread-safe on all platforms. */
bool nsfw_plat_mark_once(volatile long *flag);

/* ------------------------------------------------------------------ */
/*  VLC window icon  (Windows only; no-ops elsewhere)                  */
/* ------------------------------------------------------------------ */

/* Apply the ICOP active-window icon to all visible VLC windows.
 * Returns true if the icon was applied successfully. */
bool nsfw_plat_window_icon_enable(void);

/* Restore the original window icons. */
void nsfw_plat_window_icon_disable(void);

/* Re-apply the icon to any new windows (called periodically). */
void nsfw_plat_window_icon_refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* VLC_NSFW_PLATFORM_ABSTRACTION_H */
