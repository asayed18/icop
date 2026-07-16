#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef __linux__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include <va/va.h>

#include "nsfw_filter_vaapi.h"
#include "nsfw_filter_vaapi_internal.h"

int CreateImage(nsfw_vaapi_backend_t *b,
                int w, int h, VAImageFormat *fmt,
                VAImageID *id, VAImage *img, void **mapped)
{
    VAStatus st;
    VAImage image;

    if (!b || !b->dpy)
        return VLC_EGENERIC;

    st = vaCreateImage(b->dpy, fmt, w, h, &image);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    st = vaMapBuffer(b->dpy, image.buf, mapped);
    if (st != VA_STATUS_SUCCESS)
    {
        vaDestroyImage(b->dpy, image.image_id);
        return VLC_EGENERIC;
    }

    if (img)  *img  = image;
    if (id)   *id   = image.image_id;
    return VLC_SUCCESS;
}

void DestroyImage(nsfw_vaapi_backend_t *b,
                  VAImageID id, VAImage *img, void *mapped)
{
    if (!b || !b->dpy)
        return;
    if (mapped)
        vaUnmapBuffer(b->dpy, id);
    if (img && img->image_id != VA_INVALID_ID)
        vaDestroyImage(b->dpy, img->image_id);
    else if (id != VA_INVALID_ID)
        vaDestroyImage(b->dpy, id);
}

int nsfw_vaapi_set_analysis_size(nsfw_vaapi_backend_t *b,
                                  int width, int height)
{
    if (!b || width <= 0 || height <= 0)
        return VLC_EGENERIC;

    b->analysis_width  = width;
    b->analysis_height = height;

    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc        = VA_FOURCC_BGRA;
    fmt.bits_per_pixel = 32;
    fmt.depth         = 32;

    if (b->analysis_image_id != VA_INVALID_ID)
        DestroyImage(b, b->analysis_image_id, &b->analysis_image,
                     b->analysis_mapped);
    b->analysis_image_id = VA_INVALID_ID;
    b->analysis_mapped = NULL;

    return CreateImage(b, width, height, &fmt,
                       &b->analysis_image_id,
                       &b->analysis_image,
                       &b->analysis_mapped);
}

int nsfw_vaapi_readback_rgb(nsfw_vaapi_backend_t *b,
                            picture_t *pic,
                            uint8_t *rgb, size_t cap,
                            int *out_w, int *out_h)
{
    if (!b || !b->dpy || !pic || !rgb || !out_w || !out_h)
        return VLC_EGENERIC;

    VASurfaceID surface = vaapi_pic_surface(pic);
    if (surface == VA_INVALID_ID)
        return VLC_EGENERIC;

    int src_w = (int)pic->format.i_visible_width;
    int src_h = (int)pic->format.i_visible_height;
    if (src_w <= 0) src_w = (int)pic->format.i_width;
    if (src_h <= 0) src_h = (int)pic->format.i_height;

    /* Derive an image from the decoder surface */
    VAImage img;
    VAStatus st = vaDeriveImage(b->dpy, surface, &img);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    void *mapped = NULL;
    st = vaMapBuffer(b->dpy, img.buf, &mapped);
    if (st != VA_STATUS_SUCCESS || !mapped)
    {
        vaDestroyImage(b->dpy, img.image_id);
        return VLC_EGENERIC;
    }

    /* Determine pixel format and convert to packed RGB */
    int ret = VLC_EGENERIC;
    int rw = (int)img.width;
    int rh = (int)img.height;
    *out_w = rw;
    *out_h = rh;

    size_t needed = (size_t)rw * (size_t)rh * 3;
    if (cap < needed)
        goto done;

    if (img.format.fourcc == VA_FOURCC_NV12)
    {
        const uint8_t *y = (const uint8_t *)mapped;
        const uint8_t *uv = y + (size_t)img.pitches[0] * rh;
        for (int row = 0; row < rh; row++)
        {
            for (int col = 0; col < rw; col++)
            {
                int Y  = y[img.pitches[0] * row + col];
                int UV_idx = (row / 2) * img.pitches[1] + (col / 2) * 2;
                int U  = uv[UV_idx];
                int V  = uv[UV_idx + 1];

                int C  = Y - 16;
                int D  = U - 128;
                int E  = V - 128;

                int R = (298 * C + 409 * E + 128) >> 8;
                int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
                int B = (298 * C + 516 * D + 128) >> 8;

                uint8_t *p = rgb + ((size_t)row * rw + col) * 3;
                p[0] = R < 0 ? 0 : R > 255 ? 255 : (uint8_t)R;
                p[1] = G < 0 ? 0 : G > 255 ? 255 : (uint8_t)G;
                p[2] = B < 0 ? 0 : B > 255 ? 255 : (uint8_t)B;
            }
        }
        ret = 0;
    }
    else if (img.format.fourcc == VA_FOURCC_P010)
    {
        const uint8_t *y = (const uint8_t *)mapped;
        const uint8_t *uv = y + (size_t)img.pitches[0] * rh;
        for (int row = 0; row < rh; row++)
        {
            for (int col = 0; col < rw; col++)
            {
                int Y  = (y[img.pitches[0] * row + col * 2 + 1] << 8)
                       |  y[img.pitches[0] * row + col * 2];
                int UV_idx = (row / 2) * img.pitches[1] + (col / 2) * 4;
                int U  = (uv[UV_idx + 1] << 8) | uv[UV_idx];
                int V  = (uv[UV_idx + 3] << 8) | uv[UV_idx + 2];

                Y >>= 6; U >>= 6; V >>= 6;
                int C  = Y - 16;
                int D  = U - 128;
                int E  = V - 128;

                int R = (298 * C + 409 * E + 128) >> 8;
                int G = (298 * C - 100 * D - 208 * E + 128) >> 8;
                int B = (298 * C + 516 * D + 128) >> 8;

                uint8_t *p = rgb + ((size_t)row * rw + col) * 3;
                p[0] = R < 0 ? 0 : R > 255 ? 255 : (uint8_t)R;
                p[1] = G < 0 ? 0 : G > 255 ? 255 : (uint8_t)G;
                p[2] = B < 0 ? 0 : B > 255 ? 255 : (uint8_t)B;
            }
        }
        ret = 0;
    }

done:
    vaUnmapBuffer(b->dpy, img.buf);
    vaDestroyImage(b->dpy, img.image_id);
    return ret;
}

int nsfw_vaapi_dump_ppm(nsfw_vaapi_backend_t *b,
                        picture_t *pic, const char *path)
{
    int w = 0, h = 0;
    VASurfaceID surface = vaapi_pic_surface(pic);
    if (surface == VA_INVALID_ID)
        return VLC_EGENERIC;

    int sw = (int)pic->format.i_visible_width;
    int sh = (int)pic->format.i_visible_height;
    if (sw <= 0) sw = (int)pic->format.i_width;
    if (sh <= 0) sh = (int)pic->format.i_height;

    size_t needed = (size_t)sw * sh * 3;
    uint8_t *buf = malloc(needed);
    if (!buf)
        return VLC_ENOMEM;

    if (nsfw_vaapi_readback_rgb(b, pic, buf, needed, &w, &h) != 0)
    {
        free(buf);
        return VLC_EGENERIC;
    }

    FILE *f = fopen(path, "wb");
    if (!f)
    {
        free(buf);
        return VLC_EGENERIC;
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    fwrite(buf, 1, (size_t)w * h * 3, f);
    fclose(f);
    free(buf);
    return VLC_SUCCESS;
}

#endif /* __linux__ */
