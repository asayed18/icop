/*****************************************************************************
 * nsfw_filter_core.h: NSFW detection core library public API
 *****************************************************************************
 * Reusable core library for NSFW content detection. Independent from VLC
 * or any specific player. Provides a C-friendly API with a backend
 * abstraction so the inference engine can be swapped or mocked.
 *****************************************************************************/

#ifndef NSFW_FILTER_CORE_H
#define NSFW_FILTER_CORE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*****************************************************************************
 * Types and constants
 *****************************************************************************/

/* Opaque detector handle. */
typedef struct nsfw_detector nsfw_detector_t;

/* Sensitivity presets that map to thresholds. */
typedef enum nsfw_sensitivity {
    NSFW_SENSITIVITY_LOW    = 0,  /* threshold 0.70 – fewer false positives  */
    NSFW_SENSITIVITY_MEDIUM = 1,  /* threshold 0.50 – balanced              */
    NSFW_SENSITIVITY_HIGH   = 2   /* threshold 0.30 – fewer false negatives  */
} nsfw_sensitivity_t;

/* Detection result returned by nsfw_detector_classify. */
typedef struct nsfw_result {
    int   is_nsfw;   /* 1 if score >= threshold, 0 otherwise */
    float score;     /* Raw model output (0.0 – 1.0)        */
    float threshold; /* Threshold that was applied            */
} nsfw_result_t;

/* Tensor data layout used by ONNX models. */
enum nsfw_tensor_layout {
    NSFW_TENSOR_LAYOUT_NHWC,
    NSFW_TENSOR_LAYOUT_NCHW,
};

/* Built-in model families supported by the detector. */
typedef enum nsfw_model_profile {
    NSFW_MODEL_PROFILE_MARQO = 0,
    NSFW_MODEL_PROFILE_ADAMCODD = 1,
    NSFW_MODEL_PROFILE_FALCONSAI = 2,
    NSFW_MODEL_PROFILE_LEGACY = 3,
    NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL = 4,
    NSFW_MODEL_PROFILE_FALCONSAI_BASE = 5,
} nsfw_model_profile_t;

/* Detector configuration. model_path must remain valid for the lifetime
 * of the detector, or callers may use nsfw_detector_create_with_backend
 * which copies what it needs internally. */
typedef struct nsfw_config {
    float       threshold;    /* Detection threshold (0.0 – 1.0)  */
    nsfw_model_profile_t model_profile; /* Built-in model family         */
    int         model_width;  /* Expected model input width        */
    int         model_height; /* Expected model input height       */
    const char *model_path;   /* Path to .onnx model file (UTF-8) */
} nsfw_config_t;

/* Backend vtable for dependency injection.
 *
 * ctx:           Opaque pointer forwarded to every callback.
 * load_model:    Called once during detector creation.
 *                Returns 0 on success, negative on failure.
 * infer:         Run inference on a preprocessed tensor.
 *                input is a float array of length input_size in the same
 *                NHWC layout produced by nsfw_preprocess_frame.
 *                output receives a single float score.
 *                Returns 0 on success, negative on failure.
 * destroy:       Release backend resources. May be NULL. */
typedef struct nsfw_backend_vtable {
    void *ctx;
    int   (*load_model)(void *ctx, const char *model_path);
    int   (*infer)(void *ctx, const float *input, int input_size, float *output);
    void  (*destroy)(void *ctx);
} nsfw_backend_vtable_t;

/*****************************************************************************
 * Construction / destruction
 *****************************************************************************/

/* Create a detector that uses the built-in ONNX Runtime backend.
 * Returns NULL if ONNX Runtime is unavailable or model loading fails. */
nsfw_detector_t *nsfw_detector_create(const nsfw_config_t *config);

/* Create a detector with a custom backend (for testing or alternative
 * inference engines). The vtable is copied internally; ctx ownership
 * transfers to the detector (destroy will be called on it). */
nsfw_detector_t *nsfw_detector_create_with_backend(
    const nsfw_config_t *config, const nsfw_backend_vtable_t *vtable);

/* Release all resources held by the detector. */
void nsfw_detector_destroy(nsfw_detector_t *detector);

/*****************************************************************************
 * Classification
 *****************************************************************************/

/* Classify a single video frame.
 *
 * frame_data: pixel data in HWC layout (interleaved channels).
 * width/height: frame dimensions in pixels.
 * channels: 3 (RGB) or 4 (RGBA, alpha is ignored).
 *
 * Returns an nsfw_result_t. On error, score and threshold are 0 and
 * is_nsfw is 0. */
nsfw_result_t nsfw_detector_classify(nsfw_detector_t *detector,
                                     const uint8_t   *frame_data,
                                     int              width,
                                     int              height,
                                     int              channels);

/*****************************************************************************
 * Utility functions
 *****************************************************************************/

/* Map a sensitivity preset to a concrete threshold value. */
float nsfw_sensitivity_to_threshold(nsfw_sensitivity_t sensitivity);

/* Normalize a profile's raw score onto the shared 0.0–1.0 threshold scale. */
float nsfw_model_profile_normalize_score(nsfw_model_profile_t profile,
                                         float raw_score);

/* Preprocess a raw frame into a model-ready tensor.
 *
 * output must point to a buffer of at least
 *   3 * model_width * model_height floats.
 *
 * The output is in NHWC layout with values normalized to [0, 1]:
 *   value = pixel / 255.0
 * using bilinear interpolation for resizing.
 *
 * Returns 0 on success, negative on error. */
int nsfw_preprocess_frame(const uint8_t *frame_data,
                          int            width,
                          int            height,
                          int            channels,
                          float         *output,
                          int            model_width,
                          int            model_height);

/* Return a config struct initialized with sensible defaults. */
nsfw_config_t nsfw_config_default(void);

/* Helpers for model selection. */
const char *nsfw_model_profile_name(nsfw_model_profile_t profile);
int nsfw_model_profile_parse(const char *text, nsfw_model_profile_t *profile);
void nsfw_config_set_model_profile(nsfw_config_t *config,
                                   nsfw_model_profile_t profile);

/* Query available ONNX execution providers at runtime.
 * provider_name: "cuda", "rocm", "cpu", "tensorrt", etc.
 * Returns 1 if the named provider is available, 0 otherwise. */
int nsfw_core_has_provider(const char *provider_name);

#ifdef __cplusplus
}
#endif

#endif /* NSFW_FILTER_CORE_H */
