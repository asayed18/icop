#ifndef NSFW_CUDA_HOST_H
#define NSFW_CUDA_HOST_H

#include "nsfw_filter_core.h"

typedef struct nsfw_cuda_host_t nsfw_cuda_host_t;

int nsfw_cuda_host_start(nsfw_cuda_host_t **host, const nsfw_config_t *config);
int nsfw_cuda_host_classify(nsfw_cuda_host_t *host,
                            const uint8_t *frame_data,
                            int width, int height, int channels,
                            nsfw_result_t *result);
void nsfw_cuda_host_stop(nsfw_cuda_host_t **host);

#endif
