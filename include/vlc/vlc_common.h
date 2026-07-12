/*****************************************************************************
 * vlc_common.h: VLC common type definitions (minimal stub for build)
 *****************************************************************************/
#ifndef VLC_COMMON_H
#define VLC_COMMON_H 1

#include <stdint.h>
#include <stddef.h>

/* Opaque VLC object */
typedef struct vlc_object_t vlc_object_t;

/* Return codes */
#define VLC_SUCCESS  0
#define VLC_ENOMEM  -1
#define VLC_ETIMEOUT -2
#define VLC_EGENERIC -3

/* Convenience macro */
#define VLC_UNUSED(x) (void)(x)

/* String translation macros */
#define N_(s)  (s)
#define _(s)   (s)

/* Minimal runtime option access stubs. The real VLC host supplies these. */
char *var_InheritString(void *obj, const char *name);
int var_InheritInteger(void *obj, const char *name);
float var_InheritFloat(void *obj, const char *name);
bool var_InheritBool(void *obj, const char *name);

#endif /* VLC_COMMON_H */
