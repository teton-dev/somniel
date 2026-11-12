#!/usr/bin/env python3
"""Turn the renderer's render-target dumps (<data>/rtdump/<submit>_<serial>_<w>x<h>_f<vkformat>.raw, see vkr.c
"frame capture") into PNGs, half size, plus a line of statistics per target.

usage: rtdump.py DUMPDIR OUTDIR
Float formats are tone-mapped with x/(1+x) so HDR targets stay visible; sRGB and UNORM are written as stored.
Standard library only.
"""
import os
import re
import struct
import sys
import zlib

R8, RG8, RGBA8, RGBA8S, BGRA8, BGRA8S = 9, 16, 37, 43, 44, 50
A2B10G10R10, B10G11R11, R16F, RG16F, RGBA16F, R32F, RGBA32F = 64, 122, 76, 83, 97, 100, 109


def half(b):
    h = struct.unpack('<H', b)[0]
    s, e, m = h >> 15, (h >> 10) & 31, h & 1023
    if e == 0:
        v = m / 1024 * 2 ** -14
    elif e == 31:
        v = float('inf') if m == 0 else 0.0
    else:
        v = (1 + m / 1024) * 2 ** (e - 15)
    return -v if s else v


def uf(bits, mbits):
    e, m = bits >> mbits, bits & ((1 << mbits) - 1)
    if e == 0:
        return m / (1 << mbits) * 2 ** -14
    if e == 31:
        return 65504.0
    return (1 + m / (1 << mbits)) * 2 ** (e - 15)


def tm(v):
    v = max(0.0, v)
    v = v / (1 + v)
    return int(255 * v ** (1 / 2.2) + 0.5)


def pixel(fmt, d, o):
    if fmt in (RGBA8, RGBA8S):
        return d[o], d[o + 1], d[o + 2]
    if fmt in (BGRA8, BGRA8S):
        return d[o + 2], d[o + 1], d[o]
    if fmt == R8:
        return d[o], d[o], d[o]
    if fmt == RG8:
        return d[o], d[o + 1], 0
    if fmt == A2B10G10R10:
        v = struct.unpack_from('<I', d, o)[0]
        return (v & 1023) >> 2, ((v >> 10) & 1023) >> 2, ((v >> 20) & 1023) >> 2
    if fmt == B10G11R11:
        v = struct.unpack_from('<I', d, o)[0]
        return tm(uf(v & 2047, 6)), tm(uf((v >> 11) & 2047, 6)), tm(uf(v >> 22, 5))
    if fmt == RGBA16F:
        return tm(half(d[o:o + 2])), tm(half(d[o + 2:o + 4])), tm(half(d[o + 4:o + 6]))
    if fmt == RG16F:
        return tm(half(d[o:o + 2])), tm(half(d[o + 2:o + 4])), 0
    if fmt == R16F:
        v = tm(half(d[o:o + 2]))
        return v, v, v
    if fmt == R32F:
        v = tm(struct.unpack_from('<f', d, o)[0])
        return v, v, v
    if fmt == RGBA32F:
        r, g, b = struct.unpack_from('<3f', d, o)
        return tm(r), tm(g), tm(b)
    return None


BPP = {R8: 1, RG8: 2, RGBA8: 4, RGBA8S: 4, BGRA8: 4, BGRA8S: 4, A2B10G10R10: 4, B10G11R11: 4, R16F: 2, RG16F: 4,
       RGBA16F: 8, R32F: 4, RGBA32F: 16}


def png(path, w, h, rows):
    def chunk(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    raw = b''.join(b'\0' + bytes(r) for r in rows)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                           chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    only = sys.argv[3] if len(sys.argv) > 3 else None  # a capture id: convert just that capture
    os.makedirs(dst, exist_ok=True)
    names = sorted(os.listdir(src))
    if only is None:  # default: the newest capture
        ids = sorted({n[:6] for n in names if re.match(r'\d{6}_\d+_\d+_', n)})
        only = ids[-1] if ids else None
    for name in names:
        m = re.match(r'(\d{6})_(\d+)_(\d+)_(\d+)x(\d+)_f(\d+)\.raw$', name)
        if not m or m.group(1) != only:
            continue
        m = re.match(r'\d{6}_(\d+)_(\d+)_(\d+)x(\d+)_f(\d+)\.raw$', name)
        w, h, fmt = int(m.group(3)), int(m.group(4)), int(m.group(5))
        bpp = BPP.get(fmt)
        d = open(os.path.join(src, name), 'rb').read()
        if not bpp or len(d) < w * h * bpp:
            print(f'{name}: format {fmt} not handled')
            continue
        step = 2 if w > 64 and h > 64 else 1
        rows, tot, nz = [], [0, 0, 0], 0
        for y in range(0, h, step):
            row = bytearray()
            for x in range(0, w, step):
                p = pixel(fmt, d, (y * w + x) * bpp)
                row += bytes(p)
                tot[0] += p[0]; tot[1] += p[1]; tot[2] += p[2]
                nz += (p[0] | p[1] | p[2]) != 0
            rows.append(row)
        n = len(rows) * (len(rows[0]) // 3)
        png(os.path.join(dst, name[:-4] + '.png'), len(rows[0]) // 3, len(rows), rows)
        print(f'{name}: mean {tot[0] / n:.0f} {tot[1] / n:.0f} {tot[2] / n:.0f}, non-black {100 * nz / n:.0f}%')


if __name__ == '__main__':
    main()
