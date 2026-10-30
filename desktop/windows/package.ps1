# Make the distributable zip from desktop\build\dist (run build.bat first).
#   powershell -ExecutionPolicy Bypass -File package.ps1 -Version 0.1.0 [-Out <folder>]
param([Parameter(Mandatory = $true)][string]$Version, [string]$Out)
$ErrorActionPreference = 'Stop'
$dist = Join-Path $PSScriptRoot '..\build\dist'
if (-not $Out) { $Out = Join-Path $PSScriptRoot '..\build' }
$name = "shunti-IME-windows-$Version"
$stage = Join-Path $env:TEMP $name
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage | Out-Null
$files = 'install.bat', 'uninstall.bat', 'install.ps1', 'README.txt', 'LICENSE.txt', 'shunti_settings.exe', 'shunti_kana_fix.exe',
         'shunti_ime_x64.dll', 'shunti_ime_x86.dll', 'kkc_lex.bin', 'kkc_model.bin', 'kkc_variants.tsv', 'kkc_register.bin', 'kkc_model_light.bin'
foreach ($f in $files) { Copy-Item (Join-Path $dist $f) (Join-Path $stage $f) }
$zip = Join-Path $Out "$name.zip"
if (Test-Path $zip) { Remove-Item $zip -Force }
Compress-Archive -Path $stage -DestinationPath $zip -CompressionLevel Optimal
Remove-Item $stage -Recurse -Force
Get-Item $zip | Select-Object FullName, Length
