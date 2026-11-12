#!/usr/bin/env python3
"""Disassemble a range of a Switch NSO module with the LLVM tools.
Usage: disasm.py NSO START END [--demangle]   (START/END are hex module offsets)
Needs llvm-objcopy and llvm-objdump (C:\\Program Files\\LLVM\\bin or PATH)."""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import nso_syms  # noqa: E402

LLVM = Path(r'C:\Program Files\LLVM\bin')


def tool(name):
    p = LLVM / f'{name}.exe'
    return str(p) if p.exists() else name


def main():
    nso, start, end = Path(sys.argv[1]), int(sys.argv[2], 16), int(sys.argv[3], 16)
    img = nso_syms.load_image(nso)
    with tempfile.TemporaryDirectory() as td:
        raw, elf = os.path.join(td, 'code.bin'), os.path.join(td, 'code.o')
        Path(raw).write_bytes(bytes(img[start:end]))
        subprocess.run([tool('llvm-objcopy'), '-I', 'binary', '-O', 'elf64-littleaarch64',
                        '--rename-section', '.data=.text,alloc,load,readonly,code',
                        raw, elf], check=True)
        out = subprocess.run([tool('llvm-objdump'), '-d', '--no-show-raw-insn', '--triple=aarch64', f'--adjust-vma={start:#x}', elf],
                             capture_output=True, text=True, check=True).stdout
    print(out)


if __name__ == '__main__':
    main()


