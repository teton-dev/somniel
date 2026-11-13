# Third-party notices

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
- The NVN front end follows notes from the engage-pc-recomp project (MIT), credited in the source headers.
- Cobalt (Raytwo) is not part of Somniel: users download it separately.
