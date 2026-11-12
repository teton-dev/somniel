#!/usr/bin/env python3
"""Callers of IL2CPP methods in exefs/main: every BL/B whose target is a method matching a pattern.

usage: il2cpp_xref.py MAIN_NSO SCRIPT_JSON PATTERN [PATTERN...]
  PATTERN  a substring of the method name in script.json (e.g. "MapBattleInfoWindow$$")
Prints "target name <- caller method + offset" for each call site."""
import bisect
import json
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nso_syms  # noqa: E402


def main():
    nso, script, pats = sys.argv[1], sys.argv[2], sys.argv[3:]
    meta = json.loads(Path(script).read_text(encoding='utf-8'))
    methods = sorted({(m['Address'], m['Name']) for m in meta.get('ScriptMethod', [])})
    starts = [a for a, _ in methods]
    targets = {a: n for a, n in methods if any(p in n for p in pats)}
    img = bytes(nso_syms.load_image(Path(nso)))
    text_end = struct.unpack_from('<I', Path(nso).read_bytes(), 0x18)[0]   # text segment size (decompressed)
    words = struct.unpack_from('<%dI' % (text_end // 4), img, 0)
    for i, w in enumerate(words):
        op = w >> 26
        if op not in (0x25, 0x05):   # BL, B
            continue
        imm = w & 0x3FFFFFF
        if imm & 0x2000000:
            imm -= 0x4000000
        t = i * 4 + imm * 4
        if t in targets:
            pc = i * 4
            k = bisect.bisect_right(starts, pc) - 1
            caller = methods[k][1] if k >= 0 else '?'
            off = pc - starts[k] if k >= 0 else pc
            print(f'{targets[t]}  <- {caller}+0x{off:x}  ({"bl" if op == 0x25 else "b"} at 0x{pc:x})')


if __name__ == '__main__':
    main()
