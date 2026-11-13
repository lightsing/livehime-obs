#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$DependenciesRoot = (Join-Path $PSScriptRoot '..\.deps'),
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio",
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo',
    [string]$Generator = 'Visual Studio 17 2022',
    [ValidateRange(1, 32)][int]$Jobs = 1,
    [switch]$SkipBootstrap,
    [switch]$Offline,
    [switch]$SkipTests
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$DependenciesRoot = [IO.Path]::GetFullPath($DependenciesRoot)
if (!$SkipBootstrap) {
    & (Join-Path $PSScriptRoot 'bootstrap-deps.ps1') -DependenciesRoot $DependenciesRoot -ObsRoot $ObsRoot -Offline:$Offline
    & (Join-Path $PSScriptRoot 'bootstrap-goim-codecs.ps1') -DependenciesRoot (Join-Path $DependenciesRoot 'goim') -Offline:$Offline
}
$configureArgs = @('-S', $projectRoot, '-B', $BuildDirectory, '-G', $Generator, '-A', 'x64',
    "-DLIVEHIME_DEPS_ROOT=$DependenciesRoot", "-DOBS_ROOT=$ObsRoot", '-DBUILD_TESTING=ON')
& cmake @configureArgs
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed.' }
& cmake --build $BuildDirectory --config $Configuration --parallel $Jobs -- /nodeReuse:false
if ($LASTEXITCODE -ne 0) { throw 'Build failed.' }
if (!$SkipTests) {
    & ctest --test-dir $BuildDirectory -C $Configuration --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Tests failed.' }
}
Write-Host "Built: $(Join-Path $BuildDirectory "$Configuration\livehime-obs.dll")"
