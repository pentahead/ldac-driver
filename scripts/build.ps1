<#
.SYNOPSIS
  Reproducible build of the user-mode stack (CMake + MSVC) and, if a WDK is present, the drivers.
.EXAMPLE
  .\scripts\build.ps1 -Config Release -Test
#>
param(
    [ValidateSet('Debug', 'Release', 'RelWithDebInfo')] [string]$Config = 'Release',
    [switch]$Test,
    [switch]$Clean,
    [switch]$Drivers
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$build = Join-Path $root 'build'

function Find-Tool([string]$name, [string[]]$candidates) {
    $c = Get-Command $name -ErrorAction SilentlyContinue
    if ($c) { return $c.Source }
    foreach ($p in $candidates) { if (Test-Path $p) { return $p } }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $vs = & $vswhere -latest -products * -property installationPath
        $hit = Get-ChildItem -Path $vs -Recurse -Filter "$name.exe" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($hit) { return $hit.FullName }
    }
    throw "$name not found (install Visual Studio 2022 with 'Desktop development with C++')"
}

$cmake = Find-Tool 'cmake' @("$env:ProgramFiles\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe",
                             "$env:ProgramFiles\CMake\bin\cmake.exe")
$ctest = Join-Path (Split-Path $cmake) 'ctest.exe'

foreach ($tp in 'third_party\libldac\src\ldacBT.c', 'third_party\libsbc\src\sbc.c') {
    if (-not (Test-Path (Join-Path $root $tp))) { throw "Missing $tp. Run scripts\fetch-deps.ps1 first." }
}

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
& $cmake -S $root -B $build -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE) { throw "CMake configure failed" }
& $cmake --build $build --config $Config --parallel
if ($LASTEXITCODE) { throw "Build failed" }
Write-Host "User-mode binaries: $build\bin\$Config" -ForegroundColor Green

if ($Test) {
    & $ctest --test-dir $build -C $Config --output-on-failure
    if ($LASTEXITCODE) { throw "Tests failed" }
}

if ($Drivers) {
    & (Join-Path $PSScriptRoot 'build-drivers.ps1') -Config $Config
}
