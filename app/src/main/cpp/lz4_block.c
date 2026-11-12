/* Minimal LZ4 block decompressor for NSO segments. Returns the number of bytes
 * written, or -1 on malformed input. */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

int som_lz4_decompress(const uint8_t *src, size_t src_size, uint8_t *dst, size_t dst_cap) {
    const uint8_t *ip = src, *iend = src + src_size;
    uint8_t *op = dst, *oend = dst + dst_cap;

    while (ip < iend) {
        unsigned token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            uint8_t s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                lit += s;
            } while (s == 255);
        }
        if (lit > (size_t)(iend - ip) || lit > (size_t)(oend - op)) return -1;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= iend) break; /* last sequence carries literals only */

        if (iend - ip < 2) return -1;
        size_t off = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;
        if (off == 0 || off > (size_t)(op - dst)) return -1;

        size_t ml = token & 15;
        if (ml == 15) {
            uint8_t s;
            do {
                if (ip >= iend) return -1;
                s = *ip++;
                ml += s;
            } while (s == 255);
        }
        ml += 4;
        if (ml > (size_t)(oend - op)) return -1;
        const uint8_t *mp = op - off;
        for (size_t i = 0; i < ml; i++) op[i] = mp[i]; /* may overlap */
        op += ml;
    }
    return (int)(op - dst);
}
