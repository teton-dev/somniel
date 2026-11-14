# Fire Emblem Engage dual-screen companion (source)

Somniel bundles the **Fire Emblem Engage** companion package 1.0.0 by igawa6 for Eden Duo, unmodified, in
`app/src/main/assets/dsmod/`. It is the release `fire-emblem-engage-1.0.0` of
[igawa6/eden-duo-companions](https://github.com/igawa6/eden-duo-companions)
(`0100A6301214E000-FireEmblemEngageDS-1.0.0.dsmod.zip`, SHA-256
`7741a0215c0f469361def9a31878a2588deed164b0a923b215cb21c1459a9751`), minus the `linux-x86_64` module Android does not
use. Licence: GPL-3.0 (`LICENSE`).

This folder is the corresponding source of the package's native module
(`dualscreen/modules/android-arm64-v8a/0100A6301214E000.so`), taken from that repository at commit
`54252c710e3f652fe2082123baa52735753ce511` (the commit that added the 1.0.0 package), unmodified, in its original
paths:

- `native/modules/0100A6301214E000.cpp` and the `fee_*`, `lp_unity*` and `lp_raster.h` files it is built from
- `native/modules/third_party/stb_truetype.h` (public domain / MIT, by Sean Barrett)
- `native/modules/CMakeLists.txt` and `lp.cmake`: the upstream build scripts (target `dsmod-fee`)
- `native/README.md`: upstream build instructions

The package's other files (`manifest.json`, `fee_font.txt`, `guest/<build id>/load-plan.json`, `package.json`) are
plain data and ship as they are in `app/src/main/assets/dsmod/`.

## Building the module

The upstream scripts build every title's module and expect a full Eden Duo checkout. The Engage module alone needs
only what is in this repository:

- these sources
- `third_party/eden_duo` (the Eden Duo SDK headers and `bc_decoder`; usable as `EDEN_SOURCE_ROOT`)
- `third_party/lz4` (LZ4 v1.11.0, BSD-2-Clause, the version Eden Duo pins)

With the Android NDK, compile `0100A6301214E000.cpp`, `fee_reader.cpp`, `fee_map.cpp`, `fee_text.cpp`,
`fee_assets.cpp`, `fee_font.cpp`, `fee_sprites.cpp`, `fee_hud.cpp`, `fee_hub.cpp`, `lp_unity.cpp`,
`../eden_duo/externals/bc_decoder/bc_decoder.cpp` and `../lz4/lz4.c` into one shared library (C++20, `c++_static`,
hidden visibility) with the include directories `native/modules`, `../eden_duo/src`, `../eden_duo/externals/bc_decoder`
and `../lz4`. It exports `eden_dsmod_get_module`, `eden_dsmod_get_extensions` and `eden_dsmod_get_font_extensions`.
