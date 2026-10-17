# Building RPCS3 PORT FOR PS4 0.39

The build runs on a **Windows x64** host. It cross-compiles RPCS3 and its libraries for the PS4 with
the OpenOrbis PS4 Toolchain and llvm-mingw's Clang/LLD, links the native ELF, and turns it into a
fake-signed EBOOT (`create-fself`) and an installable PKG.

All commands below are PowerShell, run from `rpcs3-ps4/` (the repository root) unless stated
otherwise.

## 1. Tools

| Tool | Version | Used for |
| --- | --- | --- |
| [OpenOrbis PS4 Toolchain](https://github.com/OpenOrbis/OpenOrbis-PS4-Toolchain) | v0.5.4 | SDK headers and libraries, `link.x`, `create-fself`, `create-gp4`, `PkgTool.Core` (needs a .NET runtime; the script sets `DOTNET_ROLL_FORWARD=Major`) |
| [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) | 20240619, UCRT, x86_64 (`llvm-mingw-20240619-ucrt-x86_64`) | Clang/LLD for the console objects, `llvm-ar`, `llvm-nm`, `llvm-objdump`, host tools and host tests |
| CMake | 3.28 or newer | configuration |
| Ninja | any recent | build tool (one copy must be at `ps4-port/.build-tools/ninja.exe`, see step 4) |
| Python | 3.12 or newer, as `python` on `PATH` | LLVM download/preparation, Mesa archive preparation, ELF checks |
| Git and Git for Windows | Git for Windows installed in `C:/Program Files/Git` | dependency checkouts; its `bash.exe` runs FFmpeg's `configure`, its MSYS DLLs run GNU Make |
| Windows PowerShell 5.1 or PowerShell 7 | | the `ps4-port/*.ps1` scripts |

The FFmpeg step calls `C:/Program Files/Git/usr/bin/bash.exe`. If Git for Windows lives elsewhere,
pass `-GitUsrBin <Git>/usr/bin` to `prepare-make.ps1`, and in step 6 build the `rpcs3_ps4_linker`
target first, then run `./ps4-port/build-ffmpeg.ps1 -Sdk $Sdk -Llvm $Llvm -Bash <Git>/usr/bin/bash.exe`
once by hand before building `rpcs3_ps4_elf`.

## 2. Source layout

Besides this folder you need two more inputs, placed **next to** `rpcs3-ps4/` and `orbis-compat/`:

```
<release>/
  rpcs3-ps4/          this repository
  orbis-compat/       orbis-compat sources (included in this release)
  orbis-porting-kit/  https://github.com/orbis-ports/orbis-porting-kit (commit 213b0c38ff25ea7ca402c8844a6494086d0f1cae); the build uses its vkloader/
  orbis-mesa-bundle/
    orbis-mesa-4c934ebf9cda/   Mesa PS4 bundle: release orbis-mesa-4c934ebf9cda of
                               https://github.com/orbis-ports/mesa-ps4 (orbis-mesa-4c934ebf9cda.tar.gz,
                               Mesa 26.3.0-devel 4c934ebf9cda: RADV, EGL, Gallium/Zink, Vulkan headers)
```

Other locations work too: configure with `-DRPCS3_PS4_ORBIS_COMPAT=<dir>`,
`-DRPCS3_PS4_PORTING_KIT=<dir>` and `-DRPCS3_PS4_MESA_BUNDLE=<dir>`. The LLVM build
(`ps4-port/llvm-toolchain.cmake`) always reads `rpcs3-ps4/../orbis-compat/include`, so keep
`orbis-compat/` beside `rpcs3-ps4/`.

Set the two tool paths once per PowerShell session. The scripts' default values point at the
developers' own folder layout, so always pass these explicitly:

```powershell
$Sdk  = '<path>/OpenOrbis/PS4Toolchain'                   # OpenOrbis PS4 Toolchain root (has include/orbis, lib/libc++.a, link.x)
$Llvm = '<path>/llvm-mingw-20240619-ucrt-x86_64/bin'      # contains clang.exe, clang++.exe, ld.lld.exe, llvm-ar.exe
$Version = [regex]::Match((Get-Content rpcs3/util/ps4_build_version.hpp -Raw),
    'ps4_build_version\[\]\s*=\s*"([0-9]+\.[0-9]{2})"').Groups[1].Value
```

`$Version` is the version marker of the source tree; packaging refuses any other value.

## 3. Record the LLVM submodule pin

`ps4-port/fetch-llvm.py` and `ps4-port/package-native.ps1` read RPCS3's LLVM submodule pin from git
(`git ls-tree HEAD 3rdparty/llvm/llvm`) and record `git rev-parse HEAD` in the build manifest, so the
source tree must be a git work tree whose `HEAD` holds the gitlink of `3rdparty/llvm/llvm`. In a
plain copy of this folder, create it once (nothing else needs to be committed):

```powershell
git init
git update-index --add --cacheinfo "160000,ca7933e47d3a3451d81e72ac174dcb5aa28b59d1,3rdparty/llvm/llvm"
git commit -m "Record RPCS3's LLVM submodule pin"
```

Skip this step in a git clone that already records that gitlink, for example an RPCS3 clone at
`9d52b338058bda2d5a9d7cd5e4fbc8148e55a385` with this folder's `rpcs3-ps4/` copied over it.

All 3rdparty sources the console build compiles are already in `rpcs3-ps4/3rdparty/` (the RPCS3
submodules at their pinned commits; `3rdparty/GPUOpen/VulkanMemoryAllocator/include/vk_mem_alloc.h`
carries a small `RPCS3_PS4` addition), so `git submodule update` is not needed. The LLVM source is
downloaded by step 5.

## 4. Pinned dependencies

```powershell
# Abseil 20250512.1 (commit 76bb24329e8bf5f39704eb10d21b9a80befa7c81) into ps4-port/deps/abseil-cpp
./ps4-port/prepare-abseil.ps1

# GNU Make 4.4.1 (MSYS2 package, SHA-256 checked) into ps4-port/.build-tools, with Git for Windows' MSYS DLLs
./ps4-port/prepare-make.ps1

# Ninja for the LLVM build
Copy-Item <path>/ninja.exe ps4-port/.build-tools/ninja.exe
```

## 5. LLVM for the console

```powershell
./ps4-port/build-llvm.ps1 -Sdk $Sdk -CompilerBin $Llvm -Jobs 4
```

This runs `fetch-llvm.py` (downloads the llvm-project archive of commit
`ca7933e47d3a3451d81e72ac174dcb5aa28b59d1` into `ps4-port/deps/` and checks its SHA-256),
`prepare-llvm.py` (adapts the OS support files and records them in
`ps4-port/.build-native-package/llvm-source-preparation.json`), builds the host `llvm-tblgen` in
`ps4-port/.build-llvm-host` and the console LLVM libraries (`LLVMX86CodeGen`, `LLVMX86AsmParser`,
`LLVMX86Disassembler`, `LLVMMCJIT`, `LLVMPasses`) in `ps4-port/.build-llvm-orbis`.

## 6. Configure and build the native ELF

The build directory must be `ps4-port/.build-abi` (the FFmpeg script and the packaging script use it).

```powershell
cmake -S . -B ps4-port/.build-abi -G Ninja `
  "-DCMAKE_MAKE_PROGRAM=$PWD/ps4-port/.build-tools/ninja.exe" `
  -DRPCS3_PS4_PORT=ON `
  -DCMAKE_TOOLCHAIN_FILE=ps4-port/toolchain.cmake `
  "-DRPCS3_PS4_SDK=$Sdk" "-DRPCS3_PS4_LLVM_BIN=$Llvm" `
  -DRPCS3_PS4_WITH_LLVM=ON

cmake --build ps4-port/.build-abi --target rpcs3_ps4_elf -j 4
```

`rpcs3_ps4_elf` (the same as `ninja -C ps4-port/.build-abi rpcs3_ps4_elf`) first builds the
`rpcs3_ps4_linker` wrapper, then curl (`3rdparty/curl`, ExternalProject) and FFmpeg n8.1.1
(`ps4-port/build-ffmpeg.ps1` clones it into `ps4-port/deps/ffmpeg`, checks commit
`239f2c733de417201d7ad3b3b8b0d9b63285b2b1` and its header versions against `3rdparty/ffmpeg/include`),
prepares the Mesa EGL/Gallium archives (`rpcs3_ps4_mesa_gl`, `prepare-mesa-gl.py`), compiles RPCS3 and
the port, and links `ps4-port/.build-abi/ps4-port/rpcs3_ps4.elf`.

## 7. EBOOT and PKG

```powershell
./ps4-port/package-native.ps1 -Sdk $Sdk -LlvmTools $Llvm -Version $Version
```

`package-native.ps1` checks the version marker, builds `rpcs3_ps4_elf` again if needed, validates the
ELF (`verify-native-dispatch.py` and the process exit policy, with `llvm-nm`/`llvm-objdump`), writes
the build stamp (`ps4-port/tools/stamp_build.py`), runs `create-fself`, stages the package
(`sce_sys/about/right.sprx`, `sce_module/libc.prx` and `sce_module/libSceFios2.prx` from the SDK's
`samples/hello_world`, `ps4-port/assets/icon0.png`, the DejaVu fonts and the overlay icons from
`bin/`, `LICENSE`, `licenses/LLVM.txt`, `certs/cacert.pem`, `TUTORIAL.txt`, `TESTAR_NO_PS4.txt`),
builds the PKG with `create-gp4` and `PkgTool.Core`, validates it, extracts it again and compares the
hash of every file.

The output is in `ps4-port/dist/`: `RPCS3_PS4_NATIVE_<version>_<date>.pkg`, its `.sha256`, the
`build-manifest-<date>.json` and the two guides. The package's content ID is
`IV0000-RPCS30010_00-RPCS3NATIVE00001` (title ID `RPCS30010`).

Optional: `-ConfigDatabase <dir>/GuiConfigs/config_database.dat` adds the per-game settings database
of the PC RPCS3 as `/app0/config_database.dat`. It is off by default; use it only after checking that
the file's license allows redistribution (see `rpcs3/util/ps4_config_db.hpp`).

## 8. Host tests

`ps4-port/tests/` holds tests that run on the Windows host with the PS4 kernel calls mocked (memory,
fenv, events, configuration policy, controllers, RPCN account, XMB, USB/MIDI, audio, disc, cheats,
settings browser and others). Each file's header gives its compile line, for example:

```powershell
clang++ -std=c++20 -static -Wall -Wextra -I rpcs3 ps4-port/tests/mem_budget_test.cpp -o mem_budget_test.exe
./mem_budget_test.exe
```

Use the `clang++` of llvm-mingw (`$Llvm`).

## Folders created by the build

`ps4-port/deps/`, `ps4-port/.build*/` and `ps4-port/dist/` are generated (downloads, build trees,
packages) and are listed in `ps4-port/.gitignore`. `rpcs3/git-version.h` is generated at configure
time.
