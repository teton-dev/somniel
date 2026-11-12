#!/usr/bin/env python3
"""Translate shaders captured on the device (files/shaders/*.bin + *.ans) to SPIR-V with the repo's totk_xlate.

usage: translate_captured.py CAPTURE_DIR OUT_DIR XLATE_DLL
  CAPTURE_DIR   directory with <hash>.bin (u32 idx, u32 n, control 0x880, u32 len, code) and
                <hash>_<ahash>.ans (u32 nvn stage, tex[32], img[32], img_fmt[32], attr[32], fout[8], shared u32)
  OUT_DIR       receives <hash>_<ahash>.spv and .res.json
Host answers are those of the Adreno 740: no shaderFloat64, 64-byte storage buffer alignment, subgroup size 64.
"""
import json
import struct
import subprocess
import sys
from pathlib import Path

TEX = [None, "1d", "1darray", "buffer", "2d", "2darray", "2dms", "2dmsarray", "3d", "cube", "cubearray"]
ATTR = ["float", "sint", "uint"]
STAGE = {0: "vs", 1: "ps", 5: "cs"}
CODE_SKIP = 0x30


def load_bin(path):
    b = path.read_bytes()
    ctl = b[8:8 + 0x880]
    (length,) = struct.unpack_from("<I", b, 8 + 0x880)
    code = b[8 + 0x880 + 4:8 + 0x880 + 4 + length]
    c1_off, ln = struct.unpack_from("<II", ctl, 0x700)
    assert ln == length, (ln, length)
    return ctl, code, c1_off


def job_for(key, ans, ctl, code, c1_off, code_path):
    stage = struct.unpack_from("<I", ans, 0)[0]
    tex, img, fmt, attr, fout = (ans[4 + 32 * i:4 + 32 * (i + 1)] for i in range(5))
    fout = ans[4 + 128:4 + 128 + 8]
    (shared,) = struct.unpack_from("<I", ans, 4 + 128 + 8)
    name = STAGE[stage]
    skip = 0 if name == "cs" else CODE_SKIP
    code_path.write_bytes(code[skip:])
    job = {
        "key": key, "stage": name, "debug": False, "code": str(code_path), "c1": code[c1_off:].hex(),
        "tex": {str(i): TEX[t] for i, t in enumerate(tex) if 0 < t < len(TEX)},
        "img": {str(i): {"type": TEX[t], "fmt": "R8G8B8A8Unorm"} for i, t in enumerate(img) if 0 < t < len(TEX)},
        "attr": {str(i): ATTR[t] for i, t in enumerate(attr) if t},
        "fout": {str(i): ATTR[t] for i, t in enumerate(fout) if t},
        "shared": shared,
        "softfloat64": True, "sbuf_align": 64, "subgroup": 64,
    }
    if name == "ps" and ctl[0x718] & 1:
        job["early_fragment_tests"] = True
    if name == "cs":
        job["local"] = list(struct.unpack_from("<3I", ctl, 0x748))
    return job


def main():
    cap, out, dll = Path(sys.argv[1]), Path(sys.argv[2]), sys.argv[3]
    out.mkdir(parents=True, exist_ok=True)
    proc = subprocess.Popen(["dotnet", dll, "--serve"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.DEVNULL, text=True)
    ok = bad = 0
    for ans_path in sorted(cap.glob("*.ans")):
        key = ans_path.stem
        if (out / (key + ".spv")).exists():
            continue
        bin_path = cap / (key.split("_")[0] + ".bin")
        if not bin_path.exists():
            print("no .bin for", key)
            continue
        ctl, code, c1_off = load_bin(bin_path)
        job = job_for(key, ans_path.read_bytes(), ctl, code, c1_off, out / (key + ".code"))
        proc.stdin.write(json.dumps({"job": job, "out": str(out)}) + "\n")
        proc.stdin.flush()
        reply = proc.stdout.readline().strip()
        (out / (key + ".code")).unlink(missing_ok=True)
        if reply.startswith("ok"):
            ok += 1
        else:
            bad += 1
            print(key, reply[:200])
    proc.stdin.close()
    proc.wait(timeout=30)
    print(f"translated {ok}, failed {bad}")


if __name__ == "__main__":
    main()
