/*****************************************************************************
 * nsfw_filter_core.cpp: NSFW detection core library implementation
 *****************************************************************************
 * Implements the public API from nsfw_filter_core.h.
 * The ONNX Runtime backend is compiled in only when NSFW_HAS_ONNXRUNTIME
 * is defined; otherwise nsfw_detector_create returns NULL and callers
 * must use nsfw_detector_create_with_backend.
 *****************************************************************************/

#include "nsfw_filter_core.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "nsfw_onnx_providers.h"
#include "nsfw_model_registry.h"

/*****************************************************************************
 * Internal: detector state
 *****************************************************************************/

struct nsfw_detector {
    nsfw_config_t         config;
    std::string           model_path_owned;
    nsfw_backend_vtable_t backend;
    std::vector<float>    preprocessed;
};

/*****************************************************************************
 * Preprocessing
 *****************************************************************************/

static int nsfw_preprocess_frame_internal(const uint8_t *frame_data,
                                          int            width,
                                          int            height,
                                          int            channels,
                                          float         *output,
                                          int            model_width,
                                          int            model_height,
                                          const float    *mean,
                                          const float    *stddev)
{
    if (!frame_data || !output || width <= 0 || height <= 0 ||
        channels < 3 || model_width <= 0 || model_height <= 0)
        return -1;

    /*
     * The VLC filter already packs analysis pictures at the selected model
     * dimensions.  Resampling such a frame back onto its identical pixel
     * grid is a no-op. Preserve the same normalized values directly instead.
     */
    if (width == model_width && height == model_height) {
        const size_t pixel_count =
            static_cast<size_t>(model_width) * static_cast<size_t>(model_height);

        for (size_t pixel = 0; pixel < pixel_count; ++pixel) {
            for (int c = 0; c < 3; ++c) {
                float v = static_cast<float>(
                    frame_data[pixel * static_cast<size_t>(channels) + c]) /
                    255.0f;
                if (mean != nullptr && stddev != nullptr) {
                    float scale = stddev[c] != 0.0f ? stddev[c] : 1.0f;
                    v = (v - mean[c]) / scale;
                }
                output[pixel * 3 + c] = v;
            }
        }
        return 0;
    }

    const float x_scale = static_cast<float>(width)  / static_cast<float>(model_width);
    const float y_scale = static_cast<float>(height) / static_cast<float>(model_height);

    for (int y = 0; y < model_height; y++) {
        float src_y = y * y_scale;
        int   y0    = static_cast<int>(src_y);
        int   y1    = std::min(y0 + 1, height - 1);
        float yf    = src_y - static_cast<float>(y0);

        for (int x = 0; x < model_width; x++) {
            float src_x = x * x_scale;
            int   x0    = static_cast<int>(src_x);
            int   x1    = std::min(x0 + 1, width - 1);
            float xf    = src_x - static_cast<float>(x0);

            /* Bilinear interpolation for each colour channel. */
            for (int c = 0; c < 3; c++) {
                float v;

                float v00 = static_cast<float>(
                    frame_data[(y0 * width + x0) * channels + c]);
                float v10 = static_cast<float>(
                    frame_data[(y0 * width + x1) * channels + c]);
                float v01 = static_cast<float>(
                    frame_data[(y1 * width + x0) * channels + c]);
                float v11 = static_cast<float>(
                    frame_data[(y1 * width + x1) * channels + c]);

                v = v00 * (1.0f - xf) * (1.0f - yf)
                  + v10 * xf           * (1.0f - yf)
                  + v01 * (1.0f - xf) * yf
                  + v11 * xf           * yf;

                /* NHWC layout: interleaved channels. */
                v /= 255.0f;
                if (mean != nullptr && stddev != nullptr) {
                    float scale = stddev[c] != 0.0f ? stddev[c] : 1.0f;
                    v = (v - mean[c]) / scale;
                }
                output[(y * model_width + x) * 3 + c] = v;
            }
        }
    }

    return 0;
}

int nsfw_preprocess_frame(const uint8_t *frame_data,
                          int            width,
                          int            height,
                          int            channels,
                          float         *output,
                          int            model_width,
                          int            model_height)
{
    return nsfw_preprocess_frame_internal(frame_data, width, height,
                                          channels, output, model_width,
                                          model_height, nullptr, nullptr);
}

/*****************************************************************************
 * Threshold mapping
 *****************************************************************************/

float nsfw_sensitivity_to_threshold(nsfw_sensitivity_t sensitivity)
{
    switch (sensitivity) {
        case NSFW_SENSITIVITY_LOW:    return 0.70f;
        case NSFW_SENSITIVITY_MEDIUM: return 0.50f;
        case NSFW_SENSITIVITY_HIGH:   return 0.30f;
        default:                       return 0.50f;
    }
}

/*****************************************************************************
 * Default config
 *****************************************************************************/

nsfw_config_t nsfw_config_default(void)
{
    nsfw_config_t cfg;
    std::memset(&cfg, 0, sizeof(cfg));
    cfg.threshold    = 0.50f;
    nsfw_config_set_model_profile(&cfg, NSFW_MODEL_PROFILE_MARQO);
    cfg.model_path   = nullptr;
    return cfg;
}


/*****************************************************************************
 * Construction helpers
 *****************************************************************************/

nsfw_detector_t *nsfw_detector_create_with_backend(
    const nsfw_config_t         *config,
    const nsfw_backend_vtable_t *vtable)
{
    if (!config || !vtable || !vtable->load_model || !vtable->infer)
        return nullptr;

    /* Validate threshold range. */
    if (config->threshold < 0.0f || config->threshold > 1.0f)
        return nullptr;
    if (config->model_width <= 0 || config->model_height <= 0)
        return nullptr;

    auto *det = new (std::nothrow) nsfw_detector();
    if (!det) return nullptr;

    det->config = *config;
    if (config->model_path) {
        det->model_path_owned = config->model_path;
        det->config.model_path = det->model_path_owned.c_str();
    }

    det->backend = *vtable;
    det->preprocessed.resize(
        static_cast<size_t>(3 * config->model_width * config->model_height), 0.0f);

    /* Attempt to load the model through the backend. */
    if (vtable->load_model(vtable->ctx, det->config.model_path) != 0) {
        /* Backend load failed – clean up. */
        if (vtable->destroy) vtable->destroy(vtable->ctx);
        delete det;
        return nullptr;
    }

    return det;
}

nsfw_detector_t *nsfw_detector_create(const nsfw_config_t *config)
{
    std::string model_path = nsfw_resolve_model_path(config);
    if (!config || model_path.empty())
        return nullptr;

    nsfw_config_t resolved = *config;
    resolved.model_path = model_path.c_str();

    void *onnx_ctx = nsfw_onnx_create_context(
        resolved.model_profile, resolved.model_width, resolved.model_height);
    if (!onnx_ctx) {
        return nullptr;
    }

    nsfw_backend_vtable_t vtable;
    vtable.ctx        = onnx_ctx;
    vtable.load_model = nsfw_onnx_load_model;
    vtable.infer      = nsfw_onnx_infer;
    vtable.infer_batch = nsfw_onnx_infer_batch;
    vtable.destroy    = nsfw_onnx_destroy;

    return nsfw_detector_create_with_backend(&resolved, &vtable);
}

int nsfw_core_has_provider(const char *provider_name)
{
    return nsfw_onnx_has_provider(provider_name);
}

void nsfw_detector_destroy(nsfw_detector_t *detector)
{
    if (!detector) return;

    if (detector->backend.destroy)
        detector->backend.destroy(detector->backend.ctx);

    delete detector;
}

/*****************************************************************************
 * Classification
 *****************************************************************************/

int nsfw_detector_classify_batch_checked(nsfw_detector_t *detector,
                                         const uint8_t   *frame_data,
                                         int              batch_size,
                                         int              width,
                                         int              height,
                                         int              channels,
                                         nsfw_result_t   *results)
{
    const nsfw_model_profile_info *info;
    size_t frame_bytes;
    int preproc_size;
    std::vector<float> scores;

    if (results != nullptr && batch_size > 0) {
        std::memset(results, 0,
                    static_cast<size_t>(batch_size) * sizeof(*results));
    }

    if (!detector || !frame_data || !results || batch_size <= 0 ||
        width <= 0 || height <= 0 || channels < 3) {
        return -1;
    }

    frame_bytes = static_cast<size_t>(width) * static_cast<size_t>(height) *
                  static_cast<size_t>(channels);
    if (frame_bytes == 0)
        return -1;

    preproc_size = 3 * detector->config.model_width * detector->config.model_height;
    if (preproc_size <= 0)
        return -1;
    detector->preprocessed.resize(static_cast<size_t>(batch_size) *
                                  static_cast<size_t>(preproc_size));
    info = nsfw_get_model_profile_info(detector->config.model_profile);
    if (info == nullptr)
        return -1;

    for (int i = 0; i < batch_size; ++i) {
        if (nsfw_preprocess_frame_internal(
                frame_data + static_cast<size_t>(i) * frame_bytes,
                width, height, channels,
                detector->preprocessed.data() +
                    static_cast<size_t>(i) * static_cast<size_t>(preproc_size),
                detector->config.model_width, detector->config.model_height,
                info->mean, info->stddev) != 0) {
            return -1;
        }
    }

    scores.resize(static_cast<size_t>(batch_size), 0.0f);
    if (batch_size == 1 && detector->backend.infer != nullptr) {
        int infer_status = detector->backend.infer(
            detector->backend.ctx, detector->preprocessed.data(),
            preproc_size, scores.data());
        if (infer_status != 0) {
            return infer_status == NSFW_BATCH_UNSUPPORTED
                ? NSFW_BATCH_UNSUPPORTED : -1;
        }
    } else if (detector->backend.infer_batch != nullptr) {
        int infer_status = detector->backend.infer_batch(
            detector->backend.ctx, detector->preprocessed.data(),
            batch_size, preproc_size, scores.data());
        if (infer_status != 0) {
            return infer_status == NSFW_BATCH_UNSUPPORTED
                ? NSFW_BATCH_UNSUPPORTED : -1;
        }
    } else {
        return -1;
    }

    for (int i = 0; i < batch_size; ++i) {
        float score = nsfw_model_profile_normalize_score(
            detector->config.model_profile, scores[static_cast<size_t>(i)]);

        results[i].score = score;
        results[i].threshold = detector->config.threshold;
        results[i].is_nsfw = score >= detector->config.threshold ? 1 : 0;
    }

    return 0;
}

int nsfw_detector_classify_checked(nsfw_detector_t *detector,
                                   const uint8_t   *frame_data,
                                   int              width,
                                   int              height,
                                   int              channels,
                                   nsfw_result_t   *result)
{
    return nsfw_detector_classify_batch_checked(detector, frame_data, 1,
                                                width, height, channels,
                                                result);
}

nsfw_result_t nsfw_detector_classify(nsfw_detector_t *detector,
                                     const uint8_t   *frame_data,
                                     int              width,
                                     int              height,
                                     int              channels)
{
    nsfw_result_t result;

    (void)nsfw_detector_classify_checked(detector, frame_data, width, height,
                                         channels, &result);

    return result;
}
