<#
.SYNOPSIS
  Rollback/uninstall: restores every device switched by BtA2dpSrc to its journaled previous driver
  (in-box Windows A2DP by default), removes the BtA2dpL2cap package from the driver store, removes the
  service and program files. Settings are kept unless -PurgeSettings.
#>
param([switch]$PurgeSettings, [switch]$Force)
$ErrorActionPreference = 'Stop'
$admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
if (-not $admin) { throw 'Run from an elevated PowerShell prompt.' }
$dest = Join-Path $env:ProgramFiles 'BtA2dpSrc'
$switch = Join-Path $dest 'bta2dp-switch.exe'
if (-not (Test-Path $switch)) { $switch = Join-Path (Split-Path -Parent $PSScriptRoot) 'build\bin\Release\bta2dp-switch.exe' }
$journal = Join-Path $env:ProgramData 'BtA2dpSrc\driver-journal.json'

if (-not $Force) {
    $ans = Read-Host 'Restore devices and remove BtA2dpSrc? Type YES'
    if ($ans -ne 'YES') { exit 1 }
}

# 1. Restore devices FIRST so no headset is left without a working Windows driver.
if (Test-Path $journal) {
    $j = Get-Content $journal -Raw | ConvertFrom-Json
    foreach ($addr in $j.PSObject.Properties.Name) {
        Write-Host "Restoring $addr ..."
        & $switch restore $addr
        if ($LASTEXITCODE) { Write-Warning "restore of $addr failed; run '$switch restore $addr' manually" }
    }
}

# 2. Remove the service.
if (Get-Service BtA2dpSrc -ErrorAction SilentlyContinue) {
    Stop-Service BtA2dpSrc -Force -ErrorAction SilentlyContinue
    & (Join-Path $dest 'bta2dp-service.exe') uninstall
}

# 3. Remove our driver package(s) from the driver store (only packages whose original name is ours).
$enum = pnputil /enum-drivers
$current = $null
foreach ($line in $enum) {
    if ($line -match 'Published Name\s*:\s*(oem\d+\.inf)') { $current = $Matches[1] }
    if ($line -match 'Original Name\s*:\s*bta2dpl2cap\.inf' -and $current) {
        Write-Host "Removing driver package $current"
        pnputil /delete-driver $current /uninstall
        $current = $null
    }
}

# 4. Files.
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
if ($PurgeSettings) { Remove-Item -Recurse -Force (Join-Path $env:ProgramData 'BtA2dpSrc') -ErrorAction SilentlyContinue }
Write-Host 'BtA2dpSrc removed.' -ForegroundColor Green
