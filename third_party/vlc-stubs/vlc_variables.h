#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef union vlc_value_t
{
    char *psz_string;
    int64_t i_int;
    float f_float;
    bool b_bool;
    void *p_address;
} vlc_value_t;

#define VLC_VAR_STRING    0x0001
#define VLC_VAR_INTEGER   0x0002
#define VLC_VAR_FLOAT     0x0004
#define VLC_VAR_DOINHERIT  0x0008
#define VLC_VAR_ISCOMMAND  0x0010
