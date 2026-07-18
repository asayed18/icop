#include "nsfw_model_registry.h"
#include "nsfw_platform_utils.h"

#include <cstdlib>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>

/*****************************************************************************
 * Model profile table
 *****************************************************************************/

static const nsfw_model_profile_info kModelProfiles[] = {
    {
        NSFW_MODEL_PROFILE_MARQO,
        "marqo",
        { "nsfw-image-detection-384", "marqo/nsfw-image-detection-384", nullptr, nullptr, nullptr },
        "model.onnx",
        384,
        384,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 0, 0, 0 },
        1,
    },
    {
        NSFW_MODEL_PROFILE_ADAMCODD,
        "adamcodd",
        { "vit-base-nsfw-detector", "adamcodd/vit-base-nsfw-detector", nullptr, nullptr, nullptr },
        "adamcodd.onnx",
        384,
        384,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 1, 0, 0 },
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI,
        "falconsai",
        { "nsfw_image_detection", "falconsai/nsfw_image_detection", nullptr, nullptr, nullptr },
        "falconsai.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 1, 0, 0 },
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL,
        "falconsai-official",
        {
            "nsfw_image_detection_26",
            "falconsai/nsfw_image_detection_26",
            "Falconsai/nsfw_image_detection_26",
            "falconsai26",
            "falconsai-26",
        },
        "quantized_model.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 1, 0, 0 },
        1,
    },
    {
        NSFW_MODEL_PROFILE_FALCONSAI_BASE,
        "falconsai-base",
        {
            "falconsaibase",
            "falconsai_base",
            "falconsai-vit",
            "falconsai-pytorch",
            nullptr,
        },
        "falconsai_base.onnx",
        224,
        224,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 1, 0, 0 },
        1,
    },
    {
        NSFW_MODEL_PROFILE_LEGACY,
        "legacy",
        { "gantman", "nsfw-detect-onnx", "legacy", nullptr, nullptr },
        "legacy.onnx",
        299,
        299,
        { 0.5f, 0.5f, 0.5f },
        { 0.5f, 0.5f, 0.5f },
        { 1, 3, 4 },
        3,
    },
};

const nsfw_model_profile_info *nsfw_get_model_profile_info(nsfw_model_profile_t profile)
{
    for (size_t i = 0; i < sizeof(kModelProfiles) / sizeof(kModelProfiles[0]); ++i) {
        if (kModelProfiles[i].profile == profile)
            return &kModelProfiles[i];
    }
    return &kModelProfiles[0];
}

nsfw_tensor_layout nsfw_default_input_layout(nsfw_model_profile_t profile)
{
    return profile == NSFW_MODEL_PROFILE_LEGACY
        ? NSFW_TENSOR_LAYOUT_NHWC
        : NSFW_TENSOR_LAYOUT_NCHW;
}

/*****************************************************************************
 * Profile name folding helper
 *****************************************************************************/

static std::string nsfw_ascii_fold_profile_name(const char *text)
{
    std::string value;

    if (!text)
        return value;

    value.reserve(std::strlen(text));
    for (; *text; ++text) {
        unsigned char c = static_cast<unsigned char>(*text);
        if (c == '-' || c == '_' || c == '/' || c == '.' || c == ' ' )
            continue;
        value.push_back(static_cast<char>(std::tolower(c)));
    }

    return value;
}

/*****************************************************************************
 * Model path resolution
 *****************************************************************************/

static std::string nsfw_resolve_default_model_path(const nsfw_model_profile_info *info)
{
    if (!info)
        return std::string();

    {
        std::string sibling = nsfw_platform_module_sibling_path(info->runtime_filename);
        if (!sibling.empty() && nsfw_platform_file_exists(sibling.c_str()))
            return sibling;
    }

#ifdef NSFW_MODEL_PATH_MARQO
    if (info->profile == NSFW_MODEL_PROFILE_MARQO &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_MARQO))
        return std::string(NSFW_MODEL_PATH_MARQO);
#endif
#ifdef NSFW_MODEL_PATH_ADAMCODD
    if (info->profile == NSFW_MODEL_PROFILE_ADAMCODD &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_ADAMCODD))
        return std::string(NSFW_MODEL_PATH_ADAMCODD);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_FALCONSAI))
        return std::string(NSFW_MODEL_PATH_FALCONSAI);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI_OFFICIAL
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_FALCONSAI_OFFICIAL))
        return std::string(NSFW_MODEL_PATH_FALCONSAI_OFFICIAL);
#endif
#ifdef NSFW_MODEL_PATH_FALCONSAI_BASE
    if (info->profile == NSFW_MODEL_PROFILE_FALCONSAI_BASE &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_FALCONSAI_BASE))
        return std::string(NSFW_MODEL_PATH_FALCONSAI_BASE);
#endif
#ifdef NSFW_MODEL_PATH_LEGACY
    if (info->profile == NSFW_MODEL_PROFILE_LEGACY &&
        nsfw_platform_file_exists(NSFW_MODEL_PATH_LEGACY))
        return std::string(NSFW_MODEL_PATH_LEGACY);
#endif
#ifdef NSFW_DEFAULT_MODEL_PATH
    if (info->profile == NSFW_MODEL_PROFILE_MARQO &&
        nsfw_platform_file_exists(NSFW_DEFAULT_MODEL_PATH))
        return std::string(NSFW_DEFAULT_MODEL_PATH);
#endif

    return std::string();
}

std::string nsfw_resolve_model_path(const nsfw_config_t *config)
{
    const nsfw_model_profile_info *info;

    if (config && config->model_path && config->model_path[0] != '\0')
        return std::string(config->model_path);

    info = nsfw_get_model_profile_info(config ? config->model_profile
                                              : NSFW_MODEL_PROFILE_MARQO);
    {
        std::string sibling = nsfw_platform_module_sibling_path(info->runtime_filename);
        if (!sibling.empty() && nsfw_platform_file_exists(sibling.c_str()))
            return sibling;
    }

    return nsfw_resolve_default_model_path(info);
}

/*****************************************************************************
 * Score normalization
 *****************************************************************************/

float nsfw_model_profile_normalize_score(nsfw_model_profile_t profile,
                                         float raw_score)
{
    float raw_midpoint = 0.50f;
    float numerator;
    float denominator;

    if (raw_score < 0.0f)
        raw_score = 0.0f;
    if (raw_score > 1.0f)
        raw_score = 1.0f;

    switch (profile) {
        case NSFW_MODEL_PROFILE_FALCONSAI:
        case NSFW_MODEL_PROFILE_FALCONSAI_BASE:
        case NSFW_MODEL_PROFILE_FALCONSAI_OFFICIAL:
            raw_midpoint = 0.02f;
            break;
        case NSFW_MODEL_PROFILE_MARQO:
        case NSFW_MODEL_PROFILE_ADAMCODD:
        case NSFW_MODEL_PROFILE_LEGACY:
        default:
            break;
    }

    numerator = raw_score * (1.0f - raw_midpoint);
    denominator = numerator + (1.0f - raw_score) * raw_midpoint;
    return denominator > 0.0f ? numerator / denominator : 0.0f;
}

const char *nsfw_model_profile_name(nsfw_model_profile_t profile)
{
    return nsfw_get_model_profile_info(profile)->name;
}

int nsfw_model_profile_parse(const char *text, nsfw_model_profile_t *profile)
{
    if (!profile)
        return 0;

    std::string folded = nsfw_ascii_fold_profile_name(text);
    if (folded.empty()) {
        *profile = NSFW_MODEL_PROFILE_MARQO;
        return 1;
    }

    for (size_t i = 0; i < sizeof(kModelProfiles) / sizeof(kModelProfiles[0]); ++i) {
        const nsfw_model_profile_info &info = kModelProfiles[i];

        if (folded == nsfw_ascii_fold_profile_name(info.name)) {
            *profile = info.profile;
            return 1;
        }

        for (size_t alias_index = 0; alias_index < sizeof(info.aliases) / sizeof(info.aliases[0]); ++alias_index) {
            const char *alias = info.aliases[alias_index];
            if (!alias)
                continue;
            if (folded == nsfw_ascii_fold_profile_name(alias)) {
                *profile = info.profile;
                return 1;
            }
        }
    }

    *profile = NSFW_MODEL_PROFILE_MARQO;
    return 0;
}

void nsfw_config_set_model_profile(nsfw_config_t *config,
                                   nsfw_model_profile_t profile)
{
    if (!config)
        return;

    const nsfw_model_profile_info *info = nsfw_get_model_profile_info(profile);
    config->model_profile = info->profile;
    config->model_width = info->width;
    config->model_height = info->height;
}
