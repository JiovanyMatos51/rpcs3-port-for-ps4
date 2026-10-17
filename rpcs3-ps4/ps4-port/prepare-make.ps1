param([string]$GitUsrBin = 'C:/Program Files/Git/usr/bin')
$ErrorActionPreference = 'Stop'
$tools = Join-Path $PSScriptRoot '.build-tools'
$package = Join-Path $tools 'make-4.4.1-3-x86_64.pkg.tar.zst'
$digest = 'AF0BDBA17F06FE037F0194069ADAA31A8FE45F1A11381501896AEA1FAE37BD5D'
New-Item -ItemType Directory -Force $tools | Out-Null
if (!(Test-Path $package)) {
    # Download through Python (OpenSSL), which also works where Windows
    # Schannel is unavailable. Nothing is installed machine-wide.
    & python -c 'import sys, urllib.request; urllib.request.urlretrieve(sys.argv[1], sys.argv[2])' 'https://repo.msys2.org/msys/x86_64/make-4.4.1-3-x86_64.pkg.tar.zst' $package
    if ($LASTEXITCODE) { throw 'MSYS Make download failed' }
}
if ((Get-FileHash $package -Algorithm SHA256).Hash -ne $digest) { throw 'MSYS Make package digest mismatch' }
Push-Location $tools
try {
    & cmake -E tar xf $package
    if ($LASTEXITCODE) { throw 'MSYS Make extraction failed' }
    foreach ($library in 'msys-2.0.dll', 'msys-intl-8.dll', 'msys-iconv-2.dll') {
        Copy-Item -LiteralPath (Join-Path $GitUsrBin $library) -Destination 'usr/bin'
    }
    & './usr/bin/make.exe' --version
    if ($LASTEXITCODE) { throw 'MSYS Make runtime verification failed' }
} finally { Pop-Location }
