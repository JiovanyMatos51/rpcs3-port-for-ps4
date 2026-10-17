param(
    # Default tool locations are folders next to this checkout.
    [string]$Sdk = [IO.Path]::GetFullPath("$PSScriptRoot/../../RoberixUSB/tools/OpenOrbis/PS4Toolchain").Replace('\', '/'),
    [string]$CompilerBin = [IO.Path]::GetFullPath("$PSScriptRoot/../../ps4-usb-pad/tools/llvm-mingw-20240619-ucrt-x86_64/bin").Replace('\', '/'),
    [ValidateRange(1,16)][int]$Jobs = 4,
    [string]$Python = 'python'
)
$ErrorActionPreference = 'Stop'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE) { throw "Command failed ($LASTEXITCODE): $Program" }
}
$pin = 'ca7933e47d3a3451d81e72ac174dcb5aa28b59d1'
$source = "$PSScriptRoot/deps/llvm-project-$pin/llvm"
$hostBuild = "$PSScriptRoot/.build-llvm-host"
$nativeBuild = "$PSScriptRoot/.build-llvm-orbis"
$ninja = "$PSScriptRoot/.build-tools/ninja.exe"
Invoke-Checked $Python @("$PSScriptRoot/fetch-llvm.py")
Invoke-Checked $Python @("$PSScriptRoot/prepare-llvm.py")
# TableGen runs on Windows. Static MinGW runtime avoids dependence on PATH DLLs.
Invoke-Checked 'cmake' @('-S',$source,'-B',$hostBuild,'-G','Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_C_COMPILER=$CompilerBin/clang.exe",
    "-DCMAKE_CXX_COMPILER=$CompilerBin/clang++.exe", '-DCMAKE_BUILD_TYPE=Release',
    '-DLLVM_TARGETS_TO_BUILD=X86', '-DLLVM_INCLUDE_TESTS=OFF',
    '-DLLVM_INCLUDE_BENCHMARKS=OFF', '-DLLVM_INCLUDE_EXAMPLES=OFF',
    '-DLLVM_INCLUDE_DOCS=OFF', '-DLLVM_ENABLE_ZLIB=OFF', '-DLLVM_ENABLE_ZSTD=OFF',
    '-DLLVM_ENABLE_LIBXML2=OFF', '-DLLVM_ENABLE_LIBEDIT=OFF', '-DLLVM_ENABLE_LIBPFM=OFF',
    '-DLLVM_ENABLE_PROJECTS=', '-DLLVM_BUILD_TOOLS=OFF', '-DLLVM_ENABLE_WARNINGS=OFF',
    '-DCMAKE_EXE_LINKER_FLAGS=-static')
Invoke-Checked 'cmake' @('--build',$hostBuild,'--target','llvm-tblgen','--parallel',"$Jobs")
# Every library below is console ELF, with the OpenOrbis libc++ ABI.
Invoke-Checked 'cmake' @('-S',$source,'-B',$nativeBuild,'-G','Ninja',
    "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$PSScriptRoot/llvm-toolchain.cmake",
    "-DRPCS3_PS4_SDK=$Sdk", "-DRPCS3_PS4_LLVM_BIN=$CompilerBin", '-DCMAKE_BUILD_TYPE=Release',
    '-U','CMAKE_C_FLAGS','-U','CMAKE_CXX_FLAGS')
Invoke-Checked 'cmake' @('--build',$nativeBuild,'--target',
    'LLVMX86CodeGen','LLVMX86AsmParser','LLVMX86Disassembler','LLVMMCJIT','LLVMPasses',
    '--parallel',"$Jobs")
Write-Output 'Console LLVM is ready. Configure RPCS3 with -DRPCS3_PS4_WITH_LLVM=ON.'
