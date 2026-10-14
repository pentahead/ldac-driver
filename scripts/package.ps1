<#
.SYNOPSIS
  Creates dist\BtA2dpSrc-<version>.zip with binaries, scripts, docs and notices (and the driver
  package when it has been built). Development artifact - not a signed release.
#>
param([string]$Config = 'Release', [string]$Version = '0.1.0')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$stage = Join-Path $root "dist\BtA2dpSrc-$Version"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\bin", "$stage\scripts", "$stage\docs" | Out-Null
foreach ($f in 'bta2dp-service.exe', 'bta2dpctl.exe', 'bta2dp-config.exe', 'bta2dp-switch.exe', 'bta_tests.exe') {
    $p = Join-Path $root "build\bin\$Config\$f"
    if (-not (Test-Path $p)) { throw "missing $p - run scripts\build.ps1" }
    Copy-Item $p "$stage\bin"
}
$drv = Join-Path $root "build\driver\$Config\BtA2dpL2cap"
if (Test-Path "$drv\BtA2dpL2cap.sys") { Copy-Item -Recurse $drv "$stage\driver" } else { Write-Warning 'driver not built; package contains user-mode components only' }
Copy-Item (Join-Path $root 'scripts\install-dev.ps1'), (Join-Path $root 'scripts\uninstall-dev.ps1') "$stage\scripts"
Copy-Item (Join-Path $root 'docs\*.md') "$stage\docs"
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md'), (Join-Path $root 'IMPLEMENTATION_STATUS.md') $stage
Copy-Item (Join-Path $root 'third_party\libldac\LICENSE') "$stage\LICENSE.libldac.txt"
Copy-Item (Join-Path $root 'third_party\libldac\NOTICE') "$stage\NOTICE.libldac.txt"
Copy-Item (Join-Path $root 'third_party\libsbc\LICENSE') "$stage\LICENSE.libsbc.txt"
$zip = "$stage.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip
Get-FileHash $zip -Algorithm SHA256 | ForEach-Object { "$($_.Hash)  $(Split-Path $zip -Leaf)" } | Set-Content "$zip.sha256"
Write-Host "Package: $zip" -ForegroundColor Green
