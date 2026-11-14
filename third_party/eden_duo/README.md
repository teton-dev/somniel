# Eden Duo SDK files

From [igawa6/eden-duo](https://github.com/igawa6/eden-duo) at commit
`102f69ed3f2d11fc142145cd8daa0a63633bbd7b`, unmodified, in their original paths.

| File | Licence | Used by |
| --- | --- | --- |
| `src/core/mods/dsmod_module_abi.h` | GPL-3.0-or-later (`LICENSE.txt`) | Somniel's dual-screen host (`app/src/main/cpp/dsmod.cpp`) and the companion module |
| `src/core/mods/dsmod_module_extensions.h` | GPL-3.0-or-later (`LICENSE.txt`) | same |
| `src/core/mods/modules/dsmod_module_sdk.h` | GPL-3.0-or-later (`LICENSE.txt`) | the companion module (`third_party/eden_duo_companions`) |
| `externals/bc_decoder/bc_decoder.{h,cpp}` | MPL-2.0 (`externals/bc_decoder/MPL-2.0.txt`), Skyline Team / SwiftShader | the companion module |

The layout matches an Eden Duo checkout, so this folder can be passed as `EDEN_SOURCE_ROOT` when building the companion
module (see `third_party/eden_duo_companions/README.md`).
