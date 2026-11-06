#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$DependenciesRoot = (Join-Path $PSScriptRoot '..\.deps'),
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio",
    [string]$VisualStudioRoot = '',
    [switch]$Offline
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$DependenciesRoot = [IO.Path]::GetFullPath($DependenciesRoot)
$ObsRoot = [IO.Path]::GetFullPath($ObsRoot)
$lock = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'deps\dependencies.lock.json') | ConvertFrom-Json
New-Item -ItemType Directory -Force -Path $DependenciesRoot | Out-Null

foreach ($artifact in $lock.artifacts) {
    $archive = Join-Path $DependenciesRoot $artifact.file
    if (!(Test-Path -LiteralPath $archive)) {
        if ($Offline) { throw "Missing cached dependency: $archive" }
        Write-Host "Downloading $($artifact.id)..."
        Invoke-WebRequest -Uri $artifact.url -OutFile $archive
    }
    $actualHash = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($actualHash -ne $artifact.sha256) { throw "SHA256 mismatch for $archive. Expected $($artifact.sha256); got $actualHash." }
    $marker = Join-Path $DependenciesRoot $artifact.marker
    $stamp = Join-Path $DependenciesRoot ('.' + $artifact.id + '.sha256')
    $stampedHash = if (Test-Path -LiteralPath $stamp) { (Get-Content -Raw -LiteralPath $stamp).Trim() } else { '' }
    if (!(Test-Path -LiteralPath $marker) -or $stampedHash -ne $artifact.sha256) {
        $destination = Join-Path $DependenciesRoot $artifact.extractDirectory
        New-Item -ItemType Directory -Force -Path $destination | Out-Null
        [IO.Compression.ZipFile]::ExtractToDirectory($archive, $destination, $true)
        if (!(Test-Path -LiteralPath $marker)) { throw "Dependency extraction incomplete: $marker" }
        Set-Content -LiteralPath $stamp -Value $artifact.sha256 -Encoding ascii
    }
}

# Only official public headers and the user's matching OBS DLL exports are used.
# No OBS or frontend implementation is rebuilt or redistributed in this SDK.
if (!$VisualStudioRoot) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudioRoot = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}
if (!$VisualStudioRoot) { throw 'Visual Studio C++ x64 tools were not found. Install the Desktop development with C++ workload.' }
$compilerDirectory = Get-ChildItem -LiteralPath (Join-Path $VisualStudioRoot 'VC\Tools\MSVC') -Directory |
    Sort-Object { [version]$_.Name } -Descending | Select-Object -First 1
if (!$compilerDirectory) { throw "No MSVC tools in $VisualStudioRoot" }
$toolRoot = Join-Path $compilerDirectory.FullName 'bin\Hostx64\x64'
$dumpbin = Join-Path $toolRoot 'dumpbin.exe'
$lib = Join-Path $toolRoot 'lib.exe'
$sdkRoot = Join-Path $DependenciesRoot "obs-sdk-$($lock.obsVersion)-x64"
New-Item -ItemType Directory -Force -Path (Join-Path $sdkRoot 'lib'), (Join-Path $sdkRoot 'include') | Out-Null

@'
#pragma once
/* Consumer-only configuration for official Windows OBS public headers. */
#define OBS_DATA_PATH "data"
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins/64bit"
#define OBS_RELEASE_CANDIDATE 0
#define OBS_BETA 0
'@ | Set-Content -LiteralPath (Join-Path $sdkRoot 'include\obsconfig.h') -Encoding ascii

$dllManifest = @()
$qtRuntimeManifest = @()
foreach ($name in @('Qt6Core', 'Qt6Network')) {
    $qtDll = Join-Path $ObsRoot "bin\64bit\$name.dll"
    if (!(Test-Path -LiteralPath $qtDll)) { throw "Missing OBS Qt runtime: $qtDll" }
    $qtVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo($qtDll).ProductVersion
    if ($qtVersion -notmatch ('^' + [regex]::Escape($lock.qtVersion) + '($|[.+ -])')) {
        throw "The private TLS backend requires Qt $($lock.qtVersion); $qtDll reports $qtVersion."
    }
    $qtRuntimeManifest += [ordered]@{
        file = $qtDll; version = $qtVersion
        sha256 = (Get-FileHash -LiteralPath $qtDll -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
foreach ($name in @('obs', 'obs-frontend-api')) {
    $dll = Join-Path $ObsRoot "bin\64bit\$name.dll"
    if (!(Test-Path -LiteralPath $dll)) { throw "Missing OBS runtime DLL: $dll" }
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($dll).ProductVersion
    if ($version -notmatch ('^' + [regex]::Escape($lock.obsVersion) + '($|[.+ -])')) {
        throw "This dependency lock requires OBS $($lock.obsVersion); $dll reports $version."
    }
    $rawExports = & $dumpbin /NOLOGO /EXPORTS $dll
    if ($LASTEXITCODE -ne 0) { throw "dumpbin failed for $dll" }
    $exports = @($rawExports | ForEach-Object {
        if ($_ -match '^\s+\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)') { $Matches[1] }
    })
    if ($exports.Count -lt 20) { throw "Unexpectedly few exports in $dll" }
    $def = Join-Path $sdkRoot "lib\$name.def"
    @("LIBRARY $name.dll", 'EXPORTS') + $exports | Set-Content -LiteralPath $def -Encoding ascii
    & $lib /NOLOGO /MACHINE:X64 "/DEF:$def" "/OUT:$(Join-Path $sdkRoot "lib\$name.lib")"
    if ($LASTEXITCODE -ne 0) { throw "Import library generation failed for $name" }
    $dllManifest += [ordered]@{
        file = $dll; version = $version; sha256 = (Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash.ToLowerInvariant()
        importLibrary = "$name.lib"; exportCount = $exports.Count
    }
}
$manifest = [ordered]@{
    schema = 1; kind = 'public-header-sdk-with-import-libraries-from-installed-obs'
    obsVersion = $lock.obsVersion; qtVersion = $lock.qtVersion
    compiler = $compilerDirectory.Name; visualStudioRoot = $VisualStudioRoot
    sourceArchiveSha256 = ($lock.artifacts | Where-Object id -EQ 'obs-source').sha256
    dlls = $dllManifest; qtRuntime = $qtRuntimeManifest
}
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $sdkRoot 'sdk-manifest.json') -Encoding utf8
Write-Host "Dependencies ready: $DependenciesRoot"
