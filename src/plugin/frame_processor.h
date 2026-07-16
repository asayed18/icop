/*****************************************************************************
 * frame_processor.h: Frame conversion, heuristic scoring, and blocking
 *
 * Decouples pixel-level operations (packing various chroma formats to RGB,
 * heuristic skin-tone scoring, blackout/blur/watermark effects, debug
 * overlay rendering) from VLC filter orchestration and OS platform code.
 *****************************************************************************/

#ifndef VLC_NSFW_FRAME_PROCESSOR_H
#define VLC_NSFW_FRAME_PROCESSOR_H 1

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <vlc_fourcc.h>
#include <vlc_picture.h>

#include "nsfw_filter.h"  /* nsfw_block_style_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/*  Chroma / dimension utilities                                       */
/* ------------------------------------------------------------------ */

int  nsfw_fp_visible_width(const video_format_t *fmt);
int  nsfw_fp_visible_height(const video_format_t *fmt);
int  nsfw_fp_clamp_dimension(int value, int fallback);
bool nsfw_fp_is_opaque_hw_chroma(vlc_fourcc_t chroma);
void nsfw_fp_fourcc_to_string(vlc_fourcc_t chroma, char out[5]);

/* ------------------------------------------------------------------ */
/*  RGB packing  – convert any supported VLC picture to RGB for ONNX   */
/* ------------------------------------------------------------------ */

int nsfw_fp_pack_to_rgb(const picture_t *pic,
                         uint8_t *rgb, size_t rgb_capacity,
                         int target_width, int target_height,
                         int *out_width, int *out_height);

/* ------------------------------------------------------------------ */
/*  Heuristic skin-tone scoring  – fallback when no AI model is ready  */
/* ------------------------------------------------------------------ */

float nsfw_fp_heuristic_score(const picture_t *pic);

/* ------------------------------------------------------------------ */
/*  Visual blocking effects                                            */
/* ------------------------------------------------------------------ */

void nsfw_fp_block_frame(picture_t *pic, nsfw_block_style_t style);

/* ------------------------------------------------------------------ */
/*  Debug overlay  – score / threshold text + colour bar               */
/* ------------------------------------------------------------------ */

void nsfw_fp_debug_overlay(picture_t *pic, float score, float threshold);

#ifdef __cplusplus
}
#endif

#endif /* VLC_NSFW_FRAME_PROCESSOR_H */
