#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nsfw_cuda_host_protocol.h"
#include "nsfw_filter_core.h"

static int read_exact(void *data, size_t size)
{
    uint8_t *cursor = (uint8_t *)data;

    while (size > 0) {
        size_t read = fread(cursor, 1, size, stdin);
        if (read == 0)
            return -1;
        cursor += read;
        size -= read;
    }
    return 0;
}

static int write_exact(const void *data, size_t size)
{
    const uint8_t *cursor = (const uint8_t *)data;

    while (size > 0) {
        size_t written = fwrite(cursor, 1, size, stdout);
        if (written == 0)
            return -1;
        cursor += written;
        size -= written;
    }
    return 0;
}

int main(int argc, char **argv)
{
    nsfw_config_t config = nsfw_config_default();
    nsfw_detector_t *detector;
    uint8_t *frame = NULL;
    size_t capacity = 0;
    const char *profile_value = getenv("NSFW_CUDA_HOST_PROFILE");
    const char *width_value = getenv("NSFW_CUDA_HOST_WIDTH");
    const char *height_value = getenv("NSFW_CUDA_HOST_HEIGHT");
    const char *threshold_value = getenv("NSFW_CUDA_HOST_THRESHOLD_MICROS");
    int profile = profile_value != NULL ? (int)strtol(profile_value, NULL, 10)
                                        : (int)NSFW_MODEL_PROFILE_MARQO;
    int width = width_value != NULL ? (int)strtol(width_value, NULL, 10)
                                    : config.model_width;
    int height = height_value != NULL ? (int)strtol(height_value, NULL, 10)
                                      : config.model_height;
    unsigned threshold_micros = threshold_value != NULL
        ? (unsigned)strtoul(threshold_value, NULL, 10)
        : (unsigned)(config.threshold * 1000000.0f + 0.5f);
    const char *model_path = getenv("NSFW_MODEL_PATH");
    int i;

    for (i = 1; i + 1 < argc; i += 2) {
        if (strcmp(argv[i], "--profile") == 0)
            profile = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--width") == 0)
            width = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--height") == 0)
            height = atoi(argv[i + 1]);
        else if (strcmp(argv[i], "--threshold-micros") == 0)
            threshold_micros = (unsigned)strtoul(argv[i + 1], NULL, 10);
    }

    if (profile < (int)NSFW_MODEL_PROFILE_MARQO ||
        profile > (int)NSFW_MODEL_PROFILE_FALCONSAI_BASE ||
        width <= 0 || height <= 0 || threshold_micros > 1000000u) {
        return 2;
    }

    nsfw_config_set_model_profile(&config, (nsfw_model_profile_t)profile);
    config.model_width = width;
    config.model_height = height;
    config.threshold = (float)threshold_micros / 1000000.0f;
    if (model_path != NULL && model_path[0] != '\0')
        config.model_path = model_path;
    detector = nsfw_detector_create(&config);
    if (detector == NULL)
        return 3;

    setvbuf(stdout, NULL, _IONBF, 0);
    for (;;) {
        nsfw_cuda_host_request_t request;
        nsfw_cuda_host_response_t response;
        nsfw_result_t result;

        if (read_exact(&request, sizeof(request)) != 0)
            break;
        uint64_t expected_bytes;

        expected_bytes = (uint64_t)request.width *
                         (uint64_t)request.height *
                         (uint64_t)request.channels;
        if (request.magic != NSFW_CUDA_HOST_REQUEST_MAGIC ||
            request.width == 0 || request.height == 0 || request.channels != 3 ||
            request.byte_count > 64u * 1024u * 1024u ||
            expected_bytes != request.byte_count) {
            break;
        }
        if (request.byte_count > capacity) {
            uint8_t *replacement = (uint8_t *)realloc(frame, request.byte_count);
            if (replacement == NULL)
                break;
            frame = replacement;
            capacity = request.byte_count;
        }
        if (read_exact(frame, request.byte_count) != 0)
            break;

        result = nsfw_detector_classify(detector, frame, (int)request.width,
                                        (int)request.height, (int)request.channels);
        response.magic = NSFW_CUDA_HOST_RESPONSE_MAGIC;
        response.status = 0;
        response.is_nsfw = result.is_nsfw;
        response.score = result.score;
        response.threshold = result.threshold;
        if (write_exact(&response, sizeof(response)) != 0)
            break;
    }

    free(frame);
    nsfw_detector_destroy(detector);
    return 0;
}
