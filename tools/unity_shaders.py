#!/usr/bin/env python3
"""Read the compiled NVN shader blobs out of Unity asset bundles and serialized files (Unity 2020.3, Switch).

No third-party modules: a small UnityFS reader, a type-tree driven object reader and an LZ4 block decoder.

    for name, segments in shader_blobs(path):   # one entry per Shader object
        ...                                    # segments: the decompressed blob segments of every platform

Layout notes
  UnityFS      header (big endian), block table (LZ4), nodes; each node is a serialized file or a resource.
  Serialized   header, type table (with type trees in bundles), object table. Shader is class 48.
  Shader       platforms[], offsets[][], compressedLengths[][], decompressedLengths[][], compressedBlob: per platform a
               list of LZ4 segments inside compressedBlob.
"""
import struct
from pathlib import Path

SHADER_CLASS = 48


def lz4_block(src, size):
    """LZ4 block decode of src into exactly size bytes."""
    try:
        import lz4.block  # optional, much faster
        return lz4.block.decompress(bytes(src), uncompressed_size=size)
    except ImportError:
        pass
    out = bytearray()
    i, n = 0, len(src)
    while i < n:
        tok = src[i]
        i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        ml = tok & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        if off == 0:
            raise ValueError('LZ4: zero offset')
        start = len(out) - off
        if off >= ml:
            out += out[start:start + ml]
        else:   # overlapping copy: repeat the off-byte pattern
            pat = bytes(out[start:])
            out += (pat * (ml // off + 1))[:ml]
    if len(out) != size:
        raise ValueError(f'LZ4: got {len(out)} bytes, expected {size}')
    return bytes(out)


class Reader:
    def __init__(self, data, pos=0, big=False):
        self.d, self.p, self.e = data, pos, '>' if big else '<'

    def u(self, fmt):
        v = struct.unpack_from(self.e + fmt, self.d, self.p)
        self.p += struct.calcsize(self.e + fmt)
        return v[0] if len(v) == 1 else v

    def strz(self):
        end = self.d.index(b'\0', self.p)
        s = self.d[self.p:end].decode('utf-8', 'replace')
        self.p = end + 1
        return s

    def align(self, n):
        self.p = (self.p + n - 1) & -n


def unityfs_nodes(data):
    """The files inside a UnityFS bundle: [(path, bytes)]. Only LZ4/LZ4HC and stored blocks."""
    r = Reader(data, 0, True)
    if r.strz() != 'UnityFS':
        raise ValueError('not UnityFS')
    version = r.u('I')
    r.strz(), r.strz()
    _size, csize, usize, flags = r.u('qIII')
    if version >= 7:
        r.align(16)
    if flags & 0x80:
        info = data[len(data) - csize:]
    else:
        info = data[r.p:r.p + csize]
        r.p += csize
    comp = flags & 0x3f
    if comp in (2, 3):
        info = lz4_block(info, usize)
    elif comp:
        raise ValueError(f'block table compression {comp}')
    b = Reader(info, 16, True)
    blocks = [b.u('IIH') for _ in range(b.u('i'))]
    nodes = []
    for _ in range(b.u('i')):
        off, size, _fl = b.u('qqI')
        nodes.append((off, size, b.strz()))
    if flags & 0x200:
        r.align(16)
    parts = []
    for usz, csz, bf in blocks:
        raw = data[r.p:r.p + csz]
        r.p += csz
        c = bf & 0x3f
        if c in (2, 3):
            raw = lz4_block(raw, usz)
        elif c:
            raise ValueError(f'block compression {c}')
        parts.append(raw)
    blob = b''.join(parts)
    return [(path, blob[off:off + size]) for off, size, path in nodes]


class Serialized:
    """Object table and type trees of a serialized file (format 17 to 22)."""

    def __init__(self, data):
        self.d = data
        r = Reader(data, 0, True)
        meta_size, file_size, self.version, data_off = r.u('IIII')
        big = r.u('B')
        r.p += 3
        if self.version >= 22:
            meta_size, file_size, data_off, _ = r.u('IqqQ')
        if not 17 <= self.version <= 22:
            raise ValueError(f'serialized file version {self.version}')
        self.data_off = data_off
        r.e = '>' if big else '<'
        self.e = r.e
        self.unity = r.strz()
        r.u('i')
        self.has_trees = bool(r.u('B'))
        self.types = []
        for _ in range(r.u('i')):
            cls = r.u('i')
            r.u('B')
            r.u('h')
            if cls == 114:
                r.p += 16
            r.p += 16
            tree = None
            if self.has_trees:
                nn, sb = r.u('ii')
                recs = [struct.unpack_from(r.e + 'HBBIIiiiQ', data, r.p + 32 * k) for k in range(nn)]
                r.p += 32 * nn
                strings = data[r.p:r.p + sb]
                r.p += sb

                def name(o, strings=strings):
                    if o & 0x80000000:
                        return '#%d' % (o & 0x7fffffff)
                    return strings[o:strings.index(b'\0', o)].decode()
                tree = [dict(level=lv, array=tf & 1, type=name(ts), name=name(ns), size=bs, align=bool(mf & 0x4000))
                        for _v, lv, tf, ts, ns, bs, _ix, mf, _h in recs]
                if self.version >= 21:
                    deps = r.u('i')     # not "r.p += 4 * r.u()": the read moves r.p itself
                    r.p += 4 * deps
            self.types.append((cls, tree))
        self.objects = []
        for _ in range(r.u('i')):
            r.align(4)
            path_id, start, size, tid = r.u('qqIi')
            self.objects.append((path_id, data_off + start, size, self.types[tid][0], tid))

    def read(self, obj, tree=None):
        """The object as nested dicts/lists/bytes/ints, following its type tree (or the one given)."""
        _pid, start, _size, _cls, tid = obj
        tree = tree or self.types[tid][1]
        r = Reader(self.d, start, self.e == '>')
        pos = [0]

        def node():
            i = pos[0]
            n = tree[i]
            pos[0] += 1
            kids = []
            if n['array']:
                count = None
                size_i = pos[0]
                skip_subtree()          # the size node
                elem = pos[0]
                count = r.u('i')
                e = tree[elem]
                leaf = elem + 1 >= len(tree) or tree[elem + 1]['level'] <= e['level']
                if leaf and e['size'] == 1 and not e['align']:
                    v = bytes(self.d[r.p:r.p + count])
                    r.p += count
                    skip_subtree()
                else:
                    v = []
                    for _ in range(count):
                        pos[0] = elem
                        v.append(node())
                    if not count:
                        pos[0] = elem
                        skip_subtree()
                del size_i
            else:
                while pos[0] < len(tree) and tree[pos[0]]['level'] > n['level']:
                    kids.append((tree[pos[0]]['name'], node()))
                if kids:
                    v = dict(kids) if len(kids) > 1 or not tree[i + 1]['array'] else kids[0][1]
                else:
                    raw = self.d[r.p:r.p + n['size']]
                    r.p += n['size']
                    v = int.from_bytes(raw, 'big' if self.e == '>' else 'little')
            if n['align']:
                r.align(4)
            return v

        def skip_subtree():
            lv = tree[pos[0]]['level']
            pos[0] += 1
            while pos[0] < len(tree) and tree[pos[0]]['level'] > lv:
                pos[0] += 1

        return node()


def _class_ids(raw):
    """Class ids of a serialized file's type table; raises struct.error/ValueError/IndexError when raw is too short."""
    r = Reader(raw, 0, True)
    _m, _f, version, _d = r.u('IIII')
    big = r.u('B')
    r.p += 3
    if version >= 22:
        r.u('IqqQ')
    if not 17 <= version <= 22:
        return set()
    r.e = '>' if big else '<'
    r.strz()
    r.u('i')
    trees = r.u('B')
    ids = set()
    for _ in range(r.u('i')):
        cls = r.u('i')
        r.p += 3 + (16 if cls == 114 else 0) + 16
        ids.add(cls)
        if trees:
            nn, sb = r.u('ii')
            r.p += 32 * nn + sb
            if version >= 21:
                deps = r.u('i')
                r.p += 4 * deps
        if r.p > len(raw):
            raise IndexError
    return ids


def bundle_classes(path):
    """Class ids present in a bundle's serialized files, decompressing only as many blocks as the type tables need."""
    with open(path, 'rb') as f:
        head = f.read(4096)
        if head[:8] != b'UnityFS\0':
            return _class_ids(Path(path).read_bytes()) if len(head) > 48 else set()
        r = Reader(head, 0, True)
        r.strz()
        version = r.u('I')
        r.strz(), r.strz()
        size, csize, usize, flags = r.u('qIII')
        if version >= 7:
            r.align(16)
        if flags & 0x80:
            f.seek(size - csize)
            info = f.read(csize)
            data_at = r.p
        else:
            f.seek(r.p)
            info = f.read(csize)
            data_at = r.p + csize
        if flags & 0x3f in (2, 3):
            info = lz4_block(info, usize)
        elif flags & 0x3f:
            return set()
        b = Reader(info, 16, True)
        blocks = [b.u('IIH') for _ in range(b.u('i'))]
        nodes = []
        for _ in range(b.u('i')):
            off, nsize, _fl = b.u('qqI')
            nodes.append((off, nsize, b.strz()))
        if flags & 0x200:
            data_at = (data_at + 15) & -16
        f.seek(data_at)
        buf = bytearray()
        left = list(blocks)

        def more():
            if not left:
                return False
            usz, csz, bf = left.pop(0)
            raw = f.read(csz)
            c = bf & 0x3f
            if c in (2, 3):
                raw = lz4_block(raw, usz)
            elif c:
                raise ValueError(f'block compression {c}')
            buf.extend(raw)
            return True

        ids = set()
        for off, nsize, name in nodes:
            if name.endswith(('.resS', '.resource')):
                continue
            while True:
                try:
                    if len(buf) < min(off + nsize, off + 64):
                        raise IndexError
                    ids |= _class_ids(bytes(buf[off:off + nsize]))
                    break
                except (struct.error, IndexError, ValueError):
                    if len(buf) >= off + nsize or not more():
                        break
        return ids


_shader_tree = [None]


def serialized_files(path):
    data = Path(path).read_bytes()
    if data[:8] == b'UnityFS\0':
        for name, raw in unityfs_nodes(data):
            if raw[:4] not in (b'\0\0\0\0',) and len(raw) > 48 and name.endswith(('.resS', '.resource')):
                continue
            yield name, raw
    else:
        yield Path(path).name, data


def shader_blobs(path):
    """(shader name, [decompressed segment bytes of every platform]) for each Shader object in the file."""
    for name, raw in serialized_files(path):
        try:
            sf = Serialized(raw)
        except (ValueError, struct.error, IndexError):
            continue
        for obj in sf.objects:
            if obj[3] != SHADER_CLASS:
                continue
            tree = sf.types[obj[4]][1]
            if tree:
                _shader_tree[0] = tree
            elif not _shader_tree[0]:
                raise ValueError(f'{path}: Shader without a type tree; read a bundle with type trees first')
            o = sf.read(obj, tree or _shader_tree[0])
            blob = o['compressedBlob']
            segs = []
            for offs, cl, dl in zip(o['offsets'], o['compressedLengths'], o['decompressedLengths']):
                for a, b, c in zip(offs, cl, dl):
                    segs.append(lz4_block(blob[a:a + b], c))
            parsed = o.get('m_ParsedForm')
            sname = (parsed.get('m_Name') or parsed.get('#427')) if isinstance(parsed, dict) else None   # 427: m_Name
            if isinstance(sname, bytes):
                sname = sname.decode('utf-8', 'replace')
            yield sname or '?', segs


if __name__ == '__main__':
    import sys
    for p in sys.argv[1:]:
        for nm, segs in shader_blobs(p):
            print(p, nm, [len(s) for s in segs], sum(s.count(bytes.fromhex('34127698')) for s in segs))
