/* CPU decoding of BC textures for GPUs without BC support (see bc_decode.h). */
#include "bc_decode.h"

#include <string.h>

#define BCDEC_STATIC
#define BCDEC_BC4BC5_PRECISE /* signed BC4/BC5 (the default mode decodes unsigned only) */
#define BCDEC_IMPLEMENTATION
#include "../../../../../third_party/bcdec/bcdec.h"

VkFormat bc_fallback_format(VkFormat bc) {
    switch (bc) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC7_UNORM_BLOCK: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_BC4_UNORM_BLOCK: return VK_FORMAT_R8_UNORM;
    case VK_FORMAT_BC4_SNORM_BLOCK: return VK_FORMAT_R8_SNORM;
    case VK_FORMAT_BC5_UNORM_BLOCK: return VK_FORMAT_R8G8_UNORM;
    case VK_FORMAT_BC5_SNORM_BLOCK: return VK_FORMAT_R8G8_SNORM;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: return VK_FORMAT_R16G16B16A16_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

uint32_t bc_fallback_bpp(VkFormat bc) {
    switch (bc_fallback_format(bc)) {
    case VK_FORMAT_R8_UNORM:
    case VK_FORMAT_R8_SNORM: return 1;
    case VK_FORMAT_R8G8_UNORM:
    case VK_FORMAT_R8G8_SNORM: return 2;
    case VK_FORMAT_R16G16B16A16_SFLOAT: return 8;
    case VK_FORMAT_UNDEFINED: return 0;
    default: return 4;
    }
}

static uint32_t block_bytes(VkFormat bc) {
    switch (bc) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
    case VK_FORMAT_BC4_UNORM_BLOCK:
    case VK_FORMAT_BC4_SNORM_BLOCK: return 8;
    default: return 16;
    }
}

/* one 4x4 block into out (pitch: bytes per output row of the block) */
static void decode_block(VkFormat bc, const uint8_t *blk, uint8_t *out, int pitch) {
    switch (bc) {
    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
        bcdec_bc1(blk, out, pitch);
        for (int y = 0; y < 4; y++) /* BC1 without alpha: the 3-colour mode's index 3 is opaque black, not transparent */
            for (int x = 0; x < 4; x++) out[y * pitch + x * 4 + 3] = 255;
        break;
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: bcdec_bc1(blk, out, pitch); break;
    case VK_FORMAT_BC2_UNORM_BLOCK:
    case VK_FORMAT_BC2_SRGB_BLOCK: bcdec_bc2(blk, out, pitch); break;
    case VK_FORMAT_BC3_UNORM_BLOCK:
    case VK_FORMAT_BC3_SRGB_BLOCK: bcdec_bc3(blk, out, pitch); break;
    case VK_FORMAT_BC4_UNORM_BLOCK: bcdec_bc4(blk, out, pitch, 0); break;
    case VK_FORMAT_BC4_SNORM_BLOCK: bcdec_bc4(blk, out, pitch, 1); break;
    case VK_FORMAT_BC5_UNORM_BLOCK: bcdec_bc5(blk, out, pitch, 0); break;
    case VK_FORMAT_BC5_SNORM_BLOCK: bcdec_bc5(blk, out, pitch, 1); break;
    case VK_FORMAT_BC7_UNORM_BLOCK:
    case VK_FORMAT_BC7_SRGB_BLOCK: bcdec_bc7(blk, out, pitch); break;
    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
    case VK_FORMAT_BC6H_SFLOAT_BLOCK: {
        uint16_t rgb[16 * 3]; /* bcdec writes RGB halfs, its pitch counted in halfs */
        bcdec_bc6h_half(blk, rgb, 4 * 3, bc == VK_FORMAT_BC6H_SFLOAT_BLOCK);
        for (int y = 0; y < 4; y++) {
            uint16_t *row = (uint16_t *)(out + y * pitch);
            for (int x = 0; x < 4; x++) {
                row[x * 4 + 0] = rgb[(y * 4 + x) * 3 + 0];
                row[x * 4 + 1] = rgb[(y * 4 + x) * 3 + 1];
                row[x * 4 + 2] = rgb[(y * 4 + x) * 3 + 2];
                row[x * 4 + 3] = 0x3c00; /* 1.0 */
            }
        }
        break;
    }
    default: memset(out, 0, (size_t)pitch * 4); break;
    }
}

void bc_decode_level(VkFormat bc, const uint8_t *src, uint32_t src_row, uint32_t w, uint32_t h, uint32_t d, uint8_t *dst) {
    uint32_t bpp = bc_fallback_bpp(bc), bb = block_bytes(bc);
    if (!bpp) return;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    size_t dst_row = (size_t)w * bpp, dst_slice = dst_row * h;
    uint8_t tmp[4 * 4 * 8]; /* one decoded block, at most 8 bytes a texel */
    int pitch = (int)(4 * bpp);
    for (uint32_t z = 0; z < d; z++) {
        const uint8_t *slice = src + (size_t)z * src_row * bh;
        for (uint32_t by = 0; by < bh; by++) {
            const uint8_t *blk = slice + (size_t)by * src_row;
            uint32_t rows = h - by * 4 < 4 ? h - by * 4 : 4;
            for (uint32_t bx = 0; bx < bw; bx++, blk += bb) {
                decode_block(bc, blk, tmp, pitch);
                uint32_t cols = w - bx * 4 < 4 ? w - bx * 4 : 4; /* edge blocks: only the texels inside the level */
                for (uint32_t y = 0; y < rows; y++)
                    memcpy(dst + z * dst_slice + (size_t)(by * 4 + y) * dst_row + (size_t)bx * 4 * bpp, tmp + y * pitch, (size_t)cols * bpp);
            }
        }
    }
}
