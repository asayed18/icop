#ifndef NSFW_DEBUG_OVERLAY_H
#define NSFW_DEBUG_OVERLAY_H

#include <stdint.h>

void DebugScoreColor(float score, float threshold,
                     uint8_t *red, uint8_t *green, uint8_t *blue);
int  DebugGlyphIndex(char character);

#endif
