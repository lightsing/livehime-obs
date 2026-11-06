#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio",
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$QtPluginPath = '',
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
if (!$QtPluginPath) { $QtPluginPath = Join-Path $ObsRoot 'data\obs-plugins\livehime-obs\qt-plugins' }
$QtPluginPath = [IO.Path]::GetFullPath($QtPluginPath)
if (!(Test-Path -LiteralPath (Join-Path $BuildDirectory 'CMakeCache.txt'))) { throw 'Run scripts/build.ps1 first.' }
if (!(Test-Path -LiteralPath $QtPluginPath)) { throw "Qt plugin directory is missing: $QtPluginPath" }
& cmake --build $BuildDirectory --config $Configuration --parallel 1 --target network-probe -- /nodeReuse:false
if ($LASTEXITCODE -ne 0) { throw 'Network probe build failed.' }
$originalPath = $env:PATH
try {
    $env:PATH = (Join-Path $ObsRoot 'bin\64bit') + ';' + $originalPath
    # Sends one anonymous HTTPS GET to nav. Never reads plugin session storage.
    & (Join-Path $BuildDirectory "$Configuration\network-probe.exe") $QtPluginPath
    if ($LASTEXITCODE -ne 0) { throw 'Anonymous HTTPS probe failed; inspect the safe diagnostic fields above.' }
} finally {
    $env:PATH = $originalPath
}
