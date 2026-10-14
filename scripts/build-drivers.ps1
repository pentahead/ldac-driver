<#
.SYNOPSIS
  Builds the kernel driver(s) with MSBuild + WDK and creates a test-signed package (dev only).
.DESCRIPTION
  Requires: Visual Studio 2022 + Windows Driver Kit (WDK) with the VS extension, OR the WDK NuGet
  packages restored by scripts\fetch-wdk-nuget.ps1. Produces build\driver\<Config>\BtA2dpL2cap\
  (sys, inf, cat). Signing uses a locally generated test certificate; it is NOT suitable for
  distribution (see docs\SIGNING.md for attestation signing).
#>
param(
    [ValidateSet('Debug', 'Release')] [string]$Config = 'Release',
    [switch]$TestSign
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$msbuild = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -find 'MSBuild\**\Bin\MSBuild.exe' | Select-Object -First 1
if (-not $msbuild) { throw 'MSBuild not found' }

$kits = "${env:ProgramFiles(x86)}\Windows Kits\10"
$wdkHeader = Get-ChildItem "$kits\Include\*\km\wdm.h" -ErrorAction SilentlyContinue | Select-Object -First 1
$nugetWdk = Get-ChildItem (Join-Path $root 'packages\Microsoft.Windows.WDK.x64*') -Directory -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $wdkHeader -and -not $nugetWdk) {
    Write-Warning @"
No Windows Driver Kit found. The driver sources are in src\driver but cannot be compiled here.
Options:
  1. Install the WDK (winget install Microsoft.WindowsWDK.10.0.26100) and the WDK VS extension.
  2. Run scripts\fetch-wdk-nuget.ps1 (project-local NuGet WDK; no system install).
  3. Push the repo to GitHub; .github\workflows\build.yml builds the driver in the cloud.
"@
    exit 3
}

$out = Join-Path $root "build\driver\$Config"
$proj = Join-Path $root 'src\driver\l2cap\BtA2dpL2cap.vcxproj'
$extra = @()
if ($nugetWdk -and -not $wdkHeader) { $extra += "/p:WDKContentRoot=$($nugetWdk.FullName)\c\" }
& $msbuild $proj /p:Configuration=$Config /p:Platform=x64 "/p:OutDir=$out\\" /p:SignMode=Off /m @extra
if ($LASTEXITCODE) { throw 'driver build failed' }

$pkg = Join-Path $out 'BtA2dpL2cap'
if (-not (Test-Path $pkg)) { New-Item -ItemType Directory $pkg | Out-Null }
Copy-Item (Join-Path $out 'BtA2dpL2cap.sys') $pkg -Force
Copy-Item (Join-Path $out 'BtA2dpL2cap.inf') $pkg -Force -ErrorAction SilentlyContinue
if (-not (Test-Path (Join-Path $pkg 'BtA2dpL2cap.inf'))) { Copy-Item (Join-Path $root 'src\driver\l2cap\BtA2dpL2cap.inf') $pkg }

$bin = Split-Path (Get-ChildItem "$kits\bin\*\x64\inf2cat.exe" -ErrorAction SilentlyContinue | Select-Object -Last 1).FullName
if ($bin) {
    & "$bin\inf2cat.exe" /driver:$pkg /os:10_X64 | Out-Host
    if ($TestSign) {
        $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object Subject -eq 'CN=BtA2dpSrc Test Signing' | Select-Object -First 1
        if (-not $cert) {
            $cert = New-SelfSignedCertificate -Subject 'CN=BtA2dpSrc Test Signing' -Type CodeSigningCert -CertStoreLocation Cert:\CurrentUser\My
        }
        & "$bin\signtool.exe" sign /fd sha256 /sha1 $cert.Thumbprint (Join-Path $pkg 'BtA2dpL2cap.sys') (Join-Path $pkg 'BtA2dpL2cap.cat')
        Export-Certificate -Cert $cert -FilePath (Join-Path $pkg 'BtA2dpSrcTest.cer') | Out-Null
        Write-Host "Test-signed. Trust BtA2dpSrcTest.cer only on a dedicated test machine with test signing enabled." -ForegroundColor Yellow
    }
}
Write-Host "Driver package: $pkg" -ForegroundColor Green
