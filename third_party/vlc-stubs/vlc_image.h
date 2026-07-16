#pragma once

#include <vlc_common.h>
#include <vlc_picture.h>

typedef struct block_t block_t;
typedef struct decoder_t decoder_t;
typedef struct encoder_t encoder_t;

typedef struct image_handler_t image_handler_t;

struct image_handler_t {
    picture_t *(*pf_convert)(image_handler_t *, picture_t *,
                             const video_format_t *, video_format_t *);
};

image_handler_t *image_HandlerCreate(vlc_object_t *);
void image_HandlerDelete(image_handler_t *);

#define image_Convert(a, b, c, d) (a)->pf_convert((a), (b), (c), (d))
