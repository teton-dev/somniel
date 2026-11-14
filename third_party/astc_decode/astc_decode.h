#pragma once
/* ASTC LDR decoder (2D). See VERSION.txt for provenance and licence. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif
/* Decodes a whole image of width x height texels made of bw x bh blocks (16 bytes each, row major) into tightly
 * packed RGBA8. Error blocks become magenta. srgb selects the sRGB endpoint expansion. */
void astc_decode_ldr(const uint8_t *blocks, uint32_t width, uint32_t height, uint32_t bw, uint32_t bh, bool srgb,
                     uint8_t *rgba_out);
bool astc_decode_block(const uint8_t block[16], uint32_t bw, uint32_t bh, bool srgb, uint8_t *rgba_out);
#ifdef __cplusplus
}
#endif
