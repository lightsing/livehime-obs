#requires -Version 7.0
<#
.SYNOPSIS
Prepares the pinned official OBS Windows runtime for local or CI builds.
.DESCRIPTION
Returns exactly one string on the success stream: the absolute ObsRoot path.
Downloads are cached in DependenciesRoot. The official ZIP is size/SHA-256
checked before extraction into a new directory. No OBS process is launched,
no installer is run, and no existing OBS installation or profile is modified.
Offline mode uses a verified extracted runtime or the cached official ZIP.
.EXAMPLE
$obs = & .\scripts\bootstrap-obs.ps1
& .\scripts\build.ps1 -ObsRoot $obs
.EXAMPLE
$obs = & .\scripts\bootstrap-obs.ps1 -DependenciesRoot D:\build-deps -Offline
#>
[CmdletBinding()]
param(
    [string]$DependenciesRoot = (Join-Path $PSScriptRoot '..\.deps'),
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitProcess) {
    throw 'OBS runtime bootstrap requires 64-bit Windows and PowerShell 7 or newer.'
}
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lockPath = Join-Path $projectRoot 'deps\obs-runtime.lock.json'
$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
$dependencies = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'deps\dependencies.lock.json') | ConvertFrom-Json
if ($lock.schema -ne 1 -or $lock.id -ne 'obs-runtime' -or $lock.platform -ne 'windows-x64' -or
    $lock.obsVersion -notmatch '^\d+\.\d+\.\d+$' -or $lock.qtVersion -notmatch '^\d+\.\d+\.\d+$' -or
    $lock.obsVersion -ne $dependencies.obsVersion -or $lock.qtVersion -ne $dependencies.qtVersion -or
    $lock.installDirectory -ne "obs-runtime-$($lock.obsVersion)" -or
    $lock.artifact.file -ne "OBS-Studio-$($lock.obsVersion)-Windows-x64.zip" -or
    $lock.artifact.url -ne "https://github.com/obsproject/obs-studio/releases/download/$($lock.obsVersion)/$($lock.artifact.file)" -or
    $lock.artifact.sha256 -cnotmatch '^[0-9a-f]{64}$' -or $lock.artifact.size -le 0 -or
    $lock.sources.digestField -cne "sha256:$($lock.artifact.sha256)") {
    throw "Invalid or incompatible OBS runtime lock: $lockPath"
}
$DependenciesRoot = [IO.Path]::GetFullPath($DependenciesRoot)
if ($DependenciesRoot -notmatch '^[A-Za-z]:\\' -or $DependenciesRoot -match '["\r\n]') {
    throw 'DependenciesRoot must be a local absolute directory without quotes or line breaks.'
}
if ($DependenciesRoot.TrimEnd('\') -eq [IO.Path]::GetPathRoot($DependenciesRoot).TrimEnd('\')) {
    throw 'DependenciesRoot must be a dedicated dependency directory, not a drive root.'
}
$installRoot = Join-Path $DependenciesRoot $lock.installDirectory
$archivePath = Join-Path $DependenciesRoot $lock.artifact.file
$stampName = '.bootstrap-obs.json'
$lockHash = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()
$runtimeVersions = [ordered]@{
    'bin/64bit/obs64.exe' = $lock.obsVersion
    'bin/64bit/obs.dll' = $lock.obsVersion
    'bin/64bit/obs-frontend-api.dll' = $lock.obsVersion
    'bin/64bit/Qt6Core.dll' = $lock.qtVersion
    'bin/64bit/Qt6Network.dll' = $lock.qtVersion
    'bin/64bit/Qt6Gui.dll' = $lock.qtVersion
    'bin/64bit/Qt6Widgets.dll' = $lock.qtVersion
}

function Assert-OwnedPath([string]$Path) {
    $fullPath = [IO.Path]::GetFullPath($Path)
    $prefix = $DependenciesRoot.TrimEnd('\') + '\'
    if (!$fullPath.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to write outside DependenciesRoot: $fullPath"
    }
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Archive([string]$Path) {
    $item = Get-Item -LiteralPath $Path
    if ($item.Length -ne $lock.artifact.size) {
        throw "OBS ZIP size mismatch: $Path. Expected $($lock.artifact.size), got $($item.Length)."
    }
    $hash = Get-Sha256 $Path
    if ($hash -cne $lock.artifact.sha256) {
        throw "OBS ZIP SHA256 mismatch: $Path. Expected $($lock.artifact.sha256), got $hash."
    }
}

function Get-RuntimeIdentity([string]$Root) {
    $identities = @()
    foreach ($relative in $runtimeVersions.Keys) {
        $path = Join-Path $Root $relative
        if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Incomplete OBS runtime: $path" }
        if ((Get-Item -LiteralPath $path).Attributes -band [IO.FileAttributes]::ReparsePoint) {
            throw "Runtime verification does not accept a linked file: $path"
        }
        $stream = [IO.File]::OpenRead($path)
        $reader = [IO.BinaryReader]::new($stream)
        try {
            if ($stream.Length -lt 64 -or $reader.ReadUInt16() -ne 0x5a4d) { throw "Invalid PE file: $path" }
            $stream.Position = 0x3c
            $peOffset = $reader.ReadInt32()
            if ($peOffset -lt 64 -or $peOffset -gt $stream.Length - 6) { throw "Invalid PE header offset: $path" }
            $stream.Position = $peOffset
            if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
                throw "The OBS runtime is not Windows x64: $path"
            }
        } finally { $reader.Dispose() }
        $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($path)
        $fileVersion = '{0}.{1}.{2}' -f $version.FileMajorPart, $version.FileMinorPart, $version.FileBuildPart
        if ($fileVersion -cne $runtimeVersions[$relative]) {
            throw "Runtime version mismatch for $relative. Expected $($runtimeVersions[$relative]), got $fileVersion."
        }
        $identities += [ordered]@{ path = $relative; version = $fileVersion; sha256 = Get-Sha256 $path }
    }
    return $identities
}

function Assert-ExistingRuntime {
    if ((Get-Item -LiteralPath $installRoot).Attributes -band [IO.FileAttributes]::ReparsePoint) {
        throw "Refusing to use a linked runtime directory: $installRoot"
    }
    $stampPath = Join-Path $installRoot $stampName
    if (!(Test-Path -LiteralPath $stampPath -PathType Leaf)) {
        throw "Runtime directory exists without a verification manifest: $installRoot. Use a fresh DependenciesRoot or inspect it before removing it."
    }
    $stamp = Get-Content -Raw -LiteralPath $stampPath | ConvertFrom-Json
    if ($stamp.schema -ne 1 -or $stamp.lockSha256 -cne $lockHash -or $stamp.archiveSha256 -cne $lock.artifact.sha256) {
        throw "Runtime verification manifest does not match the current lock: $stampPath"
    }
    $identities = @(Get-RuntimeIdentity $installRoot)
    if (@($stamp.files).Count -ne $identities.Count) { throw "Incomplete runtime verification manifest: $stampPath" }
    foreach ($identity in $identities) {
        $saved = @($stamp.files | Where-Object { $_.path -ceq $identity.path })
        if ($saved.Count -ne 1 -or $saved[0].sha256 -cne $identity.sha256 -or $saved[0].version -cne $identity.version) {
            throw "Extracted runtime differs from its verification manifest: $($identity.path)"
        }
    }
}

function Expand-VerifiedArchive([string]$Destination) {
    $archive = [IO.Compression.ZipFile]::OpenRead($archivePath)
    try {
        $entries = [Collections.Generic.List[object]]::new()
        $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        $prefix = $Destination.TrimEnd('\') + '\'
        foreach ($entry in $archive.Entries) {
            $name = $entry.FullName.Replace('\', '/')
            $isDirectory = $name.EndsWith('/')
            $relative = $name.TrimEnd('/')
            $segments = $relative.Split('/')
            $badSegments = @($segments | Where-Object {
                $_ -in @('', '.', '..') -or $_ -match '[. ]$|[<>:"|?*\x00-\x1f]' -or
                $_ -match '^(?i:CON|PRN|AUX|NUL|COM[1-9]|LPT[1-9])(?:\.|$)'
            })
            $unixType = (([int64]$entry.ExternalAttributes -band 0xffffffff) -shr 16) -band 0xf000
            if ($badSegments.Count -or $name.StartsWith('/') -or $unixType -eq 0xa000 -or
                ($entry.ExternalAttributes -band [int][IO.FileAttributes]::ReparsePoint)) {
                throw "Unsafe path or link in OBS ZIP: $($entry.FullName)"
            }
            $target = [IO.Path]::GetFullPath((Join-Path $Destination $relative))
            if (!$target.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or !$seen.Add($target)) {
                throw "Escaping or duplicate path in OBS ZIP: $($entry.FullName)"
            }
            $entries.Add([pscustomobject]@{ entry = $entry; target = $target; directory = $isDirectory })
        }
        # Validate every name before writing the first archive member.
        foreach ($item in $entries) {
            if ($item.directory) {
                [IO.Directory]::CreateDirectory($item.target) | Out-Null
            } else {
                [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($item.target)) | Out-Null
                [IO.Compression.ZipFileExtensions]::ExtractToFile($item.entry, $item.target, $false)
            }
        }
    } finally { $archive.Dispose() }
}

if (Test-Path -LiteralPath $installRoot) {
    Assert-ExistingRuntime
    Write-Host "Verified existing OBS $($lock.obsVersion) runtime: $installRoot"
    Write-Output $installRoot
    return
}
New-Item -ItemType Directory -Force -Path $DependenciesRoot | Out-Null
if (!(Test-Path -LiteralPath $archivePath -PathType Leaf)) {
    if ($Offline) { throw "Offline mode requires the cached official OBS ZIP: $archivePath" }
    $partial = Join-Path $DependenciesRoot ($lock.artifact.file + '.' + [guid]::NewGuid().ToString('N') + '.partial')
    Assert-OwnedPath $partial
    Write-Host "Downloading official OBS $($lock.obsVersion) Windows x64 ZIP..."
    try {
        Invoke-WebRequest -Uri $lock.artifact.url -OutFile $partial -TimeoutSec 600 -MaximumRedirection 10
        Assert-Archive $partial
        Assert-OwnedPath $archivePath
        Move-Item -LiteralPath $partial -Destination $archivePath
    } catch {
        # Remove only this attempt's unique partial file, never a dependency tree.
        if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial }
        throw
    }
} else {
    Assert-Archive $archivePath
}
$stage = Join-Path $DependenciesRoot ($lock.installDirectory + '.extract-' + [guid]::NewGuid().ToString('N'))
Assert-OwnedPath $stage
Assert-OwnedPath $installRoot
New-Item -ItemType Directory -Path $stage | Out-Null
Write-Host "Extracting verified OBS runtime..."
try {
    Expand-VerifiedArchive $stage
    $identities = @(Get-RuntimeIdentity $stage)
    $manifest = [ordered]@{
        schema = 1; obsVersion = $lock.obsVersion; qtVersion = $lock.qtVersion
        lockSha256 = $lockHash; archiveSha256 = $lock.artifact.sha256
        source = $lock.artifact.url; files = $identities
    }
    $manifest | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $stage $stampName) -Encoding utf8
    # Both absolute paths were checked to be inside the dedicated dependency root.
    # Never replace or recursively remove a previously existing directory.
    if (Test-Path -LiteralPath $installRoot) { throw "Runtime destination appeared during extraction: $installRoot" }
    Move-Item -LiteralPath $stage -Destination $installRoot
} catch {
    Write-Warning "Incomplete runtime staging was retained for inspection: $stage"
    throw
}
Write-Host "OBS $($lock.obsVersion) / Qt $($lock.qtVersion) runtime verified: $installRoot"
Write-Output $installRoot
