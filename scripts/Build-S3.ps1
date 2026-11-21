param([string]$EspRoot = 'C:\esp', [string]$OutputDirectory = 'build-artifacts\usb-uart-v2-thread-fix', [switch]$UseConfiguredBuild)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
# Development artifacts must never overwrite a release or an existing build.
$development = [IO.Path]::GetFullPath((Join-Path $root $OutputDirectory))
$rootPrefix = [IO.Path]::GetFullPath($root).TrimEnd('\') + '\'
$releasePrefix = [IO.Path]::GetFullPath((Join-Path $root 'releases')).TrimEnd('\') + '\'
if (!$development.StartsWith($rootPrefix,[StringComparison]::OrdinalIgnoreCase) -or
    $development.StartsWith($releasePrefix,[StringComparison]::OrdinalIgnoreCase)) { throw 'Development output must stay inside project and outside releases.' }
if (Test-Path -LiteralPath (Join-Path $development 's3-n16r8-controller-app.bin')) { throw 'S3 output already exists. Choose a new -OutputDirectory.' }
$versionText = Get-Content -LiteralPath (Join-Path $root 'firmware/controller/main/version.h') -Raw
$versionMatch = [regex]::Match($versionText, '(?m)^#define FW_VERSION "([^"]+)"')
if (!$versionMatch.Success) { throw 'Missing source firmware version.' }
$version = $versionMatch.Groups[1].Value

$source = Join-Path $root 'firmware\controller'
$work = Join-Path $EspRoot 'lab-work\controller-esp32s3-next03'
if (!(Test-Path (Join-Path $work 'sdkconfig')) -or !(Test-Path (Join-Path $work 'CMakeLists.txt'))) { throw 'Expected prepared S3 scratch project. See BUILD.md.' }
# Update only the scratch build tree; the original matter-c6-lab project is untouched.
$sourceMainPrefix = (Join-Path $source 'main').TrimEnd('\') + '\'
$sourcePrefix = $source.TrimEnd('\') + '\'
Get-ChildItem -LiteralPath (Join-Path $source 'main') -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($sourceMainPrefix.Length)
    $target = Join-Path (Join-Path $work 'main') $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    if (!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $_.FullName).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {
        Copy-Item -LiteralPath $_.FullName -Destination $target -Force
    }
}
foreach($file in @('CMakeLists.txt','partitions_br.csv','sdkconfig.defaults','sdkconfig.defaults.otbr','sdkconfig.defaults.tandem')) {
    $inputFile=Join-Path $source $file; $target=Join-Path $work $file
    if (!(Test-Path -LiteralPath $target) -or (Get-FileHash -LiteralPath $inputFile).Hash -ne (Get-FileHash -LiteralPath $target).Hash) {Copy-Item -LiteralPath $inputFile -Destination $target -Force}
}
$configPath = Join-Path $work 'sdkconfig'
$configText = Get-Content -LiteralPath (Join-Path $source 'sdkconfig') -Raw
$originalConfigText = Get-Content -LiteralPath $configPath -Raw
foreach($flag in @('CONFIG_ESP_HTTPS_SERVER_ENABLE','CONFIG_MBEDTLS_X509_CRT_WRITE_C','CONFIG_MBEDTLS_PEM_WRITE_C','CONFIG_MBEDTLS_PK_WRITE_C')) {
    $present = [regex]::Matches($configText, "(?m)^(?:# )?${flag}(?:=.*| is not set)\r?$")
    if($present.Count -eq 1 -and $present[0].Value.Trim() -eq "${flag}=y") {continue}
    $configText = [regex]::Replace($configText, "(?m)^(?:# )?${flag}(?:=.*| is not set)\r?\n?", '')
    $configText += "`n${flag}=y`n"
}
if($configText.TrimEnd() -ne $originalConfigText.TrimEnd()) {Set-Content -LiteralPath $configPath -Value $configText -Encoding utf8}
$compiledSources = @(Get-ChildItem -LiteralPath (Join-Path $source 'main') -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($sourcePrefix.Length)
    $scratchPath = Join-Path $work $relative
    [ordered]@{file=('firmware/controller/' + ($relative -replace '\\','/'));sha256=(Get-FileHash -LiteralPath $scratchPath -Algorithm SHA256).Hash.ToLowerInvariant()}
})
$compiledSources += [ordered]@{file='firmware/controller/CMakeLists.txt';sha256=(Get-FileHash -LiteralPath (Join-Path $source 'CMakeLists.txt') -Algorithm SHA256).Hash.ToLowerInvariant()}
$env:ESP_MATTER_PATH = (Join-Path $EspRoot 'esp-matter') -replace '\\','/'
$env:SUPLA_DEVICE_PATH = (Join-Path $EspRoot 'supla-device') -replace '\\','/'
$env:PATH = (Join-Path $EspRoot 'tools\gn') + ';' + $env:PATH
# Avoid stale cache locks when resuming a build after an interrupted session.
$env:CCACHE_DISABLE = '1'
. (Join-Path $EspRoot 'esp-idf\export.ps1')
Push-Location $work
try {
    # SRC_DIRS discovery must run again when new translation units are added.
    if (!$UseConfiguredBuild) {
        & idf.py reconfigure
        if ($LASTEXITCODE -ne 0) { throw 'Controller reconfigure failed' }
    }
    # CHIP's cached GN object predates sdkconfig changes in this scratch tree.
    # Rebuild the console adapter explicitly when switching USB pads to UART.
    $shellObject = Join-Path $work 'build\esp-idf\chip\obj\third_party\connectedhomeip\src\lib\shell\libCHIPShell.streamer_esp32.cpp.o'
    if (Test-Path -LiteralPath $shellObject) {Remove-Item -LiteralPath $shellObject -Force}
    # Embedded asset copies can retain timestamps older than generated assembly.
    # Force regeneration so the compiled-source manifest also describes the bytes served.
    foreach($asset in @('index.html','app.js','theme.js','gateway.js','jsQR.js','update_public_key.pem')) {
        $generated=Join-Path $work ('build\'+$asset+'.S')
        if(Test-Path -LiteralPath $generated){Remove-Item -LiteralPath $generated -Force}
    }
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw 'Controller build failed' }
    $shellSymbols = & xtensa-esp32s3-elf-nm --undefined-only $shellObject
    if ($LASTEXITCODE -ne 0 -or ($shellSymbols -match 'usb_serial_jtag') -or !($shellSymbols -match 'uart_driver_install')) {
        throw 'Matter shell did not compile for the UART console'
    }

    $release = $development
    New-Item -ItemType Directory -Path $release -Force | Out-Null
    foreach($entry in $compiledSources) {
        $relative = $entry.file.Substring('firmware/controller/'.Length)
        if((Get-FileHash -LiteralPath (Join-Path $work $relative) -Algorithm SHA256).Hash -ne $entry.sha256) {throw 'Scratch source changed during build; release stopped.'}
        if((Get-FileHash -LiteralPath (Join-Path $root $entry.file) -Algorithm SHA256).Hash -ne $entry.sha256) {throw 'Project source changed during build; rebuild before release.'}
    }
    $compiledSources | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $release 'compiled-source-sha256.json') -Encoding utf8
    Copy-Item -LiteralPath (Join-Path $work 'sdkconfig') -Destination (Join-Path $source 'sdkconfig') -Force
    Push-Location (Join-Path $work 'build')
    try {
        & python -m esptool --chip esp32s3 merge-bin -o (Join-Path $release 's3-n16r8-controller-merged.bin') '@flash_args'
        if ($LASTEXITCODE -ne 0) { throw 'Merge failed' }
        Copy-Item bootloader\bootloader.bin (Join-Path $release 's3-bootloader.bin') -Force
        Copy-Item partition_table\partition-table.bin (Join-Path $release 's3-partition-table.bin') -Force
        Copy-Item controller.bin (Join-Path $release 's3-n16r8-controller-app.bin') -Force
    } finally { Pop-Location }
} finally { Pop-Location }
$images = @(Get-ChildItem -LiteralPath $release -Filter '*.bin' | ForEach-Object {
    [ordered]@{ file=$_.Name; bytes=$_.Length; sha256=(Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
[ordered]@{version=$version; target='ESP32-S3 N16R8 + ESP32-C6 RCP'; hardware_tests='NOT RUN'; firmware=$images} |
    ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $release 'manifest.json') -Encoding utf8
$images | ForEach-Object { '{0}  {1}' -f $_.sha256,$_.file } |
    Set-Content -LiteralPath (Join-Path $release 'SHA256SUMS.txt') -Encoding ascii


