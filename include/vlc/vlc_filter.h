/*****************************************************************************
 * vlc_filter.h: VLC filter types (minimal stub for build)
 *****************************************************************************/
#ifndef VLC_FILTER_H
#define VLC_FILTER_H 1

#include <vlc_common.h>

/* Forward declaration of picture_t */
typedef struct picture_t picture_t;

/* Filter object */
typedef struct filter_t
{
    vlc_object_t *p_parent;
    picture_t *(*pf_video_filter)(struct filter_t *, picture_t *);
    void *p_sys;
} filter_t;

#endif /* VLC_FILTER_H */
