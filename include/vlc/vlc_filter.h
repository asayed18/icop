/*****************************************************************************
 * vlc_filter.h: VLC filter types (minimal stub for build)
 *****************************************************************************/
#ifndef VLC_FILTER_H
#define VLC_FILTER_H 1

#include <vlc_common.h>
#include <vlc_picture.h>

/* Forward declaration of picture_t */
typedef struct picture_t picture_t;

/* Filter object */
typedef struct filter_t
{
    vlc_object_t *p_parent;
    picture_t *(*pf_video_filter)(struct filter_t *, picture_t *);
    void (*pf_flush)(struct filter_t *);
    struct filter_sys_t *p_sys;
    void *p_cfg;
    struct
    {
        video_format_t video;
    } fmt_in;
    struct
    {
        video_format_t video;
    } fmt_out;
} filter_t;

#endif /* VLC_FILTER_H */
