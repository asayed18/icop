/*****************************************************************************
 * nsfw_filter_d3d11.c: D3D11 backend for the NSFW video filter
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
#include <initguid.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>

#include "nsfw_filter_d3d11.h"

#define NSFW_D3D11_MAX_VIEWS 64
#define NSFW_D3D11_PLANE_COUNT 4
#define NSFW_D3D11_BLUR_DOWNSAMPLE 32
#define NSFW_D3D11_PROFILE_MAX_SAMPLES 512
#define NSFW_D3D11_PROFILE_WARMUP_SAMPLES 10
#define NSFW_D3D11_DEBUG_WIDTH 240
#define NSFW_D3D11_DEBUG_HEIGHT 72

DEFINE_GUID(NSFW_GUID_CONTEXT_MUTEX,
            0x472e8835, 0x3f8e, 0x4f93, 0xa0, 0xcb,
            0x25, 0x79, 0x77, 0x6c, 0xed, 0x86);

/* VLC 3.0.21 private D3D11 picture ABI. Keep this layout pinned to the
 * installed runtime instead of depending on uninstalled private headers. */
typedef struct nsfw_d3d11_picture_sys_t
{
    ID3D11VideoDecoderOutputView *decoder;
    union {
        ID3D11Texture2D *texture[NSFW_D3D11_PLANE_COUNT];
        ID3D11Resource *resource[NSFW_D3D11_PLANE_COUNT];
    };
    ID3D11DeviceContext *context;
    unsigned slice_index;
    ID3D11VideoProcessorInputView *processorInput;
    ID3D11VideoProcessorOutputView *processorOutput;
    ID3D11ShaderResourceView *resourceView[NSFW_D3D11_PLANE_COUNT];
    DXGI_FORMAT formatTexture;
} nsfw_d3d11_picture_sys_t;

typedef struct nsfw_d3d11_va_context_t
{
    picture_context_t context;
    void *va_surface;
    nsfw_d3d11_picture_sys_t picsys;
} nsfw_d3d11_va_context_t;

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

typedef struct nsfw_blur_constants_t
{
    float texel_x;
    float texel_y;
    float direction_x;
    float direction_y;
} nsfw_blur_constants_t;

static const char kFullscreenVertexShader[] =
    "struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };\n"
    "VSOut main(uint id : SV_VertexID) {\n"
    "  VSOut o;\n"
    "  float2 p = float2((id << 1) & 2, id & 2);\n"
    "  o.uv = p;\n"
    "  o.pos = float4(p * float2(2,-2) + float2(-1,1), 0, 1);\n"
    "  return o;\n"
    "}\n";

static const char kBlurPixelShader[] =
    "Texture2D image : register(t0);\n"
    "SamplerState linearClamp : register(s0);\n"
    "cbuffer BlurData : register(b0) { float2 texel; float2 direction; };\n"
    "float4 main(float4 pos : SV_POSITION, float2 uv : TEXCOORD0) : SV_TARGET {\n"
    "  float2 step = texel * direction;\n"
    "  float4 c = image.Sample(linearClamp, uv) * 0.2270270270;\n"
    "  c += image.Sample(linearClamp, uv + step * 1.0) * 0.1945945946;\n"
    "  c += image.Sample(linearClamp, uv - step * 1.0) * 0.1945945946;\n"
    "  c += image.Sample(linearClamp, uv + step * 2.0) * 0.1216216216;\n"
    "  c += image.Sample(linearClamp, uv - step * 2.0) * 0.1216216216;\n"
    "  c += image.Sample(linearClamp, uv + step * 3.0) * 0.0540540541;\n"
    "  c += image.Sample(linearClamp, uv - step * 3.0) * 0.0540540541;\n"
    "  c += image.Sample(linearClamp, uv + step * 4.0) * 0.0162162162;\n"
    "  c += image.Sample(linearClamp, uv - step * 4.0) * 0.0162162162;\n"
    "  return c;\n"
    "}\n";

static nsfw_d3d11_picture_sys_t *PictureSys(picture_t *picture)
{
    if (picture == NULL)
        return NULL;
    if (picture->context != NULL)
        return &((nsfw_d3d11_va_context_t *)picture->context)->picsys;
    return (nsfw_d3d11_picture_sys_t *)picture->p_sys;
}

static picture_t *NewPicture(filter_t *filter)
{
    if (filter == NULL || filter->owner.video.buffer_new == NULL)
        return NULL;
    return filter->owner.video.buffer_new(filter);
}

static void ReleasePicture(picture_t *picture)
{
    typedef void (*release_fn_t)(picture_t *);
    static release_fn_t release_fn = NULL;
    static bool loaded = false;

    if (picture == NULL)
        return;
    if (!loaded) {
        HMODULE core = GetModuleHandleW(L"libvlccore.dll");
        if (core != NULL)
            release_fn = (release_fn_t)GetProcAddress(core,
                                                       "picture_Release");
        loaded = true;
    }
    if (release_fn != NULL)
        release_fn(picture);
}

static void CopyPictureProperties(picture_t *destination,
                                  const picture_t *source)
{
    if (destination == NULL || source == NULL)
        return;
    destination->date = source->date;
    destination->b_force = source->b_force;
    destination->b_progressive = source->b_progressive;
    destination->b_top_field_first = source->b_top_field_first;
    destination->i_nb_fields = source->i_nb_fields;
}

static void BackendLock(nsfw_d3d11_backend_t *backend)
{
    EnterCriticalSection(&backend->api_lock);
    if (backend->context_mutex != NULL &&
        backend->context_mutex != INVALID_HANDLE_VALUE) {
        WaitForSingleObjectEx(backend->context_mutex, INFINITE, FALSE);
    }
}

static void BackendUnlock(nsfw_d3d11_backend_t *backend)
{
    if (backend->context_mutex != NULL &&
        backend->context_mutex != INVALID_HANDLE_VALUE) {
        ReleaseMutex(backend->context_mutex);
    }
    LeaveCriticalSection(&backend->api_lock);
}

static const char *DxgiFormatName(DXGI_FORMAT format)
{
    switch (format) {
        case DXGI_FORMAT_NV12: return "NV12";
        case DXGI_FORMAT_P010: return "P010";
        case DXGI_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
        default: return "unknown";
    }
}

bool nsfw_d3d11_is_opaque(vlc_fourcc_t chroma)
{
    return chroma == VLC_CODEC_D3D11_OPAQUE ||
           chroma == VLC_CODEC_D3D11_OPAQUE_10B;
}

const char *nsfw_d3d11_adapter_name(const nsfw_d3d11_backend_t *backend)
{
    return backend != NULL ? backend->adapter_name : "unavailable";
}

const char *nsfw_d3d11_texture_format(const nsfw_d3d11_backend_t *backend)
{
    return backend != NULL ? backend->texture_format : "unknown";
}

static void ReleaseViewCache(nsfw_d3d11_backend_t *backend)
{
    for (unsigned i = 0; i < NSFW_D3D11_MAX_VIEWS; ++i) {
        nsfw_d3d11_view_entry_t *entry = &backend->views[i];
        if (entry->input != NULL)
            ID3D11VideoProcessorInputView_Release(entry->input);
        memset(entry, 0, sizeof(*entry));
    }
}

static HRESULT CreateInputView(nsfw_d3d11_backend_t *backend,
                               ID3D11Texture2D *texture, UINT slice,
                               ID3D11VideoProcessorInputView **view)
{
    D3D11_VIDEO_PROCESSOR_INPUT_VIEW_DESC desc;

    memset(&desc, 0, sizeof(desc));
    desc.FourCC = 0;
    desc.ViewDimension = D3D11_VPIV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipSlice = 0;
    desc.Texture2D.ArraySlice = slice;
    return ID3D11VideoDevice_CreateVideoProcessorInputView(
        backend->video_device, (ID3D11Resource *)texture,
        backend->processor_enum, &desc, view);
}

static HRESULT CreateOutputView(nsfw_d3d11_backend_t *backend,
                                ID3D11Texture2D *texture, UINT slice,
                                ID3D11VideoProcessorOutputView **view)
{
    D3D11_VIDEO_PROCESSOR_OUTPUT_VIEW_DESC desc;

    memset(&desc, 0, sizeof(desc));
    desc.ViewDimension = D3D11_VPOV_DIMENSION_TEXTURE2DARRAY;
    desc.Texture2DArray.MipSlice = 0;
    desc.Texture2DArray.FirstArraySlice = slice;
    desc.Texture2DArray.ArraySize = 1;
    return ID3D11VideoDevice_CreateVideoProcessorOutputView(
        backend->video_device, (ID3D11Resource *)texture,
        backend->processor_enum, &desc, view);
}

static nsfw_d3d11_view_entry_t *FindCachedView(
    nsfw_d3d11_backend_t *backend, ID3D11Texture2D *texture, UINT slice)
{
    nsfw_d3d11_view_entry_t *empty = NULL;

    for (unsigned i = 0; i < NSFW_D3D11_MAX_VIEWS; ++i) {
        nsfw_d3d11_view_entry_t *entry = &backend->views[i];
        if (entry->texture == texture && entry->slice == slice)
            return entry;
        if (empty == NULL && entry->texture == NULL)
            empty = entry;
    }

    if (empty == NULL) {
        empty = &backend->views[backend->next_view++ % NSFW_D3D11_MAX_VIEWS];
        if (empty->input != NULL)
            ID3D11VideoProcessorInputView_Release(empty->input);
    }

    memset(empty, 0, sizeof(*empty));
    empty->texture = texture;
    empty->slice = slice;
    return empty;
}

static ID3D11VideoProcessorInputView *GetInputView(
    nsfw_d3d11_backend_t *backend, nsfw_d3d11_picture_sys_t *picsys)
{
    nsfw_d3d11_view_entry_t *entry;

    if (picsys == NULL || picsys->texture[0] == NULL)
        return NULL;
    entry = FindCachedView(backend, picsys->texture[0], picsys->slice_index);
    if (entry->input == NULL) {
        HRESULT hr = CreateInputView(backend, picsys->texture[0],
                                     picsys->slice_index, &entry->input);
        if (FAILED(hr)) {
            fprintf(stderr,
                    "icop: D3D11 input view failed slice=%u hr=0x%08lx\n",
                    picsys->slice_index, (unsigned long)hr);
            return NULL;
        }
    }
    return entry->input;
}

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

static int CreateShaders(nsfw_d3d11_backend_t *backend)
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

static int CreateProfilingQueries(nsfw_d3d11_backend_t *backend)
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

static double ProfileMedianMs(const nsfw_d3d11_backend_t *backend, int style)
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

static int CreateBgraTexture(nsfw_d3d11_backend_t *backend,
                             int width, int height,
                             ID3D11Texture2D **texture,
                             ID3D11RenderTargetView **rtv,
                             ID3D11ShaderResourceView **srv)
{
    D3D11_TEXTURE2D_DESC desc;
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
    hr = ID3D11Device_CreateTexture2D(backend->device, &desc, NULL, texture);
    if (FAILED(hr))
        return VLC_EGENERIC;

    if (rtv != NULL) {
        hr = ID3D11Device_CreateRenderTargetView(
            backend->device, (ID3D11Resource *)*texture, NULL, rtv);
        if (FAILED(hr))
            return VLC_EGENERIC;
    }
    if (srv != NULL) {
        hr = ID3D11Device_CreateShaderResourceView(
            backend->device, (ID3D11Resource *)*texture, NULL, srv);
        if (FAILED(hr))
            return VLC_EGENERIC;
    }
    return VLC_SUCCESS;
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

static int DebugGlyphIndex(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character == '.')
        return 10;
    if (character == '/')
        return 11;
    return -1;
}

static void DebugScoreColor(float score, float threshold,
                            uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const uint8_t low[] = { 0x28, 0xC7, 0x62 };
    const uint8_t middle[] = { 0xFF, 0xA6, 0x2A };
    const uint8_t high[] = { 0xE5, 0x34, 0x30 };
    const uint8_t *start = low;
    const uint8_t *end = middle;
    float progress;

    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    if (threshold < 0.001f) threshold = 0.001f;
    if (threshold > 0.999f) threshold = 0.999f;
    if (score >= threshold) {
        start = high;
        end = high;
        progress = 0.0f;
    } else {
        progress = score / threshold;
        if (progress <= 0.70f) {
            end = middle;
            progress /= 0.70f;
        } else {
            start = middle;
            end = high;
            progress = (progress - 0.70f) / 0.30f;
        }
    }
    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;
    *red = (uint8_t)(start[0] + (end[0] - start[0]) * progress + 0.5f);
    *green = (uint8_t)(start[1] + (end[1] - start[1]) * progress + 0.5f);
    *blue = (uint8_t)(start[2] + (end[2] - start[2]) * progress + 0.5f);
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

static int CreateEffectResources(nsfw_d3d11_backend_t *backend,
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

static int CreateProcessor(filter_t *filter, nsfw_d3d11_backend_t *backend)
{
    D3D11_VIDEO_PROCESSOR_CONTENT_DESC desc;
    D3D11_VIDEO_PROCESSOR_CAPS caps;
    UINT flags = 0;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.InputFrameFormat = D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE;
    desc.InputFrameRate.Numerator = filter->fmt_in.video.i_frame_rate > 0 ?
                                    filter->fmt_in.video.i_frame_rate : 30;
    desc.InputFrameRate.Denominator = filter->fmt_in.video.i_frame_rate_base > 0 ?
                                      filter->fmt_in.video.i_frame_rate_base : 1;
    desc.InputWidth = filter->fmt_in.video.i_width;
    desc.InputHeight = filter->fmt_in.video.i_height;
    desc.OutputWidth = filter->fmt_out.video.i_width;
    desc.OutputHeight = filter->fmt_out.video.i_height;
    desc.OutputFrameRate = desc.InputFrameRate;
    desc.Usage = D3D11_VIDEO_USAGE_PLAYBACK_NORMAL;

    hr = ID3D11VideoDevice_CreateVideoProcessorEnumerator(
        backend->video_device, &desc, &backend->processor_enum);
    if (FAILED(hr))
        return VLC_EGENERIC;
    hr = ID3D11VideoProcessorEnumerator_GetVideoProcessorCaps(
        backend->processor_enum, &caps);
    if (FAILED(hr) || caps.MaxInputStreams < 2)
        return VLC_EGENERIC;
    hr = ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(
        backend->processor_enum, backend->output_desc.Format, &flags);
    if (FAILED(hr) ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        return VLC_EGENERIC;
    }
    flags = 0;
    hr = ID3D11VideoProcessorEnumerator_CheckVideoProcessorFormat(
        backend->processor_enum, DXGI_FORMAT_B8G8R8A8_UNORM, &flags);
    if (FAILED(hr) ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT) == 0 ||
        (flags & D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT) == 0) {
        return VLC_EGENERIC;
    }
    hr = ID3D11VideoDevice_CreateVideoProcessor(
        backend->video_device, backend->processor_enum, 0,
        &backend->processor);
    return SUCCEEDED(hr) ? VLC_SUCCESS : VLC_EGENERIC;
}

static void ReadAdapterName(nsfw_d3d11_backend_t *backend)
{
    IDXGIDevice *dxgi_device = NULL;
    IDXGIAdapter *adapter = NULL;
    DXGI_ADAPTER_DESC desc;

    strcpy(backend->adapter_name, "unknown adapter");
    if (FAILED(ID3D11Device_QueryInterface(backend->device,
                                           &IID_IDXGIDevice,
                                           (void **)&dxgi_device)))
        return;
    if (SUCCEEDED(IDXGIDevice_GetAdapter(dxgi_device, &adapter)) &&
        SUCCEEDED(IDXGIAdapter_GetDesc(adapter, &desc))) {
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1,
                            backend->adapter_name,
                            (int)sizeof(backend->adapter_name),
                            NULL, NULL);
        backend->adapter_name[sizeof(backend->adapter_name) - 1] = '\0';
    }
    if (adapter != NULL)
        IDXGIAdapter_Release(adapter);
    IDXGIDevice_Release(dxgi_device);
}

int nsfw_d3d11_open(filter_t *filter, nsfw_d3d11_backend_t **out_backend)
{
    nsfw_d3d11_backend_t *backend;
    nsfw_d3d11_picture_sys_t *picsys;
    picture_t *probe;
    UINT data_size;
    HRESULT hr;
    const char *stage = "validation";

    if (filter == NULL || out_backend == NULL ||
        !nsfw_d3d11_is_opaque(filter->fmt_in.video.i_chroma)) {
        return VLC_EGENERIC;
    }
    *out_backend = NULL;
    backend = (nsfw_d3d11_backend_t *)calloc(1, sizeof(*backend));
    if (backend == NULL)
        return VLC_ENOMEM;
    InitializeCriticalSection(&backend->api_lock);
    backend->api_lock_ready = true;
    backend->context_mutex = INVALID_HANDLE_VALUE;

    stage = "output picture probe";
    probe = NewPicture(filter);
    if (probe == NULL)
        goto error;
    picsys = PictureSys(probe);
    if (picsys == NULL || picsys->context == NULL ||
        picsys->texture[0] == NULL) {
        ReleasePicture(probe);
        goto error;
    }

    backend->context = picsys->context;
    ID3D11DeviceContext_AddRef(backend->context);
    ID3D11DeviceContext_GetDevice(backend->context, &backend->device);
    ID3D11Texture2D_GetDesc(picsys->texture[0], &backend->output_desc);
    ReleasePicture(probe);

    data_size = sizeof(backend->context_mutex);
    hr = ID3D11DeviceContext_GetPrivateData(
        backend->context, &NSFW_GUID_CONTEXT_MUTEX, &data_size,
        &backend->context_mutex);
    if (FAILED(hr))
        backend->context_mutex = INVALID_HANDLE_VALUE;

    stage = "ID3D11VideoDevice";
    hr = ID3D11Device_QueryInterface(backend->device,
                                     &IID_ID3D11VideoDevice,
                                     (void **)&backend->video_device);
    if (FAILED(hr))
        goto error;
    stage = "ID3D11VideoContext";
    hr = ID3D11DeviceContext_QueryInterface(backend->context,
                                            &IID_ID3D11VideoContext,
                                            (void **)&backend->video_context);
    if (FAILED(hr))
        goto error;

    ReadAdapterName(backend);
    strncpy(backend->texture_format,
            DxgiFormatName(backend->output_desc.Format),
            sizeof(backend->texture_format) - 1);

    stage = "video processor";
    if (CreateProcessor(filter, backend) != VLC_SUCCESS)
        goto error;
    stage = "blur shaders";
    if (CreateShaders(backend) != VLC_SUCCESS)
        goto error;
    stage = "effect resources";
    if (CreateEffectResources(backend, filter->fmt_in.video.i_width,
                              filter->fmt_in.video.i_height) != VLC_SUCCESS) {
        goto error;
    }
    stage = "profiling queries";
    if (CreateProfilingQueries(backend) != VLC_SUCCESS)
        goto error;

    *out_backend = backend;
    return VLC_SUCCESS;

error:
    fprintf(stderr, "icop: D3D11 initialization failed at %s\n",
            stage);
    nsfw_d3d11_close(backend);
    return VLC_EGENERIC;
}

void nsfw_d3d11_close(nsfw_d3d11_backend_t *backend)
{
    if (backend == NULL)
        return;

    for (int i = 0; i < 3; ++i) {
        if (backend->rendered_count[i] > 0) {
            fprintf(stderr,
                    "icop: D3D11 rendered style=%d frames=%llu\n",
                    i, (unsigned long long)backend->rendered_count[i]);
        }
        if (backend->profile_count[i] > 0) {
            fprintf(stderr,
                    "icop: D3D11 profile style=%d frames=%llu average=%.3f ms median=%.3f ms\n",
                    i, (unsigned long long)backend->profile_count[i],
                    backend->profile_total_ms[i] /
                        (double)backend->profile_count[i],
                    ProfileMedianMs(backend, i));
        }
        if (backend->fallback_count[i] > 0) {
            fprintf(stderr,
                    "icop: D3D11 requested style=%d used black fallback for %llu frame(s)\n",
                    i, (unsigned long long)backend->fallback_count[i]);
        }
    }
    if (backend->debug_rendered_count > 0) {
        fprintf(stderr,
                "icop: D3D11 debug overlay rendered frames=%llu\n",
                (unsigned long long)backend->debug_rendered_count);
    }
    ReleaseViewCache(backend);
    if (backend->analysis_output != NULL)
        ID3D11VideoProcessorOutputView_Release(backend->analysis_output);
    if (backend->analysis_staging != NULL)
        ID3D11Texture2D_Release(backend->analysis_staging);
    if (backend->analysis_texture != NULL)
        ID3D11Texture2D_Release(backend->analysis_texture);
    if (backend->blur_down_output != NULL)
        ID3D11VideoProcessorOutputView_Release(backend->blur_down_output);
    if (backend->blur_up_input != NULL)
        ID3D11VideoProcessorInputView_Release(backend->blur_up_input);
    if (backend->render_output != NULL)
        ID3D11VideoProcessorOutputView_Release(backend->render_output);
    if (backend->render_texture != NULL)
        ID3D11Texture2D_Release(backend->render_texture);
    for (int i = 0; i < 2; ++i) {
        if (backend->blur_srv[i] != NULL)
            ID3D11ShaderResourceView_Release(backend->blur_srv[i]);
        if (backend->blur_rtv[i] != NULL)
            ID3D11RenderTargetView_Release(backend->blur_rtv[i]);
        if (backend->blur_texture[i] != NULL)
            ID3D11Texture2D_Release(backend->blur_texture[i]);
    }
    if (backend->watermark_input != NULL)
        ID3D11VideoProcessorInputView_Release(backend->watermark_input);
    if (backend->watermark_texture != NULL)
        ID3D11Texture2D_Release(backend->watermark_texture);
    if (backend->black_input != NULL)
        ID3D11VideoProcessorInputView_Release(backend->black_input);
    if (backend->black_texture != NULL)
        ID3D11Texture2D_Release(backend->black_texture);
    if (backend->debug_input != NULL)
        ID3D11VideoProcessorInputView_Release(backend->debug_input);
    if (backend->debug_texture != NULL)
        ID3D11Texture2D_Release(backend->debug_texture);
    free(backend->debug_pixels);
    if (backend->blur_constants != NULL)
        ID3D11Buffer_Release(backend->blur_constants);
    if (backend->profile_end != NULL)
        ID3D11Query_Release(backend->profile_end);
    if (backend->profile_start != NULL)
        ID3D11Query_Release(backend->profile_start);
    if (backend->profile_disjoint != NULL)
        ID3D11Query_Release(backend->profile_disjoint);
    if (backend->linear_sampler != NULL)
        ID3D11SamplerState_Release(backend->linear_sampler);
    if (backend->blur_ps != NULL)
        ID3D11PixelShader_Release(backend->blur_ps);
    if (backend->fullscreen_vs != NULL)
        ID3D11VertexShader_Release(backend->fullscreen_vs);
    if (backend->processor != NULL)
        ID3D11VideoProcessor_Release(backend->processor);
    if (backend->processor_enum != NULL)
        ID3D11VideoProcessorEnumerator_Release(backend->processor_enum);
    if (backend->video_context != NULL)
        ID3D11VideoContext_Release(backend->video_context);
    if (backend->video_device != NULL)
        ID3D11VideoDevice_Release(backend->video_device);
    if (backend->context != NULL)
        ID3D11DeviceContext_Release(backend->context);
    if (backend->device != NULL)
        ID3D11Device_Release(backend->device);
    if (backend->api_lock_ready)
        DeleteCriticalSection(&backend->api_lock);
    free(backend);
}

static RECT PictureSourceRect(const picture_t *picture)
{
    RECT rect;
    int width = picture->format.i_visible_width > 0 ?
                picture->format.i_visible_width : picture->format.i_width;
    int height = picture->format.i_visible_height > 0 ?
                 picture->format.i_visible_height : picture->format.i_height;

    rect.left = picture->format.i_x_offset;
    rect.top = picture->format.i_y_offset;
    rect.right = rect.left + width;
    rect.bottom = rect.top + height;
    return rect;
}

static void ConfigureProcessorOutput(nsfw_d3d11_backend_t *backend,
                                     int width, int height)
{
    RECT rect = { 0, 0, width, height };
    ID3D11VideoContext_VideoProcessorSetOutputTargetRect(
        backend->video_context, backend->processor, TRUE, &rect);
}

static void ConfigureProcessorStream(nsfw_d3d11_backend_t *backend,
                                     UINT index, const RECT *source,
                                     const RECT *destination, bool alpha)
{
    ID3D11VideoContext_VideoProcessorSetStreamFrameFormat(
        backend->video_context, backend->processor, index,
        D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE);
    ID3D11VideoContext_VideoProcessorSetStreamSourceRect(
        backend->video_context, backend->processor, index, TRUE, source);
    ID3D11VideoContext_VideoProcessorSetStreamDestRect(
        backend->video_context, backend->processor, index, TRUE, destination);
    ID3D11VideoContext_VideoProcessorSetStreamAlpha(
        backend->video_context, backend->processor, index, alpha ? TRUE : FALSE,
        1.0f);
    ID3D11VideoContext_VideoProcessorSetStreamAutoProcessingMode(
        backend->video_context, backend->processor, index, FALSE);
}

static HRESULT ProcessorBlitOne(nsfw_d3d11_backend_t *backend,
                                ID3D11VideoProcessorInputView *input,
                                const RECT *source_rect,
                                ID3D11VideoProcessorOutputView *output,
                                int output_width, int output_height)
{
    D3D11_VIDEO_PROCESSOR_STREAM stream;
    RECT destination = { 0, 0, output_width, output_height };

    memset(&stream, 0, sizeof(stream));
    stream.Enable = TRUE;
    stream.pInputSurface = input;
    ConfigureProcessorOutput(backend, output_width, output_height);
    ConfigureProcessorStream(backend, 0, source_rect, &destination, false);
    return ID3D11VideoContext_VideoProcessorBlt(
        backend->video_context, backend->processor, output, 0, 1, &stream);
}

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

unsigned nsfw_d3d11_decoder_surface_count(picture_t *picture)
{
    nsfw_d3d11_picture_sys_t *picsys = PictureSys(picture);
    D3D11_TEXTURE2D_DESC desc;

    if (picsys == NULL || picsys->texture[0] == NULL)
        return 0;
    ID3D11Texture2D_GetDesc(picsys->texture[0], &desc);
    return desc.ArraySize;
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

bool nsfw_d3d11_is_opaque(vlc_fourcc_t chroma)
{
    VLC_UNUSED(chroma);
    return false;
}

int nsfw_d3d11_open(filter_t *filter, nsfw_d3d11_backend_t **backend)
{
    VLC_UNUSED(filter);
    if (backend != NULL)
        *backend = NULL;
    return VLC_EGENERIC;
}

void nsfw_d3d11_close(nsfw_d3d11_backend_t *backend)
{
    VLC_UNUSED(backend);
}

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

unsigned nsfw_d3d11_decoder_surface_count(picture_t *picture)
{
    VLC_UNUSED(picture);
    return 0;
}

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

int nsfw_d3d11_dump_ppm(nsfw_d3d11_backend_t *backend,
                        picture_t *picture, const char *path)
{
    VLC_UNUSED(backend); VLC_UNUSED(picture); VLC_UNUSED(path);
    return VLC_EGENERIC;
}

const char *nsfw_d3d11_adapter_name(const nsfw_d3d11_backend_t *backend)
{
    VLC_UNUSED(backend);
    return "unavailable";
}

const char *nsfw_d3d11_texture_format(const nsfw_d3d11_backend_t *backend)
{
    VLC_UNUSED(backend);
    return "unknown";
}

#endif /* _WIN32 */
