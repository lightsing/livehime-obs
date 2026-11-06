#requires -Version 7.0
<#
.SYNOPSIS
Prepares verified corresponding Qt source and OBS build scripts for a release.
.DESCRIPTION
Caches the two original ZIP archives in .deps/source-cache, verifies their hashes
and the source/build relationship, then copies them and checksum/provenance files
to dist. Does not extract, modify, execute or compile the downloaded sources.
Returns one object with manifestPath and files (name, sha256, size, kind).
.EXAMPLE
$sources = & .\scripts\bootstrap-source.ps1
.EXAMPLE
$sources = & .\scripts\bootstrap-source.ps1 -Offline -OutputDirectory .\dist
#>
[CmdletBinding()]
param(
    [string]$CacheDirectory = (Join-Path $PSScriptRoot '..\.deps\source-cache'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist'),
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$CacheDirectory = [IO.Path]::GetFullPath($CacheDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$lock = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'deps\qt-source.lock.json') | ConvertFrom-Json
if ($lock.schema -ne 1 -or @($lock.artifacts).Count -ne 2) { throw 'Unsupported Qt source lock.' }
foreach ($artifact in $lock.artifacts) {
    if ($artifact.file -notmatch '^[A-Za-z0-9._-]+\.zip$' -or
        $artifact.archiveRoot -notmatch '^[A-Za-z0-9._-]+$' -or
        $artifact.sha256 -notmatch '^[0-9a-f]{64}$' -or $artifact.size -le 0 -or
        !([uri]$artifact.url).IsAbsoluteUri -or ([uri]$artifact.url).Scheme -ne 'https') {
        throw "Invalid source artifact lock: $($artifact.id)"
    }
}
$qt = @($lock.artifacts | Where-Object id -EQ 'qtbase-source')
$obs = @($lock.artifacts | Where-Object id -EQ 'obs-deps-build-scripts')
if ($qt.Count -ne 1 -or $obs.Count -ne 1) { throw 'The source lock must contain Qt base and OBS build scripts.' }
$qt = $qt[0]
$obs = $obs[0]
$dependencies = Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'deps\dependencies.lock.json') | ConvertFrom-Json
$component = @($dependencies.bundledRuntimeComponents | Where-Object id -EQ 'qt-schannel-backend')
if ($dependencies.qtVersion -ne $lock.qtVersion -or $dependencies.qtBundleVersion -ne $lock.obsDepsVersion -or
    $component.Count -ne 1 -or $component[0].sha256 -ne $lock.componentSha256) {
    throw 'The Qt source lock does not match the bundled runtime dependency lock. Update both before releasing.'
}

function Get-Sha256([string]$Path) {
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function Assert-Archive([string]$Path, $Artifact) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing source archive: $Path" }
    $hash = Get-Sha256 $Path
    if ($hash -ne $Artifact.sha256 -or (Get-Item -LiteralPath $Path).Length -ne $Artifact.size) {
        throw "Source checksum/size mismatch for '$Path'. Expected SHA256 $($Artifact.sha256); got $hash. Remove the damaged cache file and retry."
    }
}

foreach ($artifact in $lock.artifacts) {
    $cached = Join-Path $CacheDirectory $artifact.file
    if (!(Test-Path -LiteralPath $cached -PathType Leaf)) {
        if ($Offline) { throw "Offline mode: missing source archive '$cached'. Run once without -Offline or populate the verified cache." }
        New-Item -ItemType Directory -Force -Path $CacheDirectory | Out-Null
        $partial = Join-Path $CacheDirectory ($artifact.file + '.' + [Guid]::NewGuid().ToString('N') + '.partial')
        try {
            Write-Host "Downloading $($artifact.file)..."
            Invoke-WebRequest -Uri $artifact.url -OutFile $partial -TimeoutSec 300
            Assert-Archive $partial $artifact
            Move-Item -LiteralPath $partial -Destination $cached
        } catch {
            throw "Could not obtain '$($artifact.file)': $($_.Exception.Message)"
        } finally {
            if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial }
        }
    }
    Assert-Archive $cached $artifact
}

function Read-ZipText([IO.Compression.ZipArchive]$Archive, [string]$Name) {
    $entry = $Archive.GetEntry($Name)
    if (!$entry -or $entry.Length -gt 2MB) { throw "Missing or unexpected source archive entry: $Name" }
    $reader = [IO.StreamReader]::new($entry.Open())
    try { return $reader.ReadToEnd() } finally { $reader.Dispose() }
}

# Check the original Qt archive's release identity and the exact checksum used
# by the pinned OBS build. A same-version source archive alone is not enough.
$qtZip = [IO.Compression.ZipFile]::OpenRead((Join-Path $CacheDirectory $qt.file))
$obsZip = $null
try {
    $qtTag = (Read-ZipText $qtZip ($qt.archiveRoot + '/.tag')).Trim()
    if ($qtTag -cne $lock.qtSourceCommit) { throw 'The Qt archive .tag does not match the pinned Qt source commit.' }
    $qtConfig = Read-ZipText $qtZip ($qt.archiveRoot + '/.cmake.conf')
    if ($qtConfig -notmatch ('set\(QT_REPO_MODULE_VERSION\s+"' + [regex]::Escape($lock.qtVersion) + '"\)')) {
        throw 'The Qt source archive has an unexpected module version.'
    }
    foreach ($required in @('LICENSES/GPL-2.0-only.txt', 'src/plugins/tls/schannel/CMakeLists.txt',
                            'src/plugins/tls/schannel/qtls_schannel.cpp')) {
        if (!$qtZip.GetEntry($qt.archiveRoot + '/' + $required)) { throw "Qt source archive is missing '$required'." }
    }
    $obsZip = [IO.Compression.ZipFile]::OpenRead((Join-Path $CacheDirectory $obs.file))
    $checksum = Read-ZipText $obsZip ($obs.archiveRoot + '/' + $lock.buildEvidence.qtChecksum)
    $publishedHash = [regex]::Match($checksum, '<S N="Hash">([A-Fa-f0-9]{64})</S>')
    if (!$publishedHash.Success -or $publishedHash.Groups[1].Value.ToLowerInvariant() -ne $qt.sha256 -or
        !$checksum.Contains('<S N="Path">' + $qt.file + '</S>')) {
        throw 'The Qt source hash does not match the original OBS build checksum.'
    }
    $buildScript = Read-ZipText $obsZip ($obs.archiveRoot + '/' + $lock.buildEvidence.script)
    $versionDeclaration = '$Version = ' + "'" + $lock.qtVersion + "'"
    $uriDeclaration = '$Uri = ' + "'" + $qt.url.Substring(0, $qt.url.LastIndexOf('/submodules/')) + "'"
    if (!$buildScript.Contains($versionDeclaration) -or !$buildScript.Contains($uriDeclaration) -or
        !$buildScript.Contains([IO.Path]::GetFileName($lock.buildEvidence.windowsPatch)) -or
        !$buildScript.Contains($lock.buildEvidence.windowsPatchSha256.ToUpperInvariant()) -or
        !$buildScript.Contains('-DFEATURE_schannel:BOOL=ON')) {
        throw 'The pinned OBS Windows build script does not match the source/patch relationship.'
    }
    $patchEntry = $obsZip.GetEntry($obs.archiveRoot + '/' + $lock.buildEvidence.windowsPatch)
    if (!$patchEntry) { throw 'The OBS source archive is missing the Windows Qt patch.' }
    $stream = $patchEntry.Open()
    try { $patchHash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant() }
    finally { $stream.Dispose() }
    if ($patchHash -ne $lock.buildEvidence.windowsPatchSha256) { throw 'The original Windows Qt patch checksum does not match.' }
} finally {
    if ($obsZip) { $obsZip.Dispose() }
    $qtZip.Dispose()
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$utf8 = [Text.UTF8Encoding]::new($false)
$files = [Collections.Generic.List[object]]::new()
foreach ($artifact in $lock.artifacts) {
    $cached = Join-Path $CacheDirectory $artifact.file
    $destination = Join-Path $OutputDirectory $artifact.file
    if (![string]::Equals($cached, $destination, [StringComparison]::OrdinalIgnoreCase)) {
        Copy-Item -LiteralPath $cached -Destination $destination -Force
    }
    Assert-Archive $destination $artifact
    $files.Add([ordered]@{ name=$artifact.file; sha256=$artifact.sha256; size=$artifact.size; kind=$artifact.id })
    $checksumName = $artifact.file + '.sha256'
    $checksumPath = Join-Path $OutputDirectory $checksumName
    [IO.File]::WriteAllText($checksumPath, "$($artifact.sha256)  $($artifact.file)`n", $utf8)
    $files.Add([ordered]@{ name=$checksumName; sha256=(Get-Sha256 $checksumPath); size=(Get-Item -LiteralPath $checksumPath).Length; kind='checksum' })
}
$manifest = [ordered]@{
    schema = 1
    scope = 'Corresponding Qt base source and original OBS dependency build scripts for the bundled Schannel backend; no Qt binaries are built by this script.'
    qtVersion = $lock.qtVersion
    qtSourceCommit = $lock.qtSourceCommit
    obsDepsVersion = $lock.obsDepsVersion
    obsDepsCommit = $lock.obsDepsCommit
    component = $lock.component
    componentSha256 = $lock.componentSha256
    sourceArchiveMatchesOriginalBuildChecksum = $true
    buildEvidence = $lock.buildEvidence
    provenanceNote = $lock.provenanceNote
    instructions = 'Unpack the unmodified Qt source ZIP and the OBS build-script ZIP. The OBS archive contains its original Windows Qt configuration, checksum, patch, helper scripts and license. Retain both archives alongside the plugin binary release. The Windows patch is applied by the upstream build scripts, not by this source-packaging script.'
    origins = @($lock.artifacts | Select-Object id, file, url, checksumSource)
    files = $files.ToArray()
}
$manifestName = 'qt-source-provenance.json'
$manifestPath = Join-Path $OutputDirectory $manifestName
$json = ($manifest | ConvertTo-Json -Depth 8).Replace("`r`n", "`n") + "`n"
[IO.File]::WriteAllText($manifestPath, $json, $utf8)
$manifestHash = Get-Sha256 $manifestPath
$files.Add([ordered]@{name=$manifestName; sha256=$manifestHash; size=(Get-Item -LiteralPath $manifestPath).Length; kind='provenance'})
$manifestChecksumName = $manifestName + '.sha256'
$manifestChecksumPath = Join-Path $OutputDirectory $manifestChecksumName
[IO.File]::WriteAllText($manifestChecksumPath, "$manifestHash  $manifestName`n", $utf8)
$files.Add([ordered]@{name=$manifestChecksumName; sha256=(Get-Sha256 $manifestChecksumPath); size=(Get-Item -LiteralPath $manifestChecksumPath).Length; kind='checksum'})
Write-Host "Verified Qt source release assets are ready in '$OutputDirectory'."
[pscustomobject]@{ manifestPath=$manifestPath; files=$files.ToArray() }
