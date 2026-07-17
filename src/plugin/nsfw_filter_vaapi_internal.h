#ifndef VLC_NSFW_FILTER_VAAPI_INTERNAL_H
#define VLC_NSFW_FILTER_VAAPI_INTERNAL_H 1

#include <va/va.h>
#include <pthread.h>

#include "nsfw_filter_vaapi.h"
#include "nsfw_filter.h"

/* ---- VLC 3.0.21 VAAPI picture ABI accessors -------------------------------
 *
 * VLC's vaapi_pic_ctx (attached via picture->context):
 *   { picture_context_t s; VASurfaceID surface; picture_t *picref; }
 *
 * VLC's pic_sys_vaapi_instance (accessed via picture->p_sys->instance):
 *   { atomic_int refcount; VADisplay va_dpy;
 *     struct vlc_vaapi_instance *va_inst; unsigned num_render_targets;
 *     VASurfaceID render_targets[]; }
 *
 * VLC's picture_sys_t (picture->p_sys):
 *   { pic_sys_vaapi_instance *instance; vaapi_pic_ctx ctx; }
 */
typedef struct {
    picture_context_t s;
    VASurfaceID       surface;
    int               _pad;
    void             *picref;
} nsfw_vaapi_pic_ctx_t;

typedef struct {
    int               refcount;
    unsigned          _pad0;
    void             *va_dpy;    /* VADisplay */
    void             *va_inst;   /* opaque */
    unsigned          num_render_targets;
    VASurfaceID       render_targets[];
} nsfw_vaapi_instance_t;

typedef struct {
    nsfw_vaapi_instance_t *instance;
    nsfw_vaapi_pic_ctx_t   ctx;
} nsfw_vaapi_picture_sys_t;

static inline VASurfaceID vaapi_pic_surface(const picture_t *pic)
{
    if (pic->context)
        return ((const nsfw_vaapi_pic_ctx_t *)pic->context)->surface;
    return ((const nsfw_vaapi_picture_sys_t *)pic->p_sys)->ctx.surface;
}

static inline VADisplay vaapi_pic_dpy(const picture_t *pic)
{
    const nsfw_vaapi_picture_sys_t *psys =
        (const nsfw_vaapi_picture_sys_t *)pic->p_sys;
    if (!psys || !psys->instance)
        return NULL;
    return (VADisplay)psys->instance->va_dpy;
}

static inline unsigned vaapi_pic_surface_count(const picture_t *pic)
{
    const nsfw_vaapi_picture_sys_t *psys =
        (const nsfw_vaapi_picture_sys_t *)pic->p_sys;
    if (!psys || !psys->instance)
        return 0;
    return psys->instance->num_render_targets;
}

/* ---- Backend struct ------------------------------------------------------ */
struct nsfw_vaapi_backend_t
{
    VADisplay    dpy;
    VAConfigID   config;
    VAContextID  context;
    bool         context_valid;

    /* Analysis */
    int          analysis_width;
    int          analysis_height;
    VASurfaceID  analysis_surface;
    VAImageID    analysis_image_id;
    VAImage      analysis_image;
    void        *analysis_mapped;

    /* Blur staging */
    VASurfaceID  blur_surface[2];
    int          blur_width;
    int          blur_height;
    bool         hq_scaling_tested;
    bool         hq_scaling_available;
    VAImageID    blur_image_id[2];
    VAImage      blur_image[2];
    void        *blur_mapped[2];

    /* Watermark */
    VAImageID    watermark_image_id;
    VAImage      watermark_image;
    void        *watermark_mapped;
    int          watermark_width;
    int          watermark_height;
    VASurfaceID  watermark_surface;

    /* Black frame */
    VAImageID    black_image_id;
    VAImage      black_image;
    void        *black_mapped;
    VASurfaceID  black_surface;

    /* Debug overlay */
    VAImageID    debug_image_id;
    VAImage      debug_image;
    void        *debug_mapped;
    VASurfaceID  debug_surface;
    float        debug_score;
    float        debug_threshold;
    bool         debug_cache_valid;
    bool         debug_failure_logged;

    /* Render target for effect output */
    VASurfaceID  render_surface;

    char         driver_name[128];
    char         surface_format[16];

    pthread_mutex_t api_lock;
    bool            api_lock_ready;
};

/* ---- Helpers called across .c files -------------------------------------- */
int CreateEffectResources(nsfw_vaapi_backend_t *backend,
                          int frame_w, int frame_h);
void DestroyEffectResources(nsfw_vaapi_backend_t *backend);

int CreateImage(nsfw_vaapi_backend_t *backend,
                int w, int h, VAImageFormat *fmt,
                VAImageID *id, VAImage *img, void **mapped);
void DestroyImage(nsfw_vaapi_backend_t *backend,
                  VAImageID id, VAImage *img, void *mapped);

/* ---- Constants ----------------------------------------------------------- */
/* Keep enough source detail that VPP upscaling cannot expose macroblocks. */
#define NSFW_VAAPI_BLUR_DOWNSAMPLE 16
#define NSFW_VAAPI_DEBUG_WIDTH  240
#define NSFW_VAAPI_DEBUG_HEIGHT  72

#endif
