#requires -Version 7.0
<#
.SYNOPSIS
Installs the locked Inno Setup compiler into a local tools directory.
.DESCRIPTION
Returns one string on the success stream: the absolute ISCC.exe path. Downloads
are cached in ToolsRoot\cache. Existing verified installs are reused. Offline
mode never downloads; a cached installer or a verified existing install is needed.
Windows may use its certificate cache/network when validating Authenticode trust.
The official installer runs in current-user portable mode, without shortcuts,
file associations, PATH changes, an uninstall entry, or a restart.
.EXAMPLE
$iscc = & .\scripts\bootstrap-installer.ps1
.EXAMPLE
$iscc = & .\scripts\bootstrap-installer.ps1 -ToolsRoot D:\REA\tools -Offline
#>
[CmdletBinding()]
param(
    [string]$ToolsRoot = (Join-Path $PSScriptRoot '..\.tools'),
    [switch]$Offline
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitOperatingSystem) {
    throw 'The locked Inno Setup toolchain requires 64-bit Windows and PowerShell 7 or newer.'
}
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$lockPath = Join-Path $projectRoot 'deps\installer-toolchain.lock.json'
$lock = Get-Content -Raw -LiteralPath $lockPath | ConvertFrom-Json
if ($lock.schema -ne 1 -or $lock.id -ne 'innosetup' -or $lock.platform -ne 'windows-x64' -or
    $lock.version -notmatch '^\d+\.\d+\.\d+$' -or
    $lock.installDirectory -ne "innosetup-$($lock.version)" -or $lock.compiler -ne 'ISCC.exe' -or
    $lock.artifact.file -ne "innosetup-$($lock.version)-x64.exe" -or
    $lock.artifact.sha256 -notmatch '^[0-9a-f]{64}$') {
    throw "Invalid installer toolchain lock: $lockPath"
}
$ToolsRoot = [IO.Path]::GetFullPath($ToolsRoot)
if ($ToolsRoot -match '["\r\n]') { throw 'ToolsRoot must not contain quotes or line breaks.' }
$installRoot = Join-Path $ToolsRoot $lock.installDirectory
$compiler = Join-Path $installRoot $lock.compiler
$stampPath = Join-Path $installRoot '.bootstrap-manifest.json'
$cacheRoot = Join-Path $ToolsRoot 'cache'
$installer = Join-Path $cacheRoot $lock.artifact.file
$lockHash = (Get-FileHash -LiteralPath $lockPath -Algorithm SHA256).Hash.ToLowerInvariant()

function Assert-Publisher([string]$Path) {
    $signature = Get-AuthenticodeSignature -LiteralPath $Path
    if ($signature.Status -ne 'Valid' -or !$signature.SignerCertificate) {
        throw "Authenticode validation failed for '$Path': $($signature.Status) ($($signature.StatusMessage))."
    }
    $publisher = $signature.SignerCertificate.GetNameInfo(
        [Security.Cryptography.X509Certificates.X509NameType]::SimpleName, $false)
    if ($publisher -cne $lock.publisher) {
        throw "Unexpected publisher for '$Path': '$publisher'; expected '$($lock.publisher)'."
    }
}

function Assert-Compiler {
    foreach ($name in @('ISCC.exe', 'ISCmplr.dll', 'ISPP.dll', 'ISPPBuiltins.iss',
                         'Default.isl', 'Setup.e32', 'Setup.e64', 'SetupLdr.e32', 'SetupLdr.e64')) {
        if (!(Test-Path -LiteralPath (Join-Path $installRoot $name) -PathType Leaf)) {
            throw "Incomplete Inno Setup installation: missing '$name' in '$installRoot'."
        }
    }
    foreach ($name in @('ISCC.exe', 'ISCmplr.dll', 'ISPP.dll')) {
        Assert-Publisher (Join-Path $installRoot $name)
    }
    $stream = [IO.File]::OpenRead($compiler)
    $reader = [IO.BinaryReader]::new($stream)
    try {
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550 -or $reader.ReadUInt16() -ne 0x8664) {
            throw "The compiler is not a Windows x64 executable: $compiler"
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
    # The official binaries have 0.0.0.0 Windows version resources. Inno 7's
    # --version reports the compiler engine version; check its signature first.
    $version = ((& $compiler --version 2>&1) | Out-String).Trim()
    if ($LASTEXITCODE -ne 0 -or $version -cne $lock.version) {
        throw "Compiler version mismatch: expected $($lock.version), found '$version' in '$compiler'."
    }
}

function Assert-InstalledManifest {
    $stamp = Get-Content -Raw -LiteralPath $stampPath | ConvertFrom-Json
    if ($stamp.schema -ne 1 -or $stamp.lockSha256 -ne $lockHash -or
        $stamp.installerSha256 -ne $lock.artifact.sha256 -or @($stamp.files).Count -eq 0) {
        throw 'The installed toolchain manifest does not match the current lock.'
    }
    $prefix = $installRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    foreach ($file in $stamp.files) {
        $path = [IO.Path]::GetFullPath((Join-Path $installRoot $file.path))
        if (!$path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase) -or
            !(Test-Path -LiteralPath $path -PathType Leaf)) {
            throw "Missing or invalid installed toolchain file: $($file.path)"
        }
        $actualHash = (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($actualHash -ne $file.sha256) { throw "Installed toolchain file changed: $path" }
    }
    Assert-Compiler
}

if (Test-Path -LiteralPath $stampPath -PathType Leaf) {
    try {
        Assert-InstalledManifest
        Write-Host "Using verified Inno Setup $($lock.version): $compiler"
        Write-Output $compiler
        return
    } catch {
        Write-Host "Existing toolchain requires repair: $($_.Exception.Message)"
    }
}

if (!(Test-Path -LiteralPath $installer -PathType Leaf)) {
    if ($Offline) {
        throw "Offline mode: no verified compiler or cached installer. Place '$($lock.artifact.file)' in '$cacheRoot', or run once without -Offline."
    }
    New-Item -ItemType Directory -Force -Path $cacheRoot | Out-Null
    $partial = Join-Path $cacheRoot ($lock.artifact.file + '.' + [Guid]::NewGuid().ToString('N') + '.partial')
    try {
        Write-Host "Downloading Inno Setup $($lock.version) from the official release..."
        Invoke-WebRequest -Uri $lock.artifact.url -OutFile $partial -TimeoutSec 300
        $downloadHash = (Get-FileHash -LiteralPath $partial -Algorithm SHA256).Hash.ToLowerInvariant()
        if ($downloadHash -ne $lock.artifact.sha256) {
            throw "Download SHA256 mismatch: expected $($lock.artifact.sha256); got $downloadHash."
        }
        Move-Item -LiteralPath $partial -Destination $installer
    } catch {
        throw "Could not download the locked Inno Setup installer: $($_.Exception.Message)"
    } finally {
        if (Test-Path -LiteralPath $partial -PathType Leaf) { Remove-Item -LiteralPath $partial }
    }
}
$installerHash = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
if ($installerHash -ne $lock.artifact.sha256 -or (Get-Item -LiteralPath $installer).Length -ne $lock.artifact.size) {
    throw "Cached installer checksum/size mismatch: '$installer'. Expected SHA256 $($lock.artifact.sha256); got $installerHash. Remove this cache file and retry."
}
Assert-Publisher $installer

New-Item -ItemType Directory -Force -Path $cacheRoot, $installRoot | Out-Null
$logPath = Join-Path $cacheRoot ("innosetup-$($lock.version)-install-" + [Guid]::NewGuid().ToString('N') + '.log')
# setup.iss allows /CURRENTUSER. Its isportable.iss disables uninstall records,
# existing-install reuse and shell integration. /TASKS="" also disables fileassoc.
$arguments = @('/CURRENTUSER', '/PORTABLE=1', '/VERYSILENT', '/SUPPRESSMSGBOXES',
               '/NORESTART', '/SP-', '/NOICONS', '/TASKS=""', '/NOCLOSEAPPLICATIONS',
               '/NORESTARTAPPLICATIONS', ('/DIR="' + $installRoot + '"'), ('/LOG="' + $logPath + '"'))
Write-Host "Installing local Inno Setup $($lock.version) into '$installRoot'..."
try {
    $process = Start-Process -FilePath $installer -ArgumentList ($arguments -join ' ') -WindowStyle Hidden -Wait -PassThru
} catch {
    throw "Could not start the verified Inno Setup installer: $($_.Exception.Message). Log, if created: $logPath"
}
if ($process.ExitCode -ne 0) {
    throw "Inno Setup toolchain installation failed with exit code $($process.ExitCode). See '$logPath'."
}
Assert-Compiler
$files = @(Get-ChildItem -LiteralPath $installRoot -File -Recurse -Force |
    Where-Object FullName -NE $stampPath | Sort-Object FullName | ForEach-Object {
        [ordered]@{
            path = [IO.Path]::GetRelativePath($installRoot, $_.FullName)
            sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
        }
    })
[ordered]@{
    schema = 1
    lockSha256 = $lockHash
    installerSha256 = $installerHash
    files = $files
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $stampPath -Encoding utf8
Assert-InstalledManifest
Write-Host "Inno Setup ready: $compiler"
Write-Output $compiler
