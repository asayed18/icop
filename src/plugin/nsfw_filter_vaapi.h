/*****************************************************************************
 * nsfw_filter_vaapi.h: VAAPI backend for the NSFW video filter
 *****************************************************************************/

#ifndef VLC_NSFW_FILTER_VAAPI_H
#define VLC_NSFW_FILTER_VAAPI_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vlc_filter.h>
#include <vlc_picture.h>

#include "nsfw_filter.h"

typedef struct nsfw_vaapi_backend_t nsfw_vaapi_backend_t;

bool nsfw_vaapi_is_opaque(vlc_fourcc_t chroma);

int nsfw_vaapi_open(filter_t *filter, nsfw_vaapi_backend_t **backend);
void nsfw_vaapi_close(nsfw_vaapi_backend_t *backend);

int nsfw_vaapi_set_analysis_size(nsfw_vaapi_backend_t *backend,
                                 int width, int height);

int nsfw_vaapi_readback_rgb(nsfw_vaapi_backend_t *backend,
                            picture_t *picture,
                            uint8_t *rgb, size_t rgb_capacity,
                            int *width, int *height);

unsigned nsfw_vaapi_decoder_surface_count(picture_t *picture);

picture_t *nsfw_vaapi_render_blocked(filter_t *filter,
                                     nsfw_vaapi_backend_t *backend,
                                     picture_t *source,
                                     nsfw_block_style_t style);

picture_t *nsfw_vaapi_render_debug_overlay(filter_t *filter,
                                           nsfw_vaapi_backend_t *backend,
                                           picture_t *source,
                                           float score, float threshold);

int nsfw_vaapi_dump_ppm(nsfw_vaapi_backend_t *backend,
                        picture_t *picture, const char *path);

const char *nsfw_vaapi_driver_name(const nsfw_vaapi_backend_t *backend);
const char *nsfw_vaapi_surface_format(const nsfw_vaapi_backend_t *backend);

#endif /* VLC_NSFW_FILTER_VAAPI_H */