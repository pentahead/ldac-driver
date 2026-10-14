<#
.SYNOPSIS
  Fetches the third-party codec sources at pinned, reviewed commits (Apache-2.0).
#>
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$deps = @(
    @{ Name = 'libldac'; Url = 'https://android.googlesource.com/platform/external/libldac'; Commit = 'eeee1a3f5f8df1282e3a6d297085885fd886737b' },
    @{ Name = 'libsbc';  Url = 'https://github.com/google/libsbc';                           Commit = '6e505650145c9973d08a0bdd5e5f5e1914305e40' }
)
foreach ($d in $deps) {
    $dir = Join-Path $root "third_party\$($d.Name)"
    if (-not (Test-Path (Join-Path $dir '.git'))) {
        git clone $d.Url $dir
        if ($LASTEXITCODE) { throw "clone of $($d.Name) failed" }
    }
    git -C $dir fetch --depth 1 origin $d.Commit 2>$null
    git -C $dir checkout --quiet $d.Commit
    if ($LASTEXITCODE) { throw "checkout of $($d.Name)@$($d.Commit) failed" }
    $head = git -C $dir rev-parse HEAD
    if ($head -ne $d.Commit) { throw "$($d.Name) is at $head, expected $($d.Commit)" }
    Write-Host "$($d.Name) @ $head" -ForegroundColor Green
}
