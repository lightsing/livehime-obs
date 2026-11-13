#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist'),
    [ValidateSet('Release', 'RelWithDebInfo')][string]$Configuration = 'RelWithDebInfo'
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if (!(Test-Path -LiteralPath (Join-Path $BuildDirectory "$Configuration\livehime-obs.dll"))) {
    throw 'Build the plugin with scripts/build.ps1 before packaging.'
}
New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
$stage = Join-Path $OutputDirectory ('stage-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $stage | Out-Null
& cmake --install $BuildDirectory --config $Configuration --prefix $stage
if ($LASTEXITCODE -ne 0) { throw 'CMake install into package staging failed.' }
foreach ($name in @('README.md', 'AUTHORS.md', 'LICENSE', 'THIRD-PARTY-NOTICES.md', 'VALIDATION.md')) {
    $source = Join-Path $projectRoot $name
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $stage }
}
Copy-Item -LiteralPath (Join-Path $projectRoot 'deps') -Destination $stage -Recurse
$evidence = Join-Path $stage 'validation'
New-Item -ItemType Directory -Force -Path $evidence | Out-Null
foreach ($name in @('ctest-0.2.0.txt', 'realtime-qt-probe.json', 'load-0.2.0.json', 'loader-final-0.2.0.json', 'isolated-host-0.2.0.txt', 'installer-0.2.0.json', 'obs-exit-repeat-0.2.0.json', 'loader-0.2.1.json', 'installer-upgrade-0.2.1.json', 'installer-fresh-0.2.1.json', 'ctest-0.2.2.txt', 'loader-0.2.2.json', 'native-ui-0.2.2.json', 'installer-upgrade-0.2.2.json', 'obs-docks-0.2.2.json')) {
    $source = Join-Path $projectRoot "validation\$name"
    if (Test-Path -LiteralPath $source) { Copy-Item -LiteralPath $source -Destination $evidence }
}
if (Test-Path -LiteralPath (Join-Path $projectRoot 'validation\realtime-ui')) {
    Copy-Item -LiteralPath (Join-Path $projectRoot 'validation\realtime-ui') -Destination $evidence -Recurse
}
$hashes = @(Get-ChildItem -LiteralPath $stage -Recurse -File | ForEach-Object {
    [ordered]@{ path = [IO.Path]::GetRelativePath($stage, $_.FullName).Replace('\', '/'); sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
$hashes | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.json') -Encoding utf8
$versionMatch = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt')), 'project\(livehime-obs\s+VERSION\s+(\d+\.\d+\.\d+)\s')
if (!$versionMatch.Success) { throw 'Cannot read the project version.' }
$package = Join-Path $OutputDirectory "livehime-obs-$($versionMatch.Groups[1].Value)-windows-x64.zip"
# Copying the new archive over an older generated artifact is intentional. No directory deletion.
$temporaryArchive = Join-Path $OutputDirectory ('package-' + [guid]::NewGuid().ToString('N') + '.zip')
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $temporaryArchive, [IO.Compression.CompressionLevel]::Optimal, $false)
Move-Item -LiteralPath $temporaryArchive -Destination $package -Force
$hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash.ToLowerInvariant()
Set-Content -LiteralPath ($package + '.sha256') -Value "$hash  $([IO.Path]::GetFileName($package))" -Encoding ascii
$resolvedStage = [IO.Path]::GetFullPath($stage)
$outputPrefix = $OutputDirectory.TrimEnd([IO.Path]::DirectorySeparatorChar) + [IO.Path]::DirectorySeparatorChar
if (!$resolvedStage.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase) -or
    [IO.Path]::GetFileName($resolvedStage) -notmatch '^stage-[0-9a-f]{32}$') {
    throw "Refusing to remove unexpected package staging path: $resolvedStage"
}
Remove-Item -LiteralPath $resolvedStage -Recurse -Force
Write-Host "Package: $package"
Write-Host "SHA256: $hash"
