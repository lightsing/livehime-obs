#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$PackagePath = '',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist'),
    [string]$ToolsRoot = (Join-Path $PSScriptRoot '..\.tools'),
    [switch]$Offline,
    [switch]$FixtureIdentity,
    [switch]$KeepStage
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
$versionMatch = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt')),
    'project\(livehime-obs\s+VERSION\s+(\d+\.\d+\.\d+)\s')
if (!$versionMatch.Success) { throw 'Cannot read the project version from CMakeLists.txt.' }
$version = $versionMatch.Groups[1].Value
if (!$PackagePath) { $PackagePath = Join-Path $projectRoot "dist\livehime-obs-$version-windows-x64.zip" }
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
if (!(Test-Path -LiteralPath $PackagePath -PathType Leaf)) { throw "Build the plugin ZIP first: $PackagePath" }
if ([IO.Path]::GetFileName($PackagePath) -ne "livehime-obs-$version-windows-x64.zip") {
    throw 'The ZIP filename must match the current project version.'
}
$compiler = & (Join-Path $PSScriptRoot 'bootstrap-installer.ps1') -ToolsRoot $ToolsRoot -Offline:$Offline
$buildRoot = Join-Path $projectRoot 'build'
$stage = Join-Path $buildRoot ('installer-stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage, $OutputDirectory | Out-Null
$runtimeFiles = @(
    'obs-plugins/64bit/livehime-obs.dll',
    'data/obs-plugins/livehime-obs/livehime-update-helper.exe',
    'data/obs-plugins/livehime-obs/locale/en-US.ini',
    'data/obs-plugins/livehime-obs/locale/zh-CN.ini',
    'data/obs-plugins/livehime-obs/qt-plugins/tls/qschannelbackend.dll'
)
$archive = $null
try {
    $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    $entries = @{}
    foreach ($entry in $archive.Entries) {
        if ($entry.FullName.EndsWith('/')) { continue }
        if ($entry.FullName -match '(^/|\\|:|(^|/)\.\.(/|$))' -or $entries.ContainsKey($entry.FullName)) {
            throw "Unsafe or duplicate ZIP entry: $($entry.FullName)"
        }
        if ($entry.Length -gt 64MB) { throw "Unexpected oversized ZIP entry: $($entry.FullName)" }
        $entries[$entry.FullName] = $entry
    }
    if (!$entries.ContainsKey('SHA256SUMS.json')) { throw 'The ZIP is missing its SHA-256 manifest.' }
    $reader = [IO.StreamReader]::new($entries['SHA256SUMS.json'].Open())
    try { $manifest = @($reader.ReadToEnd() | ConvertFrom-Json) } finally { $reader.Dispose() }
    $verified = @{}
    foreach ($record in $manifest) {
        if (!$entries.ContainsKey($record.path) -or $verified.ContainsKey($record.path) -or
            $record.sha256 -notmatch '^[0-9a-fA-F]{64}$') { throw 'Invalid ZIP hash manifest.' }
        $stream = $entries[$record.path].Open()
        try { $hash = [Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant() }
        finally { $stream.Dispose() }
        if ($hash -ne $record.sha256) { throw "ZIP checksum mismatch: $($record.path)" }
        $verified[$record.path] = $true
    }
    if ($verified.Count -ne $entries.Count - 1) { throw 'ZIP contains files not covered by its hash manifest.' }
    foreach ($relative in $runtimeFiles) {
        if (!$verified.ContainsKey($relative)) { throw "Missing verified runtime file: $relative" }
        $destination = Join-Path $stage $relative
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($destination)) | Out-Null
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entries[$relative], $destination)
    }
    $archive.Dispose()
    $archive = $null
    $pluginData = Join-Path $stage 'data\obs-plugins\livehime-obs'
    $docs = Join-Path $pluginData 'docs'
    New-Item -ItemType Directory -Path $docs | Out-Null
    foreach ($name in @('README.md', 'AUTHORS.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'VALIDATION.md')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $docs
    }
    Copy-Item -LiteralPath (Join-Path $projectRoot 'deps') -Destination (Join-Path $docs 'deps') -Recurse
    $evidence = Join-Path $docs 'validation'
    New-Item -ItemType Directory -Path $evidence | Out-Null
    foreach ($name in @('ctest-0.2.0.txt', 'realtime-qt-probe.json', 'load-0.2.0.json', 'loader-final-0.2.0.json', 'isolated-host-0.2.0.txt', 'installer-0.2.0.json', 'obs-exit-repeat-0.2.0.json', 'loader-0.2.1.json', 'installer-upgrade-0.2.1.json', 'installer-fresh-0.2.1.json', 'ctest-0.2.2.txt', 'loader-0.2.2.json', 'native-ui-0.2.2.json', 'installer-upgrade-0.2.2.json', 'obs-docks-0.2.2.json',
            'ctest-0.2.4.txt', 'loader-0.2.4.json', 'ctest-0.3.0.txt', 'loader-0.3.0.json',
            'ctest-0.4.0.txt', 'loader-0.4.0.json')) {
        $source = Join-Path $projectRoot "validation\$name"
        if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $evidence }
    }
    if (Test-Path -LiteralPath (Join-Path $projectRoot 'validation\realtime-ui')) {
        Copy-Item -LiteralPath (Join-Path $projectRoot 'validation\realtime-ui') -Destination $evidence -Recurse
    }
    $files = @(Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/')
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
    [ordered]@{schema=1; version=$version; obsVersion='33.0.1'; qtVersion='6.11.1'; files=$files} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $pluginData 'install-manifest.json') -Encoding utf8
    $modernFiles = @($files | ForEach-Object {
        if ($_.path -eq 'obs-plugins/64bit/livehime-obs.dll') { $mapped = 'livehime-obs.dll' }
        elseif ($_.path.StartsWith('data/obs-plugins/livehime-obs/', [StringComparison]::Ordinal)) {
            $mapped = 'data/' + $_.path.Substring('data/obs-plugins/livehime-obs/'.Length)
        } else { throw "Unexpected source path for modern layout: $($_.path)" }
        [ordered]@{path=$mapped; sha256=$_.sha256}
    })
    [ordered]@{schema=2; layout='obs-modern-bundle'; version=$version; obsVersion='33.0.1'; qtVersion='6.11.1'; files=$modernFiles} |
        ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $stage 'manifest-modern.json') -Encoding utf8
    $compilerArgs = @("/DPayloadRoot=$stage", "/DOutputRoot=$OutputDirectory", "/DAppVersion=$version")
    if ($FixtureIdentity) { $compilerArgs += '/DFixtureIdentity=1' }
    $compilerArgs += (Join-Path $projectRoot 'installer\livehime-obs.iss')
    & $compiler @compilerArgs
    if ($LASTEXITCODE -ne 0) { throw 'Inno Setup compilation failed.' }
    $suffix = if ($FixtureIdentity) { '-fixture' } else { '' }
    $installer = Join-Path $OutputDirectory "livehime-obs-$version-windows-x64-setup$suffix.exe"
    if (!(Test-Path -LiteralPath $installer)) { throw 'Inno Setup did not produce the expected installer.' }
    $installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
    Set-Content -LiteralPath ($installer + '.sha256') -Value "$installerHash  $([IO.Path]::GetFileName($installer))" -Encoding ascii
    [ordered]@{
        schema=1; version=$version; installer=[IO.Path]::GetFileName($installer); sha256=$installerHash
        payloadZip=[IO.Path]::GetFileName($PackagePath)
        payloadZipSha256=(Get-FileHash -LiteralPath $PackagePath -Algorithm SHA256).Hash.ToLowerInvariant()
        pluginSha256=($files | Where-Object path -eq $runtimeFiles[0]).sha256
        toolchainVersion='7.1.1'; signed=$false; fixtureIdentity=[bool]$FixtureIdentity
        defaultLayout='modern-bundle-v1'; supportedUpgradeLayouts=@('modern-bundle-v1','legacy-root-v1')
    } | ConvertTo-Json | Set-Content -LiteralPath ($installer + '.json') -Encoding utf8
    Write-Host "Installer: $installer"
    Write-Host "SHA256: $installerHash"
} finally {
    if ($archive) { $archive.Dispose() }
    if (!$KeepStage) {
        $resolvedStage = [IO.Path]::GetFullPath($stage)
        $allowedPrefix = [IO.Path]::GetFullPath($buildRoot).TrimEnd('\') + '\'
        if (!$resolvedStage.StartsWith($allowedPrefix, [StringComparison]::OrdinalIgnoreCase) -or
            [IO.Path]::GetFileName($resolvedStage) -notmatch '^installer-stage-[0-9a-f]{32}$') {
            throw "Refusing to remove unexpected staging directory: $resolvedStage"
        }
        if (Test-Path -LiteralPath $resolvedStage) { Remove-Item -LiteralPath $resolvedStage -Recurse -Force }
    }
}
