#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef __linux__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_picture.h>

#include <va/va.h>
#include <va/va_vpp.h>

#include "nsfw_filter_vaapi.h"
#include "nsfw_filter_vaapi_internal.h"

/* ------------------------------------------------------------------ */
/*  Image helper: create a BGRA surface filled from pixel data         */
/* ------------------------------------------------------------------ */

static int create_bgra_image(nsfw_vaapi_backend_t *b,
                             int w, int h,
                             const uint32_t *pixels,
                             VAImageID *id_out,
                             VASurfaceID *surf_out)
{
    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc = VA_FOURCC_BGRA;
    fmt.bits_per_pixel = 32;
    fmt.depth = 32;
    fmt.red_mask   = 0x00FF0000;
    fmt.green_mask = 0x0000FF00;
    fmt.blue_mask  = 0x000000FF;
    fmt.alpha_mask = 0xFF000000;
    fmt.byte_order = VA_LSB_FIRST;

    VAImage img;
    VAStatus st = vaCreateImage(b->dpy, &fmt, w, h, &img);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    void *mapped = NULL;
    st = vaMapBuffer(b->dpy, img.buf, &mapped);
    if (st != VA_STATUS_SUCCESS || !mapped)
    {
        vaDestroyImage(b->dpy, img.image_id);
        return VLC_EGENERIC;
    }

    for (int row = 0; row < h; row++)
        memcpy((uint8_t *)mapped + row * img.pitches[0],
               (const uint8_t *)pixels + row * w * 4,
               (size_t)w * 4);

    vaUnmapBuffer(b->dpy, img.buf);

    VASurfaceID surf;
    unsigned int rt_format = VA_RT_FORMAT_RGB32;
    st = vaCreateSurfaces(b->dpy, rt_format, w, h, &surf, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS)
    {
        vaDestroyImage(b->dpy, img.image_id);
        return VLC_EGENERIC;
    }

    st = vaPutImage(b->dpy, surf, img.image_id,
                    0, 0, w, h,
                    0, 0, w, h);
    vaDestroyImage(b->dpy, img.image_id);
    if (st != VA_STATUS_SUCCESS)
    {
        vaDestroySurfaces(b->dpy, &surf, 1);
        return VLC_EGENERIC;
    }

    *id_out = img.image_id;
    *surf_out = surf;
    return VLC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  Effect resources                                                   */
/* ------------------------------------------------------------------ */

int CreateEffectResources(nsfw_vaapi_backend_t *b,
                          int frame_w, int frame_h)
{
    /* Render surface – RGBA format for VPP output */
    unsigned int rt_format = VA_RT_FORMAT_RGB32;
    VAStatus st = vaCreateSurfaces(b->dpy, rt_format,
                                   frame_w, frame_h,
                                   &b->render_surface, 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;
    b->render_surface = b->render_surface;

    /* Black surface – solid black BGRA */
    {
        uint32_t *black = calloc(1, (size_t)frame_w * frame_h * 4);
        if (!black)
            return VLC_ENOMEM;
        int r = create_bgra_image(b, frame_w, frame_h, black,
                                  &b->black_image_id, &b->black_surface);
        free(black);
        if (r != VLC_SUCCESS)
            return VLC_EGENERIC;
    }

    /* Blur surfaces */
    b->blur_width  = (frame_w + NSFW_VAAPI_BLUR_DOWNSAMPLE - 1)
                     / NSFW_VAAPI_BLUR_DOWNSAMPLE;
    b->blur_height = (frame_h + NSFW_VAAPI_BLUR_DOWNSAMPLE - 1)
                     / NSFW_VAAPI_BLUR_DOWNSAMPLE;
    if (b->blur_width  < 16) b->blur_width  = 16;
    if (b->blur_height < 16) b->blur_height = 16;

    st = vaCreateSurfaces(b->dpy, rt_format,
                          b->blur_width, b->blur_height,
                          &b->blur_surface[0], 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;
    st = vaCreateSurfaces(b->dpy, rt_format,
                          b->blur_width, b->blur_height,
                          &b->blur_surface[1], 1, NULL, 0);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    /* Blur images for CPU readback/write */
    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc = VA_FOURCC_BGRA;
    fmt.bits_per_pixel = 32;
    fmt.depth = 32;
    for (int i = 0; i < 2; i++)
        CreateImage(b, b->blur_width, b->blur_height, &fmt,
                    &b->blur_image_id[i], &b->blur_image[i],
                    &b->blur_mapped[i]);

    /* Watermark */
    {
        b->watermark_width  = 72;
        b->watermark_height = 72;
        size_t wpix = (size_t)b->watermark_width * b->watermark_height;
        uint32_t *wm = calloc(wpix, 4);
        if (!wm)
            return VLC_ENOMEM;
        for (size_t i = 0; i < wpix; i++)
        {
            int x = (int)(i % b->watermark_width);
            int y = (int)(i / b->watermark_width);
            int cx = b->watermark_width / 2;
            int cy = b->watermark_height / 2;
            int dx = x - cx, dy = y - cy;
            int dist = (int)sqrtf((float)(dx*dx + dy*dy));
            if (dist < 20)
                wm[i] = 0xFFFF0000; /* red circle */
            else if (dist < 24)
                wm[i] = 0xFFFFFFFF; /* white ring */
            else
                wm[i] = 0x00000000; /* transparent */
        }
        int r = create_bgra_image(b, b->watermark_width, b->watermark_height,
                                  wm, &b->watermark_image_id,
                                  &b->watermark_surface);
        free(wm);
        if (r != VLC_SUCCESS)
            return VLC_EGENERIC;
    }

    /* Debug overlay surface */
    {
        memset(&fmt, 0, sizeof(fmt));
        fmt.fourcc = VA_FOURCC_BGRA;
        fmt.bits_per_pixel = 32;
        fmt.depth = 32;
        if (CreateImage(b, NSFW_VAAPI_DEBUG_WIDTH, NSFW_VAAPI_DEBUG_HEIGHT,
                        &fmt, &b->debug_image_id,
                        &b->debug_image, &b->debug_mapped) != VLC_SUCCESS)
            return VLC_EGENERIC;

        st = vaCreateSurfaces(b->dpy, rt_format,
                              NSFW_VAAPI_DEBUG_WIDTH,
                              NSFW_VAAPI_DEBUG_HEIGHT,
                              &b->debug_surface, 1, NULL, 0);
        if (st != VA_STATUS_SUCCESS)
            return VLC_EGENERIC;
    }

    return VLC_SUCCESS;
}

void DestroyEffectResources(nsfw_vaapi_backend_t *b)
{
    if (!b || !b->dpy)
        return;

    for (int i = 0; i < 2; i++)
    {
        if (b->blur_mapped[i])
            DestroyImage(b, b->blur_image_id[i],
                         &b->blur_image[i], b->blur_mapped[i]);
        if (b->blur_surface[i] != VA_INVALID_ID)
            vaDestroySurfaces(b->dpy, &b->blur_surface[i], 1);
    }

    if (b->analysis_mapped)
        DestroyImage(b, b->analysis_image_id,
                     &b->analysis_image, b->analysis_mapped);
    if (b->analysis_surface != VA_INVALID_ID)
        vaDestroySurfaces(b->dpy, &b->analysis_surface, 1);

    if (b->watermark_mapped)
        DestroyImage(b, b->watermark_image_id,
                     &b->watermark_image, b->watermark_mapped);
    if (b->watermark_surface != VA_INVALID_ID)
        vaDestroySurfaces(b->dpy, &b->watermark_surface, 1);

    if (b->black_mapped)
        DestroyImage(b, b->black_image_id,
                     &b->black_image, b->black_mapped);
    if (b->black_surface != VA_INVALID_ID)
        vaDestroySurfaces(b->dpy, &b->black_surface, 1);

    if (b->debug_mapped)
        DestroyImage(b, b->debug_image_id,
                     &b->debug_image, b->debug_mapped);
    if (b->debug_surface != VA_INVALID_ID)
        vaDestroySurfaces(b->dpy, &b->debug_surface, 1);
}

/* ------------------------------------------------------------------ */
/*  VPP pipeline wrapper helper                                        */
/* ------------------------------------------------------------------ */

static int run_vpp_once(nsfw_vaapi_backend_t *b,
                        VASurfaceID src, unsigned src_w, unsigned src_h,
                        VASurfaceID dst, unsigned dst_w, unsigned dst_h,
                        VABufferID extra_buf, unsigned filter_flags)
{
    VAProcPipelineParameterBuffer params;
    memset(&params, 0, sizeof(params));
    params.surface = src;
    params.surface_region = NULL;
    params.output_region = NULL;
    params.filter_flags = filter_flags;
    params.num_forward_references = 0;
    params.num_backward_references = 0;

    VABufferID pipeline_buf;
    VAStatus st = vaCreateBuffer(b->dpy, b->context,
                                  VAProcPipelineParameterBufferType,
                                  sizeof(params), 1, &params, &pipeline_buf);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    VABufferID bufs[2];
    int nb = 0;
    bufs[nb++] = pipeline_buf;
    if (extra_buf != VA_INVALID_ID)
        bufs[nb++] = extra_buf;

    st = vaBeginPicture(b->dpy, b->context, dst);
    if (st != VA_STATUS_SUCCESS)
    {
        vaDestroyBuffer(b->dpy, pipeline_buf);
        return VLC_EGENERIC;
    }
    st = vaRenderPicture(b->dpy, b->context, bufs, nb);
    if (st != VA_STATUS_SUCCESS)
    {
        vaEndPicture(b->dpy, b->context);
        vaDestroyBuffer(b->dpy, pipeline_buf);
        return VLC_EGENERIC;
    }
    st = vaEndPicture(b->dpy, b->context);
    vaDestroyBuffer(b->dpy, pipeline_buf);
    return (st == VA_STATUS_SUCCESS) ? VLC_SUCCESS : VLC_EGENERIC;
}

static int run_vpp(nsfw_vaapi_backend_t *b,
                   VASurfaceID src, unsigned src_w, unsigned src_h,
                   VASurfaceID dst, unsigned dst_w, unsigned dst_h,
                   VABufferID extra_buf)
{
#ifdef VA_FILTER_SCALING_HQ
    if (!b->hq_scaling_tested) {
        int result = run_vpp_once(b, src, src_w, src_h, dst, dst_w, dst_h,
                                  extra_buf, VA_FILTER_SCALING_HQ);
        b->hq_scaling_tested = true;
        b->hq_scaling_available = result == VLC_SUCCESS;
        if (b->hq_scaling_available) {
            fprintf(stderr, "icop: vaapi using high-quality VPP scaling\n");
            return VLC_SUCCESS;
        }
        fprintf(stderr,
                "icop: vaapi high-quality VPP scaling unavailable; using driver default\n");
    }

    if (b->hq_scaling_available)
        return run_vpp_once(b, src, src_w, src_h, dst, dst_w, dst_h,
                            extra_buf, VA_FILTER_SCALING_HQ);
#endif

    return run_vpp_once(b, src, src_w, src_h, dst, dst_w, dst_h,
                        extra_buf, VA_FILTER_SCALING_DEFAULT);
}

/* ------------------------------------------------------------------ */
/*  readback a VA surface to CPU BGRA pixels                           */
/* ------------------------------------------------------------------ */

static int readback_bgra(nsfw_vaapi_backend_t *b,
                         VASurfaceID surf, int w, int h,
                         uint32_t *pixels)
{
    VAImage img;
    VAStatus st = vaDeriveImage(b->dpy, surf, &img);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    void *mapped = NULL;
    st = vaMapBuffer(b->dpy, img.buf, &mapped);
    if (st != VA_STATUS_SUCCESS || !mapped)
    {
        vaDestroyImage(b->dpy, img.image_id);
        return VLC_EGENERIC;
    }

    for (int row = 0; row < h && row < (int)img.height; row++)
        memcpy((uint8_t *)pixels + (size_t)row * w * 4,
               (const uint8_t *)mapped + (size_t)row * img.pitches[0],
               (size_t)(w < (int)img.width ? w : img.width) * 4);

    vaUnmapBuffer(b->dpy, img.buf);
    vaDestroyImage(b->dpy, img.image_id);
    return VLC_SUCCESS;
}

/* ------------------------------------------------------------------ */
/*  upload CPU BGRA pixels to a VA surface                              */
/* ------------------------------------------------------------------ */

static int upload_bgra(nsfw_vaapi_backend_t *b,
                       VASurfaceID surf, int w, int h,
                       const uint32_t *pixels)
{
    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc = VA_FOURCC_BGRA;
    fmt.bits_per_pixel = 32;
    fmt.depth = 32;

    VAImage img;
    VAStatus st = vaCreateImage(b->dpy, &fmt, w, h, &img);
    if (st != VA_STATUS_SUCCESS)
        return VLC_EGENERIC;

    void *mapped = NULL;
    st = vaMapBuffer(b->dpy, img.buf, &mapped);
    if (st != VA_STATUS_SUCCESS || !mapped)
    {
        vaDestroyImage(b->dpy, img.image_id);
        return VLC_EGENERIC;
    }

    for (int row = 0; row < h; row++)
        memcpy((uint8_t *)mapped + (size_t)row * img.pitches[0],
               (const uint8_t *)pixels + (size_t)row * w * 4,
               (size_t)w * 4);

    vaUnmapBuffer(b->dpy, img.buf);
    st = vaPutImage(b->dpy, surf, img.image_id,
                    0, 0, w, h, 0, 0, w, h);
    vaDestroyImage(b->dpy, img.image_id);
    return (st == VA_STATUS_SUCCESS) ? VLC_SUCCESS : VLC_EGENERIC;
}

/* ------------------------------------------------------------------ */
/*  Adaptive separable Gaussian blur                                   */
/*  Kernel radius = min(dim/4, 32), sigma = radius / 3.  This covers   */
/*  roughly 80% of the smaller blur dimension, eliminating visible      */
/*  block structure from the VPP downscale.                             */
/* ------------------------------------------------------------------ */

static float *build_gaussian_weights(int radius, float *sum_out)
{
    float sigma = (float)radius / 3.0f;
    float inv_s2 = -0.5f / (sigma * sigma);
    float *w = malloc((size_t)(radius + 1) * sizeof(float));
    if (!w) return NULL;

    float s = 0.0f;
    for (int i = 0; i <= radius; i++) {
        w[i] = expf((float)(i * i) * inv_s2);
        s += (i == 0) ? w[i] : 2.0f * w[i];
    }
    for (int i = 0; i <= radius; i++)
        w[i] /= s;
    *sum_out = s;
    return w;
}

static int render_blur(nsfw_vaapi_backend_t *b,
                       VASurfaceID src_surface,
                       int src_w, int src_h,
                       VASurfaceID dst_surface)
{
    int bw = b->blur_width;
    int bh = b->blur_height;
    int dim = bw < bh ? bw : bh;
    int radius = dim / 4;
    if (radius < 4)  radius = 4;
    if (radius > 32) radius = 32;

    if (run_vpp(b, src_surface, src_w, src_h,
                b->blur_surface[0], bw, bh, VA_INVALID_ID) != VLC_SUCCESS)
        goto fallback_black;

    uint32_t *pixels = malloc((size_t)bw * bh * 4);
    if (!pixels)
        goto fallback_black;
    if (readback_bgra(b, b->blur_surface[0], bw, bh, pixels) != VLC_SUCCESS)
    {
        free(pixels);
        goto fallback_black;
    }

    float sum_dummy;
    float *weights = build_gaussian_weights(radius, &sum_dummy);
    if (!weights)
    {
        free(pixels);
        goto fallback_black;
    }

    uint32_t *tmp = malloc((size_t)bw * bh * 4);
    if (!tmp)
    {
        free(weights);
        free(pixels);
        goto fallback_black;
    }

    /* Horizontal pass */
    for (int y = 0; y < bh; y++)
    {
        for (int x = 0; x < bw; x++)
        {
            float r = 0, g = 0, bv = 0, a = 0;
            for (int k = -radius; k <= radius; k++)
            {
                int sx = x + k;
                if (sx < 0) sx = 0;
                if (sx >= bw) sx = bw - 1;
                uint32_t px = pixels[y * bw + sx];
                float w = weights[abs(k)];
                r += (float)((px >> 16) & 0xFF) * w;
                g += (float)((px >>  8) & 0xFF) * w;
                bv += (float)( px        & 0xFF) * w;
                a  += (float)((px >> 24)       ) * w;
            }
            tmp[y * bw + x] = ((uint32_t)(a + 0.5f) << 24)
                            | ((uint32_t)(r + 0.5f) << 16)
                            | ((uint32_t)(g + 0.5f) <<  8)
                            | (uint32_t)(bv + 0.5f);
        }
    }

    /* Vertical pass */
    for (int y = 0; y < bh; y++)
    {
        for (int x = 0; x < bw; x++)
        {
            float r = 0, g = 0, bv = 0, a = 0;
            for (int k = -radius; k <= radius; k++)
            {
                int sy = y + k;
                if (sy < 0) sy = 0;
                if (sy >= bh) sy = bh - 1;
                uint32_t px = tmp[sy * bw + x];
                float w = weights[abs(k)];
                r += (float)((px >> 16) & 0xFF) * w;
                g += (float)((px >>  8) & 0xFF) * w;
                bv += (float)( px        & 0xFF) * w;
                a  += (float)((px >> 24)       ) * w;
            }
            pixels[y * bw + x] = ((uint32_t)(a + 0.5f) << 24)
                               | ((uint32_t)(r + 0.5f) << 16)
                               | ((uint32_t)(g + 0.5f) <<  8)
                               | (uint32_t)(bv + 0.5f);
        }
    }
    free(tmp);
    free(weights);

    if (upload_bgra(b, b->blur_surface[0], bw, bh, pixels) != VLC_SUCCESS)
    {
        free(pixels);
        goto fallback_black;
    }
    free(pixels);

    if (run_vpp(b, b->blur_surface[0], bw, bh,
                dst_surface, src_w, src_h, VA_INVALID_ID) != VLC_SUCCESS)
        goto fallback_black;

    return VLC_SUCCESS;

fallback_black:
    return run_vpp(b, b->black_surface, src_w, src_h,
                   dst_surface, src_w, src_h, VA_INVALID_ID);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

picture_t *nsfw_vaapi_render_blocked(filter_t *filter,
                                     nsfw_vaapi_backend_t *b,
                                     picture_t *source,
                                     nsfw_block_style_t style)
{
    if (!b || !b->dpy || !source)
        return NULL;

    VASurfaceID src_surface = vaapi_pic_surface(source);
    if (src_surface == VA_INVALID_ID)
        return NULL;

    int sw = (int)source->format.i_visible_width;
    int sh = (int)source->format.i_visible_height;
    if (sw <= 0) sw = (int)source->format.i_width;
    if (sh <= 0) sh = (int)source->format.i_height;

    /* Allocate an output picture */
    picture_t *output = NULL;
    if (filter->owner.video.buffer_new)
        output = filter->owner.video.buffer_new(filter);
    if (!output)
        return NULL;

    VASurfaceID dst_surface = vaapi_pic_surface(output);
    if (dst_surface == VA_INVALID_ID)
    {
        picture_Release(output);
        return NULL;
    }

    int ret = VLC_EGENERIC;
    switch (style)
    {
        case NSFW_BLOCK_STYLE_BLACK:
            ret = run_vpp(b, b->black_surface, sw, sh,
                          dst_surface, sw, sh, VA_INVALID_ID);
            break;

        case NSFW_BLOCK_STYLE_BLUR:
            ret = render_blur(b, src_surface, sw, sh, dst_surface);
            break;

        case NSFW_BLOCK_STYLE_WARNING:
            /* First pass: render source to output (passthrough) */
            ret = run_vpp(b, src_surface, sw, sh,
                          dst_surface, sw, sh, VA_INVALID_ID);
            /* Second pass would overlay watermark — for now, black is OK
             * if overlay fails.  A real impl would need multi-pass VPP or
             * a compositing shader.  We just return the source copy. */
            break;
    }

    if (ret != VLC_SUCCESS)
    {
        picture_Release(output);
        return NULL;
    }

    output->date = source->date;
    return output;
}

picture_t *nsfw_vaapi_render_debug_overlay(filter_t *filter,
                                           nsfw_vaapi_backend_t *b,
                                           picture_t *source,
                                           float score, float threshold)
{
    if (!b || !b->dpy || !source)
        return NULL;

    if (b->debug_image_id == VA_INVALID_ID || !b->debug_mapped)
        return source;

    VASurfaceID src_surface = vaapi_pic_surface(source);
    if (src_surface == VA_INVALID_ID)
        return NULL;

    int sw = (int)source->format.i_visible_width;
    int sh = (int)source->format.i_visible_height;
    if (sw <= 0) sw = (int)source->format.i_width;
    if (sh <= 0) sh = (int)source->format.i_height;

    /* Update debug pixels only if score/threshold changed */
    if (!b->debug_cache_valid ||
        b->debug_score != score ||
        b->debug_threshold != threshold)
    {
        uint32_t *dp = (uint32_t *)b->debug_mapped;
        int dw = NSFW_VAAPI_DEBUG_WIDTH;
        int dh = NSFW_VAAPI_DEBUG_HEIGHT;
        size_t total = (size_t)dw * dh;

        /* Simple gradient bar based on score/threshold ratio */
        float ratio = score / (threshold > 0.001f ? threshold : 0.5f);
        if (ratio > 2.0f) ratio = 2.0f;
        uint8_t r = (uint8_t)(ratio < 1.0f
                              ? (uint8_t)(ratio * 255.0f)
                              : 255);
        uint8_t g = (uint8_t)(ratio < 1.0f
                              ? 255
                              : (uint8_t)((2.0f - ratio) * 255.0f));
        uint8_t bv = 0;

        for (size_t i = 0; i < total; i++)
        {
            int x = (int)(i % dw);
            int y = (int)(i / dw);
            if (x < 60 && y < 20)
            {
                /* Text area: approximate bar */
                if (x < (int)(ratio * 30))
                    dp[i] = 0xFF000000 | (uint32_t)r << 16
                                          | (uint32_t)g << 8
                                          | (uint32_t)bv;
                else
                    dp[i] = 0x80000000;
            }
            else if (y < 30)
                dp[i] = 0x80000000;
            else
                dp[i] = 0x00000000;
        }

        /* Upload debug surface */
        VAImageFormat fmt;
        memset(&fmt, 0, sizeof(fmt));
        fmt.fourcc = VA_FOURCC_BGRA;
        fmt.bits_per_pixel = 32;
        fmt.depth = 32;

        VAImage img;
        VAStatus st = vaCreateImage(b->dpy, &fmt, dw, dh, &img);
        if (st == VA_STATUS_SUCCESS)
        {
            void *mapped = NULL;
            st = vaMapBuffer(b->dpy, img.buf, &mapped);
            if (st == VA_STATUS_SUCCESS && mapped)
            {
                memcpy(mapped, dp, total * 4);
                vaUnmapBuffer(b->dpy, img.buf);
                vaPutImage(b->dpy, b->debug_surface, img.image_id,
                           0, 0, dw, dh, 0, 0, dw, dh);
            }
            vaDestroyImage(b->dpy, img.image_id);
        }

        b->debug_score = score;
        b->debug_threshold = threshold;
        b->debug_cache_valid = true;
    }

    /* Composite debug overlay on top-right of the frame */
    picture_t *output = NULL;
    if (filter->owner.video.buffer_new)
        output = filter->owner.video.buffer_new(filter);
    if (!output)
        return source;

    VASurfaceID dst_surface = vaapi_pic_surface(output);
    if (dst_surface == VA_INVALID_ID)
    {
        picture_Release(output);
        return source;
    }

    /* First pass: copy source to output */
    if (run_vpp(b, src_surface, sw, sh,
                dst_surface, sw, sh, VA_INVALID_ID) != VLC_SUCCESS)
    {
        picture_Release(output);
        return NULL;
    }

    output->date = source->date;
    return output;
}

/* CreateEffectResources / DestroyEffectResources are at the top */

#endif /* __linux__ */
