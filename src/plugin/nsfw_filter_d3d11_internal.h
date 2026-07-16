/*****************************************************************************
 * nsfw_filter_d3d11_internal.h: D3D11 backend internal declarations
 *
 * Shared between nsfw_filter_d3d11.c (base), nsfw_filter_d3d11_readback.c,
 * and nsfw_filter_d3d11_effects.c.
 *****************************************************************************/

#ifndef VLC_NSFW_FILTER_D3D11_INTERNAL_H
#define VLC_NSFW_FILTER_D3D11_INTERNAL_H 1

#include <d3d11.h>

#include "nsfw_filter_d3d11.h"

struct nsfw_d3d11_picture_sys_t;
typedef struct nsfw_d3d11_picture_sys_t nsfw_d3d11_picture_sys_t;

#define NSFW_D3D11_MAX_VIEWS 64

#ifndef NSFW_D3D11_PROFILE_MAX_SAMPLES
# define NSFW_D3D11_PROFILE_MAX_SAMPLES 512
#endif

typedef struct nsfw_d3d11_view_entry_t
{
    ID3D11Texture2D *texture;
    UINT slice;
    ID3D11VideoProcessorInputView *input;
} nsfw_d3d11_view_entry_t;

struct nsfw_d3d11_backend_t
{
    ID3D11Device *device;
    ID3D11DeviceContext *context;
    ID3D11VideoDevice *video_device;
    ID3D11VideoContext *video_context;
    ID3D11VideoProcessorEnumerator *processor_enum;
    ID3D11VideoProcessor *processor;

    ID3D11Texture2D *analysis_texture;
    ID3D11VideoProcessorOutputView *analysis_output;
    ID3D11Texture2D *analysis_staging;
    int analysis_width;
    int analysis_height;

    ID3D11Texture2D *blur_texture[2];
    ID3D11RenderTargetView *blur_rtv[2];
    ID3D11ShaderResourceView *blur_srv[2];
    ID3D11VideoProcessorOutputView *blur_down_output;
    ID3D11VideoProcessorInputView *blur_up_input;
    int blur_width;
    int blur_height;

    ID3D11Texture2D *render_texture;
    ID3D11VideoProcessorOutputView *render_output;

    ID3D11VertexShader *fullscreen_vs;
    ID3D11PixelShader *blur_ps;
    ID3D11SamplerState *linear_sampler;
    ID3D11Buffer *blur_constants;

    ID3D11Query *profile_disjoint;
    ID3D11Query *profile_start;
    ID3D11Query *profile_end;
    double profile_total_ms[3];
    uint64_t profile_count[3];
    double profile_samples[3][NSFW_D3D11_PROFILE_MAX_SAMPLES];
    unsigned profile_sample_count[3];
    uint64_t rendered_count[3];
    uint64_t fallback_count[3];
    bool profile_enabled;
    bool analysis_logged;

    ID3D11Texture2D *watermark_texture;
    ID3D11VideoProcessorInputView *watermark_input;
    int watermark_width;
    int watermark_height;

    ID3D11Texture2D *black_texture;
    ID3D11VideoProcessorInputView *black_input;

    ID3D11Texture2D *debug_texture;
    ID3D11VideoProcessorInputView *debug_input;
    uint8_t *debug_pixels;
    float debug_score;
    float debug_threshold;
    uint64_t debug_rendered_count;
    bool debug_cache_valid;
    bool debug_logged;
    bool debug_failure_logged;

    nsfw_d3d11_view_entry_t views[NSFW_D3D11_MAX_VIEWS];
    unsigned next_view;
    D3D11_TEXTURE2D_DESC output_desc;
    HANDLE context_mutex;
    CRITICAL_SECTION api_lock;
    bool api_lock_ready;
    char adapter_name[128];
    char texture_format[16];
};

/* ---- Backend state helpers ---- */
void BackendLock(nsfw_d3d11_backend_t *backend);
void BackendUnlock(nsfw_d3d11_backend_t *backend);

/* ---- Picture helpers ---- */
nsfw_d3d11_picture_sys_t *PictureSys(picture_t *picture);
picture_t *NewPicture(filter_t *filter);
void ReleasePicture(picture_t *picture);
void CopyPictureProperties(picture_t *destination,
                           const picture_t *source);
RECT PictureSourceRect(const picture_t *picture);

/* ---- View cache ---- */
ID3D11VideoProcessorInputView *GetInputView(
    nsfw_d3d11_backend_t *backend, nsfw_d3d11_picture_sys_t *picsys);
HRESULT CreateInputView(nsfw_d3d11_backend_t *backend,
                        ID3D11Texture2D *texture, UINT slice,
                        ID3D11VideoProcessorInputView **view);
HRESULT CreateOutputView(nsfw_d3d11_backend_t *backend,
                         ID3D11Texture2D *texture, UINT slice,
                         ID3D11VideoProcessorOutputView **view);
void ReleaseViewCache(nsfw_d3d11_backend_t *backend);

/* ---- Texture / blit helpers ---- */
int CreateBgraTexture(nsfw_d3d11_backend_t *backend,
                      int width, int height,
                      ID3D11Texture2D **texture,
                      ID3D11RenderTargetView **rtv,
                      ID3D11ShaderResourceView **srv);
void ConfigureProcessorOutput(nsfw_d3d11_backend_t *backend,
                              int width, int height);
void ConfigureProcessorStream(nsfw_d3d11_backend_t *backend,
                              UINT index, const RECT *source,
                              const RECT *destination, bool alpha);
HRESULT ProcessorBlitOne(nsfw_d3d11_backend_t *backend,
                         ID3D11VideoProcessorInputView *input,
                         const RECT *source_rect,
                         ID3D11VideoProcessorOutputView *output,
                         int output_width, int output_height);

/* ---- Effects module entry points (called from base nsfw_d3d11_open/close) ---- */
int CreateShaders(nsfw_d3d11_backend_t *backend);
int CreateEffectResources(nsfw_d3d11_backend_t *backend,
                          int frame_width, int frame_height);
int CreateProfilingQueries(nsfw_d3d11_backend_t *backend);
double ProfileMedianMs(const nsfw_d3d11_backend_t *backend, int style);

#endif /* VLC_NSFW_FILTER_D3D11_INTERNAL_H */
