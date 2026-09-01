$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if (-not $env:VCPKG_ROOT) {
    throw "VCPKG_ROOT must name the CI vcpkg checkout"
}

$manifest = Get-Content (Join-Path $env:GITHUB_WORKSPACE "vcpkg.json") -Raw |
    ConvertFrom-Json
$manifestBaseline = $manifest.'builtin-baseline'

if ($env:VCPKG_BASELINE -and $env:VCPKG_BASELINE -ne $manifestBaseline) {
    throw "VCPKG_BASELINE ($env:VCPKG_BASELINE) disagrees with vcpkg.json ($manifestBaseline)"
}

git init --quiet $env:VCPKG_ROOT
git -C $env:VCPKG_ROOT remote add origin https://github.com/microsoft/vcpkg.git
git -C $env:VCPKG_ROOT fetch --quiet --depth 1 origin $manifestBaseline
git -C $env:VCPKG_ROOT checkout --quiet --detach FETCH_HEAD

$actualBaseline = (git -C $env:VCPKG_ROOT rev-parse HEAD).Trim()
if ($actualBaseline -ne $manifestBaseline) {
    throw "vcpkg checkout is $actualBaseline, expected $manifestBaseline"
}

& (Join-Path $env:VCPKG_ROOT "bootstrap-vcpkg.bat") -disableMetrics
if ($LASTEXITCODE -ne 0) {
    throw "vcpkg bootstrap failed with exit code $LASTEXITCODE"
}
