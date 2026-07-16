/*****************************************************************************
 * nsfw_filter_d3d11_effects.c: D3D11 blocked-frame rendering effects
 *
 * Shader-based blur, solid black, watermark warning, and debug overlay
 * compositing for the NSFW video filter.
 *****************************************************************************/

#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#ifdef _WIN32

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_filter.h>
#include <vlc_fourcc.h>
#include <vlc_picture.h>

#define COBJMACROS
#include <d3d11.h>
#include <d3dcompiler.h>

#include "nsfw_filter_d3d11.h"
#include "nsfw_filter_d3d11_internal.h"
#include "nsfw_d3d11_shaders.h"
#include "nsfw_debug_overlay.h"

#define NSFW_D3D11_BLUR_DOWNSAMPLE 32
#define NSFW_D3D11_PROFILE_MAX_SAMPLES 512
#define NSFW_D3D11_PROFILE_WARMUP_SAMPLES 10
#define NSFW_D3D11_DEBUG_WIDTH 240
#define NSFW_D3D11_DEBUG_HEIGHT 72

typedef struct nsfw_blur_constants_t
{
    float texel_x;
    float texel_y;
    float direction_x;
    float direction_y;
} nsfw_blur_constants_t;

static HRESULT CompileShader(const char *source, const char *target,
                             ID3DBlob **blob)
{
    ID3DBlob *errors = NULL;
    HRESULT hr = D3DCompile(source, strlen(source), "icop", NULL, NULL,
                            "main", target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                            0, blob, &errors);
    if (FAILED(hr) && errors != NULL) {
        fprintf(stderr, "icop: D3D11 shader compile failed: %s\n",
                (const char *)ID3D10Blob_GetBufferPointer(errors));
    }
    if (errors != NULL)
        ID3D10Blob_Release(errors);
    return hr;
}

int CreateShaders(nsfw_d3d11_backend_t *backend)
{
    ID3DBlob *vertex_blob = NULL;
    ID3DBlob *pixel_blob = NULL;
    D3D11_SAMPLER_DESC sampler_desc;
    D3D11_BUFFER_DESC buffer_desc;
    HRESULT hr;

    hr = CompileShader(kFullscreenVertexShader, "vs_4_0", &vertex_blob);
    if (FAILED(hr))
        goto error;
    hr = ID3D11Device_CreateVertexShader(
        backend->device, ID3D10Blob_GetBufferPointer(vertex_blob),
        ID3D10Blob_GetBufferSize(vertex_blob), NULL, &backend->fullscreen_vs);
    if (FAILED(hr))
        goto error;

    hr = CompileShader(kBlurPixelShader, "ps_4_0", &pixel_blob);
    if (FAILED(hr))
        goto error;
    hr = ID3D11Device_CreatePixelShader(
        backend->device, ID3D10Blob_GetBufferPointer(pixel_blob),
        ID3D10Blob_GetBufferSize(pixel_blob), NULL, &backend->blur_ps);
    if (FAILED(hr))
        goto error;

    memset(&sampler_desc, 0, sizeof(sampler_desc));
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler_desc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    hr = ID3D11Device_CreateSamplerState(backend->device, &sampler_desc,
                                         &backend->linear_sampler);
    if (FAILED(hr))
        goto error;

    memset(&buffer_desc, 0, sizeof(buffer_desc));
    buffer_desc.ByteWidth = sizeof(nsfw_blur_constants_t);
    buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    buffer_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = ID3D11Device_CreateBuffer(backend->device, &buffer_desc, NULL,
                                   &backend->blur_constants);
    if (FAILED(hr))
        goto error;

    ID3D10Blob_Release(vertex_blob);
    ID3D10Blob_Release(pixel_blob);
    return VLC_SUCCESS;

error:
    if (vertex_blob != NULL)
        ID3D10Blob_Release(vertex_blob);
    if (pixel_blob != NULL)
        ID3D10Blob_Release(pixel_blob);
    return VLC_EGENERIC;
}

int CreateProfilingQueries(nsfw_d3d11_backend_t *backend)
{
    D3D11_QUERY_DESC desc;

    if (getenv("NSFW_D3D11_PROFILE") == NULL)
        return VLC_SUCCESS;
    memset(&desc, 0, sizeof(desc));
    desc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
    if (FAILED(ID3D11Device_CreateQuery(backend->device, &desc,
                                        &backend->profile_disjoint)))
        return VLC_EGENERIC;
    desc.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(ID3D11Device_CreateQuery(backend->device, &desc,
                                        &backend->profile_start)) ||
        FAILED(ID3D11Device_CreateQuery(backend->device, &desc,
                                        &backend->profile_end)))
        return VLC_EGENERIC;
    backend->profile_enabled = true;
    return VLC_SUCCESS;
}

static void BeginProfile(nsfw_d3d11_backend_t *backend)
{
    if (!backend->profile_enabled)
        return;
    ID3D11DeviceContext_Begin(backend->context,
                              (ID3D11Asynchronous *)backend->profile_disjoint);
    ID3D11DeviceContext_End(backend->context,
                            (ID3D11Asynchronous *)backend->profile_start);
}

static void EndProfile(nsfw_d3d11_backend_t *backend,
                       nsfw_block_style_t style)
{
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint;
    UINT64 start = 0;
    UINT64 end = 0;
    unsigned attempts;
    int index = (int)style;

    if (!backend->profile_enabled || index < 0 || index > 2)
        return;
    ID3D11DeviceContext_End(backend->context,
                            (ID3D11Asynchronous *)backend->profile_end);
    ID3D11DeviceContext_End(backend->context,
                            (ID3D11Asynchronous *)backend->profile_disjoint);

    for (attempts = 0; attempts < 5000; ++attempts) {
        if (ID3D11DeviceContext_GetData(
                backend->context,
                (ID3D11Asynchronous *)backend->profile_disjoint,
                &disjoint, sizeof(disjoint), 0) == S_OK)
            break;
        Sleep(0);
    }
    if (attempts == 5000 || disjoint.Disjoint)
        return;
    if (ID3D11DeviceContext_GetData(
            backend->context, (ID3D11Asynchronous *)backend->profile_start,
            &start, sizeof(start), 0) != S_OK ||
        ID3D11DeviceContext_GetData(
            backend->context, (ID3D11Asynchronous *)backend->profile_end,
            &end, sizeof(end), 0) != S_OK || end < start ||
        disjoint.Frequency == 0) {
        return;
    }
    backend->profile_total_ms[index] +=
        (double)(end - start) * 1000.0 / (double)disjoint.Frequency;
    backend->profile_count[index]++;
    if (backend->profile_sample_count[index] <
        NSFW_D3D11_PROFILE_MAX_SAMPLES) {
        backend->profile_samples[index]
                                [backend->profile_sample_count[index]++] =
            (double)(end - start) * 1000.0 / (double)disjoint.Frequency;
    }
}

static int CompareProfileSamples(const void *left, const void *right)
{
    double a = *(const double *)left;
    double b = *(const double *)right;
    return (a > b) - (a < b);
}

double ProfileMedianMs(const nsfw_d3d11_backend_t *backend, int style)
{
    double sorted[NSFW_D3D11_PROFILE_MAX_SAMPLES];
    unsigned count = backend->profile_sample_count[style];
    unsigned start = count > NSFW_D3D11_PROFILE_WARMUP_SAMPLES
        ? NSFW_D3D11_PROFILE_WARMUP_SAMPLES
        : 0;
    unsigned usable = count - start;

    if (usable == 0)
        return 0.0;
    memcpy(sorted, &backend->profile_samples[style][start],
           (size_t)usable * sizeof(sorted[0]));
    qsort(sorted, usable, sizeof(sorted[0]), CompareProfileSamples);
    if ((usable & 1u) != 0)
        return sorted[usable / 2];
    return (sorted[usable / 2 - 1] + sorted[usable / 2]) * 0.5;
}

static int CreateStaticBgraInput(nsfw_d3d11_backend_t *backend,
                                 int width, int height,
                                 const uint8_t *pixels,
                                 ID3D11Texture2D **texture,
                                 ID3D11VideoProcessorInputView **input)
{
    D3D11_TEXTURE2D_DESC desc;
    D3D11_SUBRESOURCE_DATA data;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.Width = (UINT)width;
    desc.Height = (UINT)height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    memset(&data, 0, sizeof(data));
    data.pSysMem = pixels;
    data.SysMemPitch = (UINT)width * 4;
    data.SysMemSlicePitch = data.SysMemPitch * (UINT)height;

    hr = ID3D11Device_CreateTexture2D(backend->device, &desc, &data, texture);
    if (FAILED(hr))
        return VLC_EGENERIC;
    hr = CreateInputView(backend, *texture, 0, input);
    return SUCCEEDED(hr) ? VLC_SUCCESS : VLC_EGENERIC;
}

static void DrawWatermarkPixels(uint8_t *pixels, int size)
{
    int half = size / 2;
    int stroke = size / 12;
    int symbol_width = size / 9;

    if (stroke < 2)
        stroke = 2;
    if (symbol_width < 2)
        symbol_width = 2;

    for (int row = 0; row < size; ++row) {
        int span = (half * row) / (size - 1);
        int left = half - span;
        int right = half + span;
        for (int edge = 0; edge < 2; ++edge) {
            int x0 = edge == 0 ? left : right - stroke + 1;
            for (int x = 0; x < stroke; ++x) {
                int px = x0 + x;
                if (px >= 0 && px < size) {
                    uint8_t *dst = pixels + ((size_t)row * size + px) * 4;
                    dst[0] = 255; dst[1] = 255;
                    dst[2] = 255; dst[3] = 255;
                }
            }
        }
        if (row >= size - stroke) {
            for (int x = left; x <= right; ++x) {
                uint8_t *dst = pixels + ((size_t)row * size + x) * 4;
                dst[0] = 255; dst[1] = 255;
                dst[2] = 255; dst[3] = 255;
            }
        }
    }

    for (int row = size / 3; row < size / 3 + size / 4; ++row) {
        for (int x = half - symbol_width / 2;
             x < half - symbol_width / 2 + symbol_width; ++x) {
            uint8_t *dst = pixels + ((size_t)row * size + x) * 4;
            dst[0] = 255; dst[1] = 255;
            dst[2] = 255; dst[3] = 255;
        }
    }
    for (int row = (size * 3) / 4;
         row < (size * 3) / 4 + symbol_width; ++row) {
        for (int x = half - symbol_width / 2;
             x < half - symbol_width / 2 + symbol_width; ++x) {
            uint8_t *dst = pixels + ((size_t)row * size + x) * 4;
            dst[0] = 255; dst[1] = 255;
            dst[2] = 255; dst[3] = 255;
        }
    }
}

static void FillDebugRect(uint8_t *pixels, int x, int y,
                          int width, int height,
                          uint8_t red, uint8_t green, uint8_t blue,
                          uint8_t alpha)
{
    int right = x + width;
    int bottom = y + height;

    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (right > NSFW_D3D11_DEBUG_WIDTH)
        right = NSFW_D3D11_DEBUG_WIDTH;
    if (bottom > NSFW_D3D11_DEBUG_HEIGHT)
        bottom = NSFW_D3D11_DEBUG_HEIGHT;
    for (int row = y; row < bottom; ++row) {
        for (int column = x; column < right; ++column) {
            uint8_t *pixel = pixels +
                ((size_t)row * NSFW_D3D11_DEBUG_WIDTH + column) * 4;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = alpha;
        }
    }
}

static void DrawDebugPixels(uint8_t *pixels, float score, float threshold)
{
    static const uint8_t glyphs[][5] = {
        { 7, 5, 5, 5, 7 }, { 2, 6, 2, 2, 7 },
        { 7, 1, 7, 4, 7 }, { 7, 1, 7, 1, 7 },
        { 5, 5, 7, 1, 1 }, { 7, 4, 7, 1, 7 },
        { 7, 4, 7, 5, 7 }, { 7, 1, 2, 2, 2 },
        { 7, 5, 7, 5, 7 }, { 7, 5, 7, 1, 7 },
        { 0, 0, 0, 0, 2 }, { 1, 2, 2, 4, 4 },
    };
    char text[16];
    const int scale = 4;
    const int panel_x = 4;
    const int panel_y = 4;
    const int margin = 12;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
    int bar_width;
    int fill_width;
    size_t length;

    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;
    memset(pixels, 0,
           (size_t)NSFW_D3D11_DEBUG_WIDTH * NSFW_D3D11_DEBUG_HEIGHT * 4);
    snprintf(text, sizeof(text), "%.3f/%.3f", score, threshold);
    length = strlen(text);
    bar_width = (int)length * scale * 4;
    FillDebugRect(pixels, panel_x, panel_y, bar_width + margin * 2, 60,
                  0, 0, 0, 210);
    DebugScoreColor(score, threshold, &red, &green, &blue);
    for (size_t index = 0; index < length; ++index) {
        int glyph = DebugGlyphIndex(text[index]);
        int glyph_x = panel_x + margin + (int)index * scale * 4;

        if (glyph < 0)
            continue;
        for (int row = 0; row < 5; ++row) {
            for (int column = 0; column < 3; ++column) {
                if ((glyphs[glyph][row] & (1u << (2 - column))) != 0) {
                    FillDebugRect(pixels, glyph_x + column * scale,
                                  panel_y + margin + row * scale,
                                  scale, scale, red, green, blue, 255);
                }
            }
        }
    }
    if (threshold > 0.001f)
        fill_width = (int)(bar_width * score / threshold + 0.5f);
    else
        fill_width = bar_width;
    if (fill_width > bar_width)
        fill_width = bar_width;
    FillDebugRect(pixels, panel_x + margin, panel_y + 44,
                  fill_width, scale, red, green, blue, 255);
}

int CreateEffectResources(nsfw_d3d11_backend_t *backend,
                          int frame_width, int frame_height)
{
    D3D11_TEXTURE2D_DESC render_desc;
    uint8_t *watermark_pixels;
    uint8_t black_pixel[4] = { 0, 0, 0, 255 };
    HRESULT hr;

    render_desc = backend->output_desc;
    render_desc.MipLevels = 1;
    render_desc.ArraySize = 1;
    render_desc.SampleDesc.Count = 1;
    render_desc.SampleDesc.Quality = 0;
    render_desc.Usage = D3D11_USAGE_DEFAULT;
    render_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    render_desc.CPUAccessFlags = 0;
    render_desc.MiscFlags = 0;
    if (FAILED(ID3D11Device_CreateTexture2D(
            backend->device, &render_desc, NULL,
            &backend->render_texture)) ||
        FAILED(CreateOutputView(backend, backend->render_texture, 0,
                                &backend->render_output))) {
        fprintf(stderr,
                "icop: D3D11 failed to create full-size render target\n");
        return VLC_EGENERIC;
    }

    backend->blur_width =
        (frame_width + NSFW_D3D11_BLUR_DOWNSAMPLE - 1) /
        NSFW_D3D11_BLUR_DOWNSAMPLE;
    backend->blur_height =
        (frame_height + NSFW_D3D11_BLUR_DOWNSAMPLE - 1) /
        NSFW_D3D11_BLUR_DOWNSAMPLE;
    if (backend->blur_width < 16)
        backend->blur_width = 16;
    if (backend->blur_height < 16)
        backend->blur_height = 16;

    for (int i = 0; i < 2; ++i) {
        if (CreateBgraTexture(backend, backend->blur_width,
                              backend->blur_height,
                              &backend->blur_texture[i],
                              &backend->blur_rtv[i],
                              &backend->blur_srv[i]) != VLC_SUCCESS) {
            fprintf(stderr,
                    "icop: D3D11 failed to create blur texture %d\n",
                    i);
            return VLC_EGENERIC;
        }
    }
    hr = CreateOutputView(backend, backend->blur_texture[0], 0,
                          &backend->blur_down_output);
    if (FAILED(hr))
    {
        fprintf(stderr,
                "icop: D3D11 failed to create blur output view (0x%08lx)\n",
                (unsigned long)hr);
        return VLC_EGENERIC;
    }
    hr = CreateInputView(backend, backend->blur_texture[0], 0,
                         &backend->blur_up_input);
    if (FAILED(hr))
    {
        fprintf(stderr,
                "icop: D3D11 failed to create blur input view (0x%08lx)\n",
                (unsigned long)hr);
        return VLC_EGENERIC;
    }

    backend->watermark_width = 72;
    backend->watermark_height = 72;
    watermark_pixels = (uint8_t *)calloc(
        (size_t)backend->watermark_width * backend->watermark_height, 4);
    if (watermark_pixels == NULL)
        return VLC_ENOMEM;
    DrawWatermarkPixels(watermark_pixels, backend->watermark_width);
    if (CreateStaticBgraInput(backend, backend->watermark_width,
                              backend->watermark_height, watermark_pixels,
                              &backend->watermark_texture,
                              &backend->watermark_input) != VLC_SUCCESS) {
        free(watermark_pixels);
        fprintf(stderr,
                "icop: D3D11 failed to create watermark texture/view\n");
        return VLC_EGENERIC;
    }
    free(watermark_pixels);

    if (CreateStaticBgraInput(backend, 1, 1, black_pixel,
                              &backend->black_texture,
                              &backend->black_input) != VLC_SUCCESS) {
        fprintf(stderr,
                "icop: D3D11 failed to create black texture/view\n");
        return VLC_EGENERIC;
    }

    backend->debug_pixels = (uint8_t *)calloc(
        (size_t)NSFW_D3D11_DEBUG_WIDTH * NSFW_D3D11_DEBUG_HEIGHT, 4);
    if (backend->debug_pixels == NULL)
        return VLC_ENOMEM;
    if (CreateStaticBgraInput(backend, NSFW_D3D11_DEBUG_WIDTH,
                              NSFW_D3D11_DEBUG_HEIGHT,
                              backend->debug_pixels,
                              &backend->debug_texture,
                              &backend->debug_input) != VLC_SUCCESS) {
        fprintf(stderr,
                "icop: D3D11 failed to create debug overlay texture/view\n");
        return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
}

static HRESULT RenderBlack(nsfw_d3d11_backend_t *backend,
                           ID3D11VideoProcessorOutputView *output,
                           int width, int height)
{
    RECT source = { 0, 0, 1, 1 };
    return ProcessorBlitOne(backend, backend->black_input, &source, output,
                            width, height);
}

static HRESULT RenderOverlay(nsfw_d3d11_backend_t *backend,
                             ID3D11VideoProcessorInputView *source_input,
                             const RECT *source_rect,
                             ID3D11VideoProcessorInputView *overlay_input,
                             const RECT *overlay_source,
                             const RECT *overlay_destination,
                             ID3D11VideoProcessorOutputView *output,
                             int width, int height)
{
    D3D11_VIDEO_PROCESSOR_STREAM streams[2];
    RECT frame_destination = { 0, 0, width, height };

    memset(streams, 0, sizeof(streams));
    streams[0].Enable = TRUE;
    streams[0].pInputSurface = source_input;
    streams[1].Enable = TRUE;
    streams[1].pInputSurface = overlay_input;
    ConfigureProcessorOutput(backend, width, height);
    ConfigureProcessorStream(backend, 0, source_rect, &frame_destination,
                             false);
    ConfigureProcessorStream(backend, 1, overlay_source,
                             overlay_destination, true);
    return ID3D11VideoContext_VideoProcessorBlt(
        backend->video_context, backend->processor, output, 0, 2, streams);
}

static HRESULT RenderWarning(nsfw_d3d11_backend_t *backend,
                             ID3D11VideoProcessorInputView *source_input,
                             const RECT *source_rect,
                             ID3D11VideoProcessorOutputView *output,
                             int width, int height)
{
    int min_dim = width < height ? width : height;
    int size = min_dim / 8;
    int margin;
    RECT watermark_source = {
        0, 0, backend->watermark_width, backend->watermark_height
    };
    RECT watermark_destination;

    if (size < 20)
        size = 20;
    if (size > 72)
        size = 72;
    margin = size / 4;
    if (margin < 4)
        margin = 4;
    watermark_destination.left = width - size - margin;
    watermark_destination.top = height - size - margin;
    watermark_destination.right = watermark_destination.left + size;
    watermark_destination.bottom = watermark_destination.top + size;

    return RenderOverlay(backend, source_input, source_rect,
                         backend->watermark_input, &watermark_source,
                         &watermark_destination, output, width, height);
}

static HRESULT RenderBlurPass(nsfw_d3d11_backend_t *backend,
                              int source_index, int target_index,
                              float direction_x, float direction_y)
{
    D3D11_VIEWPORT viewport;
    nsfw_blur_constants_t constants;
    ID3D11ShaderResourceView *source = backend->blur_srv[source_index];
    ID3D11ShaderResourceView *null_srv = NULL;
    ID3D11RenderTargetView *null_rtv = NULL;

    memset(&viewport, 0, sizeof(viewport));
    viewport.Width = (float)backend->blur_width;
    viewport.Height = (float)backend->blur_height;
    viewport.MaxDepth = 1.0f;
    constants.texel_x = 1.0f / backend->blur_width;
    constants.texel_y = 1.0f / backend->blur_height;
    constants.direction_x = direction_x;
    constants.direction_y = direction_y;

    ID3D11DeviceContext_UpdateSubresource(
        backend->context, (ID3D11Resource *)backend->blur_constants,
        0, NULL, &constants, 0, 0);
    ID3D11DeviceContext_OMSetRenderTargets(
        backend->context, 1, &backend->blur_rtv[target_index], NULL);
    ID3D11DeviceContext_RSSetViewports(backend->context, 1, &viewport);
    ID3D11DeviceContext_IASetPrimitiveTopology(
        backend->context, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(backend->context,
                                   backend->fullscreen_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(backend->context,
                                   backend->blur_ps, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(backend->context, 0, 1,
                                             &source);
    ID3D11DeviceContext_PSSetSamplers(backend->context, 0, 1,
                                     &backend->linear_sampler);
    ID3D11DeviceContext_PSSetConstantBuffers(backend->context, 0, 1,
                                             &backend->blur_constants);
    ID3D11DeviceContext_Draw(backend->context, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(backend->context, 0, 1,
                                             &null_srv);
    ID3D11DeviceContext_OMSetRenderTargets(backend->context, 1, &null_rtv,
                                           NULL);
    return S_OK;
}

static HRESULT RenderBlur(nsfw_d3d11_backend_t *backend,
                          ID3D11VideoProcessorInputView *source_input,
                          const RECT *source_rect,
                          ID3D11VideoProcessorOutputView *output,
                          int width, int height)
{
    RECT blur_source = {
        0, 0, backend->blur_width, backend->blur_height
    };
    HRESULT hr;

    hr = ProcessorBlitOne(backend, source_input, source_rect,
                          backend->blur_down_output,
                          backend->blur_width, backend->blur_height);
    if (FAILED(hr))
        return hr;
    hr = RenderBlurPass(backend, 0, 1, 1.0f, 0.0f);
    if (FAILED(hr))
        return hr;
    hr = RenderBlurPass(backend, 1, 0, 0.0f, 1.0f);
    if (FAILED(hr))
        return hr;
    return ProcessorBlitOne(backend, backend->blur_up_input, &blur_source,
                            output, width, height);
}

picture_t *nsfw_d3d11_render_blocked(filter_t *filter,
                                     nsfw_d3d11_backend_t *backend,
                                     picture_t *source,
                                     nsfw_block_style_t style)
{
    nsfw_d3d11_picture_sys_t *source_sys;
    nsfw_d3d11_picture_sys_t *output_sys;
    ID3D11VideoProcessorInputView *input;
    picture_t *output;
    RECT source_rect;
    int width;
    int height;
    bool profile_started = false;
    nsfw_block_style_t rendered_style = style;
    HRESULT effect_hr = E_FAIL;
    HRESULT hr = E_FAIL;

    if (filter == NULL || backend == NULL || source == NULL)
        return NULL;
    output = NewPicture(filter);
    if (output == NULL) {
        ReleasePicture(source);
        return NULL;
    }
    CopyPictureProperties(output, source);
    source_sys = PictureSys(source);
    output_sys = PictureSys(output);
    source_rect = PictureSourceRect(source);
    width = output->format.i_visible_width > 0 ?
            output->format.i_visible_width : output->format.i_width;
    height = output->format.i_visible_height > 0 ?
             output->format.i_visible_height : output->format.i_height;

    BackendLock(backend);
    input = GetInputView(backend, source_sys);
    if (input != NULL && output_sys != NULL &&
        output_sys->texture[0] != NULL && backend->render_output != NULL) {
        BeginProfile(backend);
        profile_started = backend->profile_enabled;
        switch (style) {
            case NSFW_BLOCK_STYLE_BLUR:
                hr = RenderBlur(backend, input, &source_rect,
                                backend->render_output,
                                width, height);
                break;
            case NSFW_BLOCK_STYLE_WARNING:
                hr = RenderWarning(backend, input, &source_rect,
                                   backend->render_output,
                                   width, height);
                break;
            case NSFW_BLOCK_STYLE_BLACK:
            default:
                hr = RenderBlack(backend, backend->render_output,
                                 width, height);
                break;
        }
        effect_hr = hr;
        if (FAILED(effect_hr) && style != NSFW_BLOCK_STYLE_BLACK) {
            fprintf(stderr,
                    "icop: D3D11 effect style=%d failed hr=0x%08lx; attempting GPU black fallback\n",
                    (int)style, (unsigned long)effect_hr);
            if ((int)style >= 0 && (int)style <= 2)
                backend->fallback_count[(int)style]++;
            rendered_style = NSFW_BLOCK_STYLE_BLACK;
            hr = RenderBlack(backend, backend->render_output,
                             width, height);
        }
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_CopySubresourceRegion(
                backend->context,
                output_sys->resource[0], output_sys->slice_index,
                0, 0, 0, (ID3D11Resource *)backend->render_texture,
                0, NULL);
            if ((int)rendered_style >= 0 && (int)rendered_style <= 2)
                backend->rendered_count[(int)rendered_style]++;
        }
    }
    if (profile_started)
        EndProfile(backend, rendered_style);
    if (FAILED(hr)) {
        fprintf(stderr,
                "icop: D3D11 blocked render style=%d failed hr=0x%08lx input=%p output=%p\n",
                (int)style, (unsigned long)hr, (void *)input,
                (void *)backend->render_output);
    }
    BackendUnlock(backend);

    ReleasePicture(source);
    if (FAILED(hr)) {
        ReleasePicture(output);
        return NULL;
    }
    return output;
}

static void UpdateDebugTexture(nsfw_d3d11_backend_t *backend,
                               float score, float threshold)
{
    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    if (threshold < 0.0f) threshold = 0.0f;
    if (threshold > 1.0f) threshold = 1.0f;
    if (backend->debug_cache_valid && backend->debug_score == score &&
        backend->debug_threshold == threshold) {
        return;
    }

    DrawDebugPixels(backend->debug_pixels, score, threshold);
    ID3D11DeviceContext_UpdateSubresource(
        backend->context, (ID3D11Resource *)backend->debug_texture,
        0, NULL, backend->debug_pixels, NSFW_D3D11_DEBUG_WIDTH * 4, 0);
    backend->debug_score = score;
    backend->debug_threshold = threshold;
    backend->debug_cache_valid = true;
}

picture_t *nsfw_d3d11_render_debug_overlay(filter_t *filter,
                                           nsfw_d3d11_backend_t *backend,
                                           picture_t *source,
                                           float score, float threshold)
{
    nsfw_d3d11_picture_sys_t *source_sys;
    nsfw_d3d11_picture_sys_t *output_sys;
    ID3D11VideoProcessorInputView *input = NULL;
    picture_t *output;
    RECT source_rect;
    RECT overlay_source = {
        0, 0, NSFW_D3D11_DEBUG_WIDTH, NSFW_D3D11_DEBUG_HEIGHT
    };
    RECT overlay_destination;
    int width;
    int height;
    int min_dim;
    int overlay_width;
    int overlay_height;
    int margin;
    HRESULT hr = E_FAIL;

    if (filter == NULL || backend == NULL || source == NULL)
        return source;
    width = source->format.i_visible_width > 0 ?
            source->format.i_visible_width : source->format.i_width;
    height = source->format.i_visible_height > 0 ?
             source->format.i_visible_height : source->format.i_height;
    if (width <= 0 || height <= 0)
        return source;

    output = NewPicture(filter);
    if (output == NULL)
        return source;
    CopyPictureProperties(output, source);
    source_sys = PictureSys(source);
    output_sys = PictureSys(output);
    source_rect = PictureSourceRect(source);
    min_dim = width < height ? width : height;
    overlay_width = (min_dim * 5) / 18;
    if (overlay_width < 160) overlay_width = 160;
    if (overlay_width > 360) overlay_width = 360;
    margin = overlay_width / 24;
    if (margin < 4) margin = 4;
    if (overlay_width > width - margin * 2)
        overlay_width = width - margin * 2;
    overlay_height = overlay_width * NSFW_D3D11_DEBUG_HEIGHT /
                     NSFW_D3D11_DEBUG_WIDTH;
    if (overlay_height > height - margin * 2) {
        overlay_height = height - margin * 2;
        overlay_width = overlay_height * NSFW_D3D11_DEBUG_WIDTH /
                        NSFW_D3D11_DEBUG_HEIGHT;
    }
    overlay_destination.left = margin;
    overlay_destination.top = margin;
    overlay_destination.right = margin + overlay_width;
    overlay_destination.bottom = margin + overlay_height;

    BackendLock(backend);
    input = GetInputView(backend, source_sys);
    if (input != NULL && output_sys != NULL &&
        output_sys->texture[0] != NULL && backend->render_output != NULL &&
        backend->debug_input != NULL && overlay_width > 0 &&
        overlay_height > 0) {
        UpdateDebugTexture(backend, score, threshold);
        hr = RenderOverlay(backend, input, &source_rect,
                           backend->debug_input, &overlay_source,
                           &overlay_destination, backend->render_output,
                           width, height);
        if (SUCCEEDED(hr)) {
            ID3D11DeviceContext_CopySubresourceRegion(
                backend->context,
                output_sys->resource[0], output_sys->slice_index,
                0, 0, 0, (ID3D11Resource *)backend->render_texture,
                0, NULL);
            backend->debug_rendered_count++;
            if (!backend->debug_logged) {
                fprintf(stderr,
                        "icop: D3D11 debug overlay active (cached GPU composition)\n");
                backend->debug_logged = true;
            }
        }
    }
    if (FAILED(hr) && !backend->debug_failure_logged) {
        fprintf(stderr,
                "icop: D3D11 debug overlay failed hr=0x%08lx; preserving original frame\n",
                (unsigned long)hr);
        backend->debug_failure_logged = true;
    }
    BackendUnlock(backend);

    if (FAILED(hr)) {
        ReleasePicture(output);
        return source;
    }
    ReleasePicture(source);
    return output;
}

#else

#include "nsfw_filter_d3d11.h"

picture_t *nsfw_d3d11_render_blocked(filter_t *filter,
                                     nsfw_d3d11_backend_t *backend,
                                     picture_t *source,
                                     nsfw_block_style_t style)
{
    VLC_UNUSED(filter); VLC_UNUSED(backend); VLC_UNUSED(source);
    VLC_UNUSED(style);
    return NULL;
}

picture_t *nsfw_d3d11_render_debug_overlay(filter_t *filter,
                                           nsfw_d3d11_backend_t *backend,
                                           picture_t *source,
                                           float score, float threshold)
{
    VLC_UNUSED(filter); VLC_UNUSED(backend); VLC_UNUSED(score);
    VLC_UNUSED(threshold);
    return source;
}

#endif /* _WIN32 */
