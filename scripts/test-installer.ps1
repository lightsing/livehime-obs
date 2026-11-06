#requires -Version 7.0
<#
.SYNOPSIS
Validates the Windows installer using isolated, synthetic OBS installations.
.DESCRIPTION
Installs with CURRENTUSER, then checks that same-version repair retains this
scope without an explicit override. Requires the fixed
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
    [ValidateRange(15, 600)][int]$InstallerTimeoutSeconds = 120
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitProcess) {
    throw 'Run this validation in 64-bit PowerShell 7 on Windows.'
}
$InstallerPath = [IO.Path]::GetFullPath($InstallerPath)
if (!$PackagePath) {
    $projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
    $versionMatch = [regex]::Match((Get-Content -Raw -LiteralPath (Join-Path $projectRoot 'CMakeLists.txt')),
        'project\(livehime-obs\s+VERSION\s+(\d+\.\d+\.\d+)\s')
    if (!$versionMatch.Success) { throw 'Cannot read the project version from CMakeLists.txt.' }
    $PackagePath = Join-Path $projectRoot "dist\livehime-obs-$($versionMatch.Groups[1].Value)-windows-x64.zip"
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
$pluginData = 'data/obs-plugins/livehime-obs'
$manifestRelative = "$pluginData/install-manifest.json"
$uninstallerRelative = "$pluginData/installer/unins000.exe"
$runtimePaths = @(
    'obs-plugins/64bit/livehime-obs.dll',
    "$pluginData/locale/en-US.ini",
    "$pluginData/locale/zh-CN.ini",
    "$pluginData/qt-plugins/tls/qschannelbackend.dll"
)

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

function Invoke-Package([string]$Root, [string]$Label, [switch]$Uninstall, [switch]$UsePreviousScope) {
    $script:invocationNumber++
    $log = Join-Path $logsRoot ('{0:D2}-{1}.log' -f $script:invocationNumber, $Label)
    $executable = if ($Uninstall) { Get-FixturePath $Root $uninstallerRelative } else { $InstallerPath }
    Assert-True (Test-Path -LiteralPath $executable -PathType Leaf) "Missing executable: $executable"
    $arguments = @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', "/LOG=`"$log`"")
    if (!$Uninstall) {
        if (!$UsePreviousScope) { $arguments += '/CURRENTUSER' }
        $arguments += @('/SP-', '/LANG=english', '/NOCLOSEAPPLICATIONS',
                        '/NORESTARTAPPLICATIONS', "/DIR=`"$Root`"")
    }
    $process = Start-Process -FilePath $executable -ArgumentList $arguments -PassThru -WindowStyle Hidden
    if (!$process.WaitForExit($InstallerTimeoutSeconds * 1000)) {
        $invocations.Add([pscustomobject]@{ label = $Label; target = $Root; log = $log; pid = $process.Id; timedOut = $true })
        throw "Installer did not exit within $InstallerTimeoutSeconds seconds. Its PID $($process.Id) was not killed. Inspect $log before closing it."
    }
    $exitCode = $process.ExitCode
    $invocations.Add([pscustomobject]@{ label = $Label; target = $Root; log = $log; exitCode = $exitCode })
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

function New-ObsFixture([string]$Name, [string]$HostBinary, [string]$QtBinary, [switch]$MissingObs) {
    $root = Get-FixturePath $runRoot $Name
    $bin = Get-FixturePath $root 'bin/64bit'
    New-Item -ItemType Directory -Path $bin -Force | Out-Null
    if (!$MissingObs) { Copy-Item -LiteralPath $HostBinary -Destination (Join-Path $bin 'obs64.exe') }
    Copy-Item -LiteralPath $QtBinary -Destination (Join-Path $bin 'Qt6Core.dll')
    $sentinelNames = @(
        'README.md', 'LICENSE', 'portable_mode.txt', 'unrelated-root.txt', 'obs-plugins/64bit/unrelated-plugin.dll',
        'data/obs-plugins/unrelated-plugin/keep.txt', "$pluginData/user-added-note.txt",
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
    $sentinels['bin/64bit/Qt6Core.dll'] = Get-Sha256 (Join-Path $bin 'Qt6Core.dll')
    return [pscustomobject]@{ root = $root; sentinels = $sentinels }
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
    Assert-True ($manifest.schema -eq 1 -and $manifest.obsVersion -eq '33.0.1' -and $manifest.qtVersion -eq '6.11.1') `
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
    $uninstaller = Get-FixturePath $Fixture.root $uninstallerRelative
    Assert-True (Test-Path -LiteralPath $uninstaller -PathType Leaf) 'Expected private uninstaller is missing.'
    $uninstallers = @(Get-ChildItem -LiteralPath ([IO.Path]::GetDirectoryName($uninstaller)) -Filter 'unins*.exe' -File)
    Assert-True ($uninstallers.Count -eq 1) 'Reinstallation created more than one uninstaller.'
    Assert-Sentinels $Fixture
    Assert-RegistryIsolation
    return @($ownedFiles.Keys) + @($manifestRelative, $uninstallerRelative, "$pluginData/installer/unins000.dat")
}

function Assert-Uninstalled($Fixture, [string[]]$OwnedFiles) {
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
    Assert-RegistryIsolation
}

function Assert-RejectedInstall($Fixture, [string]$Label) {
    $before = Get-TreeSnapshot $Fixture.root
    $registryBefore = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)
    Assert-True ((Invoke-Package $Fixture.root $Label) -ne 0) "Installer accepted the invalid/busy target: $Label"
    Assert-True ((Get-TreeSnapshot $Fixture.root) -ceq $before) "Rejected installation changed its target: $Label"
    Assert-True ((ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)) -ceq $registryBefore) `
        "Rejected installation changed uninstall registration: $Label"
}

try {
    Write-Host "Installer fixture workspace: $runRoot"
    $archive = [IO.Compression.ZipFile]::OpenRead($PackagePath)
    try {
        foreach ($relative in $runtimePaths) {
            $entry = $archive.GetEntry($relative)
            Assert-True ($null -ne $entry) "ZIP runtime entry is missing: $relative"
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
    $validHost = Build-FixtureBinary 'host-valid' '33.0.1.0' 'x64'
    $wrongHost = Build-FixtureBinary 'host-wrong-version' '32.0.0.0' 'x64'
    $x86Host = Build-FixtureBinary 'host-x86' '33.0.1.0' 'x86'
    $validQt = Build-FixtureBinary 'qt-valid' '6.11.1.0' 'x64' -Library
    $wrongQt = Build-FixtureBinary 'qt-wrong-version' '6.10.0.0' 'x64' -Library
    $missing = New-ObsFixture 'invalid-missing-obs' $validHost $validQt -MissingObs
    $wrong = New-ObsFixture 'invalid-obs-version' $wrongHost $validQt
    $x86 = New-ObsFixture 'invalid-x86' $x86Host $validQt
    $badQt = New-ObsFixture 'invalid-qt-version' $validHost $wrongQt
    $main = New-ObsFixture '中文 空格 OBS fixture' $validHost $validQt
    $other = New-ObsFixture 'unrelated-running-obs' $validHost $validQt
    $redirect = New-ObsFixture 'different-upgrade-destination' $validHost $validQt
    $legacy = New-ObsFixture 'legacy-zip-layout' $validHost $validQt

    Invoke-Case 'reject missing OBS' { Assert-RejectedInstall $missing 'missing-obs' }
    Invoke-Case 'reject unsupported OBS version' { Assert-RejectedInstall $wrong 'wrong-obs-version' }
    Invoke-Case 'reject x86 executable' { Assert-RejectedInstall $x86 'wrong-architecture' }
    Invoke-Case 'reject unsupported Qt version' { Assert-RejectedInstall $badQt 'wrong-qt-version' }

    Invoke-Case 'reject portable bundle duplicates across four markers and two layouts' {
        $scenario = 0
        foreach ($marker in @('portable_mode', 'obs_portable_mode', 'portable_mode.txt', 'obs_portable_mode.txt')) {
            foreach ($layout in @('livehime-obs.dll', 'bin/64bit/livehime-obs.dll')) {
                $scenario++
                $fixture = New-ObsFixture "portable-duplicate-$scenario" $validHost $validQt
                Remove-Item -LiteralPath (Get-FixturePath $fixture.root 'portable_mode.txt')
                $fixture.sentinels.Remove('portable_mode.txt')
                $markerPath = Get-FixturePath $fixture.root $marker
                [IO.File]::WriteAllText($markerPath, 'SYNTHETIC portable-mode marker')
                $fixture.sentinels[$marker] = Get-Sha256 $markerPath
                $duplicate = Get-FixturePath $fixture.root "plugins/livehime-obs/$layout"
                New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($duplicate)) | Out-Null
                [IO.File]::WriteAllText($duplicate, 'SYNTHETIC duplicate plugin; never loaded')
                Assert-RejectedInstall $fixture "portable-duplicate-$marker-$scenario"
                Assert-Sentinels $fixture
            }
        }
    }

    $mainProcess = Start-OwnedObs $main
    Invoke-Case 'reject first install while target OBS runs' {
        Assert-RejectedInstall $main 'busy-first-install'
        Assert-OwnedObsAlive $mainProcess
    }
    Stop-OwnedObs $mainProcess

    $otherProcess = Start-OwnedObs $other
    Invoke-Case 'install to Chinese/space path while unrelated OBS runs' {
        Assert-True ((Invoke-Package $main.root 'first-install') -eq 0) 'First installation failed.'
        $null = Assert-Installed $main
        Assert-OwnedObsAlive $otherProcess
    }
    Stop-OwnedObs $otherProcess
    Invoke-Case 'same-version reinstall repairs plugin payload' {
        [IO.File]::WriteAllText((Get-FixturePath $main.root $runtimePaths[0]), 'SYNTHETIC OLD PLUGIN CONTENT')
        Assert-True ((Invoke-Package $main.root 'same-version-reinstall' -UsePreviousScope) -eq 0) 'Same-version reinstallation failed.'
        $null = Assert-Installed $main
    }

    $mainProcess = Start-OwnedObs $main
    Invoke-Case 'reject upgrade while target OBS runs' {
        Assert-RejectedInstall $main 'busy-upgrade'
        Assert-OwnedObsAlive $mainProcess
    }
    Invoke-Case 'reject uninstall while target OBS runs' {
        $before = Get-TreeSnapshot $main.root
        $registryBefore = ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)
        Assert-True ((Invoke-Package $main.root 'busy-uninstall' -Uninstall) -ne 0) 'Uninstaller accepted a running target.'
        Assert-OwnedObsAlive $mainProcess
        Assert-True ((Get-TreeSnapshot $main.root) -ceq $before) 'Rejected uninstall changed fixture files.'
        Assert-True ((ConvertTo-StableJson (Get-UninstallRecord CurrentUser Registry64)) -ceq $registryBefore) `
            'Rejected uninstall changed registration.'
    }
    Stop-OwnedObs $mainProcess
    Invoke-Case 'reject changing upgrade destination' {
        $originalBefore = Get-TreeSnapshot $main.root
        Assert-RejectedInstall $redirect 'redirect-upgrade'
        Assert-True ((Get-TreeSnapshot $main.root) -ceq $originalBefore) 'Rejected redirected upgrade changed the original target.'
    }
    Invoke-Case 'uninstall preserves host, unrelated files and synthetic account' {
        $ownedFiles = Assert-Installed $main
        Assert-True ((Invoke-Package $main.root 'uninstall' -Uninstall) -eq 0) 'Normal uninstall failed.'
        Assert-Uninstalled $main $ownedFiles
    }

    Invoke-Case 'adopt the old ZIP layout without losing user files' {
        foreach ($relative in $runtimePaths) {
            $path = Get-FixturePath $legacy.root $relative
            New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($path)) | Out-Null
            [IO.File]::WriteAllBytes($path, $zipPayload[$relative].bytes)
        }
        Assert-True ((Invoke-Package $legacy.root 'legacy-zip-upgrade') -eq 0) 'Installation over the old ZIP layout failed.'
        $null = Assert-Installed $legacy
    }
    Invoke-Case 'uninstall adopted ZIP payload while preserving synthetic account' {
        $ownedFiles = Assert-Installed $legacy
        Assert-True ((Invoke-Package $legacy.root 'legacy-uninstall' -Uninstall) -eq 0) 'Uninstall of adopted ZIP layout failed.'
        Assert-Uninstalled $legacy $ownedFiles
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
        schema = 1; passed = ($null -eq $failure); workspace = $runRoot
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
