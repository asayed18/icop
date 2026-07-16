/*****************************************************************************
 * frame_processor.c: Frame conversion, heuristic scoring, and blocking
 *
 * Pure pixel-level operations with no OS or VLC-plugin-internals
 * dependencies beyond the VLC picture API.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <vlc_common.h>

#include "frame_processor.h"
#include "nsfw_debug_overlay.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Internal pixel helpers                                              */
/* ================================================================== */

typedef struct
{
    bool little_endian;
    unsigned bits;
    unsigned storage_shift;
} nsfw_sample16_desc_t;

static inline unsigned SampleMask(unsigned bits)
{
    if (bits >= 16) return 0xFFFFu;
    return (1u << bits) - 1u;
}

static inline unsigned NeutralChromaSample(unsigned bits)
{
    if (bits == 0) return 0;
    return 1u << (bits - 1u);
}

static inline uint16_t ReadWord16(const uint8_t *ptr, bool little_endian)
{
    if (little_endian)
        return (uint16_t)(ptr[0] | ((uint16_t)ptr[1] << 8));
    return (uint16_t)(((uint16_t)ptr[0] << 8) | ptr[1]);
}

static inline void WriteWord16(uint8_t *ptr, uint16_t value, bool little_endian)
{
    if (little_endian) {
        ptr[0] = (uint8_t)(value & 0xFFu);
        ptr[1] = (uint8_t)(value >> 8);
    } else {
        ptr[0] = (uint8_t)(value >> 8);
        ptr[1] = (uint8_t)(value & 0xFFu);
    }
}

static inline unsigned DecodeSample16(const uint8_t *ptr, nsfw_sample16_desc_t desc)
{
    unsigned value = ReadWord16(ptr, desc.little_endian);
    value >>= desc.storage_shift;
    value &= SampleMask(desc.bits);
    return value;
}

static inline uint16_t EncodeSample16(unsigned sample, nsfw_sample16_desc_t desc)
{
    unsigned value = sample & SampleMask(desc.bits);
    return (uint16_t)(value << desc.storage_shift);
}

static inline uint8_t ScaleSampleToByte(unsigned sample, unsigned bits)
{
    unsigned max_value = SampleMask(bits);
    if (max_value == 0) return 0;
    return (uint8_t)((sample * 255u + (max_value / 2u)) / max_value);
}

static inline uint8_t DecodeSample16ToByte(const uint8_t *ptr,
                                            nsfw_sample16_desc_t desc)
{
    return ScaleSampleToByte(DecodeSample16(ptr, desc), desc.bits);
}

static inline uint8_t ClampByte(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

static inline uint8_t DebugClampByte(int value)
{
    if (value < 0) return 0;
    if (value > 255) return 255;
    return (uint8_t)value;
}

/* ================================================================== */
/*  Chroma / dimension utilities                                        */
/* ================================================================== */

int nsfw_fp_visible_width(const video_format_t *fmt)
{
    return fmt->i_visible_width > 0 ? fmt->i_visible_width : fmt->i_width;
}

int nsfw_fp_visible_height(const video_format_t *fmt)
{
    return fmt->i_visible_height > 0 ? fmt->i_visible_height : fmt->i_height;
}

int nsfw_fp_clamp_dimension(int value, int fallback)
{
    return value > 0 ? value : fallback;
}

bool nsfw_fp_is_opaque_hw_chroma(vlc_fourcc_t chroma)
{
    char f0 = (char)(chroma & 0xFFu);
    char f1 = (char)((chroma >> 8) & 0xFFu);
    char f2 = (char)((chroma >> 16) & 0xFFu);

    switch (chroma) {
        case VLC_CODEC_D3D9_OPAQUE:
        case VLC_CODEC_D3D9_OPAQUE_10B:
        case VLC_CODEC_D3D11_OPAQUE:
        case VLC_CODEC_D3D11_OPAQUE_10B:
        case VLC_CODEC_VAAPI_420:
        case VLC_CODEC_VAAPI_420_10BPP:
        case VLC_CODEC_CVPX_NV12:
        case VLC_CODEC_CVPX_I420:
        case VLC_CODEC_CVPX_BGRA:
        case VLC_CODEC_CVPX_P010:
            return true;
        default:
            break;
    }

    /*
     * Runtime VLC builds may expose additional opaque hardware FourCC values
     * not present in the local stub headers (e.g. VDV0 / VAOP families).
     * Treat those as hardware-only inputs so Open() can request software
     * conversion before block-style processing.
     */
    if ((f0 == 'V' && f1 == 'D' && f2 == 'V') ||
        (f0 == 'V' && f1 == 'A' && f2 == 'O')) {
        return true;
    }

    return false;
}

void nsfw_fp_fourcc_to_string(vlc_fourcc_t chroma, char out[5])
{
    unsigned i;

    if (!out) return;

    out[0] = (char)(chroma & 0xFFu);
    out[1] = (char)((chroma >> 8) & 0xFFu);
    out[2] = (char)((chroma >> 16) & 0xFFu);
    out[3] = (char)((chroma >> 24) & 0xFFu);
    out[4] = '\0';

    for (i = 0; i < 4; ++i) {
        if (!isprint((unsigned char)out[i]))
            out[i] = '.';
    }
}

/* ================================================================== */
/*  Planar 16-bit layout detection                                      */
/* ================================================================== */

static bool GetPlanar16Layout(vlc_fourcc_t chroma,
                               nsfw_sample16_desc_t *desc,
                               bool *swap_uv,
                               unsigned *u_step_x,
                               unsigned *u_step_y,
                               bool *has_alpha)
{
    if (!desc || !swap_uv || !u_step_x || !u_step_y || !has_alpha)
        return false;

    *swap_uv = false;
    *has_alpha = false;

    switch (chroma) {
        case VLC_CODEC_I420_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_16L:
            *desc = (nsfw_sample16_desc_t){ true, 16, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I420_16B:
            *desc = (nsfw_sample16_desc_t){ false, 16, 0 };
            *u_step_x = 1; *u_step_y = 1; return true;
        case VLC_CODEC_I422_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I422_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I422_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I422_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I422_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I422_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 1; *u_step_y = 0; return true;
        case VLC_CODEC_I444_9L:
            *desc = (nsfw_sample16_desc_t){ true, 9, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_9B:
            *desc = (nsfw_sample16_desc_t){ false, 9, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_12L:
            *desc = (nsfw_sample16_desc_t){ true, 12, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_12B:
            *desc = (nsfw_sample16_desc_t){ false, 12, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_16L:
            *desc = (nsfw_sample16_desc_t){ true, 16, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_I444_16B:
            *desc = (nsfw_sample16_desc_t){ false, 16, 0 };
            *u_step_x = 0; *u_step_y = 0; return true;
        case VLC_CODEC_YUVA_444_10L:
            *desc = (nsfw_sample16_desc_t){ true, 10, 0 };
            *u_step_x = 0; *u_step_y = 0;
            *has_alpha = true; return true;
        case VLC_CODEC_YUVA_444_10B:
            *desc = (nsfw_sample16_desc_t){ false, 10, 0 };
            *u_step_x = 0; *u_step_y = 0;
            *has_alpha = true; return true;
        default:
            return false;
    }
}

/* ================================================================== */
/*  Heuristic scoring – skin-tone detection in YCbCr space              */
/* ================================================================== */

static inline bool IsSkinYCbCr(unsigned y, unsigned cb, unsigned cr)
{
    return y > 80 && cb >= 85 && cb <= 135 && cr >= 135 && cr <= 180;
}

static float ScorePlanarYCbCr(const picture_t *pic, bool swap_uv,
                               unsigned u_step_x, unsigned u_step_y)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < y_plane->i_visible_lines; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < y_plane->i_visible_pitch; x += 2) {
            unsigned yy = y_row[x];
            unsigned uu = u_row[x >> u_step_x];
            unsigned vv = v_row[x >> u_step_x];

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScorePlanarYCbCr16(const picture_t *pic,
                                 bool swap_uv, unsigned u_step_x,
                                 unsigned u_step_y, nsfw_sample16_desc_t desc)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];
    const int width  = pic->p[Y_PLANE].i_visible_pitch;
    const int height = pic->p[Y_PLANE].i_visible_lines;
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)x * 2), desc);
            unsigned uu = DecodeSample16ToByte(
                u_row + ((size_t)(x >> u_step_x) * 2), desc);
            unsigned vv = DecodeSample16ToByte(
                v_row + ((size_t)(x >> u_step_x) * 2), desc);

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScoreP010(const picture_t *pic)
{
    const plane_t *y_plane  = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const int width  = nsfw_fp_visible_width(&pic->format);
    const int height = nsfw_fp_visible_height(&pic->format);
    const nsfw_sample16_desc_t desc = { true, 10, 6 };
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            size_t chroma = (size_t)(x >> 1) * 4;
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)x * 2), desc);
            unsigned uu = DecodeSample16ToByte(uv_row + chroma, desc);
            unsigned vv = DecodeSample16ToByte(uv_row + chroma + 2, desc);

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScoreSemiPlanarYCbCr(const picture_t *pic, bool swap_uv)
{
    const plane_t *y_plane  = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const int width  = pic->p[Y_PLANE].i_visible_pitch;
    const int height = pic->p[Y_PLANE].i_visible_lines;
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < height; y += 2) {
        const uint8_t *y_row = y_plane->p_pixels + y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < width; x += 2) {
            size_t chroma = (size_t)(x >> 1) * 2;
            unsigned yy = y_row[x];
            unsigned uu = uv_row[chroma + (swap_uv ? 1 : 0)];
            unsigned vv = uv_row[chroma + (swap_uv ? 0 : 1)];

            if (IsSkinYCbCr(yy, uu, vv))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

static float ScorePackedRGB(const picture_t *pic, int pixel_size,
                             int r_offset, int g_offset, int b_offset)
{
    const plane_t *plane = &pic->p[0];
    unsigned hits = 0;
    unsigned samples = 0;

    for (int y = 0; y < plane->i_visible_lines; y += 2) {
        const uint8_t *row = plane->p_pixels + y * plane->i_pitch;
        for (int x = 0; x < plane->i_visible_pitch; x += pixel_size * 2) {
            const uint8_t *px = row + x;
            unsigned r = px[r_offset];
            unsigned g = px[g_offset];
            unsigned b = px[b_offset];

            unsigned yy = (77u * r + 150u * g + 29u * b) >> 8;
            unsigned cb = 128u + ((-43 * (int)r - 85 * (int)g + 128 * (int)b) >> 8);
            unsigned cr = 128u + ((128 * (int)r - 107 * (int)g - 21 * (int)b) >> 8);

            if (IsSkinYCbCr(yy, cb, cr))
                hits++;
            samples++;
        }
    }

    return samples ? (float)hits / (float)samples : 0.0f;
}

float nsfw_fp_heuristic_score(const picture_t *pic)
{
    vlc_fourcc_t chroma;
    nsfw_sample16_desc_t desc;
    bool swap_uv;
    unsigned u_step_x, u_step_y;
    bool has_alpha;

    if (!pic) return 0.0f;

    chroma = pic->format.i_chroma;

    if (GetPlanar16Layout(chroma, &desc, &swap_uv,
                          &u_step_x, &u_step_y, &has_alpha))
        return ScorePlanarYCbCr16(pic, swap_uv, u_step_x, u_step_y, desc);

    switch (chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:     return ScorePlanarYCbCr(pic, false, 1, 1);
        case VLC_CODEC_YV12:     return ScorePlanarYCbCr(pic, true, 1, 1);
        case VLC_CODEC_I422:     return ScorePlanarYCbCr(pic, false, 1, 0);
        case VLC_CODEC_I444:     return ScorePlanarYCbCr(pic, false, 0, 0);
        case VLC_CODEC_YUVA:     return ScorePlanarYCbCr(pic, false, 1, 1);
        case VLC_CODEC_RGB24:    return ScorePackedRGB(pic, 3, 2, 1, 0);
        case VLC_CODEC_RGB32:    return ScorePackedRGB(pic, 4, 2, 1, 0);
        case VLC_CODEC_RGBA:     return ScorePackedRGB(pic, 4, 0, 1, 2);
        case VLC_CODEC_ARGB:     return ScorePackedRGB(pic, 4, 1, 2, 3);
        case VLC_CODEC_BGRA:     return ScorePackedRGB(pic, 4, 2, 1, 0);
        case VLC_CODEC_NV12:     return ScoreSemiPlanarYCbCr(pic, false);
        case VLC_CODEC_NV21:     return ScoreSemiPlanarYCbCr(pic, true);
        case VLC_CODEC_P010:     return ScoreP010(pic);
        default:                 return 0.0f;
    }
}

/* ================================================================== */
/*  Internal YCbCr → RGB for packing                                    */
/* ================================================================== */

static inline void YCbCrToRGB(unsigned y, unsigned cb, unsigned cr,
                               uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)cb - 128;
    int e = (int)cr - 128;

    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;

    *r = ClampByte(rr);
    *g = ClampByte(gg);
    *b = ClampByte(bb);
}

/* ================================================================== */
/*  RGB packing helpers                                                 */
/* ================================================================== */

static void PackPlanarFrame(const picture_t *pic,
                             int src_width, int src_height,
                             int dst_width, int dst_height,
                             bool swap_uv,
                             unsigned u_step_x, unsigned u_step_y,
                             uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (src_y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (src_y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            unsigned yy = y_row[src_x];
            unsigned uu = u_row[src_x >> u_step_x];
            unsigned vv = v_row[src_x >> u_step_x];

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackPlanarFrame16(const picture_t *pic,
                               int src_width, int src_height,
                               int dst_width, int dst_height,
                               bool swap_uv,
                               unsigned u_step_x, unsigned u_step_y,
                               nsfw_sample16_desc_t desc,
                               uint8_t *rgb)
{
    const plane_t *y_plane = &pic->p[Y_PLANE];
    const plane_t *u_plane = &pic->p[swap_uv ? V_PLANE : U_PLANE];
    const plane_t *v_plane = &pic->p[swap_uv ? U_PLANE : V_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *u_row = u_plane->p_pixels + (src_y >> u_step_y) * u_plane->i_pitch;
        const uint8_t *v_row = v_plane->p_pixels + (src_y >> u_step_y) * v_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)src_x * 2), desc);
            unsigned uu = DecodeSample16ToByte(
                u_row + ((size_t)(src_x >> u_step_x) * 2), desc);
            unsigned vv = DecodeSample16ToByte(
                v_row + ((size_t)(src_x >> u_step_x) * 2), desc);

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackP010Frame(const picture_t *pic,
                           int src_width, int src_height,
                           int dst_width, int dst_height,
                           uint8_t *rgb)
{
    const plane_t *y_plane  = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];
    const nsfw_sample16_desc_t desc = { true, 10, 6 };

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (src_y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            size_t chroma = (size_t)(src_x >> 1) * 4;
            unsigned yy = DecodeSample16ToByte(y_row + ((size_t)src_x * 2), desc);
            unsigned uu = DecodeSample16ToByte(uv_row + chroma, desc);
            unsigned vv = DecodeSample16ToByte(uv_row + chroma + 2, desc);

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackSemiPlanarFrame(const picture_t *pic,
                                 int src_width, int src_height,
                                 int dst_width, int dst_height,
                                 bool swap_uv,
                                 uint8_t *rgb)
{
    const plane_t *y_plane  = &pic->p[Y_PLANE];
    const plane_t *uv_plane = &pic->p[U_PLANE];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *y_row = y_plane->p_pixels + src_y * y_plane->i_pitch;
        const uint8_t *uv_row = uv_plane->p_pixels + (src_y >> 1) * uv_plane->i_pitch;

        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            size_t chroma = (size_t)(src_x >> 1) * 2;
            unsigned yy = y_row[src_x];
            unsigned uu = uv_row[chroma + (swap_uv ? 1 : 0)];
            unsigned vv = uv_row[chroma + (swap_uv ? 0 : 1)];

            uint8_t r, g, b;
            YCbCrToRGB(yy, uu, vv, &r, &g, &b);
            size_t dst = ((size_t)y * dst_width + (size_t)x) * 3;
            rgb[dst + 0] = r;
            rgb[dst + 1] = g;
            rgb[dst + 2] = b;
        }
    }
}

static void PackPackedFrame(const picture_t *pic,
                             int src_width, int src_height,
                             int dst_width, int dst_height,
                             int pixel_size,
                             int r_offset, int g_offset, int b_offset,
                             uint8_t *rgb)
{
    const plane_t *plane = &pic->p[0];

    for (int y = 0; y < dst_height; y++) {
        int src_y = (int)(((int64_t)y * src_height) / dst_height);
        const uint8_t *row = plane->p_pixels + src_y * plane->i_pitch;
        for (int x = 0; x < dst_width; x++) {
            int src_x = (int)(((int64_t)x * src_width) / dst_width);
            const uint8_t *px = row + ((size_t)src_x * pixel_size);
            size_t pixel = ((size_t)y * dst_width + (size_t)x) * 3;
            rgb[pixel + 0] = px[r_offset];
            rgb[pixel + 1] = px[g_offset];
            rgb[pixel + 2] = px[b_offset];
        }
    }
}

/* ================================================================== */
/*  Public RGB packing entry point                                      */
/* ================================================================== */

int nsfw_fp_pack_to_rgb(const picture_t *pic,
                         uint8_t *rgb, size_t rgb_capacity,
                         int target_width, int target_height,
                         int *out_width, int *out_height)
{
    vlc_fourcc_t chroma;
    int src_width, src_height;

    if (!pic || !rgb || !out_width || !out_height)
        return -1;

    chroma = pic->format.i_chroma;
    src_width  = nsfw_fp_visible_width(&pic->format);
    src_height = nsfw_fp_visible_height(&pic->format);
    if (src_width <= 0 || src_height <= 0)
        return -1;

    *out_width  = nsfw_fp_clamp_dimension(target_width, src_width);
    *out_height = nsfw_fp_clamp_dimension(target_height, src_height);

    size_t needed = (size_t)(*out_width) * (size_t)(*out_height) * 3;
    if (rgb_capacity < needed)
        return -1;

    /* Try 16-bit planar formats first */
    {
        nsfw_sample16_desc_t desc;
        bool swap_uv;
        unsigned u_step_x, u_step_y;
        bool has_alpha;

        if (GetPlanar16Layout(chroma, &desc, &swap_uv,
                              &u_step_x, &u_step_y, &has_alpha)) {
            PackPlanarFrame16(pic, src_width, src_height,
                              *out_width, *out_height,
                              swap_uv, u_step_x, u_step_y, desc, rgb);
            return 0;
        }
    }

    switch (chroma) {
        case VLC_CODEC_I420:
        case VLC_CODEC_J420:
            PackPlanarFrame(pic, src_width, src_height,
                            *out_width, *out_height, false, 1, 1, rgb);
            return 0;
        case VLC_CODEC_YV12:
            PackPlanarFrame(pic, src_width, src_height,
                            *out_width, *out_height, true, 1, 1, rgb);
            return 0;
        case VLC_CODEC_I422:
            PackPlanarFrame(pic, src_width, src_height,
                            *out_width, *out_height, false, 1, 0, rgb);
            return 0;
        case VLC_CODEC_I444:
        case VLC_CODEC_YUVA:
            PackPlanarFrame(pic, src_width, src_height,
                            *out_width, *out_height, false, 0, 0, rgb);
            return 0;
        case VLC_CODEC_NV12:
            PackSemiPlanarFrame(pic, src_width, src_height,
                                *out_width, *out_height, false, rgb);
            return 0;
        case VLC_CODEC_NV21:
            PackSemiPlanarFrame(pic, src_width, src_height,
                                *out_width, *out_height, true, rgb);
            return 0;
        case VLC_CODEC_RGB24:
            PackPackedFrame(pic, src_width, src_height,
                            *out_width, *out_height, 3, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_RGB32:
            PackPackedFrame(pic, src_width, src_height,
                            *out_width, *out_height, 4, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_RGBA:
            PackPackedFrame(pic, src_width, src_height,
                            *out_width, *out_height, 4, 0, 1, 2, rgb);
            return 0;
        case VLC_CODEC_ARGB:
            PackPackedFrame(pic, src_width, src_height,
                            *out_width, *out_height, 4, 1, 2, 3, rgb);
            return 0;
        case VLC_CODEC_BGRA:
            PackPackedFrame(pic, src_width, src_height,
                            *out_width, *out_height, 4, 2, 1, 0, rgb);
            return 0;
        case VLC_CODEC_P010:
            PackP010Frame(pic, src_width, src_height,
                          *out_width, *out_height, rgb);
            return 0;
        default:
            return -1;
    }
}

/* ================================================================== */
/*  Blocking effects – blackout / blur / warning watermark               */
/* ================================================================== */

static void BlackoutPlane(plane_t *plane, uint8_t value)
{
    for (int y = 0; y < plane->i_visible_lines; y++)
        memset(plane->p_pixels + y * plane->i_pitch, value,
               (size_t)plane->i_visible_pitch);
}

static void BlackoutPlane16(plane_t *plane, uint16_t value, bool little_endian)
{
    for (int y = 0; y < plane->i_visible_lines; y++) {
        uint8_t *row = plane->p_pixels + y * plane->i_pitch;
        for (int x = 0; x + 1 < plane->i_visible_pitch; x += 2)
            WriteWord16(row + x, value, little_endian);
    }
}

static void BlackoutPlanarFrame16(picture_t *pic, nsfw_sample16_desc_t desc,
                                   bool has_alpha)
{
    uint16_t chroma = EncodeSample16(NeutralChromaSample(desc.bits), desc);
    BlackoutPlane16(&pic->p[Y_PLANE], EncodeSample16(0, desc), desc.little_endian);
    BlackoutPlane16(&pic->p[U_PLANE], chroma, desc.little_endian);
    BlackoutPlane16(&pic->p[V_PLANE], chroma, desc.little_endian);
    if (has_alpha)
        BlackoutPlane16(&pic->p[A_PLANE],
                         EncodeSample16(SampleMask(desc.bits), desc),
                         desc.little_endian);
}

static void BlackoutP010Frame(picture_t *pic)
{
    const nsfw_sample16_desc_t desc = { true, 10, 6 };
    uint16_t chroma = EncodeSample16(NeutralChromaSample(desc.bits), desc);
    BlackoutPlane16(&pic->p[Y_PLANE], EncodeSample16(0, desc), true);
    BlackoutPlane16(&pic->p[U_PLANE], chroma, true);
}

static void BlackoutSemiPlanarFrame(picture_t *pic)
{
    BlackoutPlane(&pic->p[Y_PLANE], 0x00);
    for (int y = 0; y < pic->p[U_PLANE].i_visible_lines; y++)
        memset(pic->p[U_PLANE].p_pixels + y * pic->p[U_PLANE].i_pitch, 0x80,
               (size_t)pic->p[U_PLANE].i_visible_pitch);
}

static void FillColorRect(plane_t *plane, unsigned pixel_stride,
                           int x0, int y0, int width, int height,
                           const uint8_t *color)
{
    int plane_width, plane_height;

    if (!plane || pixel_stride == 0 || !color || width <= 0 || height <= 0)
        return;

    plane_width  = plane->i_visible_pitch / (int)pixel_stride;
    plane_height = plane->i_visible_lines;
    if (plane_width <= 0 || plane_height <= 0)
        return;

    if (x0 < 0) { width += x0; x0 = 0; }
    if (y0 < 0) { height += y0; y0 = 0; }
    if (x0 >= plane_width || y0 >= plane_height) return;
    if (x0 + width  > plane_width)  width  = plane_width  - x0;
    if (y0 + height > plane_height) height = plane_height - y0;
    if (width <= 0 || height <= 0) return;

    for (int y = 0; y < height; ++y) {
        uint8_t *row = plane->p_pixels + (size_t)(y0 + y) * plane->i_pitch +
                       (size_t)x0 * pixel_stride;
        for (int x = 0; x < width; ++x)
            memcpy(row + (size_t)x * pixel_stride, color, pixel_stride);
    }
}

static void PixelatePlane(plane_t *plane, unsigned pixel_stride,
                           unsigned block_size)
{
    int width, height;
    int x, y;

    if (!plane || pixel_stride == 0) return;
    width  = plane->i_visible_pitch / (int)pixel_stride;
    height = plane->i_visible_lines;
    if (width <= 0 || height <= 0) return;
    if (block_size == 0) block_size = 1;

    for (y = 0; y < height; y += (int)block_size) {
        int block_height = (y + (int)block_size < height)
            ? (int)block_size : (height - y);

        for (x = 0; x < width; x += (int)block_size) {
            int block_width = (x + (int)block_size < width)
                ? (int)block_size : (width - x);
            const uint8_t *sample = plane->p_pixels +
                (size_t)y * plane->i_pitch + (size_t)x * pixel_stride;

            for (int row = 0; row < block_height; ++row) {
                uint8_t *dst = plane->p_pixels +
                    (size_t)(y + row) * plane->i_pitch +
                    (size_t)x * pixel_stride;
                for (int col = 0; col < block_width; ++col)
                    memmove(dst + (size_t)col * pixel_stride, sample, pixel_stride);
            }
        }
    }
}

static unsigned FastBlurBlockSize(const picture_t *pic)
{
    int width, height, min_dim;

    if (!pic) return 16;
    width  = nsfw_fp_visible_width(&pic->format);
    height = nsfw_fp_visible_height(&pic->format);
    if (width <= 0 || height <= 0) return 16;

    min_dim = width < height ? width : height;
    unsigned block_size = (unsigned)(min_dim / 36);
    if (block_size < 8) block_size = 8;
    if (block_size > 24) block_size = 24;
    return block_size;
}

static void FastBlurFrame(picture_t *pic)
{
    nsfw_sample16_desc_t desc;
    bool swap_uv;
    unsigned u_step_x, u_step_y;
    bool has_alpha;
    unsigned block_size;

    if (!pic) return;
    block_size = FastBlurBlockSize(pic);

    if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                           &u_step_x, &u_step_y, &has_alpha)) {
        (void)swap_uv; (void)u_step_x; (void)u_step_y;
        PixelatePlane(&pic->p[Y_PLANE], 2, block_size);
        PixelatePlane(&pic->p[U_PLANE], 2, block_size);
        PixelatePlane(&pic->p[V_PLANE], 2, block_size);
        if (has_alpha)
            PixelatePlane(&pic->p[A_PLANE], 2, block_size);
        return;
    }

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420: case VLC_CODEC_J420:
        case VLC_CODEC_YV12: case VLC_CODEC_I422: case VLC_CODEC_I444:
        case VLC_CODEC_YUVA:
            PixelatePlane(&pic->p[Y_PLANE], 1, block_size);
            PixelatePlane(&pic->p[U_PLANE], 1, block_size);
            PixelatePlane(&pic->p[V_PLANE], 1, block_size);
            if (pic->format.i_chroma == VLC_CODEC_YUVA)
                PixelatePlane(&pic->p[A_PLANE], 1, block_size);
            break;
        case VLC_CODEC_NV12: case VLC_CODEC_NV21:
            PixelatePlane(&pic->p[Y_PLANE], 1, block_size);
            PixelatePlane(&pic->p[U_PLANE], 2, block_size);
            break;
        case VLC_CODEC_RGB24:
            PixelatePlane(&pic->p[0], 3, block_size);
            break;
        case VLC_CODEC_RGB32: case VLC_CODEC_RGBA:
        case VLC_CODEC_BGRA: case VLC_CODEC_ARGB:
            PixelatePlane(&pic->p[0], 4, block_size);
            break;
        case VLC_CODEC_P010:
            PixelatePlane(&pic->p[Y_PLANE], 2, block_size);
            PixelatePlane(&pic->p[U_PLANE], 4, block_size);
            break;
        default:
            /* Fall through to blackout */
            BlackoutPlane(&pic->p[Y_PLANE], 0x00);
            BlackoutPlane(&pic->p[U_PLANE], 0x80);
            BlackoutPlane(&pic->p[V_PLANE], 0x80);
            break;
    }
}

static void DrawWarningWatermarkPlane(plane_t *plane, unsigned pixel_stride,
                                       int frame_width, int frame_height,
                                       const uint8_t *color)
{
    int size, margin, x0, y0, half, stroke, symbol_width, symbol_height;

    if (!plane || !color || pixel_stride == 0 ||
        frame_width <= 0 || frame_height <= 0)
        return;

    size = (frame_width < frame_height ? frame_width : frame_height) / 8;
    if (size < 20) size = 20;
    if (size > 72) size = 72;

    margin = size / 4;
    if (margin < 4) margin = 4;

    x0 = frame_width - size - margin;
    y0 = frame_height - size - margin;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;

    half = size / 2;
    stroke = size / 12;
    if (stroke < 2) stroke = 2;

    for (int row = 0; row < size; ++row) {
        int span = (half > 0 && size > 1)
            ? (half * row) / (size - 1) : 0;
        int left = half - span;
        int right = half + span;
        if (left < 0) left = 0;
        if (right >= size) right = size - 1;

        FillColorRect(plane, pixel_stride, x0 + left, y0 + row,
                      stroke, 1, color);
        FillColorRect(plane, pixel_stride, x0 + right - stroke + 1,
                      y0 + row, stroke, 1, color);
        if (row >= size - stroke)
            FillColorRect(plane, pixel_stride, x0 + left, y0 + row,
                          right - left + 1, 1, color);
    }

    symbol_width = size / 9;
    if (symbol_width < 2) symbol_width = 2;
    symbol_height = size / 4;
    FillColorRect(plane, pixel_stride, x0 + half - symbol_width / 2,
                  y0 + size / 3, symbol_width, symbol_height, color);
    FillColorRect(plane, pixel_stride, x0 + half - symbol_width / 2,
                  y0 + (size * 3) / 4, symbol_width, symbol_width, color);
}

static void WarningWatermarkFrame(picture_t *pic)
{
    static const uint8_t kRedY8[]    = { 0x4C };
    static const uint8_t kRedU8[]    = { 0x55 };
    static const uint8_t kRedV8[]    = { 0xFF };
    static const uint8_t kRedP010Y[] = { 0x00, 0x4C };
    static const uint8_t kRedP010UV[]= { 0x00, 0x55, 0x00, 0xFF };
    static const uint8_t kRedNV12[]  = { 0x55, 0xFF };
    static const uint8_t kRedNV21[]  = { 0xFF, 0x55 };
    static const uint8_t kRedRGB24[] = { 0x18, 0x18, 0xE0 };
    static const uint8_t kRedBGRX[]  = { 0x18, 0x18, 0xE0, 0xFF };
    static const uint8_t kRedRGBA[]  = { 0xE0, 0x18, 0x18, 0xFF };
    static const uint8_t kRedARGB[]  = { 0xFF, 0xE0, 0x18, 0x18 };
    nsfw_sample16_desc_t desc;
    bool swap_uv, has_alpha;
    unsigned u_step_x, u_step_y;
    int frame_width, frame_height;

    if (!pic) return;
    frame_width  = nsfw_fp_visible_width(&pic->format);
    frame_height = nsfw_fp_visible_height(&pic->format);
    if (frame_width <= 0 || frame_height <= 0) return;

    if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                           &u_step_x, &u_step_y, &has_alpha)) {
        uint8_t red_y16[2], red_u16[2], red_v16[2];
        (void)u_step_x; (void)u_step_y; (void)has_alpha;
        WriteWord16(red_y16, EncodeSample16(0x4C, desc), desc.little_endian);
        WriteWord16(red_u16, EncodeSample16(0x55, desc), desc.little_endian);
        WriteWord16(red_v16, EncodeSample16(0xFF, desc), desc.little_endian);
        DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 2, frame_width, frame_height, red_y16);
        DrawWarningWatermarkPlane(&pic->p[U_PLANE], 2,
                                  pic->p[U_PLANE].i_visible_pitch / 2,
                                  pic->p[U_PLANE].i_visible_lines,
                                  swap_uv ? red_v16 : red_u16);
        DrawWarningWatermarkPlane(&pic->p[V_PLANE], 2,
                                  pic->p[V_PLANE].i_visible_pitch / 2,
                                  pic->p[V_PLANE].i_visible_lines,
                                  swap_uv ? red_u16 : red_v16);
        return;
    }

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420: case VLC_CODEC_J420: case VLC_CODEC_I422:
        case VLC_CODEC_I444:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 1, frame_width, frame_height, kRedY8);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 1,
                                      pic->p[U_PLANE].i_visible_pitch,
                                      pic->p[U_PLANE].i_visible_lines, kRedU8);
            DrawWarningWatermarkPlane(&pic->p[V_PLANE], 1,
                                      pic->p[V_PLANE].i_visible_pitch,
                                      pic->p[V_PLANE].i_visible_lines, kRedV8);
            break;
        case VLC_CODEC_YV12:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 1, frame_width, frame_height, kRedY8);
            DrawWarningWatermarkPlane(&pic->p[V_PLANE], 1,
                                      pic->p[V_PLANE].i_visible_pitch,
                                      pic->p[V_PLANE].i_visible_lines, kRedU8);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 1,
                                      pic->p[U_PLANE].i_visible_pitch,
                                      pic->p[U_PLANE].i_visible_lines, kRedV8);
            break;
        case VLC_CODEC_YUVA:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 1, frame_width, frame_height, kRedY8);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 1,
                                      pic->p[U_PLANE].i_visible_pitch,
                                      pic->p[U_PLANE].i_visible_lines, kRedU8);
            DrawWarningWatermarkPlane(&pic->p[V_PLANE], 1,
                                      pic->p[V_PLANE].i_visible_pitch,
                                      pic->p[V_PLANE].i_visible_lines, kRedV8);
            break;
        case VLC_CODEC_NV12:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 1, frame_width, frame_height, kRedY8);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 2,
                                      pic->p[U_PLANE].i_visible_pitch / 2,
                                      pic->p[U_PLANE].i_visible_lines, kRedNV12);
            break;
        case VLC_CODEC_NV21:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 1, frame_width, frame_height, kRedY8);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 2,
                                      pic->p[U_PLANE].i_visible_pitch / 2,
                                      pic->p[U_PLANE].i_visible_lines, kRedNV21);
            break;
        case VLC_CODEC_RGB24:
            DrawWarningWatermarkPlane(&pic->p[0], 3, frame_width, frame_height, kRedRGB24);
            break;
        case VLC_CODEC_RGB32: case VLC_CODEC_BGRA:
            DrawWarningWatermarkPlane(&pic->p[0], 4, frame_width, frame_height, kRedBGRX);
            break;
        case VLC_CODEC_RGBA:
            DrawWarningWatermarkPlane(&pic->p[0], 4, frame_width, frame_height, kRedRGBA);
            break;
        case VLC_CODEC_ARGB:
            DrawWarningWatermarkPlane(&pic->p[0], 4, frame_width, frame_height, kRedARGB);
            break;
        case VLC_CODEC_P010:
            DrawWarningWatermarkPlane(&pic->p[Y_PLANE], 2, frame_width, frame_height, kRedP010Y);
            DrawWarningWatermarkPlane(&pic->p[U_PLANE], 4,
                                      pic->p[U_PLANE].i_visible_pitch / 4,
                                      pic->p[U_PLANE].i_visible_lines, kRedP010UV);
            break;
        default:
            break;
    }
}

static void BlackoutFrameContent(picture_t *pic)
{
    nsfw_sample16_desc_t desc;
    bool swap_uv, has_alpha;
    unsigned u_step_x, u_step_y;

    if (!pic) return;

    if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                           &u_step_x, &u_step_y, &has_alpha)) {
        BlackoutPlanarFrame16(pic, desc, has_alpha);
        return;
    }

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420: case VLC_CODEC_J420:
        case VLC_CODEC_YV12: case VLC_CODEC_I422: case VLC_CODEC_I444:
            BlackoutPlane(&pic->p[Y_PLANE], 0x00);
            BlackoutPlane(&pic->p[U_PLANE], 0x80);
            BlackoutPlane(&pic->p[V_PLANE], 0x80);
            break;
        case VLC_CODEC_YUVA:
            BlackoutPlane(&pic->p[Y_PLANE], 0x00);
            BlackoutPlane(&pic->p[U_PLANE], 0x80);
            BlackoutPlane(&pic->p[V_PLANE], 0x80);
            BlackoutPlane(&pic->p[A_PLANE], 0xFF);
            break;
        case VLC_CODEC_NV12: case VLC_CODEC_NV21:
            BlackoutSemiPlanarFrame(pic);
            break;
        case VLC_CODEC_RGB24: case VLC_CODEC_RGB32:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++)
                memset(pic->p[0].p_pixels + y * pic->p[0].i_pitch, 0x00,
                       (size_t)pic->p[0].i_visible_pitch);
            break;
        case VLC_CODEC_RGBA: case VLC_CODEC_ARGB: case VLC_CODEC_BGRA:
            for (int y = 0; y < pic->p[0].i_visible_lines; y++) {
                uint8_t *row = pic->p[0].p_pixels + y * pic->p[0].i_pitch;
                for (int x = 0; x < pic->p[0].i_visible_pitch; x += 4) {
                    row[x + 0] = 0x00; row[x + 1] = 0x00;
                    row[x + 2] = 0x00; row[x + 3] = 0xFF;
                }
            }
            break;
        case VLC_CODEC_P010:
            BlackoutP010Frame(pic);
            break;
        default:
            break;
    }
}

/* ================================================================== */
/*  Public block-frame entry point                                      */
/* ================================================================== */

void nsfw_fp_block_frame(picture_t *pic, nsfw_block_style_t style)
{
    if (!pic) return;

    switch (style) {
        case NSFW_BLOCK_STYLE_BLUR:
            FastBlurFrame(pic);
            return;
        case NSFW_BLOCK_STYLE_WARNING:
            WarningWatermarkFrame(pic);
            return;
        case NSFW_BLOCK_STYLE_BLACK:
        default:
            BlackoutFrameContent(pic);
            return;
    }
}

/* ================================================================== */
/*  Debug overlay                                                       */
/* ================================================================== */

static void RgbToYuv(uint8_t red, uint8_t green, uint8_t blue,
                      uint8_t *y, uint8_t *u, uint8_t *v)
{
    *y = DebugClampByte((77 * red + 150 * green + 29 * blue) >> 8);
    *u = DebugClampByte(128 + ((-43 * red - 85 * green + 128 * blue) >> 8));
    *v = DebugClampByte(128 + ((128 * red - 107 * green - 21 * blue) >> 8));
}

static float SrgbToLinear(float value)
{
    if (value <= 0.04045f) return value / 12.92f;
    return powf((value + 0.055f) / 1.055f, 2.4f);
}

static float PqEncode(float value)
{
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 32.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 128.0f;
    const float c3 = 2392.0f / 128.0f;

    if (value <= 0.0f) return 0.0f;
    if (value > 1.0f) value = 1.0f;
    float powered = powf(value, m1);
    return powf((c1 + c2 * powered) / (1.0f + c3 * powered), m2);
}

static void EncodeP010OverlayColor(uint8_t red, uint8_t green, uint8_t blue,
                                    bool pq,
                                    uint8_t y_out[2], uint8_t uv_out[4])
{
    float r = red / 255.0f;
    float g = green / 255.0f;
    float b = blue / 255.0f;

    if (pq) {
        float r_linear = SrgbToLinear(r);
        float g_linear = SrgbToLinear(g);
        float b_linear = SrgbToLinear(b);
        float r_2020 = 0.6274f * r_linear + 0.3293f * g_linear + 0.0433f * b_linear;
        float g_2020 = 0.0691f * r_linear + 0.9195f * g_linear + 0.0114f * b_linear;
        float b_2020 = 0.0164f * r_linear + 0.0880f * g_linear + 0.8956f * b_linear;
        r = PqEncode(r_2020 * 0.0203f);
        g = PqEncode(g_2020 * 0.0203f);
        b = PqEncode(b_2020 * 0.0203f);
    }

    float y  = 0.2627f * r + 0.6780f * g + 0.0593f * b;
    float cb = (b - y) / 1.8814f;
    float cr = (r - y) / 1.4746f;
    float y_code  = 64.0f + 876.0f * y;
    float u_code  = 512.0f + 896.0f * cb;
    float v_code  = 512.0f + 896.0f * cr;

    if (y_code < 64.0f) y_code = 64.0f;
    if (y_code > 940.0f) y_code = 940.0f;
    if (u_code < 64.0f) u_code = 64.0f;
    if (u_code > 960.0f) u_code = 960.0f;
    if (v_code < 64.0f) v_code = 64.0f;
    if (v_code > 960.0f) v_code = 960.0f;

    WriteWord16(y_out, (uint16_t)((uint16_t)(y_code + 0.5f) << 6), true);
    WriteWord16(uv_out, (uint16_t)((uint16_t)(u_code + 0.5f) << 6), true);
    WriteWord16(uv_out + 2, (uint16_t)((uint16_t)(v_code + 0.5f) << 6), true);
}

static void DrawDebugTextPlane(plane_t *plane, unsigned pixel_stride,
                                int layout_width, int layout_height,
                                const uint8_t *background,
                                const uint8_t *foreground,
                                float score, float threshold)
{
    static const uint8_t kGlyphs[][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
        { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
        { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 },
        { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
        { 0, 0, 0, 0, 2 }, { 1, 2, 2, 4, 4 },
    };
    char text[16];
    int plane_width;
    int plane_height;
    int base_scale, scale_x, scale_y;
    int margin_x, margin_y;
    int panel_width, bar_width, bar_height, panel_height;
    int fill_width;
    int x0, y0;
    size_t length;

    if (!plane || !background || !foreground ||
        layout_width <= 0 || layout_height <= 0)
        return;

    plane_width  = plane->i_visible_pitch / (int)pixel_stride;
    plane_height = plane->i_visible_lines;
    if (plane_width <= 0 || plane_height <= 0) return;

    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;

    snprintf(text, sizeof(text), "%.3f/%.3f", score, threshold);
    length = strlen(text);

    base_scale = (layout_width < layout_height ? layout_width : layout_height) / 160;
    if (base_scale < 2) base_scale = 2;
    if (base_scale > 6) base_scale = 6;

    scale_x = (base_scale * plane_width + layout_width / 2) / layout_width;
    scale_y = (base_scale * plane_height + layout_height / 2) / layout_height;
    if (scale_x < 1) scale_x = 1;
    if (scale_y < 1) scale_y = 1;

    margin_x = scale_x * 3;
    margin_y = scale_y * 3;
    panel_width = (int)length * scale_x * 4 + margin_x * 2;
    bar_width = (int)length * scale_x * 4;
    bar_height = scale_y < 2 ? 2 : scale_y;
    panel_height = scale_y * 5 + margin_y * 3 + bar_height;
    x0 = margin_x;
    y0 = margin_y;

    FillColorRect(plane, pixel_stride, x0, y0, panel_width, panel_height, background);
    for (size_t index = 0; index < length; ++index) {
        int glyph = DebugGlyphIndex(text[index]);
        int glyph_x = x0 + margin_x + (int)index * scale_x * 4;
        if (glyph < 0) continue;
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if (kGlyphs[glyph][row] & (1u << (2 - column)))
                    FillColorRect(plane, pixel_stride,
                                  glyph_x + column * scale_x,
                                  y0 + margin_y + row * scale_y,
                                  scale_x, scale_y, foreground);
            }
        }
    }

    if (threshold > 0.001f)
        fill_width = (int)((bar_width * score) / threshold + 0.5f);
    else
        fill_width = bar_width;
    if (fill_width > bar_width) fill_width = bar_width;
    FillColorRect(plane, pixel_stride, x0 + margin_x,
                  y0 + margin_y * 2 + scale_y * 5,
                  fill_width, bar_height, foreground);
}

/* ================================================================== */
/*  Public debug overlay entry point                                    */
/* ================================================================== */

void nsfw_fp_debug_overlay(picture_t *pic, float score, float threshold)
{
    nsfw_sample16_desc_t desc;
    bool swap_uv, has_alpha;
    unsigned u_step_x, u_step_y;
    uint8_t red, green, blue;
    uint8_t y, u, v;
    int width, height;

    if (!pic) return;

    width  = nsfw_fp_visible_width(&pic->format);
    height = nsfw_fp_visible_height(&pic->format);
    if (width <= 0 || height <= 0) return;

    DebugScoreColor(score, threshold, &red, &green, &blue);
    RgbToYuv(red, green, blue, &y, &u, &v);

    uint8_t color_y[] = { y };
    uint8_t color_u[] = { u };
    uint8_t color_v[] = { v };
    uint8_t black_y[] = { 0x00 };
    uint8_t black_u[] = { 0x80 };
    uint8_t black_v[] = { 0x80 };

    uint8_t color_nv12[] = { u, v };
    uint8_t color_nv21[] = { v, u };
    uint8_t black_nv[]   = { 0x80, 0x80 };

    uint8_t color_bgr[]  = { blue, green, red, 0xFF };
    uint8_t black_bgr[]  = { 0x00, 0x00, 0x00, 0xFF };
    uint8_t color_rgba[] = { red, green, blue, 0xFF };
    uint8_t black_rgba[] = { 0x00, 0x00, 0x00, 0xFF };
    uint8_t color_argb[] = { 0xFF, red, green, blue };
    uint8_t black_argb[] = { 0xFF, 0x00, 0x00, 0x00 };

    uint8_t black_p010_y[2], color_p010_y[2];
    uint8_t black_p010_uv[4], color_p010_uv[4];

    if (GetPlanar16Layout(pic->format.i_chroma, &desc, &swap_uv,
                           &u_step_x, &u_step_y, &has_alpha)) {
        uint8_t black_y16[2], black_u16[2], black_v16[2];
        uint8_t color_y16[2], color_u16[2], color_v16[2];

        (void)u_step_x; (void)u_step_y; (void)has_alpha;
        WriteWord16(black_y16, EncodeSample16(0x00, desc), desc.little_endian);
        WriteWord16(black_u16, EncodeSample16(0x80, desc), desc.little_endian);
        WriteWord16(black_v16, EncodeSample16(0x80, desc), desc.little_endian);
        WriteWord16(color_y16, EncodeSample16(y, desc), desc.little_endian);
        WriteWord16(color_u16, EncodeSample16(u, desc), desc.little_endian);
        WriteWord16(color_v16, EncodeSample16(v, desc), desc.little_endian);

        DrawDebugTextPlane(&pic->p[Y_PLANE], 2, width, height,
                           black_y16, color_y16, score, threshold);
        DrawDebugTextPlane(&pic->p[U_PLANE], 2, width, height,
                           swap_uv ? black_v16 : black_u16,
                           swap_uv ? color_v16 : color_u16,
                           score, threshold);
        DrawDebugTextPlane(&pic->p[V_PLANE], 2, width, height,
                           swap_uv ? black_u16 : black_v16,
                           swap_uv ? color_u16 : color_v16,
                           score, threshold);
        return;
    }

    EncodeP010OverlayColor(0, 0, 0,
                            pic->format.transfer == TRANSFER_FUNC_SMPTE_ST2084,
                            black_p010_y, black_p010_uv);
    EncodeP010OverlayColor(red, green, blue,
                            pic->format.transfer == TRANSFER_FUNC_SMPTE_ST2084,
                            color_p010_y, color_p010_uv);

    switch (pic->format.i_chroma) {
        case VLC_CODEC_I420: case VLC_CODEC_J420:
        case VLC_CODEC_I422: case VLC_CODEC_I444: case VLC_CODEC_YUVA:
            DrawDebugTextPlane(&pic->p[Y_PLANE], 1, width, height, black_y, color_y, score, threshold);
            DrawDebugTextPlane(&pic->p[U_PLANE], 1, width, height, black_u, color_u, score, threshold);
            DrawDebugTextPlane(&pic->p[V_PLANE], 1, width, height, black_v, color_v, score, threshold);
            break;
        case VLC_CODEC_YV12:
            DrawDebugTextPlane(&pic->p[Y_PLANE], 1, width, height, black_y, color_y, score, threshold);
            DrawDebugTextPlane(&pic->p[V_PLANE], 1, width, height, black_u, color_u, score, threshold);
            DrawDebugTextPlane(&pic->p[U_PLANE], 1, width, height, black_v, color_v, score, threshold);
            break;
        case VLC_CODEC_NV12:
            DrawDebugTextPlane(&pic->p[Y_PLANE], 1, width, height, black_y, color_y, score, threshold);
            DrawDebugTextPlane(&pic->p[U_PLANE], 2, width, height, black_nv, color_nv12, score, threshold);
            break;
        case VLC_CODEC_NV21:
            DrawDebugTextPlane(&pic->p[Y_PLANE], 1, width, height, black_y, color_y, score, threshold);
            DrawDebugTextPlane(&pic->p[U_PLANE], 2, width, height, black_nv, color_nv21, score, threshold);
            break;
        case VLC_CODEC_RGB24:
            DrawDebugTextPlane(&pic->p[0], 3, width, height, black_bgr, color_bgr, score, threshold);
            break;
        case VLC_CODEC_RGB32: case VLC_CODEC_BGRA:
            DrawDebugTextPlane(&pic->p[0], 4, width, height, black_bgr, color_bgr, score, threshold);
            break;
        case VLC_CODEC_RGBA:
            DrawDebugTextPlane(&pic->p[0], 4, width, height, black_rgba, color_rgba, score, threshold);
            break;
        case VLC_CODEC_ARGB:
            DrawDebugTextPlane(&pic->p[0], 4, width, height, black_argb, color_argb, score, threshold);
            break;
        case VLC_CODEC_P010:
            DrawDebugTextPlane(&pic->p[Y_PLANE], 2, width, height, black_p010_y, color_p010_y, score, threshold);
            DrawDebugTextPlane(&pic->p[U_PLANE], 4, width, height, black_p010_uv, color_p010_uv, score, threshold);
            break;
        default:
            break;
    }
}
