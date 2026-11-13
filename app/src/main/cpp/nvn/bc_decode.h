/* CPU decoding of BC (S3TC/RGTC/BPTC) textures for GPUs without textureCompressionBC (the Mali-G68). The Adreno samples
 * BC natively and never calls this. Decoding is done with bcdec (third_party/bcdec). */
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>
#include "volk.h"

/* the uncompressed format a BC format decodes to (RGBA8 UNORM/sRGB, R8, R8G8 (S)NORM, RGBA16F for BC6H);
 * VK_FORMAT_UNDEFINED for a format that is not BC */
VkFormat bc_fallback_format(VkFormat bc);

/* bytes per texel of bc_fallback_format(bc) */
uint32_t bc_fallback_bpp(VkFormat bc);

/* Decodes one mip level of one layer: src holds its blocks row by row (src_row bytes per row of blocks, ceil(h/4) rows
 * per depth slice, d slices); dst receives w x h x d texels of the fallback format, tightly packed. */
void bc_decode_level(VkFormat bc, const uint8_t *src, uint32_t src_row, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst);
