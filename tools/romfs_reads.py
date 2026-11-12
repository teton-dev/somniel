#!/usr/bin/env python3
"""Map the device's fs_reads.log (time, image offset, size per new read region) to RomFS file paths.

usage: romfs_reads.py ROMFS_IMAGE FS_READS_LOG [--from SECONDS]
Reads the standard RomFS tables (0x50-byte header: dir hash, dir meta, file hash, file meta, data offset) of the image
built by pack_romfs.py and prints, in order, each file the game read (with the time of its first read).
"""
import bisect
import struct
import sys


def load_files(path):
    with open(path, 'rb') as f:
        h = struct.unpack('<10Q', f.read(0x50))
        dir_meta_off, dir_meta_size, file_meta_off, file_meta_size, data_off = h[3], h[4], h[7], h[8], h[9]
        f.seek(dir_meta_off)
        dmeta = f.read(dir_meta_size)
        f.seek(file_meta_off)
        fmeta = f.read(file_meta_size)

    def dir_path(off):
        parts = []
        while True:
            parent, _, _, _, _, nlen = struct.unpack_from('<6I', dmeta, off)
            name = dmeta[off + 24:off + 24 + nlen].decode('utf-8', 'replace')
            if off == 0:
                break
            parts.append(name)
            off = parent
        return '/'.join(reversed(parts))

    files, o = [], 0
    while o + 32 <= len(fmeta):
        parent, _, data, size, _, nlen = struct.unpack_from('<IIQQII', fmeta, o)
        name = fmeta[o + 32:o + 32 + nlen].decode('utf-8', 'replace')
        files.append((data_off + data, size, dir_path(parent) + '/' + name))
        o += 32 + ((nlen + 3) & ~3)
    files.sort()
    return files


def main():
    image, log = sys.argv[1], sys.argv[2]
    t_from = float(sys.argv[sys.argv.index('--from') + 1]) if '--from' in sys.argv else 0.0
    files = load_files(image)
    starts = [f[0] for f in files]
    seen, t0 = set(), None
    for line in open(log):
        t, off, _size = line.split()
        t, off = float(t), int(off, 16)
        t0 = t if t0 is None else t0
        i = bisect.bisect_right(starts, off) - 1
        if i < 0 or off >= files[i][0] + files[i][1]:
            continue
        if files[i][2] in seen or t - t0 < t_from:
            continue
        seen.add(files[i][2])
        print(f'{t - t0:8.2f}s  {files[i][1]:>11}  {files[i][2]}')


if __name__ == '__main__':
    main()
