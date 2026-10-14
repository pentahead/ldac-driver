<#
.SYNOPSIS
  Development installation on a DEDICATED TEST MACHINE. Never run automatically.
.DESCRIPTION
  1. Copies binaries to %ProgramFiles%\BtA2dpSrc and registers the BtA2dpSrc service (manual start).
  2. Optionally (-Device) switches ONE headset's A2DP driver to BtA2dpL2cap (journaled; reversible
     with uninstall-dev.ps1 or 'bta2dp-switch restore <addr>').
  Pre-conditions checked (the script refuses otherwise):
    - elevated prompt
    - for -Device: a built driver package, and either an attestation-signed package or test signing
      already enabled by YOU (bcdedit /set testsigning on; requires Secure Boot off on most PCs).
  The current Windows audio output for other devices is not touched.
.EXAMPLE
  .\scripts\install-dev.ps1                       # service + tools only
  .\scripts\install-dev.ps1 -Device 41:42:FF:BF:DB:EE
#>
param(
    [string]$Config = 'Release',
    [string]$Device,
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bin = Join-Path $root "build\bin\$Config"
$dest = Join-Path $env:ProgramFiles 'BtA2dpSrc'

$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run from an elevated PowerShell prompt.' }
foreach ($f in 'bta2dp-service.exe', 'bta2dpctl.exe', 'bta2dp-config.exe', 'bta2dp-switch.exe') {
    if (-not (Test-Path (Join-Path $bin $f))) { throw "Missing $f; run scripts\build.ps1 first." }
}

Write-Host "This installs BtA2dpSrc (development build) to $dest." -ForegroundColor Cyan
if ($Device) { Write-Host "It will ALSO replace the Windows A2DP driver for $Device (its normal Windows audio endpoint disappears until restored)." -ForegroundColor Yellow }
if (-not $Force) {
    $ans = Read-Host 'Type YES to continue'
    if ($ans -ne 'YES') { Write-Host 'Aborted.'; exit 1 }
}

# Stop a running instance before replacing files (upgrade path).
$svc = Get-Service BtA2dpSrc -ErrorAction SilentlyContinue
if ($svc -and $svc.Status -ne 'Stopped') { Stop-Service BtA2dpSrc -Force }
New-Item -ItemType Directory -Force $dest | Out-Null
Copy-Item (Join-Path $bin '*.exe') $dest -Force
Copy-Item (Join-Path $root 'THIRD_PARTY_NOTICES.md') $dest -Force
if (-not $svc) {
    & (Join-Path $dest 'bta2dp-service.exe') install
    if ($LASTEXITCODE) { throw 'service registration failed' }
}
Write-Host "Service installed (manual start): sc start BtA2dpSrc" -ForegroundColor Green

if ($Device) {
    $pkg = Join-Path $root "build\driver\$Config\BtA2dpL2cap"
    $inf = Join-Path $pkg 'BtA2dpL2cap.inf'
    if (-not (Test-Path (Join-Path $pkg 'BtA2dpL2cap.sys'))) { throw "Driver not built ($pkg). See docs\DRIVER.md." }
    $sig = Get-AuthenticodeSignature (Join-Path $pkg 'BtA2dpL2cap.sys')
    $testsigning = (bcdedit /enum '{current}' | Select-String -Pattern 'testsigning\s+Yes') -ne $null
    if ($sig.Status -ne 'Valid' -and -not $testsigning) {
        throw "Driver is not production-signed and test signing is off. Enable test signing yourself on a test machine (docs\SIGNING.md); this script will not change boot configuration."
    }
    & (Join-Path $dest 'bta2dp-switch.exe') use $Device $inf
    if ($LASTEXITCODE) { throw 'driver switch failed (device left unchanged or journaled for restore)' }
    Write-Host "Device $Device now uses BtA2dpL2cap. Restore with: bta2dp-switch restore $Device" -ForegroundColor Green
}
