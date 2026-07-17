/*****************************************************************************
 * nsfw_filter_core_test.cpp: Unit tests for the NSFW detection core library
 *****************************************************************************/

#include "nsfw_filter_core.h"
#include "nsfw_platform_utils.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

/*****************************************************************************
 * Fake backend for testing
 *****************************************************************************/

TEST(PlatformPaths, ModuleSiblingPathKeepsTheModuleDirectory)
{
    const std::string directory = nsfw_platform_get_module_directory();
    const std::string sibling =
        nsfw_platform_module_sibling_path("icop-test-sibling.bin");

    ASSERT_FALSE(directory.empty());
    ASSERT_FALSE(sibling.empty());

    const std::filesystem::path sibling_path(sibling);
    EXPECT_EQ(sibling_path.filename(), "icop-test-sibling.bin");
    EXPECT_EQ(sibling_path.parent_path().lexically_normal(),
              std::filesystem::path(directory).lexically_normal());
}

struct fake_context {
    float fixed_score;   /* Score returned by infer. */
    int   load_count;    /* How many times load_model was called. */
    int   infer_count;   /* How many times infer was called. */
    bool  load_should_fail;
};

static int fake_load_model(void *ctx, const char *model_path)
{
    auto *fc = static_cast<fake_context *>(ctx);
    fc->load_count++;
    if (fc->load_should_fail) return -1;
    (void)model_path;
    return 0;
}

static int fake_infer(void *ctx, const float *input, int input_size, float *output)
{
    auto *fc = static_cast<fake_context *>(ctx);
    fc->infer_count++;
    (void)input;
    (void)input_size;
    *output = fc->fixed_score;
    return 0;
}

static void fake_destroy(void *ctx)
{
    delete static_cast<fake_context *>(ctx);
}

/* Helper: create a vtable backed by a fake_context. */
static nsfw_backend_vtable_t make_fake_backend(float score,
                                                bool load_fail = false)
{
    auto *fc    = new fake_context{};
    fc->fixed_score     = score;
    fc->load_count      = 0;
    fc->infer_count     = 0;
    fc->load_should_fail = load_fail;

    nsfw_backend_vtable_t vt;
    vt.ctx        = fc;
    vt.load_model = fake_load_model;
    vt.infer      = fake_infer;
    vt.destroy    = fake_destroy;
    return vt;
}

static nsfw_config_t make_test_config(float threshold = 0.5f)
{
    nsfw_config_t cfg    = nsfw_config_default();
    cfg.threshold        = threshold;
    cfg.model_path       = "fake_model.onnx";
    return cfg;
}

/*****************************************************************************
 * Tests – Threshold mapping
 *****************************************************************************/

TEST(SensitivityToThreshold, LowReturns07)
{
    EXPECT_FLOAT_EQ(nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_LOW), 0.70f);
}

TEST(SensitivityToThreshold, MediumReturns05)
{
    EXPECT_FLOAT_EQ(nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_MEDIUM), 0.50f);
}

TEST(SensitivityToThreshold, HighReturns03)
{
    EXPECT_FLOAT_EQ(nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_HIGH), 0.30f);
}

TEST(SensitivityToThreshold, InvalidReturnsDefault)
{
    EXPECT_FLOAT_EQ(nsfw_sensitivity_to_threshold(
        static_cast<nsfw_sensitivity_t>(99)), 0.50f);
}

TEST(SensitivityToThreshold, OrderedMapping)
{
    float lo  = nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_LOW);
    float med = nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_MEDIUM);
    float hi  = nsfw_sensitivity_to_threshold(NSFW_SENSITIVITY_HIGH);
    EXPECT_GT(lo, med);
    EXPECT_GT(med, hi);
}

TEST(ModelScoreNormalization, MapsProfileMidpointsToSharedThreshold)
{
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_MARQO, 0.50f),
                    0.50f);
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_ADAMCODD, 0.50f),
                    0.50f);
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_LEGACY, 0.50f),
                    0.50f);
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_FALCONSAI, 0.02f),
                    0.50f);
}

TEST(ModelScoreNormalization, PreservesBoundsAndOrdering)
{
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_FALCONSAI, 0.0f),
                    0.0f);
    EXPECT_FLOAT_EQ(nsfw_model_profile_normalize_score(
                        NSFW_MODEL_PROFILE_FALCONSAI, 1.0f),
                    1.0f);
    EXPECT_LT(nsfw_model_profile_normalize_score(
                  NSFW_MODEL_PROFILE_FALCONSAI, 0.01f),
              nsfw_model_profile_normalize_score(
                  NSFW_MODEL_PROFILE_FALCONSAI, 0.03f));
}

/*****************************************************************************
 * Tests – Preprocessing
 *****************************************************************************/

TEST(Preprocess, OutputShapeMatchesModelDimensions)
{
    /* 4x4 RGB frame, model expects 2x2. Output size = 3*2*2 = 12. */
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    std::vector<float>   output(3 * 2 * 2, 0.0f);

    int rc = nsfw_preprocess_frame(frame.data(), 4, 4, 3,
                                   output.data(), 2, 2);
    ASSERT_EQ(rc, 0);

    /* All 12 values should be written (non-zero after normalisation). */
    int nonzero = 0;
    for (float v : output) {
        if (v != 0.0f) nonzero++;
    }
    EXPECT_EQ(nonzero, 3 * 2 * 2);
}

TEST(Preprocess, UniformInputProducesConsistentChannels)
{
    /* A uniform gray (128) frame should produce the same value for every
     * pixel within each channel after normalisation. */
    int mw = 4, mh = 4;
    std::vector<uint8_t> frame(8 * 8 * 3, 128);
    std::vector<float>   output(3 * mh * mw, 0.0f);

    int rc = nsfw_preprocess_frame(frame.data(), 8, 8, 3,
                                   output.data(), mw, mh);
    ASSERT_EQ(rc, 0);

    /* Check that all values in channel 0 are identical. */
    float ch0_first = output[0];
    for (int i = 1; i < mh * mw; i++) {
        EXPECT_FLOAT_EQ(output[i], ch0_first)
            << "Pixel " << i << " in channel 0 differs";
    }
}

TEST(Preprocess, NHWCLayoutCorrect)
{
    /* Create a frame where R=200, G=100, B=50 for every pixel. */
    int sw = 2, sh = 2, mw = 2, mh = 2;
    std::vector<uint8_t> frame;
    for (int i = 0; i < sw * sh; i++) {
        frame.push_back(200);  /* R */
        frame.push_back(100);  /* G */
        frame.push_back(50);   /* B */
    }
    std::vector<float> output(3 * mh * mw, 0.0f);

    int rc = nsfw_preprocess_frame(frame.data(), sw, sh, 3,
                                   output.data(), mw, mh);
    ASSERT_EQ(rc, 0);

    /* In NHWC, each pixel is laid out as RGBRGB...
     * Pixel 0 = indices [0..2], pixel 1 = [3..5], etc. */
    float r_val = output[0];
    float g_val = output[1];
    float b_val = output[2];

    /* Red channel should have the highest normalised value. */
    EXPECT_GT(r_val, g_val);
    EXPECT_GT(g_val, b_val);
}

TEST(Preprocess, RGBAInputIgnoresAlpha)
{
    /* Same RGB values with 4 channels – alpha should be ignored. */
    int mw = 2, mh = 2;
    std::vector<uint8_t> frame_rgb(2 * 2 * 3);
    std::vector<uint8_t> frame_rgba(2 * 2 * 4);

    for (int i = 0; i < 4; i++) {
        frame_rgb[i * 3 + 0] = frame_rgba[i * 4 + 0] = 100;
        frame_rgb[i * 3 + 1] = frame_rgba[i * 4 + 1] = 150;
        frame_rgb[i * 3 + 2] = frame_rgba[i * 4 + 2] = 200;
        frame_rgba[i * 4 + 3] = 255;  /* alpha – should be ignored */
    }

    std::vector<float> out_rgb(3 * mh * mw);
    std::vector<float> out_rgba(3 * mh * mw);

    ASSERT_EQ(nsfw_preprocess_frame(frame_rgb.data(),  2, 2, 3,
                                     out_rgb.data(),  mw, mh), 0);
    ASSERT_EQ(nsfw_preprocess_frame(frame_rgba.data(), 2, 2, 4,
                                     out_rgba.data(), mw, mh), 0);

    /* Outputs should be identical. */
    for (int i = 0; i < 3 * mh * mw; i++) {
        EXPECT_FLOAT_EQ(out_rgb[i], out_rgba[i])
            << "Difference at index " << i;
    }
}

TEST(Preprocess, RejectsNullFrameData)
{
    float out[12];
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    EXPECT_NE(nsfw_preprocess_frame(nullptr, 4, 4, 3, out, 2, 2), 0);
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 4, 4, 3, nullptr, 2, 2), 0);
}

TEST(Preprocess, RejectsZeroWidth)
{
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    float out[12];
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 0, 4, 3, out, 2, 2), 0);
}

TEST(Preprocess, RejectsZeroHeight)
{
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    float out[12];
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 4, 0, 3, out, 2, 2), 0);
}

TEST(Preprocess, RejectsInsufficientChannels)
{
    std::vector<uint8_t> frame(4 * 4 * 2, 128);
    float out[12];
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 4, 4, 2, out, 2, 2), 0);
}

TEST(Preprocess, RejectsZeroModelWidth)
{
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    float out[12];
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 4, 4, 3, out, 0, 2), 0);
}

TEST(Preprocess, RejectsZeroModelHeight)
{
    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    float out[12];
    EXPECT_NE(nsfw_preprocess_frame(frame.data(), 4, 4, 3, out, 2, 0), 0);
}

TEST(Preprocess, ImageNetNormalizationApplied)
{
    /* The model preprocessing now normalizes to [0, 1]. */
    int mw = 1, mh = 1;
    std::vector<uint8_t> frame = {0, 0, 0};  /* single black pixel */
    std::vector<float>   output(3, 0.0f);

    ASSERT_EQ(nsfw_preprocess_frame(frame.data(), 1, 1, 3,
                                    output.data(), mw, mh), 0);

    EXPECT_FLOAT_EQ(output[0], 0.0f);
    EXPECT_FLOAT_EQ(output[1], 0.0f);
    EXPECT_FLOAT_EQ(output[2], 0.0f);
}

/*****************************************************************************
 * Tests – Backend injection / detector lifecycle
 *****************************************************************************/

TEST(DetectorWithBackend, CreateAndDestroy)
{
    auto vt  = make_fake_backend(0.5f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);
    EXPECT_EQ(static_cast<fake_context *>(vt.ctx)->load_count, 1);

    nsfw_detector_destroy(det);
    /* destroy was called on the fake context – det is freed. */
}

TEST(DetectorWithBackend, CreateFailsWhenBackendLoadFails)
{
    auto vt  = make_fake_backend(0.5f, /*load_fail=*/true);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    EXPECT_EQ(det, nullptr);
    /* The vtable's destroy should have been called during cleanup. */
}

TEST(DetectorWithBackend, CreateRejectsNullArgs)
{
    auto cfg = make_test_config();
    auto vt  = make_fake_backend(0.5f);

    EXPECT_EQ(nsfw_detector_create_with_backend(nullptr, &vt), nullptr);
    EXPECT_EQ(nsfw_detector_create_with_backend(&cfg, nullptr), nullptr);
}

TEST(DetectorWithBackend, CreateRejectsInvalidThreshold)
{
    auto vt  = make_fake_backend(0.5f);

    auto cfg_lo = make_test_config(-0.1f);
    EXPECT_EQ(nsfw_detector_create_with_backend(&cfg_lo, &vt), nullptr);

    auto cfg_hi = make_test_config(1.5f);
    EXPECT_EQ(nsfw_detector_create_with_backend(&cfg_hi, &vt), nullptr);
}

TEST(DetectorWithBackend, CreateRejectsInvalidDimensions)
{
    auto vt = make_fake_backend(0.5f);

    auto cfg0 = make_test_config();
    cfg0.model_width = 0;
    EXPECT_EQ(nsfw_detector_create_with_backend(&cfg0, &vt), nullptr);

    auto cfg1 = make_test_config();
    cfg1.model_height = -1;
    EXPECT_EQ(nsfw_detector_create_with_backend(&cfg1, &vt), nullptr);
}

/*****************************************************************************
 * Tests – Classification via injected backend
 *****************************************************************************/

TEST(Classify, NSFWWhenScoreAboveThreshold)
{
    auto vt  = make_fake_backend(0.8f);
    auto cfg = make_test_config(0.5f);

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    /* A small 2x2 RGB frame. */
    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 2, 2, 3);

    EXPECT_EQ(result.is_nsfw, 1);
    EXPECT_FLOAT_EQ(result.score, 0.8f);
    EXPECT_FLOAT_EQ(result.threshold, 0.5f);

    nsfw_detector_destroy(det);
}

TEST(Classify, SafeWhenScoreBelowThreshold)
{
    auto vt  = make_fake_backend(0.2f);
    auto cfg = make_test_config(0.5f);

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 2, 2, 3);

    EXPECT_EQ(result.is_nsfw, 0);
    EXPECT_FLOAT_EQ(result.score, 0.2f);
    EXPECT_FLOAT_EQ(result.threshold, 0.5f);

    nsfw_detector_destroy(det);
}

TEST(Classify, BoundaryScoreEqualsThreshold)
{
    auto vt  = make_fake_backend(0.5f);
    auto cfg = make_test_config(0.5f);

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 2, 2, 3);

    /* score >= threshold → is_nsfw */
    EXPECT_EQ(result.is_nsfw, 1);
    EXPECT_FLOAT_EQ(result.score, 0.5f);

    nsfw_detector_destroy(det);
}

TEST(Classify, InferCalledOncePerClassify)
{
    auto vt  = make_fake_backend(0.1f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    auto *fc = static_cast<fake_context *>(vt.ctx);
    EXPECT_EQ(fc->infer_count, 0);

    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    nsfw_detector_classify(det, frame.data(), 2, 2, 3);
    EXPECT_EQ(fc->infer_count, 1);

    nsfw_detector_classify(det, frame.data(), 2, 2, 3);
    EXPECT_EQ(fc->infer_count, 2);

    nsfw_detector_destroy(det);
}

TEST(Classify, ReturnsZeroResultOnNullDetector)
{
    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    auto result = nsfw_detector_classify(nullptr, frame.data(), 2, 2, 3);

    EXPECT_EQ(result.is_nsfw, 0);
    EXPECT_FLOAT_EQ(result.score, 0.0f);
    EXPECT_FLOAT_EQ(result.threshold, 0.0f);
}

TEST(Classify, ReturnsZeroResultOnNullFrame)
{
    auto vt  = make_fake_backend(0.5f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    auto result = nsfw_detector_classify(det, nullptr, 2, 2, 3);
    EXPECT_EQ(result.is_nsfw, 0);

    nsfw_detector_destroy(det);
}

/*****************************************************************************
 * Tests – Default config
 *****************************************************************************/

TEST(ConfigDefault, HasExpectedValues)
{
    auto cfg = nsfw_config_default();
    EXPECT_FLOAT_EQ(cfg.threshold, 0.50f);
    EXPECT_EQ(cfg.model_profile, NSFW_MODEL_PROFILE_MARQO);
    EXPECT_EQ(cfg.model_width, 384);
    EXPECT_EQ(cfg.model_height, 384);
    EXPECT_EQ(cfg.model_path, nullptr);
}

TEST(ModelProfile, KeepsCommonThresholdForFalconsaiFamily)
{
    auto cfg = nsfw_config_default();

    nsfw_config_set_model_profile(&cfg, NSFW_MODEL_PROFILE_FALCONSAI);
    EXPECT_FLOAT_EQ(cfg.threshold, 0.50f);
    EXPECT_EQ(cfg.model_profile, NSFW_MODEL_PROFILE_FALCONSAI);
    EXPECT_EQ(cfg.model_width, 224);
    EXPECT_EQ(cfg.model_height, 224);

    cfg = nsfw_config_default();
    nsfw_config_set_model_profile(&cfg, NSFW_MODEL_PROFILE_FALCONSAI_BASE);
    EXPECT_FLOAT_EQ(cfg.threshold, 0.50f);
    EXPECT_EQ(cfg.model_profile, NSFW_MODEL_PROFILE_FALCONSAI_BASE);
}

TEST(ModelProfile, PreservesExplicitThresholdOverride)
{
    auto cfg = nsfw_config_default();
    cfg.threshold = 0.33f;

    nsfw_config_set_model_profile(&cfg, NSFW_MODEL_PROFILE_FALCONSAI);
    EXPECT_FLOAT_EQ(cfg.threshold, 0.33f);
}

TEST(ModelProfile, ParsesCommonAliases)
{
    nsfw_model_profile_t profile = NSFW_MODEL_PROFILE_LEGACY;

    EXPECT_EQ(nsfw_model_profile_parse("Marqo/nsfw-image-detection-384", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_MARQO);

    EXPECT_EQ(nsfw_model_profile_parse("vit-base-nsfw-detector", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_ADAMCODD);

    EXPECT_EQ(nsfw_model_profile_parse("Falconsai/nsfw_image_detection", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_FALCONSAI);

    EXPECT_EQ(nsfw_model_profile_parse("Falconsai/nsfw_image_detection_26", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL);

    EXPECT_EQ(nsfw_model_profile_parse("falconsai-base", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_FALCONSAI_BASE);
    EXPECT_STREQ(nsfw_model_profile_name(NSFW_MODEL_PROFILE_FALCONSAI_BASE),
                 "falconsai-base");

    EXPECT_EQ(nsfw_model_profile_parse("falconsai-official", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL);
    EXPECT_STREQ(nsfw_model_profile_name(NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL),
                 "falconsai-official");

    EXPECT_EQ(nsfw_model_profile_parse("gantman", &profile), 1);
    EXPECT_EQ(profile, NSFW_MODEL_PROFILE_LEGACY);
}



/*****************************************************************************
 * Tests – Preprocessing: upscaling and edge cases
 *****************************************************************************/

TEST(Preprocess, UpscaleFromSmallerSource)
{
    /* 2x2 source upscaled to 4x4 model. The output size must match 3*4*4. */
    std::vector<uint8_t> frame(2 * 2 * 3, 128);
    std::vector<float>   output(3 * 4 * 4, 0.0f);

    int rc = nsfw_preprocess_frame(frame.data(), 2, 2, 3,
                                   output.data(), 4, 4);
    ASSERT_EQ(rc, 0);

    /* All 48 values should be written and non-zero after normalisation. */
    int nonzero = 0;
    for (float v : output) {
        if (v != 0.0f) nonzero++;
    }
    EXPECT_EQ(nonzero, 3 * 4 * 4);
}

TEST(Preprocess, OneByOneInputToLargerModel)
{
    /* Smallest possible input: 1x1 pixel. */
    std::vector<uint8_t> frame = {200, 100, 50};
    std::vector<float>   output(3 * 4 * 4, 0.0f);

    int rc = nsfw_preprocess_frame(frame.data(), 1, 1, 3,
                                   output.data(), 4, 4);
    ASSERT_EQ(rc, 0);

    /* Due to bilinear interpolation from a single source pixel, every pixel
     * should have the same RGB triplet. */
    for (int i = 3; i < 3 * 4 * 4; i++) {
        EXPECT_FLOAT_EQ(output[i], output[i - 3])
            << "Pixel component " << i << " differs from the first triplet";
    }
}

/*****************************************************************************
 * Tests – Infer receives correct input size
 *****************************************************************************/

struct inspect_context {
    int   last_input_size;
    float fixed_score;
    bool  load_ok;
};

static int inspect_load_model(void *ctx, const char *)
{
    auto *ic = static_cast<inspect_context *>(ctx);
    ic->last_input_size = -1;
    return ic->load_ok ? 0 : -1;
}

static int inspect_infer(void *ctx, const float *, int input_size, float *output)
{
    auto *ic = static_cast<inspect_context *>(ctx);
    ic->last_input_size = input_size;
    *output = ic->fixed_score;
    return 0;
}

static void inspect_destroy(void *ctx)
{
    delete static_cast<inspect_context *>(ctx);
}

TEST(Classify, InferReceivesCorrectInputSize)
{
    auto *ic = new inspect_context{};
    ic->fixed_score     = 0.1f;
    ic->load_ok         = true;
    ic->last_input_size = -1;

    nsfw_backend_vtable_t vt;
    vt.ctx        = ic;
    vt.load_model = inspect_load_model;
    vt.infer      = inspect_infer;
    vt.destroy    = inspect_destroy;

    auto cfg = make_test_config();
    cfg.model_width  = 8;
    cfg.model_height = 6;

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 4, 4, 3);

    /* input_size should be 3 * model_width * model_height = 3*8*6 = 144. */
    EXPECT_EQ(ic->last_input_size, 3 * 8 * 6);
    EXPECT_FLOAT_EQ(result.score, 0.1f);

    nsfw_detector_destroy(det);
}

/*****************************************************************************
 * Tests – Classify with bad frame dimensions
 *****************************************************************************/

TEST(Classify, ReturnsZeroOnNegativeFrameWidth)
{
    auto vt  = make_fake_backend(0.8f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), -1, 4, 3);

    EXPECT_EQ(result.is_nsfw, 0);
    EXPECT_FLOAT_EQ(result.score, 0.0f);

    nsfw_detector_destroy(det);
}

TEST(Classify, ReturnsZeroOnNegativeFrameHeight)
{
    auto vt  = make_fake_backend(0.8f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(4 * 4 * 3, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 4, -1, 3);

    EXPECT_EQ(result.is_nsfw, 0);
    EXPECT_FLOAT_EQ(result.score, 0.0f);

    nsfw_detector_destroy(det);
}

TEST(Classify, ReturnsZeroOnBadChannelCount)
{
    auto vt  = make_fake_backend(0.8f);
    auto cfg = make_test_config();

    auto *det = nsfw_detector_create_with_backend(&cfg, &vt);
    ASSERT_NE(det, nullptr);

    std::vector<uint8_t> frame(4 * 4 * 2, 128);
    auto result = nsfw_detector_classify(det, frame.data(), 4, 4, 2);

    EXPECT_EQ(result.is_nsfw, 0);
    EXPECT_FLOAT_EQ(result.score, 0.0f);

    nsfw_detector_destroy(det);
}

/*****************************************************************************
 * Tests – Destroy safety
 *****************************************************************************/

TEST(DetectorDestroy, NullptrIsSafe)
{
    nsfw_detector_destroy(nullptr);
    /* Should not crash. */
}
