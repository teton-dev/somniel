# Third-party notices

Somniel is released under the GNU General Public License v3.0 (`LICENSE`). The third-party code below keeps its own
licence; all of these licences are compatible with GPL-3.0.

## Dual-screen companion (Eden Duo)

- `app/src/main/assets/dsmod/`: the Fire Emblem Engage dual-screen companion package 1.0.0 by igawa6
  ([igawa6/eden-duo-companions](https://github.com/igawa6/eden-duo-companions), release `fire-emblem-engage-1.0.0`),
  bundled unmodified. GPL-3.0. The corresponding source of its native module is in `third_party/eden_duo_companions`,
  with build notes in its `README.md`.
- `third_party/eden_duo`: the Eden Duo module SDK headers ([igawa6/eden-duo](https://github.com/igawa6/eden-duo),
  GPL-3.0-or-later), used by Somniel's dual-screen host and by the module, and Skyline's `bc_decoder` (MPL-2.0),
  used by the module.
- `third_party/lz4`: LZ4 v1.11.0 (BSD-2-Clause), used by the module.
- `third_party/eden_duo_companions/native/modules/third_party/stb_truetype.h`: Sean Barrett (MIT or public domain).

Somniel and the companion are not affiliated with or endorsed by the Eden project.

## libxlate.so (shader translator)

`app/src/main/jniLibs/arm64-v8a/libxlate.so` is a prebuilt NativeAOT build (linux-bionic-arm64) of the shader
translator derived from Ryujinx's shader compiler, with Somniel's patches. Somniel needs it to translate the game's
shaders on the device. Ryujinx is released under the MIT licence:

MIT License

Copyright (c) Ryujinx Team and Contributors

Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


## Other bundled code

- `third_party/SDL` (submodule, zlib licence), `third_party/volk` (MIT), `third_party/libadrenotools` (BSD-style) and
  `third_party/bcdec` (MIT, BC texture decoding for GPUs without BC support) keep their own licence files in their
  folders.
- `third_party/astc_decode` (MIT, ASTC LDR decoding for the dual-screen companion's images), from LAS Recomp via
  engage-pc-recomp; notices and version in its folder.
- The NVN front end follows notes from the engage-pc-recomp project (MIT), credited in the source headers.
- Cobalt (Raytwo) is not part of Somniel: users download it separately.
