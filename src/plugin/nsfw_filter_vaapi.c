#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef __linux__

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>

#include <va/va.h>
#include <va/va_drm.h>

#include "nsfw_filter_vaapi.h"
#include "nsfw_filter_vaapi_internal.h"

static int backend_create_config(nsfw_vaapi_backend_t *backend)
{
    VAConfigAttrib attrib;
    attrib.type = VAConfigAttribRTFormat;
    vaGetConfigAttributes(backend->dpy, VAProfileNone,
                          VAEntrypointVideoProc, &attrib, 1);
    VAStatus st = vaCreateConfig(backend->dpy, VAProfileNone,
                                 VAEntrypointVideoProc, &attrib, 1,
                                 &backend->config);
    if (st != VA_STATUS_SUCCESS)
    {
        fprintf(stderr, "icop: vaapi vaCreateConfig failed (%d)\n", st);
        return VLC_EGENERIC;
    }

    VAContextID ctx;
    st = vaCreateContext(backend->dpy, backend->config, 1920, 1088,
                         VA_PROGRESSIVE, NULL, 0, &ctx);
    if (st != VA_STATUS_SUCCESS)
    {
        vaDestroyConfig(backend->dpy, backend->config);
        fprintf(stderr, "icop: vaapi vaCreateContext failed (%d)\n", st);
        return VLC_EGENERIC;
    }
    backend->context = ctx;
    backend->context_valid = true;
    return VLC_SUCCESS;
}

bool nsfw_vaapi_is_opaque(vlc_fourcc_t chroma)
{
    return chroma == VLC_CODEC_VAAPI_420 ||
           chroma == VLC_CODEC_VAAPI_420_10BPP;
}

const char *nsfw_vaapi_driver_name(const nsfw_vaapi_backend_t *backend)
{
    return backend ? backend->driver_name : "unavailable";
}

const char *nsfw_vaapi_surface_format(const nsfw_vaapi_backend_t *backend)
{
    return backend ? backend->surface_format : "unknown";
}

int nsfw_vaapi_open(filter_t *filter, nsfw_vaapi_backend_t **out)
{
    if (!filter || !out || !nsfw_vaapi_is_opaque(filter->fmt_in.video.i_chroma))
        return VLC_EGENERIC;
    *out = NULL;

    picture_t *probe = NULL;
    if (filter->owner.video.buffer_new)
        probe = filter->owner.video.buffer_new(filter);
    if (!probe)
        return VLC_EGENERIC;

    VADisplay dpy = vaapi_pic_dpy(probe);
    if (!dpy)
    {
        picture_Release(probe);
        return VLC_EGENERIC;
    }

    nsfw_vaapi_backend_t *b = calloc(1, sizeof(*b));
    if (!b)
    {
        picture_Release(probe);
        return VLC_ENOMEM;
    }

    if (pthread_mutex_init(&b->api_lock, NULL) != 0)
    {
        free(b);
        picture_Release(probe);
        return VLC_EGENERIC;
    }
    b->api_lock_ready = true;

    b->dpy = dpy;

    /* calloc zeros all fields, but VA_INVALID_ID = 0xFFFFFFFF, so
     * explicitly init surface/image IDs. */
    {
        VASurfaceID *surfaces[] = {
            &b->analysis_surface, &b->render_surface,
            &b->blur_surface[0], &b->blur_surface[1],
            &b->watermark_surface, &b->black_surface,
            &b->debug_surface,
        };
        for (size_t i = 0; i < sizeof(surfaces)/sizeof(surfaces[0]); i++)
            *surfaces[i] = VA_INVALID_ID;
        VAImageID *images[] = {
            &b->analysis_image_id, &b->black_image_id,
            &b->watermark_image_id, &b->debug_image_id,
            &b->blur_image_id[0], &b->blur_image_id[1],
        };
        for (size_t i = 0; i < sizeof(images)/sizeof(images[0]); i++)
            *images[i] = VA_INVALID_ID;
    }

    const char *vendor = vaQueryVendorString(dpy);
    if (vendor)
    {
        strncpy(b->driver_name, vendor, sizeof(b->driver_name) - 1);
        b->driver_name[sizeof(b->driver_name) - 1] = '\0';
    }
    else
        strcpy(b->driver_name, "unknown");

    /* Detect surface format from input chroma */
    switch (filter->fmt_in.video.i_chroma)
    {
        case VLC_CODEC_VAAPI_420:
            strcpy(b->surface_format, "NV12"); break;
        case VLC_CODEC_VAAPI_420_10BPP:
            strcpy(b->surface_format, "P010"); break;
        default:
            strcpy(b->surface_format, "unknown");
    }

    if (backend_create_config(b) != VLC_SUCCESS)
    {
        picture_Release(probe);
        pthread_mutex_destroy(&b->api_lock);
        free(b);
        return VLC_EGENERIC;
    }

    int fw = filter->fmt_in.video.i_width;
    int fh = filter->fmt_in.video.i_height;
    if (CreateEffectResources(b, fw, fh) != VLC_SUCCESS)
    {
        nsfw_vaapi_close(b);
        picture_Release(probe);
        return VLC_EGENERIC;
    }

    picture_Release(probe);

    fprintf(stderr, "icop: vaapi opened driver=\"%s\" surface=%s\n",
            b->driver_name, b->surface_format);
    *out = b;
    return VLC_SUCCESS;
}

void nsfw_vaapi_close(nsfw_vaapi_backend_t *b)
{
    if (!b)
        return;

    DestroyEffectResources(b);

    if (b->render_surface != VA_INVALID_ID)
        vaDestroySurfaces(b->dpy, &b->render_surface, 1);
    if (b->context_valid)
        vaDestroyContext(b->dpy, b->context);
    if (b->config != VA_INVALID_ID)
        vaDestroyConfig(b->dpy, b->config);

    if (b->api_lock_ready)
        pthread_mutex_destroy(&b->api_lock);
    free(b);
}

unsigned nsfw_vaapi_decoder_surface_count(picture_t *pic)
{
    return pic ? vaapi_pic_surface_count(pic) : 0;
}

#else /* !__linux__ */

#include <poll.h>
#include "nsfw_filter_vaapi.h"

bool nsfw_vaapi_is_opaque(vlc_fourcc_t chroma)
{
    (void)chroma;
    return false;
}

int nsfw_vaapi_open(filter_t *filter, nsfw_vaapi_backend_t **out)
{
    if (out) *out = NULL;
    (void)filter;
    return VLC_EGENERIC;
}

void nsfw_vaapi_close(nsfw_vaapi_backend_t *b) { (void)b; }
unsigned nsfw_vaapi_decoder_surface_count(picture_t *pic) { (void)pic; return 0; }
const char *nsfw_vaapi_driver_name(const nsfw_vaapi_backend_t *b) { (void)b; return "unavailable"; }
const char *nsfw_vaapi_surface_format(const nsfw_vaapi_backend_t *b) { (void)b; return "unknown"; }

#endif
