/*****************************************************************************
 * nsfw_filter_core_integration_test.cpp: Integration tests requiring ONNX Runtime
 *****************************************************************************/

#include "nsfw_filter_core.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <cstring>
#include <string>
#include <vector>

static std::vector<uint8_t> read_binary_file(const char *path)
{
    if (path == nullptr)
        return {};

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

struct onnx_sample_fixture {
    const char *label;
    const char *path;
    int         width;
    int         height;
};

static std::vector<onnx_sample_fixture> onnx_sample_fixtures()
{
    std::vector<onnx_sample_fixture> fixtures;

#if defined(NSFW_TEST_SAMPLE_RGB) && defined(NSFW_TEST_SAMPLE_SKIN_RGB) && \
    defined(NSFW_TEST_SAMPLE_FRAME_WIDTH) && defined(NSFW_TEST_SAMPLE_FRAME_HEIGHT)
    fixtures.push_back({
        "sample",
        NSFW_TEST_SAMPLE_RGB,
        NSFW_TEST_SAMPLE_FRAME_WIDTH,
        NSFW_TEST_SAMPLE_FRAME_HEIGHT,
    });
    fixtures.push_back({
        "sample_skin",
        NSFW_TEST_SAMPLE_SKIN_RGB,
        NSFW_TEST_SAMPLE_FRAME_WIDTH,
        NSFW_TEST_SAMPLE_FRAME_HEIGHT,
    });
#endif

    return fixtures;
}

/*****************************************************************************
 * Tests – nsfw_detector_create without ONNX Runtime
 *****************************************************************************/

TEST(DetectorCreate, LoadsDefaultOnnxModel)
{
    auto cfg = nsfw_config_default();
    auto *det = nsfw_detector_create(&cfg);

    if (!det) {
        GTEST_SKIP() << "Real detector could not be created in this environment";
    }

    std::vector<uint8_t> frame(cfg.model_width * cfg.model_height * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(),
                                         cfg.model_width, cfg.model_height, 3);
    EXPECT_GE(result.score, 0.0f);
    EXPECT_LE(result.score, 1.0f);

    nsfw_detector_destroy(det);
}

class OnnxProfileIntegration : public ::testing::TestWithParam<nsfw_model_profile_t> {
};

static std::string onnx_profile_test_name(
    const ::testing::TestParamInfo<nsfw_model_profile_t> &info)
{
    std::string name = nsfw_model_profile_name(info.param)
        ? std::string(nsfw_model_profile_name(info.param))
        : std::string("unknown");

    for (char &ch : name) {
        unsigned char value = static_cast<unsigned char>(ch);
        if (!(std::isalnum(value) || ch == '_'))
            ch = '_';
    }

    if (name.empty())
        name = "unknown";

    if (std::isdigit(static_cast<unsigned char>(name[0])))
        name.insert(name.begin(), 'p');

    return name;
}

TEST_P(OnnxProfileIntegration, BlankFrameInferenceSucceeds)
{
    auto cfg = nsfw_config_default();
    nsfw_config_set_model_profile(&cfg, GetParam());

    auto *det = nsfw_detector_create(&cfg);
    if (!det) {
        GTEST_SKIP() << "Real detector could not be created in this environment";
    }

    std::vector<uint8_t> frame(32 * 32 * 3, 127);
    auto result = nsfw_detector_classify(det, frame.data(), 32, 32, 3);

    EXPECT_FLOAT_EQ(result.threshold, cfg.threshold);
    EXPECT_TRUE(std::isfinite(result.score));
    EXPECT_GE(result.score, 0.0f);
    EXPECT_LE(result.score, 1.0f);
    EXPECT_EQ(result.is_nsfw, result.score >= cfg.threshold ? 1 : 0);

    nsfw_detector_destroy(det);
}

TEST_P(OnnxProfileIntegration, SampleFixturesClassifySuccessfully)
{
    auto fixtures = onnx_sample_fixtures();
    if (fixtures.empty()) {
        GTEST_SKIP() << "Sample frame fixtures are not configured for this build";
    }

    auto cfg = nsfw_config_default();
    nsfw_config_set_model_profile(&cfg, GetParam());

    auto *det = nsfw_detector_create(&cfg);
    if (!det) {
        GTEST_SKIP() << "Real detector could not be created in this environment";
    }

    for (const auto &fixture : fixtures) {
        SCOPED_TRACE(std::string("fixture=") + fixture.label);

        auto frame = read_binary_file(fixture.path);
        ASSERT_EQ(frame.size(),
                  static_cast<size_t>(fixture.width * fixture.height * 3));

        auto result = nsfw_detector_classify(det, frame.data(),
                                             fixture.width, fixture.height, 3);

        EXPECT_FLOAT_EQ(result.threshold, cfg.threshold);
        EXPECT_TRUE(std::isfinite(result.score));
        EXPECT_GE(result.score, 0.0f);
        EXPECT_LE(result.score, 1.0f);
        EXPECT_EQ(result.is_nsfw, result.score >= cfg.threshold ? 1 : 0);
    }

    nsfw_detector_destroy(det);
}

INSTANTIATE_TEST_SUITE_P(AllProfiles,
                         OnnxProfileIntegration,
                         ::testing::Values(NSFW_MODEL_PROFILE_MARQO,
                                           NSFW_MODEL_PROFILE_ADAMCODD,
                                           NSFW_MODEL_PROFILE_FALCONSAI,
                                           NSFW_MODEL_PROFILE_FALCONSAI_BASE,
                                           NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL,
                                           NSFW_MODEL_PROFILE_LEGACY),
                         onnx_profile_test_name);

/*****************************************************************************
 * Tests – Detector create resolves default model_path
 *****************************************************************************/

TEST(DetectorCreate, UsesDefaultModelWhenModelPathNull)
{
    nsfw_config_t cfg    = nsfw_config_default();
    cfg.threshold        = 0.5f;
    cfg.model_path       = nullptr;

    auto *det = nsfw_detector_create(&cfg);
    if (!det) {
        GTEST_SKIP() << "Real detector could not be created in this environment";
    }
    nsfw_detector_destroy(det);
}
