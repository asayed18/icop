/*****************************************************************************
 * nsfw_filter_d3d11.c: D3D11 backend base — shared infrastructure
 *
 * Backend struct, view cache, video processor, texture creation, and
 * the public API functions that are neither pure readback nor pure
 * effects rendering.
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
#include <dxgi.h>

#include "nsfw_filter_d3d11.h"

struct nsfw_d3d11_picture_sys_t;
typedef struct nsfw_d3d11_picture_sys_t nsfw_d3d11_picture_sys_t;
#include "nsfw_filter_d3d11_internal.h"

#define NSFW_D3D11_MAX_VIEWS 64
#define NSFW_D3D11_PLANE_COUNT 4

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

/* Patch over the NSFW_D3D11_PROFILE_MAX_SAMPLES reference in the backend
 * struct — the effects file defines this too.  Keep it consistent. */
#ifndef NSFW_D3D11_PROFILE_MAX_SAMPLES
# define NSFW_D3D11_PROFILE_MAX_SAMPLES 512
#endif

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

nsfw_d3d11_picture_sys_t *PictureSys(picture_t *picture)
{
    if (picture == NULL)
        return NULL;
    if (picture->context != NULL)
        return &((nsfw_d3d11_va_context_t *)picture->context)->picsys;
    return (nsfw_d3d11_picture_sys_t *)picture->p_sys;
}

picture_t *NewPicture(filter_t *filter)
{
    if (filter == NULL || filter->owner.video.buffer_new == NULL)
        return NULL;
    return filter->owner.video.buffer_new(filter);
}

void ReleasePicture(picture_t *picture)
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

void CopyPictureProperties(picture_t *destination,
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

void BackendLock(nsfw_d3d11_backend_t *backend)
{
    EnterCriticalSection(&backend->api_lock);
    if (backend->context_mutex != NULL &&
        backend->context_mutex != INVALID_HANDLE_VALUE) {
        WaitForSingleObjectEx(backend->context_mutex, INFINITE, FALSE);
    }
}

void BackendUnlock(nsfw_d3d11_backend_t *backend)
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

void ReleaseViewCache(nsfw_d3d11_backend_t *backend)
{
    for (unsigned i = 0; i < NSFW_D3D11_MAX_VIEWS; ++i) {
        nsfw_d3d11_view_entry_t *entry = &backend->views[i];
        if (entry->input != NULL)
            ID3D11VideoProcessorInputView_Release(entry->input);
        memset(entry, 0, sizeof(*entry));
    }
}

HRESULT CreateInputView(nsfw_d3d11_backend_t *backend,
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

HRESULT CreateOutputView(nsfw_d3d11_backend_t *backend,
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

ID3D11VideoProcessorInputView *GetInputView(
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

int CreateBgraTexture(nsfw_d3d11_backend_t *backend,
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

RECT PictureSourceRect(const picture_t *picture)
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

void ConfigureProcessorOutput(nsfw_d3d11_backend_t *backend,
                              int width, int height)
{
    RECT rect = { 0, 0, width, height };
    ID3D11VideoContext_VideoProcessorSetOutputTargetRect(
        backend->video_context, backend->processor, TRUE, &rect);
}

void ConfigureProcessorStream(nsfw_d3d11_backend_t *backend,
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

HRESULT ProcessorBlitOne(nsfw_d3d11_backend_t *backend,
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

unsigned nsfw_d3d11_decoder_surface_count(picture_t *picture)
{
    nsfw_d3d11_picture_sys_t *picsys = PictureSys(picture);
    D3D11_TEXTURE2D_DESC desc;

    if (picsys == NULL || picsys->texture[0] == NULL)
        return 0;
    ID3D11Texture2D_GetDesc(picsys->texture[0], &desc);
    return desc.ArraySize;
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

unsigned nsfw_d3d11_decoder_surface_count(picture_t *picture)
{
    VLC_UNUSED(picture);
    return 0;
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
