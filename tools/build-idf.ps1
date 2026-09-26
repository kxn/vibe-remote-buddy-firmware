param([Parameter(Mandatory=$true)][string]$IdfPath,[ValidateSet("q2","o8","q2-f4")][string]$Variant="o8")
$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $IdfPath 'export.ps1')
if ($LASTEXITCODE -ne 0) { throw 'ESP-IDF environment activation failed' }
& python (Join-Path $IdfPath 'tools/idf.py') -C (Join-Path $repoRoot 'firmware/esp32s3') -B (Join-Path $repoRoot "build/esp32s3-$Variant") "-DSDKCONFIG=$repoRoot/build/esp32s3-$Variant/sdkconfig" "-DSDKCONFIG_DEFAULTS=$repoRoot/firmware/esp32s3/sdkconfig.defaults;$repoRoot/firmware/esp32s3/sdkconfig.$Variant.defaults" "-DBUDDY_VARIANT=$Variant" -DS3_HCI_PROBE=OFF -DS3_USB_QUALIFY=OFF -DS3_CODEC_METRICS=OFF build
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
