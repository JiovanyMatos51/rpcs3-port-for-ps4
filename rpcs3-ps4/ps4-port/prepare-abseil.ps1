# Protobuf 33.4 requires this Abseil revision. Keep the checkout outside the
# port's tracked sources, while retaining a reproducible download command.
$ErrorActionPreference = 'Stop'
$abseilDirectory = Join-Path $PSScriptRoot 'deps/abseil-cpp'
$abseilCommit = '76bb24329e8bf5f39704eb10d21b9a80befa7c81'
if (-not (Test-Path -LiteralPath $abseilDirectory)) {
    & git -c credential.interactive=never -c http.sslBackend=openssl clone `
        --filter=blob:none --depth 1 --branch 20250512.1 `
        https://github.com/abseil/abseil-cpp.git $abseilDirectory
    if ($LASTEXITCODE -ne 0) { throw 'Abseil download failed.' }
}
$abseilHead = & git -C $abseilDirectory rev-parse HEAD
if ($LASTEXITCODE -ne 0 -or $abseilHead -ne $abseilCommit) {
    throw "Abseil must be at $abseilCommit; found $abseilHead."
}
Write-Output "Abseil ready: $abseilHead"
