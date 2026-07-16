#pragma once

#include <stdint.h>

#include <vlc_common.h>
#include <vlc_fourcc.h>

enum
{
    Y_PLANE = 0,
    U_PLANE = 1,
    V_PLANE = 2,
    A_PLANE = 3,
};

typedef struct plane_t
{
    uint8_t *p_pixels;
    int      i_pitch;
    int      i_visible_pitch;
    int      i_visible_lines;
} plane_t;

typedef struct video_format_t
{
    vlc_fourcc_t i_chroma;
    int          i_width;
    int          i_height;
    int          i_visible_width;
    int          i_visible_height;
    int          i_frame_rate;
    int          i_frame_rate_base;
} video_format_t;

typedef struct picture_t
{
    video_format_t format;
    plane_t        p[4];
    vlc_tick_t     date;
} picture_t;
