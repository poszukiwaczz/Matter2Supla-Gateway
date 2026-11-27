param(
    [ValidateSet('standard','singleusb')][string]$Variant = 'standard',
    [string]$SigningKey = 'M:\matter2supla\next-0.3\private\update-signing-key.pem',
    [string]$EspRoot = 'C:\esp',
    [switch]$UseConfiguredBuild,
    [switch]$CompileOnly   # build and verify only: no artifacts, no build number consumed
)
# One release path for the ESP32-S3 controller:
#   build -> sign -> hash -> manifest -> release notes, all from one source tree.
# Version/build numbers come only from firmware/controller/main/version.h; the
# single-USB variant is CONFIG_M2S_SINGLE_USB_RCP_OTA=y (+1 on FW_BUILD).
# Outputs go to build-artifacts/<version>-b<build>/ and never overwrite anything.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'firmware\controller'
$lab = [System.IO.Path]::GetFullPath((Join-Path $EspRoot 'lab-work'))
$work = Join-Path $lab "matter2supla-s3-$Variant"

# --- version -----------------------------------------------------------------
$versionText = Get-Content -LiteralPath (Join-Path $source 'main\version.h') -Raw
$base = [regex]::Match($versionText, '(?m)^#define FW_VERSION_BASE "([^"]+)"').Groups[1].Value
$baseBuild = [int][regex]::Match($versionText, '(?m)^#define FW_BUILD_BASE (\d+)').Groups[1].Value
if (!$base -or !$baseBuild) { throw 'version.h: missing FW_VERSION_BASE / FW_BUILD_BASE.' }
if ($Variant -eq 'singleusb') { $version = "$base-singleusb"; $build = $baseBuild + 1 } else { $version = $base; $build = $baseBuild }

# A compile-only run can verify a version that already has a release.
$release = Join-Path $root ("build-artifacts\{0}-b{1}" -f $version, $build)
if (!$CompileOnly) {
    Get-ChildItem -LiteralPath (Join-Path $root 'build-artifacts') -Filter manifest.json -Recurse -ErrorAction SilentlyContinue | ForEach-Object {
        $m = Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json
        if ($m.PSObject.Properties.Name -contains 'build' -and [int]$m.build -eq $build) {
            throw "FW_BUILD $build already released in $($_.DirectoryName). Raise FW_BUILD_BASE in version.h."
        }
        if ($m.version -eq $version) { throw "Version $version already released in $($_.DirectoryName)." }
    }
    if (Test-Path -LiteralPath $release) { throw "Output $release already exists." }
    if (!(Test-Path -LiteralPath $SigningKey)) { throw "Signing key not found: $SigningKey" }
}

$gitCommit = (& git -C $root rev-parse --short HEAD).Trim()
$gitDirty = [bool](& git -C $root status --porcelain -- firmware/controller)

# Build copies are generated from this checkout. Each variant owns its build cache.
New-Item -ItemType Directory -Path $work -Force | Out-Null
$scratchMain = Join-Path $work 'main'
if (Test-Path -LiteralPath $scratchMain) {
    foreach ($directory in @($lab, $work, $scratchMain)) {
        $item = Get-Item -LiteralPath $directory -Force
        if ($item.FullName -ne $directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
            throw "Unexpected scratch directory: $directory"
        }
    }
    Remove-Item -LiteralPath $scratchMain -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $source 'main') -Destination $scratchMain -Recurse -Force
$protocol = Join-Path $root 'firmware\rcp-ota-common'
$scratchProtocol = Join-Path $work 'rcp-ota-common'
New-Item -ItemType Directory -Path $scratchProtocol -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $protocol 'rcp_uart_ota_proto.h') -Destination $scratchProtocol -Force
foreach ($file in @('CMakeLists.txt','dependencies.lock','partitions_br.csv','sdkconfig','sdkconfig.defaults','sdkconfig.defaults.otbr','sdkconfig.defaults.tandem')) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $work $file) -Force
}
$configPath = Join-Path $work 'sdkconfig'
$configText = Get-Content -LiteralPath $configPath -Raw
$flagLine = if ($Variant -eq 'singleusb') { 'CONFIG_M2S_SINGLE_USB_RCP_OTA=y' } else { '# CONFIG_M2S_SINGLE_USB_RCP_OTA is not set' }
$configText = [regex]::Replace($configText, '(?m)^(?:# )?CONFIG_M2S_SINGLE_USB_RCP_OTA(?:=.*| is not set)\r?\n?', '')
$configText = $configText.TrimEnd() + "`n$flagLine`n"
[System.IO.File]::WriteAllText($configPath, $configText, [System.Text.UTF8Encoding]::new($false))

$compiledSources = @(Get-ChildItem -LiteralPath (Join-Path $source 'main'),$protocol -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($root.Length + 1)
    [ordered]@{ file = ($relative -replace '\\','/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})

$env:ESP_MATTER_PATH = (Join-Path $EspRoot 'esp-matter') -replace '\\','/'
$env:SUPLA_DEVICE_PATH = (Join-Path $EspRoot 'supla-device') -replace '\\','/'
$env:PATH = (Join-Path $EspRoot 'tools\gn') + ';' + $env:PATH
$env:CCACHE_DISABLE = '1'
. (Join-Path $EspRoot 'esp-idf\export.ps1')

Push-Location $work
try {
    if (!$UseConfiguredBuild) { & idf.py reconfigure; if ($LASTEXITCODE -ne 0) { throw 'reconfigure failed' } }
    $shellObject = Join-Path $work 'build\esp-idf\chip\obj\third_party\connectedhomeip\src\lib\shell\libCHIPShell.streamer_esp32.cpp.o'
    if (Test-Path -LiteralPath $shellObject) { Remove-Item -LiteralPath $shellObject -Force }
    foreach ($asset in @('index.html','app.js','theme.js','gateway.js','jsQR.js','update_public_key.pem')) {
        $generated = Join-Path $work ('build\' + $asset + '.S'); if (Test-Path -LiteralPath $generated) { Remove-Item -LiteralPath $generated -Force }
    }
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'Controller build failed' }
    $shellSymbols = & xtensa-esp32s3-elf-nm --undefined-only $shellObject
    if ($LASTEXITCODE -ne 0 -or ($shellSymbols -match 'usb_serial_jtag') -or !($shellSymbols -match 'uart_driver_install')) { throw 'Matter shell did not compile for the UART console' }
    $built = Get-Content -LiteralPath (Join-Path $work 'sdkconfig') -Raw
    if (($Variant -eq 'singleusb') -ne ($built -match '(?m)^CONFIG_M2S_SINGLE_USB_RCP_OTA=y')) { throw 'Variant flag lost during reconfigure.' }

    if ($CompileOnly) { Write-Host "Compile OK: $version (FW_BUILD $build, $Variant)"; return }
    foreach ($entry in $compiledSources) {
        if ((Get-FileHash -LiteralPath (Join-Path $root $entry.file) -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) { throw 'Project source changed during build; rebuild.' }
    }
    New-Item -ItemType Directory -Path $release -Force | Out-Null
    $prefix = "Matter2Supla-$version-S3-N16R8"
    Copy-Item 'build\controller.bin' (Join-Path $release "$prefix-APP.bin")
    Copy-Item 'build\bootloader\bootloader.bin' (Join-Path $release 's3-bootloader.bin')
    Copy-Item 'build\partition_table\partition-table.bin' (Join-Path $release 's3-partition-table.bin')
    Copy-Item 'build\controller.map' (Join-Path $release 'controller.map')
    Copy-Item 'build\controller.elf' (Join-Path $release 'controller.elf')
    Push-Location 'build'
    try {
        & python -m esptool --chip esp32s3 merge-bin -o (Join-Path $release "$prefix-FULL-FLASH-0x0.bin") '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw 'merge-bin failed' }
    } finally { Pop-Location }
    # Keep the source sdkconfig in sync only if actual non-BOM content differs (never dirty clean builds).
    $builtNorm = ([regex]::Replace($built, '(?m)^CONFIG_M2S_SINGLE_USB_RCP_OTA=y\r?$', '# CONFIG_M2S_SINGLE_USB_RCP_OTA is not set')).TrimStart([char]0xFEFF)
    $srcSdkPath = Join-Path $source 'sdkconfig'
    $srcNorm = (Get-Content -LiteralPath $srcSdkPath -Raw).TrimStart([char]0xFEFF)
    if ($builtNorm -ne $srcNorm) {
        [System.IO.File]::WriteAllText($srcSdkPath, $builtNorm, (New-Object System.Text.UTF8Encoding($false)))
    }
} finally {
    Pop-Location
}

$app = Join-Path $release "$prefix-APP.bin"
& python (Join-Path $PSScriptRoot 'Build-Update.py') $app (Join-Path $release "$prefix-OTA.m2s") --version $version --build $build --key $SigningKey
if ($LASTEXITCODE -ne 0) { throw 'Signing failed' }

$compiledSources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $release 'compiled-source-sha256.json') -Encoding utf8
$files = @(Get-ChildItem -LiteralPath $release -File | Where-Object { $_.Extension -in '.bin','.m2s' } | Sort-Object Name | ForEach-Object {
    [ordered]@{ file = $_.Name; bytes = $_.Length; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{
    version = $version; build = $build; variant = $Variant
    git_commit = $gitCommit; git_dirty = $gitDirty
    built_utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    target = 'ESP32-S3 N16R8 + ESP32-C6 RCP'
    hardware_tests = 'NOT RUN'
    firmware = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
$files | ForEach-Object { '{0}  {1}' -f $_.sha256, $_.file } | Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii

$notes = @("## $version (FW_BUILD $build, $Variant, commit $gitCommit$(if($gitDirty){'+dirty'}))", '', '| Plik | Bajty | SHA-256 |', '|---|---:|---|')
$notes += $files | ForEach-Object { '| `{0}` | {1} | `{2}` |' -f $_.file, $_.bytes, $_.sha256 }
$notes += '', 'Testy sprzetowe: NOT RUN (uzupelnij manifest.json po tescie dlugotrwalym).'
$notes | Set-Content -LiteralPath (Join-Path $release 'RELEASE-NOTES.md') -Encoding utf8
Write-Host "Release ready: $release"
if ($gitDirty) { Write-Warning 'Built from uncommitted firmware sources: commit before publishing this release.' }
