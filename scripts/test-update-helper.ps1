#requires -Version 7.0
[CmdletBinding()]
param([Parameter(Mandatory)][string]$HelperPath)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$HelperPath = (Resolve-Path -LiteralPath $HelperPath).Path
$runId = [guid]::NewGuid().ToString('N')
$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
$run = Join-Path $tempRoot "livehime-helper-$runId"
$obsDir = Join-Path $run 'OBS 中文 with spaces'
$bundleDir = Join-Path $run 'portable plugin'
$cache = Join-Path $run 'update cache'
$hostExe = Join-Path $obsDir "lh-test-$runId.exe"
$installer = Join-Path $cache 'setup fixture.exe'
$cacheHelper = Join-Path $cache 'helper copy.exe'
$marker = Join-Path $cache 'launched.txt'
$children = [Collections.Generic.List[Diagnostics.Process]]::new()
$signals = [Collections.Generic.List[string]]::new()
$finishInstaller = Join-Path $cache 'installer.done'
$signals.Add($finishInstaller)

function Assert([bool]$condition, [string]$message) {
    if (!$condition) { throw $message }
}
function Start-Fixture([string]$path, [string[]]$arguments) {
    $info = [Diagnostics.ProcessStartInfo]::new($path)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.WindowStyle = 'Hidden'
    $info.Environment['LIVEHIME_UPDATE_FIXTURE_ID'] = $runId
    foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
    $process = [Diagnostics.Process]::Start($info)
    $children.Add($process)
    return $process
}
function Start-HostFixture([string]$label) {
    $signal = Join-Path $run "$label.done"
    $signals.Add($signal)
    return Start-Fixture $hostExe @('--wait', $signal)
}
function Start-Helper([int64]$processId, [string]$digest, [switch]$WithBundle) {
    $arguments = @('--pid', "$processId", '--installer', $installer, '--sha256', $digest, '--obs-dir', $obsDir)
    if ($WithBundle) { $arguments += @('--bundle-dir', $bundleDir) }
    return Start-Fixture $cacheHelper $arguments
}
function Wait-Exit([Diagnostics.Process]$process, [int]$expected) {
    Assert ($process.WaitForExit(8000)) "Timed out waiting for fixture PID $($process.Id)."
    Assert ($process.ExitCode -eq $expected) "PID $($process.Id) returned $($process.ExitCode), expected $expected."
}
function Wait-Until([scriptblock]$condition, [string]$message) {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (!(& $condition) -and $timer.ElapsedMilliseconds -lt 8000) { Start-Sleep -Milliseconds 40 }
    Assert ([bool](& $condition)) $message
}

try {
    foreach ($dir in $obsDir, $bundleDir, $cache) { $null = [IO.Directory]::CreateDirectory($dir) }
    $fixtureSource = Join-Path $run 'fixture.cs'
    @'
using System;
using System.IO;
using System.Threading;
internal static class Fixture {
    [STAThread] private static int Main(string[] args) {
        bool host = args.Length == 2 && args[0] == "--wait";
        string here = AppDomain.CurrentDomain.BaseDirectory;
        string stop = host ? args[1] : Path.Combine(here, "installer.done");
        if (!host) File.AppendAllLines(Path.Combine(here, "launched.txt"), args);
        DateTime deadline = DateTime.UtcNow.AddSeconds(35);
        while (!File.Exists(stop) && DateTime.UtcNow < deadline) Thread.Sleep(40);
        return File.Exists(stop) ? 0 : 1;
    }
}
'@ | Set-Content -LiteralPath $fixtureSource -Encoding utf8
    $csc = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    & $csc /nologo /target:winexe /platform:x64 "/out:$hostExe" $fixtureSource
    Assert ($LASTEXITCODE -eq 0) 'Could not compile synthetic host/installer.'
    Copy-Item -LiteralPath $hostExe -Destination $installer
    Copy-Item -LiteralPath $HelperPath -Destination $cacheHelper
    $digest = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash

    $parent = Start-HostFixture 'parent'
    $other = Start-HostFixture 'other'
    $first = Start-Helper $parent.Id $digest -WithBundle
    $second = Start-Helper $parent.Id $digest -WithBundle
    Wait-Until { $first.HasExited -or $second.HasExited } 'Duplicate helper failed to coalesce.'
    if ($first.HasExited) { $primary = $second; $duplicate = $first } else { $primary = $first; $duplicate = $second }
    Wait-Exit $duplicate 2
    Assert (!$primary.HasExited -and !(Test-Path -LiteralPath $marker)) 'Installer started before the parent exited.'
    [IO.File]::WriteAllText((Join-Path $run 'parent.done'), '')
    Wait-Exit $parent 0
    Start-Sleep -Milliseconds 800
    Assert (!$primary.HasExited -and !$other.HasExited -and !(Test-Path -LiteralPath $marker)) 'Another OBS instance did not defer setup.'
    [IO.File]::WriteAllText((Join-Path $run 'other.done'), '')
    Wait-Exit $other 0
    Wait-Until { [IO.File]::Exists($marker) -and (Get-Item -LiteralPath $marker).Length -gt 0 } 'The synthetic installer did not start.'
    $received = @(Get-Content -LiteralPath $marker -Encoding utf8)
    Assert ($received.Count -eq 2 -and $received[0] -ceq "/OBSDIR=$obsDir" -and $received[1] -ceq "/DIR=$bundleDir") 'Interactive installer arguments changed.'
    $duringSetup = Start-Helper $parent.Id $digest
    Wait-Exit $duringSetup 2
    [IO.File]::WriteAllText($finishInstaller, '')
    Wait-Exit $primary 0

    # The original host PID is now stale, just as it can be when the helper is launched on OBS exit.
    Remove-Item -LiteralPath $marker
    $afterExit = Start-Helper $parent.Id $digest
    Wait-Exit $afterExit 0
    $received = @(Get-Content -LiteralPath $marker -Encoding utf8)
    Assert ($received.Count -eq 1 -and $received[0] -ceq "/OBSDIR=$obsDir") 'The optional bundle directory must stay optional.'

    Remove-Item -LiteralPath $marker
    $writer = [IO.File]::Open($installer, 'Open', 'Write', 'ReadWrite')
    try { Wait-Exit (Start-Helper $parent.Id $digest) 4 } finally { $writer.Dispose() }
    Assert (!(Test-Path -LiteralPath $marker)) 'An installer still open for writing unexpectedly ran.'
    $pending = Start-HostFixture 'pending'
    $tampered = Start-Helper $pending.Id $digest
    $append = [IO.File]::Open($installer, 'Append', 'Write', 'None')
    try { $append.WriteByte(0) } finally { $append.Dispose() }
    [IO.File]::WriteAllText((Join-Path $run 'pending.done'), '')
    Wait-Exit $pending 0
    Wait-Exit $tampered 4
    Assert (!(Test-Path -LiteralPath $marker)) 'Modified installer unexpectedly ran.'
    Wait-Exit (Start-Helper $parent.Id 'invalid-digest') 1
    Write-Output 'Deferred helper passed parent exit, multiple hosts, duplicate requests, pinned setup, path arguments, and tamper checks.'
} finally {
    # Ask only these synthetic fixtures to exit; never signal or terminate a real OBS process.
    foreach ($signal in $signals) { if ([IO.Directory]::Exists((Split-Path -Parent $signal))) { [IO.File]::WriteAllText($signal, '') } }
    $allExited = $true
    foreach ($child in $children) {
        if (!$child.HasExited -and !$child.WaitForExit(8000)) { $allExited = $false }
        $child.Dispose()
    }
    $resolved = [IO.Path]::GetFullPath($run)
    if ($allExited -and [IO.Directory]::Exists($resolved) -and
        [IO.Path]::GetDirectoryName($resolved) -eq $tempRoot.TrimEnd('\') -and
        [IO.Path]::GetFileName($resolved) -ceq "livehime-helper-$runId") {
        Remove-Item -LiteralPath $resolved -Recurse -Force
    }
}
