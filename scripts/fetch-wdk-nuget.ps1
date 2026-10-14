<#
.SYNOPSIS
  OPT-IN: restores the Windows Driver Kit as project-local NuGet packages into .\packages
  (no system-wide installation; delete the folder to remove). Downloads several hundred MB.
#>
param([string]$Version = '10.0.26100.2454')
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$pkgDir = Join-Path $root 'packages'
$nuget = Join-Path $pkgDir 'nuget.exe'
New-Item -ItemType Directory -Force $pkgDir | Out-Null
if (-not (Test-Path $nuget)) {
    Invoke-WebRequest 'https://dist.nuget.org/win-x86-commandline/latest/nuget.exe' -OutFile $nuget
}
foreach ($p in 'Microsoft.Windows.SDK.CPP', 'Microsoft.Windows.SDK.CPP.x64', 'Microsoft.Windows.WDK.x64') {
    & $nuget install $p -Version $Version -OutputDirectory $pkgDir -NonInteractive
    if ($LASTEXITCODE) { throw "nuget install $p failed" }
}
Write-Host "WDK restored under $pkgDir. Now run scripts\build-drivers.ps1." -ForegroundColor Green
