#ifndef NSFW_MODEL_REGISTRY_H
#define NSFW_MODEL_REGISTRY_H

#include <cstddef>
#include <string>

#include "nsfw_filter_core.h"

struct nsfw_model_profile_info {
    nsfw_model_profile_t profile;
    const char           *name;
    const char           *aliases[5];
    const char           *runtime_filename;
    int                   width;
    int                   height;
    float                 mean[3];
    float                 stddev[3];
    bool                  legacy_multiclass;
    size_t                nsfw_index;
};

const nsfw_model_profile_info *nsfw_get_model_profile_info(nsfw_model_profile_t profile);
std::string nsfw_resolve_model_path(const nsfw_config_t *config);

#endif
