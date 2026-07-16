/*****************************************************************************
 * nsfw_filter_d3d11_readback.c: D3D11 analysis readback pipeline
 *
 * GPU → CPU readback for frame analysis: set_analysis_size, readback_rgb,
 * dump_ppm.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>

#define COBJMACROS
#include <d3d11.h>

#include "nsfw_filter_d3d11.h"
#include "nsfw_filter_d3d11_internal.h"

int nsfw_d3d11_set_analysis_size(nsfw_d3d11_backend_t *backend,
                                 int width, int height)
{
    D3D11_TEXTURE2D_DESC staging_desc;
    ID3D11Texture2D *texture = NULL;
    ID3D11VideoProcessorOutputView *output = NULL;
    ID3D11Texture2D *staging = NULL;
    HRESULT hr;

    if (backend == NULL || width <= 0 || height <= 0)
        return VLC_EGENERIC;
    if (backend->analysis_width == width &&
        backend->analysis_height == height &&
        backend->analysis_texture != NULL &&
        backend->analysis_staging != NULL) {
        return VLC_SUCCESS;
    }

    BackendLock(backend);
    if (CreateBgraTexture(backend, width, height, &texture, NULL, NULL) !=
        VLC_SUCCESS) {
        BackendUnlock(backend);
        return VLC_EGENERIC;
    }
    hr = CreateOutputView(backend, texture, 0, &output);
    if (FAILED(hr)) {
        ID3D11Texture2D_Release(texture);
        BackendUnlock(backend);
        return VLC_EGENERIC;
    }

    memset(&staging_desc, 0, sizeof(staging_desc));
    staging_desc.Width = (UINT)width;
    staging_desc.Height = (UINT)height;
    staging_desc.MipLevels = 1;
    staging_desc.ArraySize = 1;
    staging_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    staging_desc.SampleDesc.Count = 1;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    hr = ID3D11Device_CreateTexture2D(backend->device, &staging_desc, NULL,
                                      &staging);
    if (FAILED(hr)) {
        ID3D11VideoProcessorOutputView_Release(output);
        ID3D11Texture2D_Release(texture);
        BackendUnlock(backend);
        return VLC_EGENERIC;
    }

    if (backend->analysis_output != NULL)
        ID3D11VideoProcessorOutputView_Release(backend->analysis_output);
    if (backend->analysis_staging != NULL)
        ID3D11Texture2D_Release(backend->analysis_staging);
    if (backend->analysis_texture != NULL)
        ID3D11Texture2D_Release(backend->analysis_texture);
    backend->analysis_texture = texture;
    backend->analysis_output = output;
    backend->analysis_staging = staging;
    backend->analysis_width = width;
    backend->analysis_height = height;
    BackendUnlock(backend);
    return VLC_SUCCESS;
}

int nsfw_d3d11_readback_rgb(nsfw_d3d11_backend_t *backend,
                            picture_t *picture,
                            uint8_t *rgb, size_t rgb_capacity,
                            int *width, int *height)
{
    nsfw_d3d11_picture_sys_t *picsys;
    ID3D11VideoProcessorInputView *input;
    D3D11_MAPPED_SUBRESOURCE mapped;
    RECT source_rect;
    size_t needed;
    HRESULT hr;

    if (backend == NULL || picture == NULL || rgb == NULL ||
        width == NULL || height == NULL ||
        backend->analysis_texture == NULL ||
        backend->analysis_staging == NULL) {
        return VLC_EGENERIC;
    }
    needed = (size_t)backend->analysis_width * backend->analysis_height * 3;
    if (rgb_capacity < needed)
        return VLC_EGENERIC;

    picsys = PictureSys(picture);
    source_rect = PictureSourceRect(picture);
    BackendLock(backend);
    input = GetInputView(backend, picsys);
    if (input == NULL) {
        BackendUnlock(backend);
        return VLC_EGENERIC;
    }
    hr = ProcessorBlitOne(backend, input, &source_rect,
                          backend->analysis_output,
                          backend->analysis_width,
                          backend->analysis_height);
    if (SUCCEEDED(hr)) {
        ID3D11DeviceContext_CopyResource(
            backend->context, (ID3D11Resource *)backend->analysis_staging,
            (ID3D11Resource *)backend->analysis_texture);
        memset(&mapped, 0, sizeof(mapped));
        hr = ID3D11DeviceContext_Map(
            backend->context, (ID3D11Resource *)backend->analysis_staging,
            0, D3D11_MAP_READ, 0, &mapped);
    }
    if (SUCCEEDED(hr)) {
        for (int y = 0; y < backend->analysis_height; ++y) {
            const uint8_t *src = (const uint8_t *)mapped.pData +
                                 (size_t)y * mapped.RowPitch;
            uint8_t *dst = rgb +
                           (size_t)y * backend->analysis_width * 3;
            for (int x = 0; x < backend->analysis_width; ++x) {
                dst[x * 3 + 0] = src[x * 4 + 2];
                dst[x * 3 + 1] = src[x * 4 + 1];
                dst[x * 3 + 2] = src[x * 4 + 0];
            }
        }
        ID3D11DeviceContext_Unmap(
            backend->context, (ID3D11Resource *)backend->analysis_staging, 0);
    }
    if (SUCCEEDED(hr) && !backend->analysis_logged) {
        fprintf(stderr,
                "icop: D3D11 analysis readback active at %dx%d (no full-frame CPU copy)\n",
                backend->analysis_width, backend->analysis_height);
        backend->analysis_logged = true;
    }
    BackendUnlock(backend);

    if (FAILED(hr))
        return VLC_EGENERIC;
    *width = backend->analysis_width;
    *height = backend->analysis_height;
    return VLC_SUCCESS;
}

int nsfw_d3d11_dump_ppm(nsfw_d3d11_backend_t *backend,
                        picture_t *picture, const char *path)
{
    nsfw_d3d11_picture_sys_t *picsys;
    ID3D11VideoProcessorInputView *input;
    ID3D11Texture2D *bgra = NULL;
    ID3D11VideoProcessorOutputView *bgra_output = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC desc;
    D3D11_MAPPED_SUBRESOURCE mapped;
    RECT source_rect;
    uint8_t *rgb = NULL;
    FILE *file = NULL;
    int width;
    int height;
    HRESULT hr = E_FAIL;

    if (backend == NULL || picture == NULL || path == NULL)
        return VLC_EGENERIC;
    width = picture->format.i_visible_width > 0 ?
            picture->format.i_visible_width : picture->format.i_width;
    height = picture->format.i_visible_height > 0 ?
             picture->format.i_visible_height : picture->format.i_height;
    if (width <= 0 || height <= 0)
        return VLC_EGENERIC;
    rgb = (uint8_t *)malloc((size_t)width * height * 3);
    if (rgb == NULL)
        return VLC_ENOMEM;

    picsys = PictureSys(picture);
    source_rect = PictureSourceRect(picture);
    BackendLock(backend);
    if (CreateBgraTexture(backend, width, height, &bgra, NULL, NULL) !=
        VLC_SUCCESS)
        goto done_locked;
    if (FAILED(CreateOutputView(backend, bgra, 0, &bgra_output)))
        goto done_locked;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(ID3D11Device_CreateTexture2D(backend->device, &desc, NULL,
                                            &staging)))
        goto done_locked;
    input = GetInputView(backend, picsys);
    if (input == NULL || FAILED(ProcessorBlitOne(
            backend, input, &source_rect, bgra_output, width, height)))
        goto done_locked;
    ID3D11DeviceContext_CopyResource(backend->context,
                                     (ID3D11Resource *)staging,
                                     (ID3D11Resource *)bgra);
    memset(&mapped, 0, sizeof(mapped));
    if (FAILED(ID3D11DeviceContext_Map(backend->context,
                                       (ID3D11Resource *)staging, 0,
                                       D3D11_MAP_READ, 0, &mapped)))
        goto done_locked;
    for (int y = 0; y < height; ++y) {
        const uint8_t *src = (const uint8_t *)mapped.pData +
                             (size_t)y * mapped.RowPitch;
        uint8_t *dst = rgb + (size_t)y * width * 3;
        for (int x = 0; x < width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    ID3D11DeviceContext_Unmap(backend->context,
                              (ID3D11Resource *)staging, 0);
    hr = S_OK;

done_locked:
    if (staging != NULL)
        ID3D11Texture2D_Release(staging);
    if (bgra_output != NULL)
        ID3D11VideoProcessorOutputView_Release(bgra_output);
    if (bgra != NULL)
        ID3D11Texture2D_Release(bgra);
    BackendUnlock(backend);

    if (SUCCEEDED(hr)) {
        file = fopen(path, "wb");
        if (file == NULL ||
            fprintf(file, "P6\n%d %d\n255\n", width, height) < 0 ||
            fwrite(rgb, 1, (size_t)width * height * 3, file) !=
                (size_t)width * height * 3) {
            hr = E_FAIL;
        }
        if (file != NULL)
            fclose(file);
    }
    free(rgb);
    return SUCCEEDED(hr) ? VLC_SUCCESS : VLC_EGENERIC;
}

#else

#include "nsfw_filter_d3d11.h"

int nsfw_d3d11_set_analysis_size(nsfw_d3d11_backend_t *backend,
                                 int width, int height)
{
    VLC_UNUSED(backend); VLC_UNUSED(width); VLC_UNUSED(height);
    return VLC_EGENERIC;
}

int nsfw_d3d11_readback_rgb(nsfw_d3d11_backend_t *backend,
                            picture_t *picture, uint8_t *rgb,
                            size_t rgb_capacity, int *width, int *height)
{
    VLC_UNUSED(backend); VLC_UNUSED(picture); VLC_UNUSED(rgb);
    VLC_UNUSED(rgb_capacity); VLC_UNUSED(width); VLC_UNUSED(height);
    return VLC_EGENERIC;
}

int nsfw_d3d11_dump_ppm(nsfw_d3d11_backend_t *backend,
                        picture_t *picture, const char *path)
{
    VLC_UNUSED(backend); VLC_UNUSED(picture); VLC_UNUSED(path);
    return VLC_EGENERIC;
}

#endif /* _WIN32 */
