#!/usr/bin/env python3
"""Find the symbol containing an offset. Usage: symbolize.py SYMDIR module+0xOFFSET [...]
Reads SYMDIR/<module>.syms.tsv written by nso_syms.py. Demangles nothing; the
mangled name is the key the hook table uses."""
import bisect
import re
import sys
from pathlib import Path


def load(symdir, module):
    rows = []
    for line in (Path(symdir) / f'{module}.syms.tsv').read_text(encoding='utf-8').splitlines():
        a, s, k, n = line.split('\t')
        if k == 'FUNC':
            rows.append((int(a, 16), int(s, 16), n))
    rows.sort()
    return rows, [r[0] for r in rows]


def main():
    symdir = sys.argv[1]
    cache = {}
    for arg in sys.argv[2:]:
        m = re.match(r'(\w+)\+(?:0x)?([0-9a-fA-F]+)$', arg)
        mod, off = m.group(1), int(m.group(2), 16)
        if mod not in cache:
            cache[mod] = load(symdir, mod)
        rows, keys = cache[mod]
        i = bisect.bisect_right(keys, off) - 1
        if i >= 0:
            a, s, n = rows[i]
            print(f'{arg:20} {n}  (+0x{off - a:x} of 0x{s:x})')
        else:
            print(f'{arg:20} ?')


if __name__ == '__main__':
    main()
