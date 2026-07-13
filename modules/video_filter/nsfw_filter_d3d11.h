/*****************************************************************************
 * nsfw_filter_d3d11.h: D3D11 backend for the NSFW video filter
 *****************************************************************************/

#ifndef VLC_NSFW_FILTER_D3D11_H
#define VLC_NSFW_FILTER_D3D11_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vlc_filter.h>
#include <vlc_picture.h>

#include "nsfw_filter.h"

typedef struct nsfw_d3d11_backend_t nsfw_d3d11_backend_t;

bool nsfw_d3d11_is_opaque(vlc_fourcc_t chroma);

int nsfw_d3d11_open(filter_t *filter, nsfw_d3d11_backend_t **backend);
void nsfw_d3d11_close(nsfw_d3d11_backend_t *backend);

int nsfw_d3d11_set_analysis_size(nsfw_d3d11_backend_t *backend,
                                 int width, int height);

int nsfw_d3d11_readback_rgb(nsfw_d3d11_backend_t *backend,
                            picture_t *picture,
                            uint8_t *rgb, size_t rgb_capacity,
                            int *width, int *height);

unsigned nsfw_d3d11_decoder_surface_count(picture_t *picture);

picture_t *nsfw_d3d11_render_blocked(filter_t *filter,
                                     nsfw_d3d11_backend_t *backend,
                                     picture_t *source,
                                     nsfw_block_style_t style);

picture_t *nsfw_d3d11_render_debug_overlay(filter_t *filter,
                                           nsfw_d3d11_backend_t *backend,
                                           picture_t *source,
                                           float score, float threshold);

int nsfw_d3d11_dump_ppm(nsfw_d3d11_backend_t *backend,
                        picture_t *picture, const char *path);

const char *nsfw_d3d11_adapter_name(const nsfw_d3d11_backend_t *backend);
const char *nsfw_d3d11_texture_format(const nsfw_d3d11_backend_t *backend);

#endif /* VLC_NSFW_FILTER_D3D11_H */
