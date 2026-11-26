param([string]$EspRoot = 'C:\esp')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$src = Join-Path $root 'firmware\c6-rcp-ota'
$work = Join-Path $EspRoot 'lab-work\c6-rcp-2xota-scratch'
$outDir = Join-Path $root 'build-artifacts\c6-rcp-2xota'
$vh = Get-Content -LiteralPath (Join-Path $src 'main/rcp_ota_version.h') -Raw
$rcpVersion = [regex]::Match($vh, 'RCP_OTA_VERSION\s+"([^"]+)"').Groups[1].Value
if (!$rcpVersion) { throw 'rcp_ota_version.h: missing RCP_OTA_VERSION' }
New-Item -ItemType Directory -Path $work, $outDir -Force | Out-Null

# Copy project into clean scratch tree
Copy-Item "$src\CMakeLists.txt", "$src\partitions.csv", "$src\sdkconfig.defaults", "$src\sdkconfig.defaults.esp32c6" $work -Force
New-Item -ItemType Directory -Path "$work\main" -Force | Out-Null
Copy-Item "$src\main\*" "$work\main\" -Recurse -Force

$env:CCACHE_DISABLE = '1'
. (Join-Path $EspRoot 'esp-idf\export.ps1')

Push-Location $work
try {
    Remove-Item "$work\sdkconfig" -Force -ErrorAction SilentlyContinue
    & idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.defaults.esp32c6" set-target esp32c6
    if ($LASTEXITCODE -ne 0) { throw "idf.py set-target esp32c6 failed ($LASTEXITCODE)" }
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed ($LASTEXITCODE)" }

    $appBin = Join-Path $outDir "Matter2Supla-RCP-C6-$rcpVersion-APP.bin"
    $factoryBin = Join-Path $outDir "Matter2Supla-RCP-C6-$rcpVersion-FULL-FLASH-0x0.bin"
    Copy-Item "$work\build\esp_ot_rcp.bin" $appBin -Force

    & python -m esptool --chip esp32c6 merge-bin -o $factoryBin `
        --flash-mode dio --flash-freq 80m --flash-size 4MB `
        0x0 "$work\build\bootloader\bootloader.bin" `
        0x8000 "$work\build\partition_table\partition-table.bin" `
        0x10000 "$work\build\ota_data_initial.bin" `
        0x20000 "$work\build\esp_ot_rcp.bin"
    if ($LASTEXITCODE -ne 0) { throw "esptool merge-bin failed ($LASTEXITCODE)" }
    Write-Host "C6 2xOTA RCP build complete: $appBin & $factoryBin"
} finally {
    Pop-Location
}
