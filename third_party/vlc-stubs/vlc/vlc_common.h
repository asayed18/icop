/*****************************************************************************
 * vlc_common.h: VLC common type definitions (minimal stub for build)
 *****************************************************************************/
#ifndef VLC_COMMON_H
#define VLC_COMMON_H 1

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

typedef int64_t vlc_tick_t;
#define CLOCK_FREQ ((vlc_tick_t)1000000)
#define VLC_TICK_INVALID ((vlc_tick_t)-1)

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

typedef void config_chain_t;

struct vlc_common_members
{
    const char *object_type;
    struct vlc_common_members *parent;
};

/* Opaque VLC object */
typedef struct vlc_common_members vlc_object_t;

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
