#requires -Version 7.0
<#
.SYNOPSIS
Validates the Windows installer using isolated, synthetic OBS installations.
.DESCRIPTION
Uses /CURRENTUSER only for isolated test bundles, with explicit /OBSDIR pointing
at synthetic host fixtures. Normal installation uses the shared ProgramData
bundle and requests administrator privileges. Requires the fixed
Livehime OBS uninstall key to be absent in HKCU before starting. No real OBS
executable, account data, or configuration is copied or launched. The fake OBS
process is compiled locally, performs no networking, and exits when signalled.
All fixtures and logs are retained, including after failure. This script never
recursively removes a directory or automatically uninstalls after a failed test.
.EXAMPLE
.\scripts\test-installer.ps1 -InstallerPath .\dist\livehime-obs-0.1.0-windows-x64-setup.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)][string]$InstallerPath,
    [string]$Workspace = (Join-Path $PSScriptRoot '..\build\installer-validation'),
    [string]$PackagePath = '',
    [switch]$FixtureIdentity,
    [ValidateRange(15, 600)][int]$InstallerTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitProcess) {
    throw 'Run this validation in 64-bit PowerShell 7 on Windows.'
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
if (!$PackagePath) {
    if ($InstallerPath -cnotmatch '-setup(?:-fixture)?\.exe\z') { throw 'Supply -PackagePath for a renamed installer.' }
    $PackagePath = $InstallerPath -creplace '-setup(?:-fixture)?\.exe\z', '.zip'
}
$PackagePath = [IO.Path]::GetFullPath($PackagePath)
$Workspace = [IO.Path]::GetFullPath($Workspace)
foreach ($path in @($InstallerPath, $PackagePath, $Workspace)) {
    if ($path -match '["\r\n]' -or $path -notmatch '^[A-Za-z]:\\') {
        throw "Only local absolute paths without quotes or line breaks are supported: $path"
    }
}
foreach ($path in @($InstallerPath, $PackagePath)) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing input: $path" }
}
if ($Workspace.TrimEnd('\') -eq [IO.Path]::GetPathRoot($Workspace).TrimEnd('\')) {
    throw 'Workspace must be a dedicated directory, not a drive root.'
}

$uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{D39A69EF-8533-4CB2-8A13-770CBCA96340}_is1'
if ($FixtureIdentity) {
    if ([IO.Path]::GetFileName($InstallerPath) -notlike '*-fixture.exe') { throw 'FixtureIdentity requires the dedicated fixture installer.' }
    $uninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{7F302FCE-7BA4-4ABE-92CF-62D0937D87AD}_is1'
}
$pluginData = 'data'
$manifestRelative = "$pluginData/install-manifest.json"
$uninstallerRelative = 'installer/unins000.exe'
$legacyData = 'data/obs-plugins/livehime-obs'
$runtimePaths = @(
    'livehime-obs.dll',
    "$pluginData/livehime-update-helper.exe",
    "$pluginData/locale/en-US.ini",
    "$pluginData/locale/zh-CN.ini",
    "$pluginData/qt-plugins/tls/qschannelbackend.dll"
)
$zipPathMap = @{
    'livehime-obs.dll' = 'obs-plugins/64bit/livehime-obs.dll'
    'data/livehime-update-helper.exe' = 'data/obs-plugins/livehime-obs/livehime-update-helper.exe'
    'data/locale/en-US.ini' = 'data/obs-plugins/livehime-obs/locale/en-US.ini'
    'data/locale/zh-CN.ini' = 'data/obs-plugins/livehime-obs/locale/zh-CN.ini'
    'data/qt-plugins/tls/qschannelbackend.dll' = 'data/obs-plugins/livehime-obs/qt-plugins/tls/qschannelbackend.dll'
}

function Get-UninstallRecord([Microsoft.Win32.RegistryHive]$Hive,
                             [Microsoft.Win32.RegistryView]$View = [Microsoft.Win32.RegistryView]::Registry64) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive, $View)
    $key = $null
    try {
        $key = $base.OpenSubKey($uninstallKey, $false)
        $values = [ordered]@{}
        if ($key) {
            foreach ($name in @($key.GetValueNames() | Sort-Object)) {
                $values[$name] = [ordered]@{
                    kind = $key.GetValueKind($name).ToString()
                    value = $key.GetValue($name, $null, [Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)
                }
            }
        }
        return [pscustomobject]@{ exists = ($null -ne $key); values = $values }
    } finally {
        if ($key) { $key.Dispose() }
        $base.Dispose()
    }
}

function ConvertTo-StableJson($Value) { ConvertTo-Json -InputObject $Value -Depth 12 -Compress }
function Get-ErrorDetails([Management.Automation.ErrorRecord]$Record) {
    $invocation = $Record.InvocationInfo
    return [pscustomobject]@{
        message = $Record.Exception.Message
        exceptionType = $Record.Exception.GetType().FullName
        exception = $Record.Exception.ToString()
        scriptStackTrace = $Record.ScriptStackTrace
        fullyQualifiedErrorId = $Record.FullyQualifiedErrorId
        positionMessage = $(if ($invocation) { $invocation.PositionMessage } else { $null })
        scriptName = $(if ($invocation) { $invocation.ScriptName } else { $null })
        scriptLineNumber = $(if ($invocation) { $invocation.ScriptLineNumber } else { $null })
        offsetInLine = $(if ($invocation) { $invocation.OffsetInLine } else { $null })
    }
}
function Assert-True([bool]$Condition, [string]$Message) {
    if (!$Condition) { throw $Message }
}
function Get-Sha256([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function Get-FixturePath([string]$Root, [string]$Relative) {
    if ([IO.Path]::IsPathRooted($Relative) -or $Relative -match '(^|[/\\])\.\.([/\\]|$)|:') {
        throw "Unsafe relative fixture path: $Relative"
    }
    $result = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    $prefix = [IO.Path]::GetFullPath($Root).TrimEnd('\') + '\'
    Assert-True ($result.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) "Path escapes fixture: $result"
    return $result
}

# Refuse to adopt a real or previous incomplete installation's uninstall log.
# HKCU\SOFTWARE (including this uninstall key) is shared across WOW64 views.
# HKLM\SOFTWARE is redirected, so its two views are checked independently.
# https://learn.microsoft.com/windows/win32/winprog64/shared-registry-keys
Assert-True (!(Get-UninstallRecord CurrentUser Registry64).exists) `
    'HKCU already contains the Livehime OBS uninstall key. No tests were run. Inspect that installation first.'
$machineBefore = @(
    ConvertTo-StableJson (Get-UninstallRecord LocalMachine Registry64)
    ConvertTo-StableJson (Get-UninstallRecord LocalMachine Registry32)
)

New-Item -ItemType Directory -Force -Path $Workspace | Out-Null
$runRoot = Join-Path $Workspace ('run-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $runRoot | Out-Null
$logsRoot = Join-Path $runRoot 'logs'
$sourcesRoot = Join-Path $runRoot 'fixture-sources'
$processRoot = Join-Path $runRoot 'process-signals'
foreach ($path in @($logsRoot, $sourcesRoot, $processRoot)) { New-Item -ItemType Directory -Path $path | Out-Null }
$results = [Collections.Generic.List[object]]::new()
$invocations = [Collections.Generic.List[object]]::new()
$ownedProcesses = [Collections.Generic.List[object]]::new()
$zipPayload = @{}
$script:invocationNumber = 0
$failure = $null

function Assert-RegistryIsolation {
    Assert-True ((ConvertTo-StableJson (Get-UninstallRecord LocalMachine Registry64)) -ceq $machineBefore[0]) `
        'The installer changed the machine-wide 64-bit uninstall registration.'
    Assert-True ((ConvertTo-StableJson (Get-UninstallRecord LocalMachine Registry32)) -ceq $machineBefore[1]) `
        'The installer changed the machine-wide 32-bit uninstall registration.'
    $user64 = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)
    $user32 = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry32)
    Assert-True ($user64 -ceq $user32) 'The shared HKCU uninstall key differs between its logical WOW64 views.'
}

function Get-TreeSnapshot([string]$Root) {
    if (!(Test-Path -LiteralPath $Root)) { return '{"exists":false}' }
    $entries = @(Get-ChildItem -LiteralPath $Root -Recurse -Force | ForEach-Object {
        [pscustomobject]@{
            path = [IO.Path]::GetRelativePath($Root, $_.FullName).Replace('\', '/')
            kind = $(if ($_.PSIsContainer) { 'directory' } else { 'file' })
            sha256 = $(if ($_.PSIsContainer) { '' } else { Get-Sha256 $_.FullName })
        }
    } | Sort-Object path)
    return ConvertTo-StableJson $entries
}

function Invoke-Case([string]$Name, [scriptblock]$Body) {
    Write-Host "[installer-test] $Name"
    $watch = [Diagnostics.Stopwatch]::StartNew()
    try {
        $null = & $Body
        Assert-RegistryIsolation
        $results.Add([pscustomobject]@{ name = $Name; passed = $true; seconds = $watch.Elapsed.TotalSeconds })
    } catch {
        $results.Add([pscustomobject]@{
            name = $Name; passed = $false; seconds = $watch.Elapsed.TotalSeconds
            error = $_.Exception.Message; errorDetails = Get-ErrorDetails $_
        })
        throw
    }
}

function Invoke-Package($Fixture, [string]$Label, [switch]$Uninstall, [switch]$DefaultBundle, [switch]$Legacy) {
    $Root = $Fixture.root
    $script:invocationNumber++
    $log = Join-Path $logsRoot ('{0:D2}-{1}.log' -f $script:invocationNumber, $Label)
    $executable = if (!$Uninstall) { $InstallerPath }
        elseif ($Legacy) { Get-FixturePath $Root "$legacyData/$uninstallerRelative" }
        else { Get-FixturePath $Root $uninstallerRelative }
    Assert-True (Test-Path -LiteralPath $executable -PathType Leaf) "Missing executable: $executable"
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"")
    if (!$Uninstall) {
        $arguments += @('/CURRENTUSER', '/SP-', '/LANG=english', '/NOCLOSEAPPLICATIONS',
                        '/NORESTARTAPPLICATIONS', "/OBSDIR=`"$($Fixture.host.root)`"")
        if (!$DefaultBundle) { $arguments += "/DIR=`"$Root`"" }
    }
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (!$process.WaitForExit($InstallerTimeoutSeconds * 1000)) {
        $invocations.Add([pscustomobject]@{ label = $Label; target = $Root; log = $log; pid = $process.Id; timedOut = $true })
        throw "Installer did not exit within $InstallerTimeoutSeconds seconds. Its PID $($process.Id) was not killed. Inspect $log before closing it."
    }
    $exitCode = $process.ExitCode
    $invocations.Add([pscustomobject]@{ label = $Label; target = $Root; obsRoot = $Fixture.host.root; log = $log; exitCode = $exitCode })
    $process.Dispose()
    return $exitCode
}

function Build-FixtureBinary([string]$Name, [string]$Version, [string]$Platform, [switch]$Library) {
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET\Framework64\v4.0.30319\csc.exe'
    Assert-True (Test-Path -LiteralPath $compiler -PathType Leaf) "Missing local C# compiler: $compiler"
    $source = Join-Path $sourcesRoot ($Name + '.cs')
    $extension = if ($Library) { '.dll' } else { '.exe' }
    $output = Join-Path $sourcesRoot ($Name + $extension)
    $body = if ($Library) { 'public sealed class MetadataOnlyFixture { }' } else {
@'
public static class SyntheticObs {
    public static int Main(string[] args) {
        if (args.Length != 2) return 2;
        System.IO.File.WriteAllText(args[1], System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
        while (!System.IO.File.Exists(args[0])) System.Threading.Thread.Sleep(100);
        return 0;
    }
}
'@
    }
    @"
using System.Reflection;
[assembly: AssemblyVersion("$Version")]
[assembly: AssemblyFileVersion("$Version")]
[assembly: AssemblyTitle("Synthetic installer fixture; not OBS")]
$body
"@ | Set-Content -LiteralPath $source -Encoding utf8
    $target = if ($Library) { 'library' } else { 'exe' }
    $compilerOutput = & $compiler /nologo "/target:$target" "/platform:$Platform" "/out:$output" $source 2>&1
    $compilerExit = $LASTEXITCODE
    $compilerOutput | Out-File -LiteralPath (Join-Path $logsRoot ("compile-$Name.log")) -Encoding utf8
    Assert-True ($compilerExit -eq 0 -and (Test-Path -LiteralPath $output -PathType Leaf)) "Fixture compilation failed: $Name"
    return $output
}

function New-ObsFixture([string]$Name, [string]$HostBinary, [string]$QtBinary, [switch]$MissingObs, [switch]$MissingQt) {
    $root = Get-FixturePath $runRoot "hosts/$Name"
    $bin = Get-FixturePath $root 'bin/64bit'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    if (!$MissingObs) { Copy-Item -LiteralPath $HostBinary -Destination (Join-Path $bin 'obs64.exe') }
    if (!$MissingQt) { Copy-Item -LiteralPath $QtBinary -Destination (Join-Path $bin 'Qt6Core.dll') }
    $sentinelNames = @(
        'README.md', 'LICENSE', 'portable_mode.txt', 'unrelated-root.txt', 'obs-plugins/64bit/unrelated-plugin.dll',
        'data/obs-plugins/unrelated-plugin/keep.txt',
        'config/obs-studio/plugin_config/livehime-obs/session.dpapi',
        'config/obs-studio/plugin_config/livehime-obs/device-id',
        'config/obs-studio/plugin_config/livehime-obs/webview/synthetic-state.txt',
        'config/obs-studio/basic/profiles/Synthetic/service.json'
    )
    $sentinels = @{}
    foreach ($relative in $sentinelNames) {
        $path = Get-FixturePath $root $relative
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
        [IO.File]::WriteAllText($path, "SYNTHETIC; NOT CREDENTIALS; preserve $Name/$relative")
        $sentinels[$relative] = Get-Sha256 $path
    }
    if (!$MissingObs) { $sentinels['bin/64bit/obs64.exe'] = Get-Sha256 (Join-Path $bin 'obs64.exe') }
    if (!$MissingQt) { $sentinels['bin/64bit/Qt6Core.dll'] = Get-Sha256 (Join-Path $bin 'Qt6Core.dll') }
    return [pscustomobject]@{ root = $root; sentinels = $sentinels }
}

function New-BundleFixture([string]$Name, $HostFixture) {
    $root = Get-FixturePath $runRoot "bundles/$Name/livehime-obs"
    $sentinels = @{}
    foreach ($relative in @('user-added-note.txt', 'data/user-added-note.txt', 'data/custom/keep.txt')) {
        $path = Get-FixturePath $root $relative
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
        [IO.File]::WriteAllText($path, "SYNTHETIC; preserve bundle file $Name/$relative")
        $sentinels[$relative] = Get-Sha256 $path
    }
    return [pscustomobject]@{
        root = $root; host = $HostFixture; sentinels = $sentinels
        hostSnapshot = Get-TreeSnapshot $HostFixture.root
    }
}

function Assert-Sentinels($Fixture) {
    foreach ($relative in $Fixture.sentinels.Keys) {
        $path = Get-FixturePath $Fixture.root $relative
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Removed unowned fixture file: $path"
        Assert-True ((Get-Sha256 $path) -ceq $Fixture.sentinels[$relative]) "Changed unowned fixture file: $path"
    }
}

function Start-OwnedObs($Fixture) {
    $token = [guid]::NewGuid().ToString('N')
    $stopFlag = Join-Path $processRoot ($token + '.stop')
    $readyFlag = Join-Path $processRoot ($token + '.ready')
    $path = Get-FixturePath $Fixture.root 'bin/64bit/obs64.exe'
    $process = Start-Process -FilePath $path -ArgumentList @("`"$stopFlag`"", "`"$readyFlag`"") `
        -WorkingDirectory ([IO.Path]::GetDirectoryName($path)) -WindowStyle Hidden -PassThru
    $owned = [pscustomobject]@{ process = $process; path = $path; stopFlag = $stopFlag; started = $process.StartTime.ToUniversalTime().Ticks }
    $ownedProcesses.Add($owned)
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while (!(Test-Path -LiteralPath $readyFlag) -and !$process.HasExited -and $watch.Elapsed.TotalSeconds -lt 10) {
        Start-Sleep -Milliseconds 50
    }
    Assert-True (!$process.HasExited -and (Test-Path -LiteralPath $readyFlag)) "Synthetic process did not become ready: $path"
    $observed = Get-CimInstance Win32_Process -Filter "ProcessId = $($process.Id)"
    Assert-True ($observed -and [string]::Equals($observed.ExecutablePath, $path, [StringComparison]::OrdinalIgnoreCase)) `
        "Cannot confirm fixture executable path through WMI: PID $($process.Id)"
    return $owned
}

function Assert-OwnedObsAlive($Owned) {
    $Owned.process.Refresh()
    Assert-True (!$Owned.process.HasExited) "Installer closed the synthetic OBS process: PID $($Owned.process.Id)"
}

function Stop-OwnedObs($Owned) {
    $Owned.process.Refresh()
    if ($Owned.process.HasExited) { return }
    [IO.File]::WriteAllText($Owned.stopFlag, 'normal exit requested by fixture owner')
    if (!$Owned.process.WaitForExit(10000)) {
        # This fallback is limited to a PID and start time created by this run.
        # It never searches for or terminates real OBS processes by image name.
        $current = Get-Process -Id $Owned.process.Id -ErrorAction Stop
        Assert-True ($current.StartTime.ToUniversalTime().Ticks -eq $Owned.started -and
                    [string]::Equals($current.Path, $Owned.path, [StringComparison]::OrdinalIgnoreCase)) `
            'Refusing to terminate a process whose ownership cannot be confirmed.'
        $current.Kill()
        Assert-True ($current.WaitForExit(10000)) 'Synthetic process did not exit after targeted cleanup.'
    }
}

function Assert-Installed($Fixture) {
    $manifestPath = Get-FixturePath $Fixture.root $manifestRelative
    Assert-True (Test-Path -LiteralPath $manifestPath -PathType Leaf) 'Installed payload manifest is missing.'
    $manifest = Get-Content -Raw -LiteralPath $manifestPath | ConvertFrom-Json
    Assert-True ($manifest.schema -eq 2 -and $manifest.layout -eq 'obs-modern-bundle' -and
                $manifest.obsVersion -eq '33.0.1' -and $manifest.qtVersion -eq '6.11.1') `
        'Unexpected installed manifest schema or host compatibility profile.'
    $ownedFiles = [Collections.Generic.Dictionary[string, string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($entry in $manifest.files) {
        $relative = [string]$entry.path
        Assert-True ($relative -ceq $relative.Replace('\', '/') -and
                    ($relative -cin $runtimePaths -or $relative.StartsWith("$pluginData/docs/", [StringComparison]::Ordinal))) `
            "Manifest contains an unexpected installation destination: $relative"
        Assert-True ($entry.sha256 -cmatch '^[0-9a-f]{64}$' -and !$ownedFiles.ContainsKey($relative)) `
            "Invalid or duplicate manifest entry: $relative"
        $path = Get-FixturePath $Fixture.root $relative
        Assert-True (Test-Path -LiteralPath $path -PathType Leaf) "Missing installed payload: $relative"
        Assert-True ((Get-Sha256 $path) -ceq $entry.sha256) "Payload hash mismatch: $relative"
        $ownedFiles.Add($relative, [string]$entry.sha256)
    }
    foreach ($relative in $runtimePaths) {
        Assert-True ($ownedFiles.ContainsKey($relative)) "Runtime file absent from manifest: $relative"
        Assert-True ($ownedFiles[$relative] -ceq $zipPayload[$relative].sha256) "Installed runtime differs from ZIP: $relative"
    }
    foreach ($relative in @("$pluginData/docs/README.md", "$pluginData/docs/AUTHORS.md", "$pluginData/docs/LICENSE", "$pluginData/docs/THIRD-PARTY-NOTICES.md", "$pluginData/docs/VALIDATION.md")) {
        Assert-True ($ownedFiles.ContainsKey($relative)) "Required private documentation absent from manifest: $relative"
    }
    $record = Get-UninstallRecord CurrentUser Registry64
    Assert-True $record.exists 'Current-user uninstall registration is missing.'
    Assert-True ($record.values.Contains('InstallLocation')) 'Uninstall entry does not record InstallLocation.'
    $registeredRoot = [IO.Path]::GetFullPath([string]$record.values['InstallLocation'].value).TrimEnd('\')
    Assert-True ([string]::Equals($registeredRoot, $Fixture.root.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase)) `
        "Uninstall registration points outside its fixture: $registeredRoot"
    foreach ($name in @('InstallLayout', 'BundleRoot', 'ObsRoot')) {
        Assert-True ($record.values.Contains('Inno Setup CodeFile: ' + $name)) "Missing installer registration field: $name"
    }
    Assert-True ($record.values['Inno Setup CodeFile: InstallLayout'].value -eq 'modern-bundle-v1') 'Unexpected registered bundle layout.'
    Assert-True ([string]::Equals([string]$record.values['Inno Setup CodeFile: BundleRoot'].value, $Fixture.root, [StringComparison]::OrdinalIgnoreCase)) `
        'Registered bundle root differs from the actual destination.'
    Assert-True ([string]::Equals([string]$record.values['Inno Setup CodeFile: ObsRoot'].value, $Fixture.host.root, [StringComparison]::OrdinalIgnoreCase)) `
        'Registered host root differs from the selected synthetic OBS.'
    $statePath = Get-FixturePath $Fixture.root 'data/install-state.ini'
    Assert-True (Test-Path -LiteralPath $statePath -PathType Leaf) 'Installer-owned host state is missing.'
    $state = Get-Content -Raw -LiteralPath $statePath
    Assert-True ($state.Contains('ObsRoot=' + $Fixture.host.root)) 'Uninstaller state does not preserve the selected host.'
    $uninstaller = Get-FixturePath $Fixture.root $uninstallerRelative
    Assert-True (Test-Path -LiteralPath $uninstaller -PathType Leaf) 'Expected private uninstaller is missing.'
    $uninstallers = @(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($uninstaller)) -Filter 'unins*.exe' -File)
    Assert-True ($uninstallers.Count -eq 1) 'Reinstallation created more than one uninstaller.'
    Assert-True (!(Test-Path -LiteralPath (Get-FixturePath $Fixture.root 'obs-plugins'))) 'Bundle contains the obsolete OBS-root layout.'
    Assert-True (!(Test-Path -LiteralPath (Get-FixturePath $Fixture.root 'data/obs-plugins'))) 'Bundle contains nested legacy plugin data.'
    Assert-Sentinels $Fixture
    Assert-Sentinels $Fixture.host
    Assert-True ((Get-TreeSnapshot $Fixture.host.root) -ceq $Fixture.hostSnapshot) 'Installation wrote into the selected OBS host.'
    Assert-RegistryIsolation
    return @($ownedFiles.Keys) + @($manifestRelative, $uninstallerRelative, 'installer/unins000.dat', 'data/install-state.ini')
}

function Assert-Uninstalled($Fixture, [string[]]$OwnedFiles, [switch]$Legacy) {
    # Inno's uninstall process can return before its helper deletes unins*.
    # Wait only for owned files; the strict assertions below still identify
    # the exact residue on timeout and verify registration and sentinels.
    $watch = [Diagnostics.Stopwatch]::StartNew()
    while ($watch.ElapsedMilliseconds -lt 10000) {
        $remaining = @($OwnedFiles | Where-Object {
            Test-Path -LiteralPath (Get-FixturePath $Fixture.root $_)
        })
        if ($remaining.Count -eq 0) { break }
        Start-Sleep -Milliseconds 100
    }
    foreach ($relative in $OwnedFiles) {
        Assert-True (!(Test-Path -LiteralPath (Get-FixturePath $Fixture.root $relative))) "Uninstaller left owned payload behind: $relative"
    }
    Assert-True (!(Get-UninstallRecord CurrentUser Registry64).exists) 'Uninstall registration remains after uninstall.'
    Assert-Sentinels $Fixture
    Assert-Sentinels $Fixture.host
    if (!$Legacy) {
        Assert-True ((Get-TreeSnapshot $Fixture.host.root) -ceq $Fixture.hostSnapshot) 'Uninstall changed the selected OBS host.'
    }
    Assert-RegistryIsolation
}

function Assert-RejectedInstall($Fixture, [string]$Label, [string]$ExpectedMessage = '', [switch]$DefaultBundle) {
    $before = Get-TreeSnapshot $Fixture.root
    $hostBefore = Get-TreeSnapshot $Fixture.host.root
    $registryBefore = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)
    Assert-True ((Invoke-Package $Fixture $Label -DefaultBundle:$DefaultBundle) -ne 0) "Installer accepted the invalid/busy target: $Label"
    Assert-True ((Get-TreeSnapshot $Fixture.root) -ceq $before) "Rejected installation changed its target: $Label"
    Assert-True ((Get-TreeSnapshot $Fixture.host.root) -ceq $hostBefore) "Rejected installation changed its selected host: $Label"
    Assert-True ((ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)) -ceq $registryBefore) `
        "Rejected installation changed uninstall registration: $Label"
    if ($ExpectedMessage) {
        $log = Get-Content -Raw -LiteralPath $invocations[$invocations.Count - 1].log
        Assert-True ($log.Contains($ExpectedMessage)) "Installation was rejected for an unexpected reason: $Label"
    }
}

try {
    Write-Host "Installer fixture workspace: $runRoot"
    $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        foreach ($relative in $runtimePaths) {
            $zipRelative = $zipPathMap[$relative]
            $entry = $archive.GetEntry($zipRelative)
            Assert-True ($null -ne $entry) "ZIP runtime entry is missing: $zipRelative"
            $inputStream = $entry.Open()
            $buffer = [IO.MemoryStream]::new()
            $sha = [Security.Cryptography.SHA256]::Create()
            try {
                $inputStream.CopyTo($buffer)
                $bytes = $buffer.ToArray()
                $hash = ($sha.ComputeHash($bytes) | ForEach-Object { $_.ToString('x2') }) -join ''
                $zipPayload[$relative] = [pscustomobject]@{ bytes = $bytes; sha256 = $hash }
            } finally { $sha.Dispose(); $buffer.Dispose(); $inputStream.Dispose() }
        }
    } finally { $archive.Dispose() }
    $validHost = Build-FixtureBinary 'host-minimum' '33.0.0.0' 'x64'
    $patchHost = Build-FixtureBinary 'host-patch' '33.0.3.0' 'x64'
    $minorHost = Build-FixtureBinary 'host-newer-minor' '33.1.0.0' 'x64'
    $newerHost = Build-FixtureBinary 'host-newer-major' '34.0.0.0' 'x64'
    $wrongHost = Build-FixtureBinary 'host-wrong-version' '32.0.0.0' 'x64'
    $x86Host = Build-FixtureBinary 'host-x86' '33.0.1.0' 'x86'
    $validQt = Build-FixtureBinary 'qt-valid' '6.11.1.0' 'x64' -Library
    $newerQt = Build-FixtureBinary 'qt-newer' '6.12.2.0' 'x64' -Library
    $missing = New-BundleFixture 'invalid-missing-obs' (New-ObsFixture 'missing-obs' $validHost $validQt -MissingObs)
    $wrong = New-BundleFixture 'invalid-obs-version' (New-ObsFixture 'wrong-obs' $wrongHost $validQt)
    $x86 = New-BundleFixture 'invalid-x86' (New-ObsFixture 'x86' $x86Host $validQt)
    $missingQt = New-BundleFixture 'invalid-missing-qt' (New-ObsFixture 'missing-qt' $validHost $validQt -MissingQt)
    $mainHost = New-ObsFixture '中文 空格 OBS fixture' $patchHost $validQt
    $main = New-BundleFixture '中文 空格 bundle fixture' $mainHost
    $otherHost = New-ObsFixture 'unrelated-running-obs' $validHost $validQt
    $redirect = New-BundleFixture 'different-upgrade-destination' $mainHost
    $legacy = New-BundleFixture 'legacy-zip-layout' (New-ObsFixture 'legacy-zip-host' $validHost $validQt)

    Invoke-Case 'reject missing OBS' { Assert-RejectedInstall $missing 'missing-obs' }
    Invoke-Case 'reject unsupported OBS version' { Assert-RejectedInstall $wrong 'wrong-obs-version' }
    Invoke-Case 'reject x86 executable' { Assert-RejectedInstall $x86 'wrong-architecture' }
    Invoke-Case 'reject missing Qt runtime' { Assert-RejectedInstall $missingQt 'missing-qt' }

    foreach ($protectedRoot in @(
        [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData),
        [Environment]::GetFolderPath([Environment+SpecialFolder]::ProgramFiles)
    )) {
        $protectedName = [IO.Path]::GetFileName($protectedRoot)
        Invoke-Case "reject current-user write to $protectedName before changing files" {
            # This is the only negative test outside the run directory. No
            # fixture or directory is created there, and the exact bundle is
            # snapshotted before/after to prove the guard ran before writes.
            $protectedFixture = [pscustomobject]@{
                root = Join-Path $protectedRoot 'obs-studio\plugins\livehime-obs'
                host = $mainHost
            }
            Assert-RejectedInstall $protectedFixture "protected-$protectedName" `
                'Administrative installation is required for this shared or protected directory:'
        }
    }

    Invoke-Case 'reject bundle directory name that cannot match the plugin DLL' {
        $badName = [pscustomobject]@{
            root = Get-FixturePath $runRoot 'bundles/wrong-plugin-name'
            host = $mainHost
        }
        Assert-RejectedInstall $badName 'invalid-bundle-name'
    }
    Invoke-Case 'reject old ZIP plugin in the OBS root without adopting or deleting it' {
        foreach ($relative in $runtimePaths) {
            $path = Get-FixturePath $legacy.host.root $zipPathMap[$relative]
            New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllBytes($path, $zipPayload[$relative].bytes)
        }
        Assert-RejectedInstall $legacy 'legacy-zip-duplicate' 'another active copy'
        Assert-Sentinels $legacy.host
    }
    $inPlace = [pscustomobject]@{ root = $legacy.host.root; host = $legacy.host; sentinels = $legacy.host.sentinels }
    Invoke-Case 'keep an unregistered ZIP intact when Setup uses its default bundle' {
        $sharedRoot = [Environment]::GetFolderPath([Environment+SpecialFolder]::CommonApplicationData)
        $default = [pscustomobject]@{ root = Join-Path $sharedRoot 'obs-studio/plugins/livehime-obs'; host = $legacy.host }
        Assert-RejectedInstall $default 'legacy-zip-default-bundle' -DefaultBundle
    }
    Invoke-Case 'require the ZIP update helper before accepting an explicit OBS-root update' {
        $helperPath = Get-FixturePath $inPlace.root $zipPathMap['data/livehime-update-helper.exe']
        Remove-Item -LiteralPath $helperPath
        Assert-RejectedInstall $inPlace 'legacy-zip-no-helper'
        [IO.File]::WriteAllText($helperPath, 'OLD synthetic update helper')
    }
    Invoke-Case 'replace the original ZIP in its explicit OBS root and preserve unrelated files' {
        $oldDll = Get-FixturePath $inPlace.root $zipPathMap['livehime-obs.dll']
        [IO.File]::WriteAllText($oldDll, 'OLD synthetic plugin')
        $noteRelative = "$legacyData/user-added-note.txt"
        $note = Get-FixturePath $inPlace.root $noteRelative
        [IO.File]::WriteAllText($note, 'SYNTHETIC user file inside plugin data')
        $inPlace.sentinels[$noteRelative] = Get-Sha256 $note
        Assert-True ((Invoke-Package $inPlace 'legacy-zip-adopt') -eq 0) 'Explicit in-place ZIP update failed.'
        $record = Get-UninstallRecord CurrentUser Registry64
        Assert-True ($record.values['Inno Setup CodeFile: InstallLayout'].value -eq 'legacy-root-v1') 'Wrong adopted layout.'
        Assert-True ($record.values['Inno Setup CodeFile: ObsRoot'].value -eq $inPlace.root) 'Adopted host registration moved.'
        Assert-True ([IO.Path]::TrimEndingDirectorySeparator($record.values['InstallLocation'].value) -eq $inPlace.root) 'Adopted install root moved.'
        $manifest = Get-Content -LiteralPath (Get-FixturePath $inPlace.root "$legacyData/install-manifest.json") -Raw | ConvertFrom-Json
        Assert-True ($manifest.schema -eq 1) 'The ZIP update used the wrong manifest.'
        foreach ($relative in $runtimePaths) {
            $zipRelative = $zipPathMap[$relative]
            Assert-True ($zipRelative -cin $manifest.files.path) "Missing legacy manifest entry: $zipRelative"
            Assert-True ((Get-Sha256 (Get-FixturePath $inPlace.root $zipRelative)) -ceq $zipPayload[$relative].sha256) "Legacy ZIP payload mismatch: $zipRelative"
        }
        Assert-Sentinels $inPlace.host
        Assert-RegistryIsolation
        $ownedFiles = @($manifest.files.path) + @("$legacyData/install-manifest.json", "$legacyData/install-state.ini",
            "$legacyData/$uninstallerRelative", "$legacyData/installer/unins000.dat")
        Assert-True ((Invoke-Package $inPlace 'legacy-zip-uninstall' -Uninstall -Legacy) -eq 0) 'Adopted ZIP uninstall failed.'
        Assert-Uninstalled $inPlace $ownedFiles -Legacy
    }
    Invoke-Case 'reject nested binary layout inside the selected bundle' {
        $nested = New-BundleFixture 'nested-binary-duplicate' (New-ObsFixture 'nested-binary-host' $validHost $validQt)
        $duplicate = Get-FixturePath $nested.root 'bin/64bit/livehime-obs.dll'
        New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($duplicate)) | Out-Null
        [IO.File]::WriteAllText($duplicate, 'SYNTHETIC nested duplicate; never loaded')
        Assert-RejectedInstall $nested 'nested-binary-duplicate'
    }
    $scenario = 0
    foreach ($marker in @('portable_mode', 'obs_portable_mode', 'portable_mode.txt', 'obs_portable_mode.txt')) {
        foreach ($layout in @('livehime-obs.dll', 'bin/64bit/livehime-obs.dll')) {
            $scenario++
            Invoke-Case "reject portable duplicate $marker / $layout" {
                $portableHost = New-ObsFixture "portable-duplicate-$scenario" $validHost $validQt
                Remove-Item -LiteralPath (Get-FixturePath $portableHost.root 'portable_mode.txt')
                $portableHost.sentinels.Remove('portable_mode.txt')
                $markerPath = Get-FixturePath $portableHost.root $marker
                [IO.File]::WriteAllText($markerPath, 'SYNTHETIC portable-mode marker')
                $portableHost.sentinels[$marker] = Get-Sha256 $markerPath
                $duplicate = Get-FixturePath $portableHost.root "plugins/livehime-obs/$layout"
                New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($duplicate)) | Out-Null
                [IO.File]::WriteAllText($duplicate, 'SYNTHETIC duplicate plugin; never loaded')
                $fixture = New-BundleFixture "portable-duplicate-$scenario" $portableHost
                Assert-RejectedInstall $fixture "portable-duplicate-$scenario"
                Assert-Sentinels $portableHost
            }
        }
    }

    $mainProcess = Start-OwnedObs $mainHost
    Invoke-Case 'reject first install while target OBS runs' {
        Assert-RejectedInstall $main 'busy-first-install'
        Assert-OwnedObsAlive $mainProcess
    }
    Stop-OwnedObs $mainProcess

    $otherProcess = Start-OwnedObs $otherHost
    Invoke-Case 'install on OBS 33.0.3 in a Chinese/space path while unrelated OBS runs' {
        Assert-True ((Invoke-Package $main 'first-install') -eq 0) 'First installation failed.'
        $null = Assert-Installed $main
        Assert-OwnedObsAlive $otherProcess
    }
    Stop-OwnedObs $otherProcess
    Invoke-Case 'keep the registered bundle after OBS and its Qt runtime update in place' {
        Copy-Item -LiteralPath $minorHost -Destination (Get-FixturePath $mainHost.root 'bin/64bit/obs64.exe') -Force
        Copy-Item -LiteralPath $newerQt -Destination (Get-FixturePath $mainHost.root 'bin/64bit/Qt6Core.dll') -Force
        $mainHost.sentinels['bin/64bit/obs64.exe'] = Get-Sha256 $minorHost
        $mainHost.sentinels['bin/64bit/Qt6Core.dll'] = Get-Sha256 $newerQt
        $main.hostSnapshot = Get-TreeSnapshot $mainHost.root
        Assert-True ((Invoke-Package $main 'after-host-update') -eq 0) 'The registered installation rejected the updated OBS host.'
        $null = Assert-Installed $main
    }
    Invoke-Case 'same-version reinstall repairs plugin payload' {
        [IO.File]::WriteAllText((Get-FixturePath $main.root $runtimePaths[0]), 'SYNTHETIC OLD PLUGIN CONTENT')
        Assert-True ((Invoke-Package $main 'same-version-reinstall') -eq 0) 'Same-version reinstallation failed.'
        $null = Assert-Installed $main
    }

    $mainProcess = Start-OwnedObs $mainHost
    Invoke-Case 'reject upgrade while target OBS runs' {
        Assert-RejectedInstall $main 'busy-upgrade'
        Assert-OwnedObsAlive $mainProcess
    }
    Invoke-Case 'reject uninstall while target OBS runs' {
        $before = Get-TreeSnapshot $main.root
        $hostBefore = Get-TreeSnapshot $mainHost.root
        $registryBefore = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)
        Assert-True ((Invoke-Package $main 'busy-uninstall' -Uninstall) -ne 0) 'Uninstaller accepted a running target.'
        Assert-OwnedObsAlive $mainProcess
        Assert-True ((Get-TreeSnapshot $main.root) -ceq $before) 'Rejected uninstall changed fixture files.'
        Assert-True ((Get-TreeSnapshot $mainHost.root) -ceq $hostBefore) 'Rejected uninstall changed host files.'
        Assert-True ((ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)) -ceq $registryBefore) `
            'Rejected uninstall changed registration.'
    }
    Stop-OwnedObs $mainProcess
    Invoke-Case 'reject changing upgrade destination' {
        $originalBefore = Get-TreeSnapshot $main.root
        Assert-RejectedInstall $redirect 'redirect-upgrade'
        Assert-True ((Get-TreeSnapshot $main.root) -ceq $originalBefore) 'Rejected redirected upgrade changed the original target.'
    }
    Invoke-Case 'reject changing the selected OBS host during an upgrade' {
        $originalHostBefore = Get-TreeSnapshot $mainHost.root
        $redirectHost = [pscustomobject]@{ root = $main.root; host = $otherHost }
        Assert-RejectedInstall $redirectHost 'redirect-host-upgrade'
        Assert-True ((Get-TreeSnapshot $mainHost.root) -ceq $originalHostBefore) 'Rejected host change modified the original host.'
    }
    Invoke-Case 'uninstall preserves host, unrelated files and synthetic account' {
        $ownedFiles = Assert-Installed $main
        Assert-True ((Invoke-Package $main 'uninstall' -Uninstall) -eq 0) 'Normal uninstall failed.'
        Assert-Uninstalled $main $ownedFiles
    }

    $modern = New-BundleFixture 'manual-modern-bundle' (New-ObsFixture 'manual-modern-host' $newerHost $newerQt)
    Invoke-Case 'repair an unregistered modern bundle on a newer OBS major and Qt minor' {
        foreach ($relative in $runtimePaths) {
            $path = Get-FixturePath $modern.root $relative
            New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllBytes($path, $zipPayload[$relative].bytes)
        }
        Assert-True ((Invoke-Package $modern 'manual-modern-repair') -eq 0) 'Installation over the modern flat bundle failed.'
        $null = Assert-Installed $modern
    }
    Invoke-Case 'uninstall repaired modern bundle while preserving synthetic account' {
        $ownedFiles = Assert-Installed $modern
        Assert-True ((Invoke-Package $modern 'manual-modern-uninstall' -Uninstall) -eq 0) 'Uninstall of repaired modern bundle failed.'
        Assert-Uninstalled $modern $ownedFiles
    }
} catch {
    $failure = $_
} finally {
    foreach ($owned in $ownedProcesses) {
        try { Stop-OwnedObs $owned } catch {
            $results.Add([pscustomobject]@{
                name = 'owned fixture process cleanup'; passed = $false
                error = $_.Exception.Message; errorDetails = Get-ErrorDetails $_
            })
            if (!$failure) { $failure = $_ }
        }
    }
    $report = [ordered]@{
        schema = 2; layout = 'obs-modern-bundle'; passed = ($null -eq $failure); workspace = $runRoot; fixtureIdentity = [bool]$FixtureIdentity
        installer = $InstallerPath; installerSha256 = Get-Sha256 $InstallerPath
        package = $PackagePath; packageSha256 = Get-Sha256 $PackagePath
        tests = $results.ToArray(); invocations = $invocations.ToArray()
        error = $(if ($failure) { $failure.Exception.Message } else { $null })
        errorDetails = $(if ($failure) { Get-ErrorDetails $failure } else { $null })
        retainedCurrentUserRegistration = (Get-UninstallRecord CurrentUser Registry64).exists
        scope = 'Synthetic OBS metadata and process fixtures only; no real OBS module loading, credentials or network operations.'
    }
    $reportPath = Join-Path $runRoot 'validation.json'
    $report | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Host "Validation report: $reportPath"
    Write-Host 'Fixtures and logs were retained. No directory was recursively removed.'
}
if ($failure) {
    Write-Host "Installer validation failed. Inspect $runRoot. A failed installation is not automatically uninstalled."
    Write-Host ((Get-ErrorDetails $failure) | ConvertTo-Json -Depth 6)
    throw $failure
}
Write-Host "Installer validation passed ($($results.Count) cases). HKCU registration was removed by the tested uninstallers."
