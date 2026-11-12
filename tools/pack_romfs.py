#!/usr/bin/env python3
"""Pack a directory tree into a Horizon RomFS image (the format nn::fs::MountRom parses).

Usage: pack_romfs.py SOURCE_DIR OUTPUT_IMAGE

Layout: 0x50-byte header, directory hash table, directory entries, file hash
table, file entries, then file data (16-byte aligned, data section 0x1000
aligned). Standard library only. The image holds the user's own game data, so
keep it outside the repository.
"""
import os
import struct
import sys
from pathlib import Path

NONE = 0xFFFFFFFF


def bucket_count(n):
    if n < 3:
        return 3
    if n < 19:
        return n | 1
    c = n
    while any(c % p == 0 for p in (2, 3, 5, 7, 11, 13, 17)):
        c += 1
    return c


def name_hash(parent, name):
    h = parent ^ 123456789
    for ch in name:
        h = ((h >> 5) | (h << 27)) & 0xFFFFFFFF
        h ^= ch
    return h


def align(v, a):
    return (v + a - 1) & ~(a - 1)


class Dir:
    __slots__ = ('name', 'path', 'parent', 'dirs', 'files', 'offset', 'sibling', 'child_dir', 'child_file', 'hash_next')

    def __init__(self, name, path, parent):
        self.name, self.path, self.parent = name, path, parent
        self.dirs, self.files = [], []
        self.offset = self.sibling = self.child_dir = self.child_file = self.hash_next = NONE


class File:
    __slots__ = ('name', 'path', 'parent', 'size', 'offset', 'data', 'sibling', 'hash_next')

    def __init__(self, name, path, parent, size):
        self.name, self.path, self.parent, self.size = name, path, parent, size
        self.offset = self.data = self.sibling = self.hash_next = NONE


def scan(root_path):
    root = Dir(b'', root_path, None)
    all_dirs, all_files = [], []

    def walk(d):
        all_dirs.append(d)
        entries = sorted(os.scandir(d.path), key=lambda e: os.fsencode(e.name))
        for e in entries:
            nm = os.fsencode(e.name)
            if e.is_dir(follow_symlinks=False):
                sub = Dir(nm, e.path, d)
                d.dirs.append(sub)
            elif e.is_file(follow_symlinks=False):
                f = File(nm, e.path, d, e.stat().st_size)
                d.files.append(f)
                all_files.append(f)
        for sub in d.dirs:
            walk(sub)

    sys.setrecursionlimit(10000)
    walk(root)
    return root, all_dirs, all_files


def main():
    src, out = Path(sys.argv[1]), Path(sys.argv[2])
    if out.exists():
        sys.exit(f'{out} already exists; remove it first')
    root, dirs, files = scan(str(src))
    print(f'{len(dirs)} directories, {len(files)} files')

    # entry offsets inside the two meta tables
    off = 0
    for d in dirs:
        d.offset = off
        off += 0x18 + align(len(d.name), 4)
    dir_meta_size = off
    off = 0
    for f in files:
        f.offset = off
        off += 0x20 + align(len(f.name), 4)
    file_meta_size = off

    # data offsets, in file order
    pos = 0
    for f in files:
        pos = align(pos, 16)
        f.data = pos
        pos += f.size
    data_size = pos

    # links
    for d in dirs:
        for i, sub in enumerate(d.dirs):
            sub.sibling = d.dirs[i + 1].offset if i + 1 < len(d.dirs) else NONE
        d.child_dir = d.dirs[0].offset if d.dirs else NONE
        for i, f in enumerate(d.files):
            f.sibling = d.files[i + 1].offset if i + 1 < len(d.files) else NONE
        d.child_file = d.files[0].offset if d.files else NONE

    # hash tables
    nd, nf = bucket_count(len(dirs)), bucket_count(len(files))
    dir_buckets, file_buckets = [NONE] * nd, [NONE] * nf
    for d in dirs:
        parent_off = d.parent.offset if d.parent else 0
        b = name_hash(parent_off, d.name) % nd
        d.hash_next = dir_buckets[b]
        dir_buckets[b] = d.offset
    for f in files:
        b = name_hash(f.parent.offset, f.name) % nf
        f.hash_next = file_buckets[b]
        file_buckets[b] = f.offset

    dir_hash_off = 0x200
    dir_hash_size = nd * 4
    dir_meta_off = align(dir_hash_off + dir_hash_size, 4)
    file_hash_off = align(dir_meta_off + dir_meta_size, 4)
    file_hash_size = nf * 4
    file_meta_off = align(file_hash_off + file_hash_size, 4)
    data_off = align(file_meta_off + file_meta_size, 0x1000)

    header = struct.pack('<10Q', 0x50, dir_hash_off, dir_hash_size, dir_meta_off, dir_meta_size, file_hash_off,
                         file_hash_size, file_meta_off, file_meta_size, data_off)
    with open(out, 'wb') as o:
        o.write(header)
        o.seek(dir_hash_off)
        o.write(struct.pack(f'<{nd}I', *dir_buckets))
        o.seek(dir_meta_off)
        for d in dirs:
            parent_off = d.parent.offset if d.parent else 0
            o.write(struct.pack('<6I', parent_off, d.sibling, d.child_dir, d.child_file, d.hash_next, len(d.name)))
            o.write(d.name + b'\0' * (align(len(d.name), 4) - len(d.name)))
        o.seek(file_hash_off)
        o.write(struct.pack(f'<{nf}I', *file_buckets))
        o.seek(file_meta_off)
        for f in files:
            o.write(struct.pack('<IIQQII', f.parent.offset, f.sibling, f.data, f.size, f.hash_next, len(f.name)))
            o.write(f.name + b'\0' * (align(len(f.name), 4) - len(f.name)))
        total = data_off
        for i, f in enumerate(files):
            o.seek(data_off + f.data)
            with open(f.path, 'rb') as s:
                while True:
                    blk = s.read(8 << 20)
                    if not blk:
                        break
                    o.write(blk)
            if i % 20000 == 0:
                print(f'  {i}/{len(files)} files', flush=True)
        o.truncate(data_off + data_size)
    print(f'wrote {out}: {data_off + data_size} bytes (data at 0x{data_off:x})')


if __name__ == '__main__':
    main()
