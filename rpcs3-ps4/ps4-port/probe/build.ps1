param(
    [Parameter(Mandatory = $true)][string]$Sdk,
    [Parameter(Mandatory = $true)][string]$LlvmBin
)

$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $repo 'ps4-port\.build-probe-package'
$stage = Join-Path $build 'pkg'
$clang = Join-Path $LlvmBin 'clang++.exe'
$linker = Join-Path $LlvmBin 'ld.lld.exe'
$fself = Join-Path $Sdk 'bin\windows\create-fself.exe'
$gp4tool = Join-Path $Sdk 'bin\windows\create-gp4.exe'
$pkgtool = Join-Path $Sdk 'bin\windows\PkgTool.Core.exe'
$contentId = 'IV0000-RPCS30002_00-PS4PROBE00000002'

foreach ($file in @($clang, $linker, $fself, $gp4tool, $pkgtool)) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Missing build tool: $file" }
}

function Invoke-Checked([string]$file, [string[]]$arguments) {
    & $file @arguments
    if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $file" }
}

New-Item -ItemType Directory -Force $build, (Join-Path $stage 'sce_sys\about'), (Join-Path $stage 'sce_module') | Out-Null
$includes = @('-nostdinc++', '-isystem', (Join-Path $Sdk 'include\c++\v1'), '-isystem', (Join-Path $Sdk 'include'), '-I' + (Join-Path $repo 'ps4-port\compat'), '-I' + $repo, '-I' + (Join-Path $repo 'rpcs3'))
$compile = @('--target=x86_64-pc-freebsd12-elf', '-std=c++23', '-D__ORBIS__', '-DRPCS3_PS4', '-D_GNU_SOURCE', '-D_POSIX_C_SOURCE=200809L', '-fPIC', '-funwind-tables') + $includes

Invoke-Checked $clang ($compile + @('-c', (Join-Path $repo 'rpcs3\Crypto\sha256.cpp'), '-o', (Join-Path $build 'sha256.o')))
Invoke-Checked $clang ($compile + @('-c', (Join-Path $PSScriptRoot 'main.cpp'), '-o', (Join-Path $build 'main.o')))
Invoke-Checked $linker @(
    '-m', 'elf_x86_64', '-pie', '--script', (Join-Path $Sdk 'link.x'), '--eh-frame-hdr',
    '-o', (Join-Path $build 'probe.elf'), '-L', (Join-Path $Sdk 'lib'),
    (Join-Path $Sdk 'lib\crt1.o'), (Join-Path $build 'main.o'), (Join-Path $build 'sha256.o'),
    '-lc', '-lkernel', '-lc++'
)

$env:OO_PS4_TOOLCHAIN = $Sdk
Invoke-Checked $fself @('-in', (Join-Path $build 'probe.elf'), '--out', (Join-Path $build 'probe.oelf'), '--eboot', (Join-Path $stage 'eboot.bin'), '--paid', '0x3800000000000011')

$sample = Join-Path $Sdk 'samples\hello_world'
Copy-Item (Join-Path $sample 'sce_sys\icon0.png') (Join-Path $stage 'sce_sys\icon0.png')
Copy-Item (Join-Path $sample 'sce_sys\about\right.sprx') (Join-Path $stage 'sce_sys\about\right.sprx')
Copy-Item (Join-Path $sample 'sce_module\libc.prx') (Join-Path $stage 'sce_module\libc.prx')
Copy-Item (Join-Path $sample 'sce_module\libSceFios2.prx') (Join-Path $stage 'sce_module\libSceFios2.prx')

$env:DOTNET_ROLL_FORWARD = 'Major'
Push-Location $stage
try {
    Invoke-Checked $pkgtool @('sfo_new', 'sce_sys/param.sfo')
    $entries = @(
        @('APP_TYPE', 'Integer', '4', '1'),
        @('APP_VER', 'Utf8', '8', '1.00'),
        @('ATTRIBUTE', 'Integer', '4', '0'),
        @('CATEGORY', 'Utf8', '4', 'gd'),
        @('CONTENT_ID', 'Utf8', '48', $contentId),
        @('DOWNLOAD_DATA_SIZE', 'Integer', '4', '0'),
        @('SYSTEM_VER', 'Integer', '4', '0'),
        @('TITLE', 'Utf8', '128', 'RPCS3 PS4 Port Probe 2'),
        @('TITLE_ID', 'Utf8', '12', 'RPCS30002'),
        @('VERSION', 'Utf8', '8', '1.00')
    )
    foreach ($entry in $entries) {
        Invoke-Checked $pkgtool @('sfo_setentry', 'sce_sys/param.sfo', $entry[0], '--type', $entry[1], '--maxsize', $entry[2], '--value', $entry[3])
    }
    Invoke-Checked $gp4tool @('-out', 'pkg.gp4', "--content-id=$contentId", '--files', 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx')
    Invoke-Checked $pkgtool @('pkg_build', 'pkg.gp4', '.')
    $package = "$contentId.pkg"
    Invoke-Checked $pkgtool @('pkg_validate', $package)
    Get-FileHash -Algorithm SHA256 -LiteralPath $package | Select-Object Path, Hash
}
finally {
    Pop-Location
}
