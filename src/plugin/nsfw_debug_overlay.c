#include "nsfw_debug_overlay.h"

void DebugScoreColor(float score, float threshold,
                     uint8_t *red, uint8_t *green, uint8_t *blue)
{
    const uint8_t low[]    = { 0x28, 0xC7, 0x62 };
    const uint8_t middle[] = { 0xFF, 0xA6, 0x2A };
    const uint8_t high[]   = { 0xE5, 0x34, 0x30 };
    const uint8_t *start = low;
    const uint8_t *end   = middle;
    float progress;

    if (score < 0.0f) score = 0.0f;
    if (score > 1.0f) score = 1.0f;
    if (threshold < 0.001f) threshold = 0.001f;
    if (threshold > 0.999f) threshold = 0.999f;

    if (score >= threshold) {
        start = end = high;
        progress = 0.0f;
    } else {
        progress = score / threshold;
        if (progress <= 0.70f) {
            end = middle;
            progress /= 0.70f;
        } else {
            start = middle;
            end = high;
            progress = (progress - 0.70f) / 0.30f;
        }
    }

    if (progress < 0.0f) progress = 0.0f;
    if (progress > 1.0f) progress = 1.0f;

    *red   = (uint8_t)(start[0] + (end[0] - start[0]) * progress + 0.5f);
    *green = (uint8_t)(start[1] + (end[1] - start[1]) * progress + 0.5f);
    *blue  = (uint8_t)(start[2] + (end[2] - start[2]) * progress + 0.5f);
}

int DebugGlyphIndex(char character)
{
    if (character >= '0' && character <= '9')
        return character - '0';
    if (character == '.')
        return 10;
    if (character == '/')
        return 11;
    return -1;
}
