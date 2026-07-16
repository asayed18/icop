/*****************************************************************************
 * nsfw_backend.c: Hardware backend detection & registration
 *
 * All backends (D3D11, VAAPI, …) are wrapped through a single
 * nsfw_backend_ops_t vtable so that the main filter code never needs
 * to know which accelerator is in use.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>
#include <vlc_image.h>

#include "nsfw_filter.h"
#include "nsfw_filter_internal.h"
#include "nsfw_filter_d3d11.h"
#ifdef NSFW_HAVE_VAAPI
# include "nsfw_filter_vaapi.h"
#endif
#include "frame_processor.h"

/* ------------------------------------------------------------------ */
/*  D3D11 backend wrappers                                             */
/* ------------------------------------------------------------------ */

static void d3d11_close(void *backend)
{
    nsfw_d3d11_close((nsfw_d3d11_backend_t *)backend);
}

static int d3d11_set_analysis_size(void *backend, int width, int height)
{
    return nsfw_d3d11_set_analysis_size((nsfw_d3d11_backend_t *)backend,
                                         width, height);
}

static int d3d11_readback_rgb(void *backend, picture_t *pic,
                               uint8_t *rgb, size_t cap,
                               int *w, int *h)
{
    return nsfw_d3d11_readback_rgb((nsfw_d3d11_backend_t *)backend,
                                    pic, rgb, cap, w, h);
}

static unsigned d3d11_decoder_surface_count(void *backend, picture_t *pic)
{
    (void)backend;
    return nsfw_d3d11_decoder_surface_count(pic);
}

static picture_t *d3d11_render_blocked(filter_t *filter, void *backend,
                                        picture_t *source,
                                        nsfw_block_style_t style)
{
    return nsfw_d3d11_render_blocked(filter,
                                      (nsfw_d3d11_backend_t *)backend,
                                      source, style);
}

static picture_t *d3d11_render_debug_overlay(filter_t *filter,
                                              void *backend,
                                              picture_t *source,
                                              float score, float threshold)
{
    return nsfw_d3d11_render_debug_overlay(filter,
                                            (nsfw_d3d11_backend_t *)backend,
                                            source, score, threshold);
}

static int d3d11_dump_ppm(void *backend, picture_t *pic, const char *path)
{
    return nsfw_d3d11_dump_ppm((nsfw_d3d11_backend_t *)backend, pic, path);
}

static const char *d3d11_adapter_name(const void *backend)
{
    return nsfw_d3d11_adapter_name((const nsfw_d3d11_backend_t *)backend);
}

static const char *d3d11_format_name(const void *backend)
{
    return nsfw_d3d11_texture_format(
        (const nsfw_d3d11_backend_t *)backend);
}

static nsfw_backend_ops_t d3d11_ops = {
    .close               = d3d11_close,
    .set_analysis_size   = d3d11_set_analysis_size,
    .readback_rgb        = d3d11_readback_rgb,
    .decoder_surface_count = d3d11_decoder_surface_count,
    .render_blocked      = d3d11_render_blocked,
    .render_debug_overlay = d3d11_render_debug_overlay,
    .dump_ppm            = d3d11_dump_ppm,
    .adapter_name        = d3d11_adapter_name,
    .format_name         = d3d11_format_name,
};

/* ------------------------------------------------------------------ */
/*  VAAPI backend wrappers                                             */
/* ------------------------------------------------------------------ */

#ifdef NSFW_HAVE_VAAPI

static void vaapi_close(void *backend)
{
    nsfw_vaapi_close((nsfw_vaapi_backend_t *)backend);
}

static int vaapi_set_analysis_size(void *backend, int width, int height)
{
    return nsfw_vaapi_set_analysis_size((nsfw_vaapi_backend_t *)backend,
                                         width, height);
}

static int vaapi_readback_rgb(void *backend, picture_t *pic,
                               uint8_t *rgb, size_t cap,
                               int *w, int *h)
{
    return nsfw_vaapi_readback_rgb((nsfw_vaapi_backend_t *)backend,
                                    pic, rgb, cap, w, h);
}

static unsigned vaapi_decoder_surface_count(void *backend, picture_t *pic)
{
    (void)backend;
    return nsfw_vaapi_decoder_surface_count(pic);
}

static picture_t *vaapi_render_blocked(filter_t *filter, void *backend,
                                        picture_t *source,
                                        nsfw_block_style_t style)
{
    return nsfw_vaapi_render_blocked(filter,
                                      (nsfw_vaapi_backend_t *)backend,
                                      source, style);
}

static picture_t *vaapi_render_debug_overlay(filter_t *filter,
                                              void *backend,
                                              picture_t *source,
                                              float score, float threshold)
{
    return nsfw_vaapi_render_debug_overlay(filter,
                                            (nsfw_vaapi_backend_t *)backend,
                                            source, score, threshold);
}

static int vaapi_dump_ppm(void *backend, picture_t *pic, const char *path)
{
    return nsfw_vaapi_dump_ppm((nsfw_vaapi_backend_t *)backend, pic, path);
}

static const char *vaapi_adapter_name(const void *backend)
{
    return nsfw_vaapi_driver_name((const nsfw_vaapi_backend_t *)backend);
}

static const char *vaapi_format_name(const void *backend)
{
    return nsfw_vaapi_surface_format(
        (const nsfw_vaapi_backend_t *)backend);
}

static nsfw_backend_ops_t vaapi_ops = {
    .close               = vaapi_close,
    .set_analysis_size   = vaapi_set_analysis_size,
    .readback_rgb        = vaapi_readback_rgb,
    .decoder_surface_count = vaapi_decoder_surface_count,
    .render_blocked      = vaapi_render_blocked,
    .render_debug_overlay = vaapi_render_debug_overlay,
    .dump_ppm            = vaapi_dump_ppm,
    .adapter_name        = vaapi_adapter_name,
    .format_name         = vaapi_format_name,
};

#endif /* NSFW_HAVE_VAAPI */

/* ------------------------------------------------------------------ */
/*  Backend detection & open                                           */
/* ------------------------------------------------------------------ */

static vlc_fourcc_t PreferredOpaqueChroma(vlc_fourcc_t opaque_chroma)
{
    switch (opaque_chroma) {
        case VLC_CODEC_VAAPI_420:
        case VLC_CODEC_CVPX_NV12:
            return VLC_CODEC_NV12;
        case VLC_CODEC_VAAPI_420_10BPP:
        case VLC_CODEC_CVPX_P010:
            return VLC_CODEC_P010;
        default:
            return VLC_CODEC_I420;
    }
}

int nsfw_backend_open(filter_t *filter)
{
    filter_sys_t *sys = filter->p_sys;
    vlc_fourcc_t chroma = filter->fmt_in.video.i_chroma;

    if (nsfw_d3d11_is_opaque(chroma)) {
        nsfw_d3d11_backend_t *d3d11 = NULL;
        if (nsfw_d3d11_open(filter, &d3d11) != VLC_SUCCESS) {
            fprintf(stderr,
                    "icop: D3D11 backend initialization failed;"
                    " requesting CPU fallback\n");
            return VLC_EGENERIC;
        }
        sys->backend_data = d3d11;
        sys->backend_ops = &d3d11_ops;
        fprintf(stderr,
                "icop: video backend=d3d11 adapter=\"%s\" texture=%s\n",
                nsfw_d3d11_adapter_name(d3d11),
                nsfw_d3d11_texture_format(d3d11));
        if (sys->debug_overlay)
            fprintf(stderr,
                    "icop: D3D11 debug overlay enabled"
                    " (cached GPU composition)\n");
        return VLC_SUCCESS;
    }

    if (nsfw_fp_is_opaque_hw_chroma(chroma)) {
        vlc_fourcc_t target = PreferredOpaqueChroma(chroma);

#ifdef NSFW_HAVE_VAAPI
        if (nsfw_vaapi_is_opaque(chroma)) {
            nsfw_vaapi_backend_t *vaapi = NULL;
            if (nsfw_vaapi_open(filter, &vaapi) == VLC_SUCCESS) {
                sys->backend_data = vaapi;
                sys->backend_ops = &vaapi_ops;
                fprintf(stderr,
                        "icop: video backend=vaapi driver=\"%s\""
                        " surface=%s\n",
                        nsfw_vaapi_driver_name(vaapi),
                        nsfw_vaapi_surface_format(vaapi));
                if (sys->debug_overlay)
                    fprintf(stderr,
                            "icop: VAAPI debug overlay enabled"
                            " (cached GPU composition)\n");
                return VLC_SUCCESS;
            }
            fprintf(stderr,
                    "icop: VAAPI backend initialization failed;"
                    " falling back to image conversion path\n");
        }
#endif

        /* fallback: image_Convert opaque → software */
        fprintf(stderr,
                "icop: opaque hardware chroma %4.4s not directly supported;"
                " using VLC image conversion for analysis,"
                " blocked frames will be dropped fail-closed\n",
                (const char *)&chroma);
        sys->opaque_fallback = true;
        sys->opaque_analysis_chroma = target;
        sys->image_handler = CreateImageHandler(filter);
        if (sys->image_handler == NULL) {
            fprintf(stderr,
                    "icop: failed to create VLC image converter"
                    " for opaque-hw analysis\n");
            return VLC_ENOMEM;
        }
        return VLC_SUCCESS;
    }

    /* software (CPU) input chroma */
    fprintf(stderr,
            "icop: video backend=cpu input=%4.4s\n",
            (const char *)&chroma);
    return VLC_SUCCESS;
}
