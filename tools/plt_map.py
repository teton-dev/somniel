#!/usr/bin/env python3
"""Map the PLT stubs of a Switch NSO module to the symbols they jump to.

usage: plt_map.py NSO [ADDR ...]   (hex module offsets; with none, prints every stub)
Each stub is `adrp x16, page; ldr x17, [x16, #off]; add x16, x16, #off; br x17`; its GOT slot is matched to the
module's DT_JMPREL (R_AARCH64_JUMP_SLOT) relocations to name the target.
"""
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import nso_syms  # noqa: E402

DT_JMPREL, DT_PLTRELSZ, DT_STRTAB, DT_SYMTAB = 23, 2, 5, 6


def main():
    img = nso_syms.load_image(Path(sys.argv[1]))
    u32 = lambda o: struct.unpack_from('<I', img, o)[0]
    u64 = lambda o: struct.unpack_from('<Q', img, o)[0]
    mod0 = u32(4)
    o = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    dyn = {}
    while u64(o):
        dyn.setdefault(u64(o), u64(o + 8))
        o += 16
    slot_name = {}
    for r in range(dyn[DT_JMPREL], dyn[DT_JMPREL] + dyn[DT_PLTRELSZ], 24):
        off, info = u64(r), u64(r + 8)
        sym = dyn[DT_SYMTAB] + (info >> 32) * 24
        slot_name[off] = nso_syms.cstr(img, dyn[DT_STRTAB] + u32(sym))
    stubs = {}
    for a in range(0, len(img) - 16, 4):
        i0, i1, i3 = u32(a), u32(a + 4), u32(a + 12)
        if (i0 & 0x9F00001F) != 0x90000010 or i3 != 0xD61F0220 or (i1 & 0xFFC003FF) != 0xF9400211:
            continue
        imm = ((i0 >> 29) & 3) | (((i0 >> 5) & 0x7FFFF) << 2)
        if imm & (1 << 20):
            imm -= 1 << 21
        slot = (a & ~0xFFF) + (imm << 12) + ((i1 >> 10) & 0xFFF) * 8
        if slot in slot_name:
            stubs[a] = slot_name[slot]
    want = [int(x, 16) for x in sys.argv[2:]]
    for a in sorted(want or stubs):
        print(f'{a:x}\t{stubs.get(a, "?")}')


if __name__ == '__main__':
    main()
