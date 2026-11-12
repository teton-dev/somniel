#!/usr/bin/env python3
"""Disassemble a range of exefs/main with IL2CPP method names on call targets and literal loads.

usage: il2cpp_disasm.py MAIN_NSO SCRIPT_JSON START END
  SCRIPT_JSON  Il2CppDumper's script.json (ScriptMethod: Address -> Name), e.g. work\\il2cpp_dump\\script.json
  START/END    hex module offsets (the dump's RVA)
Lines with "bl"/"b" to a known method get "; <name>" appended."""
import json
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


def main():
    nso, script, start, end = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
    names = {}
    meta = json.loads(Path(script).read_text(encoding='utf-8'))
    for m in meta.get('ScriptMethod', []):
        names.setdefault(m['Address'], m['Name'])
    out = subprocess.run([sys.executable, str(HERE / 'disasm.py'), nso, start, end], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.search(r'\b(bl|b)\s+0x([0-9a-f]+)', line)
        if m:
            n = names.get(int(m.group(2), 16))
            if n:
                line = re.sub(r'<[^>]*>', '', line) + '   ; ' + n
        print(line)


if __name__ == '__main__':
    main()
