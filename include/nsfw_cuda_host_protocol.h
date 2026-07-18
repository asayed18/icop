#ifndef NSFW_CUDA_HOST_PROTOCOL_H
#define NSFW_CUDA_HOST_PROTOCOL_H

#include <stdint.h>

#define NSFW_CUDA_HOST_REQUEST_MAGIC 0x49434F50u
#define NSFW_CUDA_HOST_RESPONSE_MAGIC 0x49434F52u
#define NSFW_CUDA_HOST_READY_MAGIC 0x49435244u
#define NSFW_CUDA_HOST_MAX_BATCH_SIZE 4u

#pragma pack(push, 1)
typedef struct nsfw_cuda_host_request_t {
    uint32_t magic;
    uint32_t width;
    uint32_t height;
    uint32_t channels;
    uint32_t frame_count;
    uint32_t frame_byte_count;
} nsfw_cuda_host_request_t;

typedef struct nsfw_cuda_host_response_t {
    uint32_t magic;
    int32_t  status;
    int32_t  is_nsfw;
    float    score;
    float    threshold;
} nsfw_cuda_host_response_t;
#pragma pack(pop)

#endif
