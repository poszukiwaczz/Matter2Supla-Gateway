param([string]$EspRoot = 'C:\esp')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$source = Join-Path $root 'firmware-s3-controller'
$work = Join-Path $EspRoot 'lab-work\controller-esp32s3-next03'
$outDir = Join-Path $root 'do-wgrania'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null

# Sync main/ and config files into S3 scratch tree
$sourceMainPrefix = (Join-Path $source 'main').TrimEnd('\') + '\'
Get-ChildItem -LiteralPath (Join-Path $source 'main') -File -Recurse | ForEach-Object {
    $relative = $_.FullName.Substring($sourceMainPrefix.Length)
    $target = Join-Path (Join-Path $work 'main') $relative
    New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force | Out-Null
    Copy-Item -LiteralPath $_.FullName -Destination $target -Force
}
foreach ($file in @('CMakeLists.txt','partitions_br.csv','sdkconfig','sdkconfig.defaults','sdkconfig.defaults.otbr','sdkconfig.defaults.tandem')) {
    Copy-Item -LiteralPath (Join-Path $source $file) -Destination (Join-Path $work $file) -Force
}

$env:ESP_MATTER_PATH = (Join-Path $EspRoot 'esp-matter') -replace '\\','/'
$env:SUPLA_DEVICE_PATH = (Join-Path $EspRoot 'supla-device') -replace '\\','/'
$env:PATH = (Join-Path $EspRoot 'tools\gn') + ';' + $env:PATH
$env:CCACHE_DISABLE = '1'
. (Join-Path $EspRoot 'esp-idf\export.ps1')

Push-Location $work
try {
    & idf.py reconfigure
    if ($LASTEXITCODE -ne 0) { throw "idf.py reconfigure failed ($LASTEXITCODE)" }
    & idf.py build
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed ($LASTEXITCODE)" }

    $appBin = Join-Path $outDir 'Matter2Supla-0.4.1-singleusb-S3-N16R8-USB-UPDATE-0x10000.bin'
    $factoryBin = Join-Path $outDir 'Matter2Supla-0.4.1-singleusb-S3-N16R8-FACTORY.bin'
    Copy-Item "$work\build\controller.bin" $appBin -Force

    & python -m esptool --chip esp32s3 merge-bin -o $factoryBin `
        --flash-mode dio --flash-freq 80m --flash-size 16MB `
        0x0 "$work\build\bootloader\bootloader.bin" `
        0x8000 "$work\build\partition_table\partition-table.bin" `
        0x4a6000 "$work\build\ota_data_initial.bin" `
        0x10000 "$work\build\controller.bin"
    if ($LASTEXITCODE -ne 0) { throw "esptool merge-bin failed ($LASTEXITCODE)" }
    Write-Host "S3 0.4.1-singleusb build complete: $appBin & $factoryBin"
} finally {
    Pop-Location
    # Always restore shared S3 scratch tree back to Codex beta4
    $beta4 = 'k:\AI PROJEKTY\matter2supla\beta\0.3.2-beta4-dev\firmware\controller'
    Remove-Item "$work\main\rcp_uart_ota_host.cpp", "$work\main\rcp_uart_ota_proto.h" -Force -ErrorAction SilentlyContinue
    Copy-Item "$beta4\main\*" "$work\main\" -Recurse -Force
    foreach ($f in @('CMakeLists.txt','partitions_br.csv','sdkconfig','sdkconfig.defaults','sdkconfig.defaults.otbr','sdkconfig.defaults.tandem')) {
        Copy-Item "$beta4\$f" "$work\$f" -Force
    }
    Write-Host "Restored C:\esp\lab-work\controller-esp32s3-next03 back to beta4."
}
