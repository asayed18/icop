/*****************************************************************************
 * nsfw_filter_benchmark.cpp: Simple integration benchmark for NSFW models
 *****************************************************************************/

#include "nsfw_filter_core.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

struct benchmark_profile_case {
    nsfw_model_profile_t profile;
    const char           *name;
};

struct benchmark_resolution_case {
    const char *label;
    int         width;
    int         height;
    int         iterations;
};

struct prepared_resolution_case {
    benchmark_resolution_case resolution;
    std::vector<uint8_t>      safe_frame;
    std::vector<uint8_t>      nsfw_frame;
};

static std::vector<uint8_t> read_binary_file(const char *path)
{
    std::ifstream stream(path, std::ios::binary);

    if (!stream)
        return {};

    stream.seekg(0, std::ios::end);
    std::streamsize size = stream.tellg();
    if (size <= 0)
        return {};

    stream.seekg(0, std::ios::beg);
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    if (!stream.read(reinterpret_cast<char *>(bytes.data()), size))
        return {};

    return bytes;
}

static std::vector<uint8_t> resize_rgb_bilinear(const uint8_t *src,
                                                int            src_w,
                                                int            src_h,
                                                int            dst_w,
                                                int            dst_h)
{
    std::vector<uint8_t> dst(static_cast<size_t>(dst_w) * dst_h * 3);
    const float x_scale = static_cast<float>(src_w) / static_cast<float>(dst_w);
    const float y_scale = static_cast<float>(src_h) / static_cast<float>(dst_h);

    for (int y = 0; y < dst_h; ++y) {
        const float src_y = y * y_scale;
        const int   y0    = static_cast<int>(src_y);
        const int   y1    = std::min(y0 + 1, src_h - 1);
        const float  yf    = src_y - static_cast<float>(y0);

        for (int x = 0; x < dst_w; ++x) {
            const float src_x = x * x_scale;
            const int   x0    = static_cast<int>(src_x);
            const int   x1    = std::min(x0 + 1, src_w - 1);
            const float  xf    = src_x - static_cast<float>(x0);

            for (int c = 0; c < 3; ++c) {
                const float p00 = static_cast<float>(
                    src[(y0 * src_w + x0) * 3 + c]);
                const float p10 = static_cast<float>(
                    src[(y0 * src_w + x1) * 3 + c]);
                const float p01 = static_cast<float>(
                    src[(y1 * src_w + x0) * 3 + c]);
                const float p11 = static_cast<float>(
                    src[(y1 * src_w + x1) * 3 + c]);

                float value = p00 * (1.0f - xf) * (1.0f - yf)
                            + p10 * xf           * (1.0f - yf)
                            + p01 * (1.0f - xf) * yf
                            + p11 * xf           * yf;

                if (value < 0.0f) value = 0.0f;
                if (value > 255.0f) value = 255.0f;
                dst[(y * dst_w + x) * 3 + c] =
                    static_cast<uint8_t>(std::lround(value));
            }
        }
    }

    return dst;
}

static double classify_average_ms(nsfw_detector_t *detector,
                                  const uint8_t   *frame_data,
                                  int              width,
                                  int              height,
                                  int              channels,
                                  int              iterations,
                                  nsfw_result_t    *last_result)
{
    using clock = std::chrono::steady_clock;

    auto start = clock::now();
    nsfw_result_t result{};

    for (int i = 0; i < iterations; ++i) {
        result = nsfw_detector_classify(detector, frame_data,
                                        width, height, channels);
    }

    auto elapsed = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
        clock::now() - start).count();

    if (last_result)
        *last_result = result;

    return elapsed / static_cast<double>(iterations);
}

int main()
{
#if !defined(NSFW_BENCHMARK_SAMPLE_RGB) || !defined(NSFW_BENCHMARK_SAMPLE_SKIN_RGB) || \
    !defined(NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH) || !defined(NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT)
    std::cerr << "Benchmark fixtures were not generated for this build.\n";
    return 2;
#else
    const benchmark_profile_case profiles[] = {
        { NSFW_MODEL_PROFILE_MARQO, "marqo" },
        { NSFW_MODEL_PROFILE_ADAMCODD, "adamcodd" },
        { NSFW_MODEL_PROFILE_FALCONSAI, "falconsai" },
        { NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL, "falconsai-official" },
        { NSFW_MODEL_PROFILE_LEGACY, "legacy" },
    };
    const benchmark_resolution_case resolutions[] = {
        { "fullhd", 1920, 1080, 5 },
        { "4k", 3840, 2160, 3 },
    };

    const std::vector<uint8_t> safe_base =
        read_binary_file(NSFW_BENCHMARK_SAMPLE_RGB);
    const std::vector<uint8_t> nsfw_base =
        read_binary_file(NSFW_BENCHMARK_SAMPLE_SKIN_RGB);

    if (safe_base.size() !=
        static_cast<size_t>(NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH *
                            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT * 3) ||
        nsfw_base.size() !=
        static_cast<size_t>(NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH *
                            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT * 3)) {
        std::cerr << "Benchmark fixture size mismatch.\n";
        return 1;
    }

    std::vector<prepared_resolution_case> prepared_resolutions;
    prepared_resolutions.reserve(sizeof(resolutions) / sizeof(resolutions[0]));
    for (const auto &resolution : resolutions) {
        prepared_resolution_case prepared;
        prepared.resolution = resolution;
        prepared.safe_frame = resize_rgb_bilinear(
            safe_base.data(),
            NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH,
            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT,
            resolution.width,
            resolution.height);
        prepared.nsfw_frame = resize_rgb_bilinear(
            nsfw_base.data(),
            NSFW_BENCHMARK_SAMPLE_FRAME_WIDTH,
            NSFW_BENCHMARK_SAMPLE_FRAME_HEIGHT,
            resolution.width,
            resolution.height);
        prepared_resolutions.push_back(std::move(prepared));
    }

    std::cout << std::fixed << std::setprecision(3);
    std::cout << "model,resolution,safe_ms,nsfw_ms,avg_ms,accuracy_pct,safe_score,nsfw_score\n";

    for (const auto &profile : profiles) {
        nsfw_config_t cfg = nsfw_config_default();
        nsfw_config_set_model_profile(&cfg, profile.profile);

        nsfw_detector_t *det = nsfw_detector_create(&cfg);
        if (!det) {
            std::cerr << "Skipping model " << profile.name
                      << " because the detector could not be created in this environment\n";
            continue;
        }

        for (const auto &prepared : prepared_resolutions) {
            const auto &resolution = prepared.resolution;

            nsfw_result_t safe_check{};
            nsfw_result_t nsfw_check{};

            (void)nsfw_detector_classify(det, prepared.safe_frame.data(),
                                         resolution.width, resolution.height, 3);
            (void)nsfw_detector_classify(det, prepared.nsfw_frame.data(),
                                         resolution.width, resolution.height, 3);

            const double safe_ms = classify_average_ms(
                det, prepared.safe_frame.data(), resolution.width, resolution.height, 3,
                resolution.iterations, &safe_check);
            const double nsfw_ms = classify_average_ms(
                det, prepared.nsfw_frame.data(), resolution.width, resolution.height, 3,
                resolution.iterations, &nsfw_check);

            const int safe_ok = (safe_check.is_nsfw == 0) ? 1 : 0;
            const int nsfw_ok = (nsfw_check.is_nsfw == 1) ? 1 : 0;
            const double accuracy_pct = 100.0 * static_cast<double>(safe_ok + nsfw_ok) / 2.0;
            const double avg_ms = (safe_ms + nsfw_ms) / 2.0;

            std::cout << profile.name << ','
                      << resolution.label << ','
                      << safe_ms << ','
                      << nsfw_ms << ','
                      << avg_ms << ','
                      << accuracy_pct << ','
                      << safe_check.score << ','
                      << nsfw_check.score << '\n';
        }

        nsfw_detector_destroy(det);
    }

    return 0;
#endif
}
