#!/usr/bin/env python3
"""Ahead-of-time shader translation: every NVN shader stage in the game's Unity data to SPIR-V, before play.

usage: aot_shaders.py DATA_DIR OUT_DIR XLATE_DLL [--exefs DIR] [--cap CAPTURE_DIR] [--workers N] [--rescan]
  DATA_DIR     the extracted romfs Data directory (data.unity3d, Resources, StreamingAssets/**/*.bundle)
  --exefs      the extracted exefs directory: main holds the engine's built-in programs
  OUT_DIR      receives <hash>.spv and <hash>.res.json, plus scan.json (which files hold shaders) and stages.tsv
  --cap        a device capture directory (<hash>.bin); reports captured shaders the scan did not find

Why one translation per shader is enough: the translator can ask about draw state (texture types, vertex attribute
types, colour output types). For Engage it never asks about textures (tex_queried is empty for every shader seen), and
attributes and outputs are float. So a stage is translated once with default answers and named by its code hash alone,
the hash the device computes (som_shader_hash in nvn_api_obj.c: FNV-1a 64 over the 0x880-byte control section and the
code). The renderer falls back to per-draw-state variants (<hash>_<answers>) only when the state is not the default.

Blob layout (Unity 2020.3 Switch): each decompressed segment holds programs; a stage is its NVN control section
(magic 0x98761234, 0x880 bytes; +0x700 c1 offset, +0x704 code length, +0x714 stage) directly followed by its code.
"""
import argparse
import concurrent.futures
import itertools
import json
import struct
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import unity_shaders  # noqa: E402

MAGIC = bytes.fromhex('34127698')
STAGE = {0: 'vs', 1: 'ps', 5: 'cs'}
TEX = [None, '1d', '1darray', 'buffer', '2d', '2darray', '2dms', '2dmsarray', '3d', 'cube', 'cubearray']   # .ans codes
CTL = 0x880
CODE_SKIP = 0x30


def fnv(data):
    """FNV-1a 64 (som_shader_hash), 8 bytes per Python step through a table-free split is not possible, so bytewise."""
    h = 1469598103934665603
    for b in data:
        h = ((h ^ b) * 1099511628211) & 0xffffffffffffffff
    return h


def stages_in(seg):
    """(stage id, control, code) for each NVN stage in a decompressed blob segment."""
    pos = 0
    while (pos := seg.find(MAGIC, pos)) >= 0:
        at, pos = pos, pos + 4
        if at + CTL > len(seg):
            continue
        c1, length = struct.unpack_from('<II', seg, at + 0x700)
        stage = struct.unpack_from('<I', seg, at + 0x714)[0]
        if stage > 5 or not 0 < length <= len(seg) - at - CTL or c1 > length:
            continue
        yield stage, seg[at:at + CTL], seg[at + CTL:at + CTL + length]
        pos = at + CTL + length


def nso_rodata(path):
    """The decompressed read-only data segment of an NSO."""
    raw = Path(path).read_bytes()
    if raw[:4] != b'NSO0':
        raise ValueError(f'{path} is not an NSO')
    flags = struct.unpack_from('<I', raw, 0xC)[0]
    off, _mem, dec = struct.unpack_from('<III', raw, 0x20)
    seg = raw[off:off + struct.unpack_from('<I', raw, 0x64)[0]]
    return unity_shaders.lz4_block(seg, dec) if flags & 2 else seg


def scan_file(path):
    try:
        return str(path), SHADER in unity_shaders.bundle_classes(path)
    except Exception as e:   # a damaged or unknown container: report, do not stop the scan
        return str(path), f'{type(e).__name__}: {e}'


SHADER = unity_shaders.SHADER_CLASS


def extract_file(path):
    out = []
    for name, segs in unity_shaders.shader_blobs(path):
        for seg in segs:
            for stage, ctl, code in stages_in(seg):
                out.append((fnv(ctl + code), stage, name, ctl, code))
    return out


def worker(dll, jobs, out):
    proc = subprocess.Popen(['dotnet', dll, '--serve'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    ok, bad = 0, {}
    try:
        for key, stage, ctl, code, tex in jobs:
            name = STAGE[stage]
            c1 = struct.unpack_from('<I', ctl, 0x700)[0]
            code_path = out / (key + '.code')
            code_path.write_bytes(code[0 if name == 'cs' else CODE_SKIP:])
            job = {'key': key, 'stage': name, 'debug': False, 'code': str(code_path), 'c1': code[c1:].hex(),
                   'tex': tex, 'img': {}, 'attr': {}, 'fout': {}, 'shared': 0,
                   'softfloat64': True, 'sbuf_align': 64, 'subgroup': 64}
            if name == 'ps' and ctl[0x718] & 1:
                job['early_fragment_tests'] = True
            if name == 'cs':
                job['local'] = list(struct.unpack_from('<3I', ctl, 0x748))
            proc.stdin.write(json.dumps({'job': job, 'out': str(out)}) + '\n')
            proc.stdin.flush()
            reply = proc.stdout.readline().strip()
            code_path.unlink(missing_ok=True)
            if reply.startswith('ok'):
                ok += 1
            else:
                bad[key] = reply[:300] or 'translator exited'
                if not reply:
                    break
    finally:
        proc.stdin.close()
        proc.wait(timeout=60)
    return ok, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('data', type=Path)
    ap.add_argument('out', type=Path)
    ap.add_argument('dll')
    ap.add_argument('--cap', type=Path)
    ap.add_argument('--exefs', type=Path, help='extracted exefs directory (main): the engine built-in programs')
    ap.add_argument('--workers', type=int, default=12)
    ap.add_argument('--rescan', action='store_true')
    a = ap.parse_args()
    a.out.mkdir(parents=True, exist_ok=True)
    t0 = time.monotonic()

    scan_path = a.out / 'scan.json'
    scan = json.loads(scan_path.read_text()) if scan_path.exists() and not a.rescan else {}
    scan = {p: v for p, v in scan.items() if Path(p).is_file()}   # entries of another run directory or game dump
    # Bundles, data.unity3d, and extensionless serialized files ("Resources/unity default resources" holds the
    # engine's own error/clear/GUI shaders). Media and data files are never Unity containers.
    files = sorted(p for p in a.data.rglob('*') if p.is_file() and p.suffix in ('.bundle', '.unity3d', '.assets', '')
                   and str(p) not in scan)
    if files:
        print(f'scanning {len(files)} files for Shader objects', flush=True)
        with concurrent.futures.ProcessPoolExecutor(a.workers) as pool:
            scan.update(pool.map(scan_file, files, chunksize=64))
        scan_path.write_text(json.dumps(scan, indent=0))
    holders = sorted(p for p, v in scan.items() if v is True)
    errors = {p: v for p, v in scan.items() if isinstance(v, str)}
    print(f'{len(holders)} files hold shaders, {len(errors)} unreadable; {time.monotonic() - t0:.0f} s', flush=True)
    for p, v in list(errors.items())[:10]:
        print('  unreadable', p, v)

    # bundles first: their type trees serve data.unity3d, which has none
    holders.sort(key=lambda p: (not p.endswith('.bundle'), p))
    stages, sources = {}, {}
    for p in holders:
        try:
            found = extract_file(p)
        except Exception as e:
            print('  extract failed', p, type(e).__name__, e)
            continue
        for h, stage, name, ctl, code in found:
            key = '%016x' % h
            stages.setdefault(key, (stage, ctl, code))
            sources.setdefault(key, (name, Path(p).name))
    if a.exefs:   # the engine's utility programs (blits, movie YUV, one compute) sit in main's rodata, same layout
        for h, stage, ctl, code in ((fnv(c + d), s, c, d) for s, c, d in stages_in(nso_rodata(a.exefs / 'main'))):
            key = '%016x' % h
            stages.setdefault(key, (stage, ctl, code))
            sources.setdefault(key, ('(engine)', 'exefs/main'))
    by = {}
    for stage, _, _ in stages.values():
        by[stage] = by.get(stage, 0) + 1
    print(f'{len(stages)} unique stages {by}; {time.monotonic() - t0:.0f} s', flush=True)
    (a.out / 'stages.tsv').write_text(''.join(f'{k}\t{STAGE.get(stages[k][0], stages[k][0])}\t{sources[k][0]}\t{sources[k][1]}\n'
                                              for k in sorted(stages)), encoding='utf-8')

    if a.cap:
        cap = {p.stem for p in a.cap.glob('*.bin')}
        miss = sorted(cap - set(stages))
        print(f'captured on device: {len(cap)}; found by the scan: {len(cap) - len(miss)}; not found: {len(miss)}')
        for k in miss:   # shaders outside Unity's Shader objects (engine built-ins): translate them from the capture
            b = (a.cap / (k + '.bin')).read_bytes()
            ctl = b[8:8 + CTL]
            length = struct.unpack_from('<I', b, 8 + CTL)[0]
            stages[k] = (struct.unpack_from('<I', ctl, 0x714)[0], ctl, b[12 + CTL:12 + CTL + length])

    def run(todo):
        n = max(1, min(a.workers, len(todo)))
        ok, bad = 0, {}
        with concurrent.futures.ThreadPoolExecutor(n) as pool:
            for o, b in pool.map(lambda sl: worker(a.dll, sl, a.out), [todo[i::n] for i in range(n)]):
                ok += o
                bad.update(b)
        return ok, bad

    todo = [(k, s, c, d, {}) for k, (s, c, d) in sorted(stages.items()) if s in STAGE and not (a.out / (k + '.spv')).exists()]
    skipped = sum(1 for s, _, _ in stages.values() if s not in STAGE)
    print(f'translating {len(todo)} stages ({len(stages) - len(todo) - skipped} already done, {skipped} other-stage skipped)', flush=True)
    ok, bad = run(todo)

    # The few stages whose translation asked for a texture's type (tex_queried in the .res.json) get one variant per
    # type of the queried slots, named "<hash>_t" + two hex digits of slot and one of type per slot; the renderer
    # builds the same name from the bound textures (shader_load in vkr.c).
    todo = []
    for k, (s, c, d) in sorted(stages.items()):
        res = a.out / (k + '.res.json')
        if s not in (0, 1) or not res.exists():
            continue
        slots = json.loads(res.read_text(encoding='utf-8'))['tex_queried'][:7]
        if not slots:
            continue
        combos = itertools.product(range(1, len(TEX)), repeat=len(slots)) if len(slots) <= 2 else \
            ((t,) * len(slots) for t in range(1, len(TEX)))
        for types in combos:
            vk = k + '_t' + ''.join('%02x%x' % st for st in zip(slots, types))
            if not (a.out / (vk + '.spv')).exists():
                todo.append((vk, s, c, d, {str(sl): TEX[t] for sl, t in zip(slots, types)}))
    if todo:
        print(f'translating {len(todo)} texture-type variants', flush=True)
        o, b = run(todo)
        ok += o
        bad.update(b)
    for k, msg in sorted(bad.items())[:20]:
        print('  failed', k, msg)
    (a.out / 'failures.txt').write_text(''.join(f'{k}\t{m}\n' for k, m in sorted(bad.items())), encoding='utf-8')
    print(f'translated {ok}, failed {len(bad)}; total {time.monotonic() - t0:.0f} s')


if __name__ == '__main__':
    main()
