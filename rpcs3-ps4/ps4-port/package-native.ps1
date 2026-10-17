param(
    # Default tool locations are folders next to this checkout.
    [string]$Sdk = [IO.Path]::GetFullPath("$PSScriptRoot/../../RoberixUSB/tools/OpenOrbis/PS4Toolchain").Replace('\', '/'),
    [string]$Build = "$PSScriptRoot/.build-abi",
    [ValidatePattern('^[0-9]{1,2}\.[0-9]{2}$')]
    [string]$Version = '0.39',
    [string]$Python = 'python',
    # llvm-nm/llvm-objdump used by the exit-policy validation (0.25 and later).
    [string]$LlvmTools = [IO.Path]::GetFullPath("$PSScriptRoot/../../ps4-usb-pad/tools/llvm-mingw-20240619-ucrt-x86_64/bin").Replace('\', '/'),
    # Optional: a PC RPCS3 GuiConfigs/config_database.dat shipped read-only as /app0/config_database.dat
    # (rpcs3/util/ps4_config_db.hpp reads it when /data/rpcs3 has no newer copy). Off by default: pass it only
    # after the licence question in ps4_config_db.hpp has been answered for redistribution.
    [string]$ConfigDatabase = ''
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$sourceVersion = [regex]::Match((Get-Content -LiteralPath "$repo/rpcs3/util/ps4_build_version.hpp" -Raw),
    'ps4_build_version\[\]\s*=\s*"([0-9]+\.[0-9]{2})"').Groups[1].Value
if ($sourceVersion -ne $Version) { throw "Native source version $sourceVersion differs from requested package version $Version" }
$versionParts = $Version.Split('.')
$sfoVersion = '{0:D2}.{1}' -f [int]$versionParts[0], $versionParts[1]
$buildPath = (Resolve-Path $Build).Path
$withLlvm = (Get-Content -LiteralPath "$buildPath/CMakeCache.txt") -contains 'RPCS3_PS4_WITH_LLVM:BOOL=ON'
if ([version]$Version -ge [version]'0.21' -and !$withLlvm) {
    throw 'Version 0.21 and later requires the original LLVM recompilers'
}
$llvmPreparation = $null
if ($withLlvm) {
    $llvmPreparation = Get-Content -LiteralPath "$PSScriptRoot/.build-native-package/llvm-source-preparation.json" -Raw | ConvertFrom-Json
    # safe.directory: the checkout belongs to another Windows account (git 'dubious ownership').
    $llvmPinLine = & git -c safe.directory=* -C $repo ls-tree HEAD 3rdparty/llvm/llvm
    if ($LASTEXITCODE -or !$llvmPinLine) { throw "git ls-tree failed ($LASTEXITCODE): cannot read the LLVM submodule pin" }
    $llvmPin = ([string]$llvmPinLine).Split()[2]
    if ($llvmPreparation.commit -ne $llvmPin) { throw 'Prepared LLVM differs from the RPCS3 submodule pin' }
    $llvmSource = "$PSScriptRoot/deps/llvm-project-$llvmPin"
    foreach ($file in $llvmPreparation.files.PSObject.Properties) {
        if ((Get-FileHash -LiteralPath "$llvmSource/$($file.Name)" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $file.Value.prepared_sha256) {
            throw "LLVM OS support differs from its recorded preparation: $($file.Name)"
        }
    }
    if ([version]$Version -ge [version]'0.22') {
        if (!$llvmPreparation.port_helpers) { throw 'The native LLVM process symbol adapter is not recorded' }
        foreach ($helper in $llvmPreparation.port_helpers.PSObject.Properties) {
            if ((Get-FileHash -LiteralPath "$PSScriptRoot/$($helper.Name)" -Algorithm SHA256).Hash.ToLowerInvariant() -ne $helper.Value) {
                throw "Native LLVM helper differs from its recorded preparation: $($helper.Name)"
            }
        }
    }
}
$elf = Join-Path $buildPath 'ps4-port/rpcs3_ps4.elf'
$tools = Join-Path $Sdk 'bin/windows'
$work = Join-Path $PSScriptRoot '.build-native-package'
$stage = Join-Path $work 'stage'
$output = Join-Path $PSScriptRoot 'dist'
$contentId = 'IV0000-RPCS30010_00-RPCS3NATIVE00001'
function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE) { throw "Command failed ($LASTEXITCODE): $Program" }
}
function Get-ElfLoadSegments([byte[]]$Bytes) {
    $phoff = [BitConverter]::ToUInt64($Bytes, 0x20)
    $phentsize = [BitConverter]::ToUInt16($Bytes, 0x36)
    $phnum = [BitConverter]::ToUInt16($Bytes, 0x38)
    for ($i = 0; $i -lt $phnum; $i++) {
        $p = [int]($phoff + $i * $phentsize)
        [pscustomobject]@{
            Type = [BitConverter]::ToUInt32($Bytes, $p); Flags = [BitConverter]::ToUInt32($Bytes, $p + 4)
            Vaddr = [BitConverter]::ToUInt64($Bytes, $p + 16); Filesz = [BitConverter]::ToUInt64($Bytes, $p + 32)
            Memsz = [BitConverter]::ToUInt64($Bytes, $p + 40)
        }
    }
}
# create-fself shortens the OELF data segment by the distance from the end of its file-backed part to the next
# 16 KiB boundary (link-data-tail.x makes that distance zero). Every writable byte of the ELF must stay inside the
# OELF's writable segment: the PS4 loader refuses a module with a relocation pointing past it (CE-34878-0 at start).
function Test-OelfDataCoverage([string]$ElfPath, [string]$OelfPath) {
    $elfLoads = @(Get-ElfLoadSegments ([System.IO.File]::ReadAllBytes($ElfPath)) | Where-Object { $_.Type -eq 1 -and ($_.Flags -band 2) })
    $oelfLoads = @(Get-ElfLoadSegments ([System.IO.File]::ReadAllBytes($OelfPath)) | Where-Object { $_.Type -eq 1 -and ($_.Flags -band 2) })
    if (!$elfLoads.Count -or !$oelfLoads.Count) { throw 'No writable PT_LOAD segment in the ELF or the OELF' }
    $elfEnd = ($elfLoads | ForEach-Object { $_.Vaddr + $_.Memsz } | Measure-Object -Maximum).Maximum
    $oelfEnd = ($oelfLoads | ForEach-Object { $_.Vaddr + $_.Memsz } | Measure-Object -Maximum).Maximum
    if ($oelfEnd -lt $elfEnd) {
        throw ('OELF data segment ends at 0x{0:x}, before the ELF data end 0x{1:x}; the PS4 loader would refuse it (CE-34878-0)' -f [uint64]$oelfEnd, [uint64]$elfEnd)
    }
    Write-Output ('OELF data coverage OK: ELF writable end 0x{0:x}, OELF writable end 0x{1:x}' -f [uint64]$elfEnd, [uint64]$oelfEnd)
}
# On this console abort() ends in a SIGABRT core dump and _exit()/_Exit() in SIGSYS; both are
# reported as CE-34878-0. Reject an ELF whose fatal path still aborts, whose frontend still exits
# by itself, or which kept musl's exit() instead of runtime/ps4_exit_guard.c.
function Test-NativeExitPolicy([string]$ElfPath, [string]$FrontendObjects, [string]$ToolDir) {
    $nmTool = Join-Path $ToolDir 'llvm-nm.exe'
    $objdumpTool = Join-Path $ToolDir 'llvm-objdump.exe'
    foreach ($tool in $nmTool, $objdumpTool) {
        if (!(Test-Path -LiteralPath $tool)) { throw "Exit-policy validation needs $tool (pass -LlvmTools <llvm bin directory>)" }
    }
    $failures = [System.Collections.Generic.List[string]]::new()
    $symbols = @(& $nmTool $ElfPath)
    if ($LASTEXITCODE) { throw "llvm-nm failed ($LASTEXITCODE) on $ElfPath" }

    # 1. report_fatal_error must record and park; it must never call abort/_exit/_Exit.
    $fatalNames = @($symbols | ForEach-Object {
        if ($_ -match '^[0-9a-fA-F]+\s+[TtWw]\s+(_Z18report_fatal_error\S*)$') { $Matches[1] } })
    if (!$fatalNames.Count) { $failures.Add('report_fatal_error is not defined in the ELF; the fatal path cannot be verified') }
    foreach ($name in $fatalNames) {
        $code = @(& $objdumpTool -d --no-show-raw-insn "--disassemble-symbols=$name" $ElfPath)
        if ($LASTEXITCODE) { throw "llvm-objdump failed ($LASTEXITCODE) on $name" }
        $instructions = @($code | Where-Object { $_ -match '^\s+[0-9a-f]+:\s' })
        if (!$instructions.Count) { $failures.Add("report_fatal_error ($name) could not be disassembled"); continue }
        $calls = @($instructions | Where-Object { $_ -match '\b(call|jmp)\w*\s+\S+\s+<(abort|_exit|_Exit)(@plt)?>' })
        foreach ($line in $calls) { $failures.Add("report_fatal_error still ends the process: $($line.Trim())") }
        Write-Output "Exit policy: report_fatal_error has $($instructions.Count) instructions, $($calls.Count) abort/_exit/_Exit calls"
    }

    # 2. musl exit() runs static destructors and then _exit (SIGSYS): exactly one exit(), from ps4_exit_guard.c.
    $exitDefinitions = @($symbols | Where-Object { $_ -match '^[0-9a-fA-F]+\s+[TtWw]\s+exit$' })
    if ($exitDefinitions.Count -ne 1) { $failures.Add("expected exactly one exit() definition in the ELF, found $($exitDefinitions.Count)") }

    # 3. The frontend objects must not reference _exit/_Exit at all.
    $objects = @(Get-ChildItem -LiteralPath $FrontendObjects -Recurse -File -Filter '*.o' -ErrorAction SilentlyContinue)
    if (!$objects.Count) { $failures.Add("no frontend objects found under $FrontendObjects") }
    $guard = @($objects | Where-Object { $_.Name -eq 'ps4_exit_guard.c.o' })
    if ($guard.Count -ne 1) { $failures.Add('runtime/ps4_exit_guard.c.o is missing from the frontend objects') }
    else {
        $guardSymbols = @(& $nmTool --defined-only $guard[0].FullName)
        if ($LASTEXITCODE) { throw "llvm-nm failed ($LASTEXITCODE) on $($guard[0].FullName)" }
        if (!@($guardSymbols | Where-Object { $_ -match '^[0-9a-fA-F]+\s+T\s+exit$' }).Count) {
            $failures.Add('ps4_exit_guard.c.o does not define exit()')
        }
    }
    foreach ($object in $objects) {
        $undefined = @(& $nmTool -u $object.FullName)
        if ($LASTEXITCODE) { throw "llvm-nm failed ($LASTEXITCODE) on $($object.FullName)" }
        foreach ($symbol in $undefined) {
            if ($symbol -match '^\s*U\s+(_exit|_Exit)$') { $failures.Add("$($object.Name) calls $($Matches[1]) (SIGSYS on PS4)") }
        }
    }
    Write-Output "Exit policy: $($objects.Count) frontend objects scanned for _exit/_Exit"

    # 4. Informational: the clean way to leave is sceSystemServiceLoadExec.
    if (!@($symbols | Where-Object { $_ -match '\ssceSystemServiceLoadExec$' }).Count) {
        Write-Warning 'sceSystemServiceLoadExec is not referenced; leaving the application relies on the PS button only'
    }

    if ($failures.Count) {
        foreach ($failure in $failures) { Write-Output "Exit policy FAIL: $failure" }
        throw "Native exit-policy validation FAILED ($($failures.Count) problem(s), listed above)"
    }
    Write-Output 'Native exit-policy validation passed'
}
Invoke-Checked 'cmake' @('--build', $buildPath, '--target', 'rpcs3_ps4_elf', '-j', '4')
if (!(Test-Path $elf)) { throw 'The actual RPCS3 native ELF is missing' }
$dispatchArguments = @("$PSScriptRoot/verify-native-dispatch.py", $elf, '--opengl')
if ($withLlvm) { $dispatchArguments += '--llvm' }
Invoke-Checked $Python $dispatchArguments
$exitPolicyChecked = [version]$Version -ge [version]'0.25'
if ($exitPolicyChecked) {
    Test-NativeExitPolicy $elf (Join-Path $buildPath 'ps4-port/CMakeFiles/rpcs3_ps4_frontend.dir') $LlvmTools
}
# Build stamp (SHA-256 of the loaded segments) written into the ELF before create-fself; the persistent SPU object
# cache stays off in an unstamped build.
$buildStamp = [string](& $Python -I "$PSScriptRoot/tools/stamp_build.py" $elf | Select-Object -Last 1)
if ($LASTEXITCODE -or $buildStamp -notmatch '^[0-9a-f]{64}$') { throw "stamp_build.py failed ($LASTEXITCODE): $buildStamp" }
Write-Output "Build stamp: $buildStamp"
New-Item -ItemType Directory -Force $work, $output, "$stage/sce_sys/about", "$stage/sce_module", "$stage/fonts" | Out-Null
$env:OO_PS4_TOOLCHAIN = (Resolve-Path $Sdk).Path
$env:DOTNET_ROLL_FORWARD = 'Major'
# create-fself records the input name in the module info, so pass it as a bare file name.
Push-Location (Split-Path -Parent $elf)
try {
    Invoke-Checked "$tools/create-fself.exe" @('-in', (Split-Path -Leaf $elf), '--out', "$work/rpcs3.oelf", '--eboot', "$stage/eboot.bin", '--paid', '0x3800000000000011')
}
finally { Pop-Location }
Test-OelfDataCoverage $elf "$work/rpcs3.oelf"
foreach ($relative in 'sce_sys/about/right.sprx', 'sce_module/libc.prx', 'sce_module/libSceFios2.prx') {
    Copy-Item -LiteralPath "$Sdk/samples/hello_world/$relative" -Destination "$stage/$relative"
}
Copy-Item -LiteralPath "$PSScriptRoot/assets/icon0.png" -Destination "$stage/sce_sys/icon0.png"
Copy-Item -LiteralPath "$repo/bin/fonts/DejaVuSans.ttf","$repo/bin/fonts/DejaVuSansMono.ttf" -Destination "$stage/fonts"
Copy-Item -LiteralPath "$repo/LICENSE" -Destination "$stage/LICENSE"
$licenseResources = @()
if ($withLlvm) {
    New-Item -ItemType Directory -Force "$stage/licenses" | Out-Null
    Copy-Item -LiteralPath "$llvmSource/llvm/LICENSE.TXT" -Destination "$stage/licenses/LLVM.txt"
    $licenseResources += 'licenses/LLVM.txt'
}
# Public root certificates used to verify HTTPS (patch download). Shipped as /app0/certs/cacert.pem.
if (Test-Path -LiteralPath "$PSScriptRoot/assets/certs/cacert.pem") {
    New-Item -ItemType Directory -Force "$stage/certs" | Out-Null
    Copy-Item -LiteralPath "$PSScriptRoot/assets/certs/cacert.pem" -Destination "$stage/certs/cacert.pem"
    $licenseResources += 'certs/cacert.pem'
}
# Optional per-game database of the PC (-ConfigDatabase). The stage folder is reused between runs, so a copy
# left by an earlier run is removed when this run does not ask for it.
$databaseResources = @()
$configDatabaseInfo = 'none'
if ($ConfigDatabase) {
    if (!(Test-Path -LiteralPath $ConfigDatabase -PathType Leaf)) { throw "-ConfigDatabase: file not found: $ConfigDatabase" }
    $databaseSize = (Get-Item -LiteralPath $ConfigDatabase).Length
    # Same limit as the console reader (ps4_config_db.hpp max_size)
    if ($databaseSize -gt 16MB) { throw "-ConfigDatabase: $databaseSize bytes is larger than 16 MiB; not a config database" }
    $database = Get-Content -LiteralPath $ConfigDatabase -Raw -Encoding UTF8 | ConvertFrom-Json
    if ($null -eq $database.games -or $null -eq $database.return_code -or [int64]$database.return_code -lt 0) {
        throw '-ConfigDatabase: not the PC JSON ({"return_code": 0, "timestamp": N, "games": {...}})'
    }
    $databaseTitles = @($database.games.PSObject.Properties).Count
    if (!$databaseTitles) { throw '-ConfigDatabase: the games object is empty' }
    $databaseTimestamp = if ($null -ne $database.timestamp) { [int64]$database.timestamp } else { $null }
    Write-Warning 'Bundling the PC config_database.dat: ship it only with the licence question of ps4_config_db.hpp answered'
    Copy-Item -LiteralPath $ConfigDatabase -Destination "$stage/config_database.dat"
    $databaseResources += 'config_database.dat'
    $configDatabaseInfo = if ($null -ne $databaseTimestamp) { [string]$databaseTimestamp } else { 'bundled, no timestamp' }
    Write-Output "Config database bundled: $databaseTitles titles, timestamp $configDatabaseInfo"
}
elseif (Test-Path -LiteralPath "$stage/config_database.dat") {
    Remove-Item -LiteralPath "$stage/config_database.dat"
}
# Include every native overlay icon with its original relative path.
# Windows PowerShell 5.1 (.NET Framework) has no [IO.Path]::GetRelativePath.
$binRoot = (Resolve-Path -LiteralPath "$repo/bin").Path.TrimEnd('\', '/')
$uiResources = @(Get-ChildItem -LiteralPath "$repo/bin/Icons/ui" -File -Recurse | ForEach-Object {
    if (!$_.FullName.StartsWith($binRoot + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Overlay icon outside bin: $($_.FullName)"
    }
    $relative = $_.FullName.Substring($binRoot.Length + 1).Replace('\', '/')
    $destination = Join-Path $stage $relative
    New-Item -ItemType Directory -Force ([IO.Path]::GetDirectoryName($destination)) | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $destination
    $relative
})
if (!$uiResources.Count) { throw 'The native overlay icons are missing' }

# Name shown on the PS4 home screen (param.sfo TITLE, max 127 bytes).
$appTitle = 'RPCS3 PORT FOR PS4 by roberix studio & mifi studios'
$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$gitHead = & git -c safe.directory=* -C $repo rev-parse HEAD
if ($LASTEXITCODE -or !$gitHead) { throw "git rev-parse HEAD failed ($LASTEXITCODE)" }
$manifest = [ordered]@{
    title = $appTitle
    title_id = 'RPCS30010'
    app_version = $Version
    sfo_app_version = $sfoVersion
    build_time_utc = (Get-Date).ToUniversalTime().ToString('o')
    git_head = [string]$gitHead
    local_changes = $true
    native_ui_resource_count = $uiResources.Count
    native_renderers = @('Vulkan', 'OpenGL (Mesa Zink/RADV)')
    llvm_enabled = $withLlvm
    native_ppu_decoders = $(if ($withLlvm) { @('LLVM', 'Interpreter (static)') } else { @('Interpreter (static)') })
    native_spu_decoders = $(if ($withLlvm) { @('LLVM', 'ASMJIT', 'Interpreter (static)') } else { @('ASMJIT', 'Interpreter (static)') })
    llvm_source = $llvmPreparation
    exit_policy_verified = $exitPolicyChecked
    config_database = $configDatabaseInfo
    build_stamp = $buildStamp
    elf_sha256 = (Get-FileHash $elf -Algorithm SHA256).Hash
    eboot_sha256 = (Get-FileHash "$stage/eboot.bin" -Algorithm SHA256).Hash
    status = 'Native link and package validation only; PS4 execution and PS3 game compatibility unverified'
}
$manifestJson = $manifest | ConvertTo-Json -Depth 8
# The manifest ships inside the package: no field may carry a build-machine path.
if ($manifestJson -match '(?i)(^|[^a-z0-9])[a-z]:(\\\\|/)') { throw 'build-manifest.json would contain an absolute host path' }
$manifestJson | Set-Content -LiteralPath "$stage/build-manifest.json" -Encoding utf8
Copy-Item -LiteralPath "$PSScriptRoot/TESTAR_NO_PS4.txt" -Destination "$stage/TESTAR_NO_PS4.txt"
Copy-Item -LiteralPath "$PSScriptRoot/TUTORIAL.txt" -Destination "$stage/TUTORIAL.txt"
Push-Location $stage
try {
    Invoke-Checked "$tools/PkgTool.Core.exe" @('sfo_new', 'sce_sys/param.sfo')
    $entries = @(
        @('APP_TYPE','Integer','4','1'), @('APP_VER','Utf8','8',$sfoVersion),
        @('ATTRIBUTE','Integer','4','0'), @('CATEGORY','Utf8','4','gd'),
        @('CONTENT_ID','Utf8','48',$contentId), @('DOWNLOAD_DATA_SIZE','Integer','4','0'),
        @('SYSTEM_VER','Integer','4','0'), @('TITLE','Utf8','128',$appTitle),
        @('TITLE_ID','Utf8','12','RPCS30010'), @('VERSION','Utf8','8',$sfoVersion))
    foreach ($entry in $entries) {
        Invoke-Checked "$tools/PkgTool.Core.exe" @('sfo_setentry','sce_sys/param.sfo',$entry[0],
            '--type',$entry[1],'--maxsize',$entry[2],'--value',$entry[3])
    }
    $files = 'eboot.bin sce_sys/about/right.sprx sce_sys/param.sfo sce_sys/icon0.png sce_module/libc.prx sce_module/libSceFios2.prx fonts/DejaVuSans.ttf fonts/DejaVuSansMono.ttf LICENSE build-manifest.json TESTAR_NO_PS4.txt TUTORIAL.txt'
    $files += ' ' + (($uiResources + $licenseResources + $databaseResources) -join ' ')
    Invoke-Checked "$tools/create-gp4.exe" @('-out','native.gp4',"--content-id=$contentId",'--files',$files)
    # The SDK template lists only its sample directories. Add our font root
    # so the package builder can place the actual launcher resources.
    [xml]$project = Get-Content -LiteralPath 'native.gp4' -Raw
    $fontsDirectory = $project.CreateElement('dir')
    $fontsDirectory.SetAttribute('targ_name', 'fonts')
    $project.psproject.rootdir.AppendChild($fontsDirectory) | Out-Null
    foreach ($resource in ($uiResources + $licenseResources)) {
        $parent = $project.psproject.rootdir
        foreach ($directory in ($resource.Split('/') | Select-Object -SkipLast 1)) {
            $existing = @($parent.ChildNodes | Where-Object { $_.Name -eq 'dir' -and $_.GetAttribute('targ_name') -eq $directory })
            if ($existing.Count) { $parent = $existing[0] }
            else {
                $node = $project.CreateElement('dir')
                $node.SetAttribute('targ_name', $directory)
                $parent.AppendChild($node) | Out-Null
                $parent = $node
            }
        }
    }
    $project.Save((Join-Path $stage 'native.gp4'))
    Invoke-Checked "$tools/PkgTool.Core.exe" @('pkg_build','native.gp4',$work)
} finally { Pop-Location }
$built = Join-Path $work "$contentId.pkg"
Invoke-Checked "$tools/PkgTool.Core.exe" @('pkg_validate',$built)
# Read back the package's actual filesystem and compare the embedded EBOOT.
$verification = Join-Path $work "verify-$stamp"
Invoke-Checked "$tools/PkgTool.Core.exe" @('pkg_extract',$built,$verification)
$embeddedRoot = Join-Path $verification 'uroot'
$embedded = (Get-FileHash "$embeddedRoot/eboot.bin" -Algorithm SHA256).Hash
if ($embedded -ne $manifest.eboot_sha256) { throw 'Package contains a different EBOOT' }
# Metadata lives in the PKG entry table, outside the extracted PFS uroot.
# pkg_extractentry takes a table index; resolve it by name instead of using
# fixed indices so changes to the SDK's table order cannot break validation.
$packageEntries = & "$tools/PkgTool.Core.exe" pkg_listentries $built
if ($LASTEXITCODE) { throw 'Cannot read package metadata entries' }
New-Item -ItemType Directory -Force "$verification/sce_sys" | Out-Null
foreach ($metadata in @(@('ICON0_PNG','icon0.png'), @('PARAM_SFO','param.sfo'))) {
    $pattern = '^\S+\s+\S+\s+\S+\s+(\d+)\s+' + $metadata[0] + '$'
    $rows = @($packageEntries | Where-Object { $_ -match $pattern })
    if ($rows.Count -ne 1) { throw "Cannot locate package metadata: $($metadata[0])" }
    $index = [regex]::Match($rows[0], $pattern).Groups[1].Value
    Invoke-Checked "$tools/PkgTool.Core.exe" @('pkg_extractentry', $built, $index, "$verification/sce_sys/$($metadata[1])")
}
# PkgTool adds PUBTOOLINFO/PUBTOOLVER to the embedded SFO. Compare every
# user-supplied entry, allowing these generated publishing fields.
$stagedSfo = & "$tools/PkgTool.Core.exe" sfo_listentries "$stage/sce_sys/param.sfo"
if ($LASTEXITCODE) { throw 'Cannot inspect staged SFO' }
$embeddedSfo = & "$tools/PkgTool.Core.exe" sfo_listentries "$verification/sce_sys/param.sfo"
if ($LASTEXITCODE) { throw 'Cannot inspect embedded SFO' }
foreach ($entry in ($stagedSfo | Select-Object -Skip 1)) {
    if ($embeddedSfo -notcontains $entry) { throw "Package SFO entry differs: $entry" }
}
foreach ($resource in (@('sce_sys/icon0.png', 'fonts/DejaVuSans.ttf', 'fonts/DejaVuSansMono.ttf', 'build-manifest.json', 'LICENSE', 'TESTAR_NO_PS4.txt', 'TUTORIAL.txt') + $uiResources + $licenseResources + $databaseResources)) {
    $resourceRoot = if ($resource.StartsWith('sce_sys/')) { $verification } else { $embeddedRoot }
    if ((Get-FileHash "$resourceRoot/$resource" -Algorithm SHA256).Hash -ne
        (Get-FileHash "$stage/$resource" -Algorithm SHA256).Hash) {
        throw "Package resource does not match the staged build: $resource"
    }
}
$final = Join-Path $output "RPCS3_PS4_NATIVE_${Version}_$stamp.pkg"
Copy-Item -LiteralPath $built -Destination $final
Copy-Item -LiteralPath "$stage/build-manifest.json" -Destination "$output/build-manifest-$stamp.json"
Copy-Item -LiteralPath "$PSScriptRoot/TESTAR_NO_PS4.txt" -Destination "$output/TESTAR_NO_PS4.txt"
Copy-Item -LiteralPath "$PSScriptRoot/TUTORIAL.txt" -Destination "$output/TUTORIAL.txt"
$digest = (Get-FileHash $final -Algorithm SHA256).Hash
"$digest  $([IO.Path]::GetFileName($final))" | Set-Content -LiteralPath "$final.sha256" -Encoding ascii
Write-Output "Native package: $final"
Write-Output "Verified EBOOT SHA256: $embedded"
Write-Output "PKG SHA256: $digest"
