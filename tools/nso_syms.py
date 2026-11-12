#!/usr/bin/env python3
"""Dump the dynamic symbols of Switch NSO modules as TSV: module, name, offset, size, kind.

Offsets are relative to the module base. Standard library only (the LZ4 block
format is decoded here). Usage: nso_syms.py OUTDIR NSO [NSO...]

The NSO header layout follows the public Switchbrew NSO documentation; the
dynamic-section walk is the same idea as recomp/nso.py in engage-pc-recomp (MIT).
"""
import struct
import sys
from pathlib import Path

DT_NULL, DT_HASH, DT_STRTAB, DT_SYMTAB, DT_GNU_HASH = 0, 4, 5, 6, 0x6FFFFEF5


def lz4_block(src: bytes, dst_size: int) -> bytes:
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                s = src[i]
                i += 1
                lit += s
                if s != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | (src[i + 1] << 8)
        i += 2
        ml = token & 15
        if ml == 15:
            while True:
                s = src[i]
                i += 1
                ml += s
                if s != 255:
                    break
        ml += 4
        start = len(out) - off
        for k in range(ml):
            out.append(out[start + k])
    if len(out) != dst_size:
        raise ValueError(f'lz4: got {len(out)} bytes, expected {dst_size}')
    return bytes(out)


def load_image(path: Path):
    raw = path.read_bytes()
    assert raw[:4] == b'NSO0', path
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    segs = []
    for i in range(3):
        file_off, mem_off, dec = struct.unpack_from('<III', raw, 0x10 + i * 0x10)
        comp = struct.unpack_from('<I', raw, 0x60 + i * 4)[0]
        data = raw[file_off:file_off + comp]
        if flags & (1 << i):
            data = lz4_block(data, dec)
        segs.append((mem_off, data))
    bss = struct.unpack_from('<I', raw, 0x3C)[0]
    end = segs[2][0] + len(segs[2][1]) + bss
    img = bytearray((end + 0xFFF) & ~0xFFF)
    for off, d in segs:
        img[off:off + len(d)] = d
    return img


def cstr(img, off):
    return img[off:img.index(b'\0', off)].decode('utf-8', 'replace')


def symbols(img):
    u32 = lambda o: struct.unpack_from('<I', img, o)[0]
    u64 = lambda o: struct.unpack_from('<Q', img, o)[0]
    mod0 = u32(4)
    assert img[mod0:mod0 + 4] == b'MOD0'
    dyn_off = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    dyn = {}
    o = dyn_off
    while True:
        tag, val = u64(o), u64(o + 8)
        o += 16
        if tag == DT_NULL:
            break
        dyn.setdefault(tag, val)
    strtab, symtab = dyn[DT_STRTAB], dyn[DT_SYMTAB]
    if DT_HASH in dyn:
        n = u32(dyn[DT_HASH] + 4)
    else:
        h = dyn[DT_GNU_HASH]
        nb, symoff, bloom = u32(h), u32(h + 4), u32(h + 8)
        buckets = h + 16 + bloom * 8
        last = max([u32(buckets + 4 * i) for i in range(nb)] + [0])
        if last < symoff:
            n = symoff
        else:
            chains = buckets + 4 * nb
            while not u32(chains + 4 * (last - symoff)) & 1:
                last += 1
            n = last + 1
    for i in range(n):
        o = symtab + i * 24
        name, info, _other, shndx = struct.unpack_from('<IBBH', img, o)
        value, size = struct.unpack_from('<QQ', img, o + 8)
        nm = cstr(img, strtab + name) if name else ''
        if not nm:
            continue
        kind = 'FUNC' if (info & 15) == 2 else 'OBJ' if (info & 15) == 1 else 'OTHER'
        yield nm, value, size, kind, shndx != 0


def main():
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    for p in map(Path, sys.argv[2:]):
        img = load_image(p)
        rows = [(n, v, s, k) for n, v, s, k, defined in symbols(img) if defined and v]
        with open(out / f'{p.name}.syms.tsv', 'w', encoding='utf-8') as f:
            for n, v, s, k in sorted(rows, key=lambda r: r[1]):
                f.write(f'{v:x}\t{s:x}\t{k}\t{n}\n')
        print(f'{p.name}: {len(rows)} defined symbols')


if __name__ == '__main__':
    main()
