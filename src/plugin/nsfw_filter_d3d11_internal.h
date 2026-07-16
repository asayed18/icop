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
