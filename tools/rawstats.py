#!/usr/bin/env python3
"""Value statistics of an RGBA16F render-target dump (see vkr.c frame capture): per channel min / max / mean and
counts of NaN, infinite, negative and zero values.  usage: rawstats.py FILE.raw"""
import math
import struct
import sys

d = open(sys.argv[1], 'rb').read()
n = len(d) // 8
vals = struct.unpack(f'<{n * 4}e', d[:n * 8])
for c, name in enumerate('RGBA'):
    ch = vals[c::4]
    nan = sum(1 for v in ch if v != v)
    inf = sum(1 for v in ch if v == v and math.isinf(v))
    fin = [v for v in ch if v == v and not math.isinf(v)]
    neg = sum(1 for v in fin if v < 0)
    zero = sum(1 for v in fin if v == 0)
    print(f'{name}: min {min(fin) if fin else 0:.4g} max {max(fin) if fin else 0:.4g} mean {sum(fin) / max(1, len(fin)):.4g}'
          f'  nan {nan} inf {inf} negative {neg} zero {zero} of {n}')
