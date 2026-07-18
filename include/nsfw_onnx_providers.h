#ifndef NSFW_ONNX_PROVIDERS_H
#define NSFW_ONNX_PROVIDERS_H

#include "nsfw_filter_core.h"

#ifdef __cplusplus
extern "C" {
#endif

void *nsfw_onnx_create_context(nsfw_model_profile_t profile,
                                int model_width, int model_height);
int   nsfw_onnx_load_model(void *ctx, const char *model_path);
int   nsfw_onnx_infer(void *ctx, const float *input, int input_size, float *output);
int   nsfw_onnx_infer_batch(void *ctx, const float *input, int batch_size,
                             int input_size, float *output);
void  nsfw_onnx_destroy(void *ctx);
int   nsfw_onnx_has_provider(const char *provider_name);

#ifdef __cplusplus
}
#endif

#endif
