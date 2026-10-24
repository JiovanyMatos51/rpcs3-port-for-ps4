# Third-party notices

RPCS3 PORT FOR PS4 is licensed under the GNU General Public License, version 2 ([`LICENSE`](LICENSE)).
It is built from, and its PKG contains, the components below. Each component keeps its own license;
this file is a guide to them, not a license.

Paths are relative to the folder of the source release (the folder that holds `rpcs3-ps4/`,
`orbis-compat/` and `licenses/`). The PKG carries this file and `LICENSE` at the root of the
application folder (`/app0/`) and the license texts of the components in its own `licenses/` folder;
the section [License texts in the PKG](#license-texts-in-the-pkg) lists them.

"In this release" means the component's source is part of the source release. Components marked
"downloaded" are fetched by the build scripts at the pinned version; their license texts are copied
in [`licenses/`](licenses).

## Linked into the console executable or shipped in the PKG

| Component | Version / pin | Source | License | License text |
| --- | --- | --- | --- | --- |
| [RPCS3](https://github.com/RPCS3/rpcs3): emulation core, renderers, recompilers, interface icons (`bin/Icons/ui`) | upstream `9d52b338058bda2d5a9d7cd5e4fbc8148e55a385`, with the port's changes | in this release: `rpcs3-ps4/` | GPL-2.0 | `LICENSE`, `rpcs3-ps4/LICENSE` |
| RPCS3 PORT FOR PS4 (Roberix Studio & Mifi Studios) | 0.40 | in this release: `rpcs3-ps4/ps4-port/`, `rpcs3-ps4/rpcs3/**/ps4_*`, the port's changes in RPCS3 and orbis-compat | GPL-2.0 | `LICENSE` |
| Application icon (`ps4-port/assets/icon0.png`, installed as `sce_sys/icon0.png`) | 0.40 | in this release: `rpcs3-ps4/ps4-port/assets/icon0.png` | the studio logo of the port's authors (Roberix Studio & Mifi Studios), supplied by them; it replaces the RPCS3 logo, which earlier versions used as the application icon. Same license as the rest of `rpcs3-ps4/ps4-port/` (row above) | `LICENSE` |
| [LLVM](https://llvm.org) (PPU/SPU recompilers, in-process JIT) | llvm-project `ca7933e47d3a3451d81e72ac174dcb5aa28b59d1` (RPCS3's submodule pin) | downloaded by `ps4-port/fetch-llvm.py` | Apache-2.0 WITH LLVM-exception | `licenses/LLVM-LICENSE.TXT` (also in the PKG as `licenses/LLVM.txt`) |
| [Mesa](https://mesa3d.org): RADV (Vulkan), EGL, Gallium/Zink (OpenGL) | 26.3.0-devel `4c934ebf9cda976704e9b32cb24d2c41e6ab25f5`, bundle `orbis-mesa-4c934ebf9cda` of [orbis-ports/mesa-ps4](https://github.com/orbis-ports/mesa-ps4) | downloaded (prebuilt bundle, see `BUILDING.md`) | MIT | `licenses/Mesa-MIT.txt` |
| Khronos headers (Vulkan, EGL, GLES, KHR) from the Mesa bundle | as shipped in the bundle | downloaded | Apache-2.0 (Vulkan headers: Apache-2.0 OR MIT) | `licenses/Khronos-Apache-2.0.txt` |
| [zlib](https://zlib.net) | `libz.a` 1.3.1 from the Mesa bundle; headers 1.3.2 (`da607da739fa`) | in this release: `rpcs3-ps4/3rdparty/zlib/zlib` (headers) | Zlib | `rpcs3-ps4/3rdparty/zlib/zlib/LICENSE`, `licenses/zlib-Zlib.txt` |
| [FFmpeg](https://ffmpeg.org) (audio/video decoding) | n8.1.1 `239f2c733de417201d7ad3b3b8b0d9b63285b2b1`, configured without `--enable-gpl` and `--enable-nonfree` | downloaded by `ps4-port/build-ffmpeg.ps1` | LGPL-2.1-or-later | `licenses/FFmpeg-LICENSE.md`, `licenses/FFmpeg-COPYING.LGPLv2.1` |
| FFmpeg headers of RPCS3's `ffmpeg-core` (the console build uses only `include/`; `lib/windows` is not used) | `649d9d9353184a17ae78d9df4a6fbbe7456f0428` | in this release: `rpcs3-ps4/3rdparty/ffmpeg` | LGPL-2.1 | `rpcs3-ps4/3rdparty/ffmpeg/copyright` |
| [wolfSSL](https://www.wolfssl.com) (TLS for RPCN and curl) | 5.9.4-stable `3c5eead44904` | in this release: `rpcs3-ps4/3rdparty/wolfssl/wolfssl` | GPL-3.0, with wolfSSL's exception that lets RPCS3 use it under GPL-2.0 | `rpcs3-ps4/3rdparty/wolfssl/wolfssl/COPYING`, `rpcs3-ps4/3rdparty/wolfssl/wolfssl/LICENSING` |
| [curl](https://curl.se) (HTTP) | 8.22.0 `01346829096c` | in this release: `rpcs3-ps4/3rdparty/curl/curl` | curl (MIT-style) | `rpcs3-ps4/3rdparty/curl/curl/COPYING` |
| Mozilla root certificates (`ps4-port/assets/certs/cacert.pem`, installed as `/app0/certs/cacert.pem`) | data of 2024-07-02, 148 roots; identical to wolfSSL's `wolfcrypt/src/port/Espressif/esp_crt_bundle/cacrt_all.pem` | in this release | MPL-2.0 (https://mozilla.org/MPL/2.0/) | header of the file |
| RPCS3 per-game configuration database (`config_database.dat`, installed as `/app0/config_database.dat` when the PKG is built with `package-native.ps1 -ConfigDatabase`) | the snapshot given to `-ConfigDatabase`, unchanged; its `timestamp` is the `config_database` field of the PKG's `build-manifest.json` (`none` when the PKG does not carry the file, `bundled, no timestamp` when the file has no `timestamp`) | not in this release: data the RPCS3 project publishes on its server (`https://api.rpcs3.net/config/?api=v1`), which the PC RPCS3 saves as `GuiConfigs/config_database.dat`; bundled by the port's authors | none stated by its publisher. It is RPCS3 project data: the GPL-2.0 of this port does not cover it, and this port grants no license for it | none published |
| [miniupnpc](https://miniupnp.tuxfamily.org) (UPnP) | 2.3.2 (miniupnp `d66872e34d9f`) | in this release: `rpcs3-ps4/3rdparty/miniupnp/miniupnp` | BSD-3-Clause | `rpcs3-ps4/3rdparty/miniupnp/miniupnp/LICENSE` |
| [Protocol Buffers](https://protobuf.dev) (RPCN protocol) | 33.4 `edaa823d8b36` | in this release: `rpcs3-ps4/3rdparty/protobuf/protobuf` | BSD-3-Clause | `rpcs3-ps4/3rdparty/protobuf/protobuf/LICENSE` |
| [Abseil](https://abseil.io) (Protobuf dependency) | 20250512.1 `76bb24329e8bf5f39704eb10d21b9a80befa7c81` | downloaded by `ps4-port/prepare-abseil.ps1` | Apache-2.0 | `licenses/Abseil-LICENSE.txt` |
| [Zstandard](https://github.com/facebook/zstd) | 1.5.7 `f8745da6ff1a` | in this release: `rpcs3-ps4/3rdparty/zstd/zstd` | BSD-3-Clause OR GPL-2.0 | `rpcs3-ps4/3rdparty/zstd/zstd/LICENSE`, `COPYING` |
| [libpng](http://www.libpng.org) | 1.6.59 `cd952f49f95b` | in this release: `rpcs3-ps4/3rdparty/libpng/libpng` | libpng-2.0 | `rpcs3-ps4/3rdparty/libpng/libpng/LICENSE` |
| [AsmJit](https://asmjit.com) (SPU ASMJIT) | `416f7356967c` | in this release: `rpcs3-ps4/3rdparty/asmjit/asmjit` | Zlib | `rpcs3-ps4/3rdparty/asmjit/asmjit/LICENSE.md` |
| [yaml-cpp](https://github.com/jbeder/yaml-cpp) (RPCS3 fork) | `51a5d623e3fd` | in this release: `rpcs3-ps4/3rdparty/yaml-cpp/yaml-cpp` | MIT | `rpcs3-ps4/3rdparty/yaml-cpp/yaml-cpp/LICENSE` |
| [pugixml](https://pugixml.org) | `c8033ce9d039` | in this release: `rpcs3-ps4/3rdparty/pugixml` | MIT | `rpcs3-ps4/3rdparty/pugixml/LICENSE.md` |
| [SoundTouch](https://www.surina.net/soundtouch) (RPCS3 fork) | 2.4.1 `a0fba77b6f9c` | in this release: `rpcs3-ps4/3rdparty/SoundTouch/soundtouch` | LGPL-2.1 | `rpcs3-ps4/3rdparty/SoundTouch/soundtouch/COPYING.TXT` |
| [Fusion](https://github.com/xioTechnologies/Fusion) (motion sensor fusion) | 1.3.3 `932542401189` | in this release: `rpcs3-ps4/3rdparty/fusion/fusion` | MIT | `rpcs3-ps4/3rdparty/fusion/fusion/LICENSE.md` |
| [glslang](https://github.com/KhronosGroup/glslang) (SPIR-V) | 16.2.0 `f0bd0257c308` | in this release: `rpcs3-ps4/3rdparty/glslang/glslang` | BSD-3-Clause and others | `rpcs3-ps4/3rdparty/glslang/glslang/LICENSE.txt` |
| [Vulkan Memory Allocator](https://gpuopen.com/vulkan-memory-allocator) | 3.4.0 `3aa921224c15`, with the port's `RPCS3_PS4` addition (`vmaPs4FreeEmptyBlocks`) in `include/vk_mem_alloc.h` | in this release: `rpcs3-ps4/3rdparty/GPUOpen/VulkanMemoryAllocator` | MIT | `rpcs3-ps4/3rdparty/GPUOpen/VulkanMemoryAllocator/LICENSE.txt` |
| [stb](https://github.com/nothings/stb) (images, fonts) | `013ac3beddff` | in this release: `rpcs3-ps4/3rdparty/stblib/stb` | MIT OR Unlicense (public domain) | `rpcs3-ps4/3rdparty/stblib/stb/LICENSE` |
| [libusb](https://libusb.info) headers (the API is implemented over sceUsbd in `ps4-port/runtime/ps4_libusb.cpp`) | 1.0.30 `87a55632db62` | in this release: `rpcs3-ps4/3rdparty/libusb/libusb` | LGPL-2.1-or-later | `rpcs3-ps4/3rdparty/libusb/libusb/COPYING` |
| [RtMidi](https://github.com/thestk/rtmidi) C API header (extended with a PS4 USB entry in the build tree) | `1e5b49925aa6` | in this release: `rpcs3-ps4/3rdparty/rtmidi/rtmidi` | RtMidi license (MIT-style) | `rpcs3-ps4/3rdparty/rtmidi/rtmidi/LICENSE` |
| [OpenAL Soft](https://openal-soft.org) headers (AL/ALC API of the capture adapter, `ps4-port/runtime/ps4_capture.cpp`) | 1.25.2 `b2c48f7718ef` | in this release: `rpcs3-ps4/3rdparty/OpenAL/openal-soft` | LGPL-2.0-or-later | `rpcs3-ps4/3rdparty/OpenAL/openal-soft/COPYING` |
| bcdec (BC texture decoding) | as in RPCS3 | in this release: `rpcs3-ps4/3rdparty/bcdec` | MIT | header of `bcdec.hpp` |
| unordered_dense | as in RPCS3 | in this release: `rpcs3-ps4/3rdparty/unordered_dense` | MIT | header of `unordered_dense.h` |
| PINE IPC server header | as in RPCS3 | in this release: `rpcs3-ps4/3rdparty/pine` | GPL-2.0 (relicensed for RPCS3 by its author) | header of `pine_server.h` |
| Khronos OpenGL headers (`glext.h`, `khrplatform.h`) | as in RPCS3 | in this release: `rpcs3-ps4/3rdparty/GL` | Khronos (MIT-style) | file headers |
| [orbis-compat](https://github.com/orbis-ports/orbis-compat) (POSIX/libkernel compatibility runtime), by Mikołaj Mikołajczyk | `6922a7b6f4291a28edd239f1a4db50ed7aa00359`, with the port's changes | in this release: `orbis-compat/` (`src/`, `include/`) | MIT; `include/sys/ioccom.h` BSD-3-Clause (FreeBSD) | `orbis-compat/LICENSE`, `orbis-compat/licenses/` |
| [orbis-porting-kit](https://github.com/orbis-ports/orbis-porting-kit) (Vulkan loader, `vkloader/`), by Mikołaj Mikołajczyk | `213b0c38ff25ea7ca402c8844a6494086d0f1cae` | downloaded (see `BUILDING.md`) | MIT (its `cmake/orbis-tls.ld` is GPL-3.0-only and is not used by this build) | `licenses/orbis-porting-kit-LICENSE.txt` |
| [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain): SDK headers, `link.x`, `crt1.o`, packaging tools | v0.5.4 | downloaded | GPL-3.0 (tools, `link.x`, `include/orbis` headers); `PkgTool.Core`/LibOrbisPkg LGPL-3.0 | `licenses/OpenOrbis-PS4-Toolchain-LICENSE.txt` |
| OpenOrbis musl C library (`libc.a`) | as shipped in the toolchain | downloaded | MIT | `licenses/musl-MIT.txt` |
| LLVM libc++, libc++abi, libunwind, compiler-rt builtins of the toolchain | LLVM 11.0.0, as shipped in the toolchain | downloaded | Apache-2.0 WITH LLVM-exception | `licenses/LLVM-libcxx-LICENSE.TXT`, `licenses/LLVM-libcxxabi-LICENSE.TXT`, `licenses/LLVM-libunwind-LICENSE.TXT`, `licenses/LLVM-compiler-rt-LICENSE.TXT` |
| [DejaVu fonts](https://dejavu-fonts.github.io) (DejaVu Sans, DejaVu Sans Mono; launcher text) | 2.37 | in this release: `rpcs3-ps4/bin/fonts` | Bitstream Vera and Arev font licenses; DejaVu changes in the public domain | `licenses/DejaVu-Fonts-LICENSE.txt` |

`package-native.ps1` also copies `sce_sys/about/right.sprx`, `sce_module/libc.prx` and
`sce_module/libSceFios2.prx` from the OpenOrbis toolchain's `samples/hello_world` into the PKG. They
come with the toolchain and are not part of this source release.

The host tools used only to build (llvm-mingw, CMake, Ninja, Python, Git for Windows and its MSYS
runtime, GNU Make from MSYS2) are not linked into the console executable or shipped in the PKG.

## License texts in the PKG

`package-native.ps1` copies these texts into the PKG from the files the build used (`3rdparty/`,
`ps4-port/deps/`, the orbis-compat and orbis-porting-kit checkouts, the OpenOrbis toolchain and the
font files). It stops when one of them is missing or when this list and the PKG's `licenses/` folder
differ. The third column names where the same license text is in the source release.

| File in the PKG | Component | Same text in the source release |
| --- | --- | --- |
| `LICENSE` | RPCS3, RPCS3 PORT FOR PS4, PINE IPC server header | `LICENSE`, `rpcs3-ps4/LICENSE` |
| `THIRD_PARTY_NOTICES.md` | this file | `THIRD_PARTY_NOTICES.md` |
| `certs/cacert.pem` | Mozilla root certificates (license in the file header) | `rpcs3-ps4/ps4-port/assets/certs/cacert.pem` |
| `licenses/LLVM.txt` | LLVM | `licenses/LLVM-LICENSE.TXT` |
| `licenses/LLVM-libcxx-LICENSE.TXT` | LLVM libc++ of the toolchain | `licenses/LLVM-libcxx-LICENSE.TXT` |
| `licenses/LLVM-libcxxabi-LICENSE.TXT` | LLVM libc++abi of the toolchain | `licenses/LLVM-libcxxabi-LICENSE.TXT` |
| `licenses/LLVM-libunwind-LICENSE.TXT` | LLVM libunwind of the toolchain | `licenses/LLVM-libunwind-LICENSE.TXT` |
| `licenses/LLVM-compiler-rt-LICENSE.TXT` | LLVM compiler-rt builtins of the toolchain | `licenses/LLVM-compiler-rt-LICENSE.TXT` |
| `licenses/musl-MIT.txt` | OpenOrbis musl C library | `licenses/musl-MIT.txt` |
| `licenses/OpenOrbis-PS4-Toolchain-LICENSE.txt` | OpenOrbis PS4 Toolchain | `licenses/OpenOrbis-PS4-Toolchain-LICENSE.txt` |
| `licenses/Mesa-MIT.txt` | Mesa | `licenses/Mesa-MIT.txt` |
| `licenses/Khronos-Apache-2.0.txt` | Khronos headers of the Mesa bundle | `licenses/Khronos-Apache-2.0.txt` |
| `licenses/Khronos-GL-headers.txt` | Khronos OpenGL headers | the notices at the top of `rpcs3-ps4/3rdparty/GL/glext.h` and `rpcs3-ps4/3rdparty/GL/KHR/khrplatform.h` |
| `licenses/zlib-Zlib.txt` | zlib (`libz.a` of the Mesa bundle) | `licenses/zlib-Zlib.txt` |
| `licenses/zlib-LICENSE.txt` | zlib headers | `rpcs3-ps4/3rdparty/zlib/zlib/LICENSE` |
| `licenses/FFmpeg-LICENSE.md` | FFmpeg | `licenses/FFmpeg-LICENSE.md` |
| `licenses/FFmpeg-COPYING.LGPLv2.1` | FFmpeg | `licenses/FFmpeg-COPYING.LGPLv2.1` |
| `licenses/FFmpeg-core-copyright.txt` | FFmpeg headers of `ffmpeg-core` | `rpcs3-ps4/3rdparty/ffmpeg/copyright` |
| `licenses/wolfSSL-COPYING.txt` | wolfSSL | `rpcs3-ps4/3rdparty/wolfssl/wolfssl/COPYING` |
| `licenses/wolfSSL-LICENSING.txt` | wolfSSL (the exception for GPL-2.0 projects) | `rpcs3-ps4/3rdparty/wolfssl/wolfssl/LICENSING` |
| `licenses/curl-COPYING.txt` | curl | `rpcs3-ps4/3rdparty/curl/curl/COPYING` |
| `licenses/miniupnpc-LICENSE.txt` | miniupnpc | `rpcs3-ps4/3rdparty/miniupnp/miniupnp/LICENSE` |
| `licenses/protobuf-LICENSE.txt` | Protocol Buffers | `rpcs3-ps4/3rdparty/protobuf/protobuf/LICENSE` |
| `licenses/Abseil-LICENSE.txt` | Abseil | `licenses/Abseil-LICENSE.txt` |
| `licenses/zstd-LICENSE.txt` | Zstandard (BSD-3-Clause) | `rpcs3-ps4/3rdparty/zstd/zstd/LICENSE` |
| `licenses/zstd-COPYING.txt` | Zstandard (GPL-2.0) | `rpcs3-ps4/3rdparty/zstd/zstd/COPYING` |
| `licenses/libpng-LICENSE.txt` | libpng | `rpcs3-ps4/3rdparty/libpng/libpng/LICENSE` |
| `licenses/AsmJit-LICENSE.md` | AsmJit | `rpcs3-ps4/3rdparty/asmjit/asmjit/LICENSE.md` |
| `licenses/yaml-cpp-LICENSE.txt` | yaml-cpp | `rpcs3-ps4/3rdparty/yaml-cpp/yaml-cpp/LICENSE` |
| `licenses/pugixml-LICENSE.md` | pugixml | `rpcs3-ps4/3rdparty/pugixml/LICENSE.md` |
| `licenses/SoundTouch-COPYING.txt` | SoundTouch | `rpcs3-ps4/3rdparty/SoundTouch/soundtouch/COPYING.TXT` |
| `licenses/Fusion-LICENSE.md` | Fusion | `rpcs3-ps4/3rdparty/fusion/fusion/LICENSE.md` |
| `licenses/glslang-LICENSE.txt` | glslang | `rpcs3-ps4/3rdparty/glslang/glslang/LICENSE.txt` |
| `licenses/VulkanMemoryAllocator-LICENSE.txt` | Vulkan Memory Allocator | `rpcs3-ps4/3rdparty/GPUOpen/VulkanMemoryAllocator/LICENSE.txt` |
| `licenses/stb-LICENSE.txt` | stb | `rpcs3-ps4/3rdparty/stblib/stb/LICENSE` |
| `licenses/libusb-COPYING.txt` | libusb headers | `rpcs3-ps4/3rdparty/libusb/libusb/COPYING` |
| `licenses/RtMidi-LICENSE.txt` | RtMidi C API header | `rpcs3-ps4/3rdparty/rtmidi/rtmidi/LICENSE` |
| `licenses/OpenAL-Soft-COPYING.txt` | OpenAL Soft headers | `rpcs3-ps4/3rdparty/OpenAL/openal-soft/COPYING` |
| `licenses/bcdec-LICENSE.txt` | bcdec | the notice at the top of `rpcs3-ps4/3rdparty/bcdec/bcdec.hpp` |
| `licenses/unordered_dense-LICENSE.txt` | unordered_dense | the notice at the top of `rpcs3-ps4/3rdparty/unordered_dense/include/unordered_dense.h` |
| `licenses/PINE-NOTICE.txt` | PINE IPC server header (its license text is `LICENSE`, GPL-2.0) | the notice at the top of `rpcs3-ps4/3rdparty/pine/pine_server.h` |
| `licenses/orbis-compat-LICENSE.txt` | orbis-compat | `orbis-compat/LICENSE` |
| `licenses/orbis-compat-BSD-3-Clause-FreeBSD.txt` | orbis-compat `include/sys/ioccom.h` | `orbis-compat/licenses/BSD-3-Clause-FreeBSD.txt` |
| `licenses/orbis-porting-kit-LICENSE.txt` | orbis-porting-kit | `licenses/orbis-porting-kit-LICENSE.txt` |
| `licenses/DejaVu-Fonts-LICENSE.txt` | DejaVu fonts | the copyright and license records stored in `rpcs3-ps4/bin/fonts/DejaVuSans.ttf` and `DejaVuSansMono.ttf`; `licenses/DejaVu-Fonts-LICENSE.txt` holds those of `DejaVuSans.ttf` |

## In `rpcs3-ps4/3rdparty/` but not used by the console build

These are RPCS3's desktop dependencies. Their folders are kept as RPCS3 has them; the submodule
folders marked "empty" hold no files in this release.

| Component | Folder | State |
| --- | --- | --- |
| 7-Zip | `3rdparty/7zip/7zip` | empty |
| FAudio | `3rdparty/FAudio` | empty |
| cubeb | `3rdparty/cubeb/cubeb` | empty |
| discord-rpc | `3rdparty/discord-rpc/discord-rpc` | empty |
| GameMode (Feral Interactive) | `3rdparty/feralinteractive/feralinteractive` | empty |
| hidapi | `3rdparty/hidapi/hidapi` | empty |
| SDL | `3rdparty/libsdl-org/SDL` | empty |
| OpenCV | `3rdparty/opencv/opencv` | empty |
| LLVM submodule | `3rdparty/llvm/llvm` | empty (the console build downloads LLVM at this pin) |
| MoltenVK | `3rdparty/MoltenVK` | CMake files only |
| OpenAL Soft library | `3rdparty/OpenAL/openal-soft` | sources present; only the headers are used |
| `ffmpeg-core` Windows libraries | `3rdparty/ffmpeg/lib/windows` | present; not used |
