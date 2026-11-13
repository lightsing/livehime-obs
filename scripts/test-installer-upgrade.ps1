#requires -Version 7.0
<#
.SYNOPSIS
Isolated modern-0.1 / legacy-0.2 and modern-0.2.1 to 0.2.2 registered-upgrade matrix.
.DESCRIPTION
Uses only the separate fixture AppId, locally compiled OBS/Qt metadata fixtures,
and synthetic account/scene files. No real OBS executable or account is read or
started. Original installer sources are frozen under validation/installer-upgrade-fixtures.
All artifacts remain after the test. Failed installations are never auto-uninstalled.
#>
[CmdletBinding()]
param(
    [string]$InstallerPath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.2-windows-x64-setup-fixture.exe'),
    [string]$PackagePath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.2-windows-x64.zip'),
    [string]$OldLegacyPackagePath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.0-windows-x64.zip'),
    [string]$OldCurrentPackagePath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.1-windows-x64.zip'),
    [string]$OldCurrentInstallerPath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.1-windows-x64-setup-fixture.exe'),
    [string]$OldModernPackagePath = 'D:/workspace/livehime-obs/dist/livehime-obs-0.1.0-windows-x64.zip',
    [string]$RegressedInstallerPath = (Join-Path $PSScriptRoot '../dist/livehime-obs-0.2.0-windows-x64-setup-fixture.exe'),
    [string]$Workspace = (Join-Path $PSScriptRoot '../build/installer-upgrade-validation'),
    [string]$InnoCompiler = 'D:/REA/tools/innosetup-7.1.1/ISCC.exe',
    [ValidateRange(15,600)][int]$InstallerTimeoutSeconds = 120,
    [switch]$PrepareOnly
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (!$IsWindows -or ![Environment]::Is64BitProcess) { throw 'Requires 64-bit Windows PowerShell 7.' }
$projectRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
foreach ($name in @('InstallerPath','PackagePath','OldLegacyPackagePath','OldModernPackagePath','OldCurrentPackagePath','OldCurrentInstallerPath','RegressedInstallerPath','Workspace','InnoCompiler')) {
    $value = [IO.Path]::GetFullPath((Get-Variable -Name $name -ValueOnly))
    if ($value -notmatch '^[A-Za-z]:\\' -or $value -match '["\r\n]') { throw "Unsafe local path: $name" }
    Set-Variable -Name $name -Value $value
}
if ([IO.Path]::GetFileName($InstallerPath) -notmatch '-fixture\.exe$') { throw 'Only a dedicated fixture installer may be tested.' }
if ($Workspace.TrimEnd('\') -eq [IO.Path]::GetPathRoot($Workspace).TrimEnd('\')) { throw 'Workspace cannot be a drive root.' }
foreach ($inputPath in @($PackagePath,$OldLegacyPackagePath,$OldModernPackagePath,$OldCurrentPackagePath,$OldCurrentInstallerPath,$RegressedInstallerPath,$InnoCompiler)) {
    if (!(Test-Path -LiteralPath $inputPath -PathType Leaf)) { throw "Missing input: $inputPath" }
}
if (!$PrepareOnly -and !(Test-Path -LiteralPath $InstallerPath -PathType Leaf)) { throw 'Build the new fixture installer before running the matrix.' }

$fixtureGuid = '{7F302FCE-7BA4-4ABE-92CF-62D0937D87AD}'
$formalGuid = '{D39A69EF-8533-4CB2-8A13-770CBCA96340}'
function Assert([bool]$Condition,[string]$Message) { if (!$Condition) { throw $Message } }
function Json($Value) { ConvertTo-Json -InputObject $Value -Depth 15 -Compress }
function Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant() }
function SafePath([string]$Root,[string]$Relative) {
    Assert (![IO.Path]::IsPathRooted($Relative) -and $Relative -notmatch '(^|[/\\])\.\.([/\\]|$)|:') "Unsafe relative path: $Relative"
    $path = [IO.Path]::GetFullPath((Join-Path $Root $Relative))
    Assert ($path.StartsWith([IO.Path]::GetFullPath($Root).TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) 'Fixture path escaped its root.'
    return $path
}
function Record([string]$Guid,[Microsoft.Win32.RegistryHive]$Hive,[Microsoft.Win32.RegistryView]$View) {
    $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($Hive,$View)
    $key = $null
    try {
        $key = $base.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Uninstall\${Guid}_is1",$false)
        $values = [ordered]@{}
        if ($key) { foreach ($name in @($key.GetValueNames()|Sort-Object)) { $values[$name]=[ordered]@{kind=$key.GetValueKind($name).ToString();value=$key.GetValue($name,$null,[Microsoft.Win32.RegistryValueOptions]::DoNotExpandEnvironmentNames)} } }
        return [pscustomobject]@{exists=($null -ne $key);values=$values}
    } finally { if ($key) {$key.Dispose()};$base.Dispose() }
}
function FixtureRecord { Record $fixtureGuid CurrentUser Registry64 }
$protectedBefore = @{}
foreach ($guid in @($formalGuid,$fixtureGuid)) {
    foreach ($hive in @('CurrentUser','LocalMachine')) {
        foreach ($view in @('Registry64','Registry32')) {
            $record = Record $guid $hive $view
            if ($guid -eq $fixtureGuid) { Assert (!$record.exists) 'Fixture AppId already registered. Inspect/uninstall that isolated fixture before testing.' }
            if ($guid -eq $formalGuid -or $hive -eq 'LocalMachine') { $protectedBefore["$guid/$hive/$view"] = Json $record }
        }
    }
}
function AssertIsolation {
    foreach ($id in $protectedBefore.Keys) {
        $parts=$id.Split('/')
        Assert ((Json (Record $parts[0] $parts[1] $parts[2])) -ceq $protectedBefore[$id]) "Protected registration changed: $id"
    }
}
$runRoot=Join-Path $Workspace ('run-'+(Get-Date -Format yyyyMMdd-HHmmss)+'-'+[guid]::NewGuid().ToString('N'))
foreach ($folder in @('logs','sources','signals','installers','stages')) { New-Item -ItemType Directory -Force -Path (Join-Path $runRoot $folder)|Out-Null }
$logs=Join-Path $runRoot 'logs';$sources=Join-Path $runRoot 'sources'
$results=[Collections.Generic.List[object]]::new();$invocations=[Collections.Generic.List[object]]::new();$ownedProcesses=[Collections.Generic.List[object]]::new();$upgrades=[Collections.Generic.List[object]]::new()
$failure=$null;$script:number=0
$mapping=[ordered]@{
    'obs-plugins/64bit/livehime-obs.dll'='livehime-obs.dll'
    'data/obs-plugins/livehime-obs/locale/en-US.ini'='data/locale/en-US.ini'
    'data/obs-plugins/livehime-obs/locale/zh-CN.ini'='data/locale/zh-CN.ini'
    'data/obs-plugins/livehime-obs/qt-plugins/tls/qschannelbackend.dll'='data/qt-plugins/tls/qschannelbackend.dll'
}
function ReadPayload([string]$Zip) {
    $archive=[IO.Compression.ZipFile]::OpenRead($Zip)
    try {
        $hashEntry=$archive.GetEntry('SHA256SUMS.json');Assert ($null -ne $hashEntry) 'ZIP lacks SHA256SUMS.json.'
        $reader=[IO.StreamReader]::new($hashEntry.Open())
        try {$declared=@($reader.ReadToEnd()|ConvertFrom-Json)} finally {$reader.Dispose()}
        $entries=@{};foreach($entry in $archive.Entries){if(!$entry.FullName.EndsWith('/')){Assert (!$entries.ContainsKey($entry.FullName)) 'Duplicate ZIP entry.';$entries[$entry.FullName]=$entry}}
        $verified=@{}
        foreach($item in $declared){
            Assert ($item.path -notmatch '(^/|\\|:|(^|/)\.\.(/|$))' -and $item.sha256 -match '^[a-fA-F0-9]{64}$' -and !$verified.ContainsKey($item.path)) 'Invalid ZIP manifest.'
            Assert ($entries.ContainsKey($item.path) -and $entries[$item.path].Length -le 64MB) 'Missing or oversized ZIP entry.'
            $stream=$entries[$item.path].Open();try{$actual=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData($stream)).ToLowerInvariant()}finally{$stream.Dispose()}
            Assert ($actual -eq $item.sha256) 'ZIP SHA256 mismatch.';$verified[$item.path]=$true
        }
        Assert ($verified.Count -eq $entries.Count-1) 'Unverified ZIP file.'
        $payload=@{}
        foreach($legacy in $mapping.Keys){
            $name=if($verified.ContainsKey($legacy)){$legacy}else{$mapping[$legacy]}
            Assert ($verified.ContainsKey($name)) "Missing runtime payload: $legacy"
            $stream=$entries[$name].Open();$memory=[IO.MemoryStream]::new()
            try{$stream.CopyTo($memory);$payload[$legacy]=$memory.ToArray()}finally{$stream.Dispose();$memory.Dispose()}
        }
        return $payload
    } finally {$archive.Dispose()}
}
function WriteStage([string]$Layout,[string]$Version,$Payload) {
    $stage=Join-Path $runRoot "stages/$Layout"
    foreach($legacy in $mapping.Keys){$relative=if($Layout -eq 'modern'){$mapping[$legacy]}else{$legacy};$file=SafePath $stage $relative;New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($file))|Out-Null;[IO.File]::WriteAllBytes($file,$Payload[$legacy])}
    $data=if($Layout -eq 'modern'){'data'}else{'data/obs-plugins/livehime-obs'}
    foreach($name in @('README.md','LICENSE','THIRD-PARTY-NOTICES.md','VALIDATION.md')){$file=SafePath $stage "$data/docs/$name";New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($file))|Out-Null;Copy-Item -LiteralPath (Join-Path $projectRoot $name) -Destination $file}
    $files=@(Get-ChildItem -LiteralPath $stage -File -Recurse|Sort-Object FullName|ForEach-Object{[ordered]@{path=[IO.Path]::GetRelativePath($stage,$_.FullName).Replace('\','/');sha256=Hash $_.FullName}})
    $manifest=[ordered]@{schema=$(if($Layout -eq 'modern'){2}else{1});version=$Version;obsVersion='33.0.1';qtVersion='6.11.1';files=$files}
    if($Layout -eq 'modern'){$manifest.layout='obs-modern-bundle'}
    $manifest|ConvertTo-Json -Depth 8|Set-Content -LiteralPath (SafePath $stage "$data/install-manifest.json") -Encoding utf8
    return $stage
}
function BuildOld([string]$Layout,[string]$Version,[string]$Stage) {
    $source=Join-Path $projectRoot "validation/installer-upgrade-fixtures/old-$Layout.iss"
    Assert (Test-Path -LiteralPath $source) "Missing frozen fixture source: $Layout"
    $args=@("/DPayloadRoot=$Stage","/DOutputRoot=$(Join-Path $runRoot 'installers')","/DAppVersion=$Version")
    if($Layout -eq 'legacy'){$args+='/DFixtureIdentity=1'}
    $output=& $InnoCompiler @args $source 2>&1
    $code=$LASTEXITCODE;$output|Out-File -LiteralPath (Join-Path $logs "compile-old-$Layout.log") -Encoding utf8
    Assert ($code -eq 0) "Old $Layout fixture compilation failed."
    $suffix=if($Layout -eq 'modern'){'-old-modern'}else{'-fixture-old-legacy'}
    $file=Join-Path $runRoot "installers/livehime-obs-$Version-windows-x64-setup$suffix.exe"
    Assert (Test-Path -LiteralPath $file) 'Compiled old fixture installer missing.'
    return $file
}
function BuildBinary([string]$Name,[string]$Version,[switch]$Library) {
    $source=Join-Path $sources "$Name.cs";$output=Join-Path $sources ($Name+$(if($Library){'.dll'}else{'.exe'}))
    $body=if($Library){'public sealed class MetadataOnlyFixture {}'}else{@'
public static class SyntheticObs {
 public static int Main(string[] args) {
  if(args.Length!=2)return 2;
  System.IO.File.WriteAllText(args[1],System.Diagnostics.Process.GetCurrentProcess().Id.ToString());
  while(!System.IO.File.Exists(args[0]))System.Threading.Thread.Sleep(100);
  return 0;
 }
}
'@}
    @"
using System.Reflection;
[assembly: AssemblyVersion("$Version")]
[assembly: AssemblyFileVersion("$Version")]
[assembly: AssemblyTitle("Synthetic installer fixture; not OBS")]
$body
"@|Set-Content -LiteralPath $source -Encoding utf8
    $compiler=Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    $target=if($Library){'library'}else{'exe'}
    $text=& $compiler /nologo /platform:x64 "/target:$target" "/out:$output" $source 2>&1
    $code=$LASTEXITCODE;$text|Out-File -LiteralPath (Join-Path $logs "compile-$Name.log")
    Assert ($code -eq 0) 'Synthetic binary compilation failed.';return $output
}
function NewHost([string]$Name) {
    $root=SafePath $runRoot "$Name/host";$sentinels=@{}
    foreach($relative in @('bin/64bit/obs64.exe','bin/64bit/Qt6Core.dll')){$file=SafePath $root $relative;New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($file))|Out-Null;Copy-Item -LiteralPath $(if($relative.EndsWith('.exe')){$hostBinary}else{$qtBinary}) -Destination $file;$sentinels[$file]=Hash $file}
    foreach($relative in @('portable_mode.txt','config/obs-studio/plugin_config/livehime-obs/session.dpapi','config/obs-studio/basic/scenes/Synthetic.json','config/obs-studio/basic/profiles/Synthetic/service.json','obs-plugins/64bit/unrelated-plugin.dll')){$file=SafePath $root $relative;New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($file))|Out-Null;[IO.File]::WriteAllText($file,'SYNTHETIC ONLY; NOT CREDENTIALS');$sentinels[$file]=Hash $file}
    return [pscustomobject]@{root=$root;sentinels=$sentinels}
}
function AddSentinel($Fixture,[string]$Absolute) {New-Item -ItemType Directory -Force -Path ([IO.Path]::GetDirectoryName($Absolute))|Out-Null;[IO.File]::WriteAllText($Absolute,'SYNTHETIC USER FILE; must survive uninstall');$Fixture.sentinels[$Absolute]=Hash $Absolute}
function AssertSentinels($Fixture){foreach($path in $Fixture.sentinels.Keys){Assert (Test-Path -LiteralPath $path -PathType Leaf) 'Unowned file removed.';Assert ((Hash $path) -ceq $Fixture.sentinels[$path]) 'Unowned file changed.'}}
function Tree([string]$Root){if(!(Test-Path -LiteralPath $Root)){return '[]'};return Json @(Get-ChildItem -LiteralPath $Root -Recurse -Force|Sort-Object FullName|ForEach-Object{[ordered]@{path=[IO.Path]::GetRelativePath($Root,$_.FullName).Replace('\','/');directory=$_.PSIsContainer;hash=$(if(!$_.PSIsContainer){Hash $_.FullName}else{''})}})}
function InvokePackage([string]$Exe,[string]$Label,[string[]]$Extra=@(),[switch]$Uninstall,[switch]$UsePreviousScope){
    $script:number++;$log=Join-Path $logs ('{0:D2}-{1}.log' -f $script:number,$Label)
    $args=@('/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART',"/LOG=`"$log`"")
    if(!$Uninstall){if(!$UsePreviousScope){$args+='/CURRENTUSER'};$args+=@('/SP-','/LANG=english','/NOCLOSEAPPLICATIONS','/NORESTARTAPPLICATIONS')}
    $args+=$Extra
    $p=Start-Process -FilePath $Exe -ArgumentList $args -WindowStyle Hidden -PassThru
    if(!$p.WaitForExit($InstallerTimeoutSeconds*1000)){throw "Installer timed out; owned invocation PID $($p.Id) left for inspection."}
    $code=$p.ExitCode;$p.Dispose();$invocations.Add([ordered]@{label=$Label;executable=[IO.Path]::GetFileName($Exe);exitCode=$code;log=[IO.Path]::GetRelativePath($runRoot,$log).Replace('\','/')})
    return $code
}
function Case([string]$Name,[scriptblock]$Body){Write-Host "[upgrade-test] $Name";$timer=[Diagnostics.Stopwatch]::StartNew();try{$null=& $Body;AssertIsolation;$results.Add([ordered]@{name=$Name;passed=$true;seconds=$timer.Elapsed.TotalSeconds})}catch{$results.Add([ordered]@{name=$Name;passed=$false;seconds=$timer.Elapsed.TotalSeconds;error=$_.Exception.Message});throw}}
function StartBusy($Fixture){
    $token=[guid]::NewGuid().ToString('N');$stop=Join-Path $runRoot "signals/$token.stop";$ready=Join-Path $runRoot "signals/$token.ready";$exe=SafePath $Fixture.root 'bin/64bit/obs64.exe'
    $process=Start-Process -FilePath $exe -ArgumentList @("`"$stop`"","`"$ready`"") -WorkingDirectory ([IO.Path]::GetDirectoryName($exe)) -WindowStyle Hidden -PassThru
    $owned=[pscustomobject]@{process=$process;stop=$stop;path=$exe;started=$process.StartTime.ToUniversalTime().Ticks};$ownedProcesses.Add($owned)
    $timer=[Diagnostics.Stopwatch]::StartNew();while(!(Test-Path -LiteralPath $ready)-and !$process.HasExited-and $timer.Elapsed.TotalSeconds-lt 10){Start-Sleep -Milliseconds 50}
    Assert (!$process.HasExited -and (Test-Path -LiteralPath $ready)) 'Synthetic OBS did not start.'
    $observed=Get-CimInstance Win32_Process -Filter "ProcessId = $($process.Id)";Assert ($observed -and [string]::Equals($observed.ExecutablePath,$exe,[StringComparison]::OrdinalIgnoreCase)) 'Synthetic process identity could not be confirmed.';return $owned
}
function StopBusy($Owned){$Owned.process.Refresh();if(!$Owned.process.HasExited){[IO.File]::WriteAllText($Owned.stop,'normal fixture exit');Assert ($Owned.process.WaitForExit(10000)) 'Owned synthetic process failed to exit.'}}
function AssertRejected($Fixture,[string]$InstallRoot,[string]$Label,[string[]]$ExtraArguments=@(),[string]$AdditionalRoot='',[string]$Exe=$InstallerPath){
    $tree=Tree $Fixture.root;$bundleTree=Tree $InstallRoot;$extraTree=if($AdditionalRoot){Tree $AdditionalRoot}else{''};$record=Json (FixtureRecord)
    Assert ((InvokePackage $Exe $Label $ExtraArguments) -ne 0) "Accepted rejected-upgrade scenario: $Label"
    Assert ((Tree $Fixture.root)-ceq $tree) "Rejected $Label changed host."
    Assert ((Tree $InstallRoot)-ceq $bundleTree) "Rejected $Label changed install root."
    if($AdditionalRoot){Assert ((Tree $AdditionalRoot)-ceq $extraTree) "Rejected $Label changed alternate target."}
    Assert ((Json (FixtureRecord))-ceq $record) "Rejected $Label changed registration."
}
function AssertPayload([string]$Root,[string]$Layout,$Payload,[string]$Version){
    $data=if($Layout-eq 'modern'){'data'}else{'data/obs-plugins/livehime-obs'}
    $manifestPath=SafePath $Root "$data/install-manifest.json";Assert (Test-Path -LiteralPath $manifestPath) 'Missing manifest.'
    $manifest=Get-Content -Raw -LiteralPath $manifestPath|ConvertFrom-Json;Assert ($manifest.version -eq $Version) 'Wrong payload manifest version.'
    $owned=[Collections.Generic.List[string]]::new()
    foreach($entry in $manifest.files){$file=SafePath $Root $entry.path;Assert ((Hash $file)-ceq $entry.sha256) "Installed manifest hash mismatch: $($entry.path)";$owned.Add($file)}
    foreach($legacy in $mapping.Keys){$relative=if($Layout-eq 'modern'){$mapping[$legacy]}else{$legacy};$file=SafePath $Root $relative;$expected=[Convert]::ToHexString([Security.Cryptography.SHA256]::HashData([byte[]]$Payload[$legacy])).ToLowerInvariant();Assert ((Hash $file)-ceq $expected) "Runtime bytes differ: $relative"}
    $owned.Add($manifestPath);return ,$owned
}
function AssertRegistration([string]$Root,[string]$HostRoot,[string]$Layout,[switch]$LegacyOld){
    $record=FixtureRecord;Assert $record.exists 'Missing HKCU registration.'
    Assert ([string]::Equals(([string]$record.values['InstallLocation'].value).TrimEnd('\'),$Root.TrimEnd('\'),[StringComparison]::OrdinalIgnoreCase)) 'InstallLocation changed.'
    Assert ([string]::Equals(([string]$record.values['Inno Setup: App Path'].value).TrimEnd('\'),$Root.TrimEnd('\'),[StringComparison]::OrdinalIgnoreCase)) 'Registered Inno app path changed.'
    Assert ([string]::Equals(([string]$record.values['Inno Setup CodeFile: ObsRoot'].value).TrimEnd('\'),$HostRoot.TrimEnd('\'),[StringComparison]::OrdinalIgnoreCase)) 'Registered OBS host changed.'
    if(!$LegacyOld){$expected=if($Layout-eq 'modern'){'modern-bundle-v1'}else{'legacy-root-v1'};Assert ($record.values['Inno Setup CodeFile: InstallLayout'].value -eq $expected) 'Wrong registered install layout.'}
    return $record
}
function UninstallAndCheck($Fixture,[string]$Root,[string]$UninstallRelative,$Owned){
    $exe=SafePath $Root $UninstallRelative;$dat=[IO.Path]::ChangeExtension($exe,'.dat');$all=@($Owned)+@($exe,$dat)
    foreach($relative in @('data/install-state.ini','data/obs-plugins/livehime-obs/install-state.ini')){$state=SafePath $Root $relative;if(Test-Path -LiteralPath $state){$all+=$state}}
    Assert ((InvokePackage $exe 'uninstall' @() -Uninstall)-eq 0) 'Uninstall failed.'
    $timer=[Diagnostics.Stopwatch]::StartNew();do{$remaining=@($all|Where-Object{Test-Path -LiteralPath $_});if(!$remaining.Count){break};Start-Sleep -Milliseconds 100}while($timer.Elapsed.TotalSeconds-lt 10)
    Assert ($remaining.Count-eq 0) 'Owned payload remains after uninstall.'
    Assert (!(FixtureRecord).exists) 'Fixture registration remains after uninstall.';AssertSentinels $Fixture;AssertIsolation
}

try {
    $newPayload=ReadPayload $PackagePath;$oldPayload=ReadPayload $OldLegacyPackagePath;$oldModernPayload=ReadPayload $OldModernPackagePath;$oldCurrentPayload=ReadPayload $OldCurrentPackagePath
    $modernStage=WriteStage modern '0.1.0' $oldModernPayload;$legacyStage=WriteStage legacy '0.2.0' $oldPayload
    $oldModern=BuildOld modern '0.1.0' $modernStage;$oldLegacy=BuildOld legacy '0.2.0' $legacyStage
    if(!$PrepareOnly){
        $hostBinary=BuildBinary 'synthetic-obs' '33.0.1.0';$qtBinary=BuildBinary 'synthetic-qt' '6.11.1.0' -Library
        foreach($scenario in @('modern','legacy','current')){
            $layout=if($scenario-eq 'legacy'){'legacy'}else{'modern'}
            $fixture=NewHost $scenario;$other=NewHost "$scenario-other";$root=if($layout-eq 'modern'){SafePath $runRoot "$scenario/bundle/livehime-obs"}else{$fixture.root}
            $otherRoot=if($layout-eq 'modern'){SafePath $runRoot "$scenario-other/bundle/livehime-obs"}else{$other.root}
            $uninstallRelative=if($layout-eq 'modern'){'installer/unins000.exe'}else{'data/obs-plugins/livehime-obs/installer/unins000.exe'}
            $data=if($layout-eq 'modern'){'data'}else{'data/obs-plugins/livehime-obs'}
            AddSentinel $fixture (SafePath $root "$data/user-added-note.txt")
            $legacyOld=$layout-eq 'legacy';$oldExe=if($scenario-eq 'current'){$OldCurrentInstallerPath}elseif($legacyOld){$oldLegacy}else{$oldModern};$version=if($scenario-eq 'current'){'0.2.1'}elseif($legacyOld){'0.2.0'}else{'0.1.0'};$payload=if($scenario-eq 'current'){$oldCurrentPayload}elseif($legacyOld){$oldPayload}else{$oldModernPayload}
            Case "$scenario install frozen old release" {
                $args=@("/DIR=`"$root`"");if(!$legacyOld){$args+="/OBSDIR=`"$($fixture.root)`""}
                Assert ((InvokePackage $oldExe "$scenario-old-install" $args)-eq 0) 'Old fixture installation failed.'
                $oldOwned=AssertPayload $root $layout $payload $version;$null=AssertRegistration $root $fixture.root $layout -LegacyOld:$legacyOld;AssertSentinels $fixture
            }
            $oldRecord=FixtureRecord;$exe=SafePath $root $uninstallRelative;$dat=[IO.Path]::ChangeExtension($exe,'.dat');$oldDatLength=(Get-Item -LiteralPath $dat).Length
            $dllRelative=if($layout-eq 'modern'){'livehime-obs.dll'}else{'obs-plugins/64bit/livehime-obs.dll'};$oldDllHash=Hash (SafePath $root $dllRelative)
            # Capture before upgrade, including any old-only owned paths in the original manifest.
            $oldOwned=AssertPayload $root $layout $payload $version
            if($scenario-eq 'modern'){
                Case 'reproduce 0.2.0 legacy installer TargetChanged regression on registered modern 0.1' {
                    AssertRejected $fixture $root 'modern-regressed-0.2-default' @() '' $RegressedInstallerPath
                    $regressionLog=Join-Path $logs ('{0:D2}-modern-regressed-0.2-default.log' -f $script:number)
                    Assert ((Get-Content -Raw -LiteralPath $regressionLog)-match 'already installed|TargetChanged') 'Old 0.2 fixture did not reproduce the expected destination rejection.'
                }
            }
            $busy=StartBusy $fixture
            Case "$scenario busy selected host rejects upgrade" {AssertRejected $fixture $root "$scenario-busy";Assert (!$busy.process.HasExited) 'Setup closed synthetic OBS.'}
            StopBusy $busy
            Case "$scenario explicit host change rejects upgrade" {AssertRejected $fixture $root "$scenario-host-change" @("/OBSDIR=`"$($other.root)`"") $other.root}
            Case "$scenario explicit physical target change rejects upgrade" {AssertRejected $fixture $root "$scenario-root-change" @("/DIR=`"$otherRoot`"") $otherRoot}
            Case "$scenario default registered upgrade without DIR OBSDIR or scope flags" {
                Assert ((InvokePackage $InstallerPath "$scenario-default-upgrade" -UsePreviousScope)-eq 0) 'Default upgrade failed.'
                $null=AssertPayload $root $layout $newPayload '0.2.2';$record=AssertRegistration $root $fixture.root $layout
                Assert ((Hash (SafePath $root $dllRelative))-cne $oldDllHash) 'Upgrade did not change the old plugin DLL bytes.'
                foreach($field in @('InstallLocation','Inno Setup: App Path','UninstallString','QuietUninstallString')){Assert ($record.values[$field].value -ceq $oldRecord.values[$field].value) "Upgrade changed $field"}
                Assert ((Get-Item -LiteralPath $dat).Length-gt $oldDatLength) 'Upgrade did not append to existing uninstall log.'
                Assert (@(Get-ChildItem -LiteralPath $root -Filter 'unins*.exe' -Recurse -File).Count-eq 1) 'Upgrade created a second uninstaller.'
                if($layout-eq 'modern'){Assert (!(Test-Path -LiteralPath (SafePath $root 'obs-plugins/64bit/livehime-obs.dll'))) 'Modern upgrade accidentally nested legacy DLL.'}
                else{Assert (!(Test-Path -LiteralPath (SafePath $root 'livehime-obs.dll'))) 'Legacy upgrade accidentally installed modern root DLL.'}
                $upgrades.Add([ordered]@{
                    layout=$layout;fromVersion=$version;toVersion='0.2.2';scope='HKCU'
                    oldPluginSha256=$oldDllHash;newPluginSha256=Hash (SafePath $root $dllRelative)
                    oldUninstallDatBytes=$oldDatLength;newUninstallDatBytes=(Get-Item -LiteralPath $dat).Length
                    oldManifestSchema=$(if($legacyOld){1}else{2});newManifestSchema=$(if($legacyOld){1}else{2})
                    registeredLayout=$record.values['Inno Setup CodeFile: InstallLayout'].value
                    installLocationPreserved=$true;appPathPreserved=$true;hostRootPreserved=$true
                    upgradePassedExplicitScope=$false;upgradePassedExplicitDirectory=$false;upgradePassedExplicitObsDirectory=$false
                    uninstallPathPreserved=$true;uninstallerCount=1
                    physicalRoot=[IO.Path]::GetRelativePath($runRoot,$root).Replace('\','/')
                    hostRoot=[IO.Path]::GetRelativePath($runRoot,$fixture.root).Replace('\','/')
                })
                AssertSentinels $fixture
            }
            $busy=StartBusy $fixture
            Case "$scenario upgraded uninstaller rejects busy selected host" {
                $tree=Tree $fixture.root;$bundleTree=Tree $root;$recordBefore=Json (FixtureRecord)
                Assert ((InvokePackage $exe "$scenario-busy-uninstall" @() -Uninstall)-ne 0) 'Upgraded uninstaller accepted a running selected host.'
                Assert (!$busy.process.HasExited) 'Uninstaller closed synthetic OBS.'
                Assert ((Tree $fixture.root)-ceq $tree) 'Rejected uninstall changed host.'
                Assert ((Tree $root)-ceq $bundleTree) 'Rejected uninstall changed install root.'
                Assert ((Json (FixtureRecord))-ceq $recordBefore) 'Rejected uninstall changed registration.'
            }
            StopBusy $busy
            Case "$scenario upgraded uninstall preserves synthetic user data" {$owned=AssertPayload $root $layout $newPayload '0.2.2';UninstallAndCheck $fixture $root $uninstallRelative (@($owned)+@($oldOwned))}
        }
        $fresh=NewHost 'fresh-modern';$freshRoot=SafePath $runRoot 'fresh-modern/bundle/livehime-obs';AddSentinel $fresh (SafePath $freshRoot 'data/user-added-note.txt')
        Case 'fresh install defaults to modern bundle bytes' {Assert ((InvokePackage $InstallerPath 'fresh-modern-install' @("/DIR=`"$freshRoot`"","/OBSDIR=`"$($fresh.root)`""))-eq 0) 'Fresh modern installation failed.';$null=AssertPayload $freshRoot modern $newPayload '0.2.2';$null=AssertRegistration $freshRoot $fresh.root modern;AssertSentinels $fresh}
        Case 'fresh modern uninstall preserves user data' {$owned=AssertPayload $freshRoot modern $newPayload '0.2.2';UninstallAndCheck $fresh $freshRoot 'installer/unins000.exe' $owned}
    }
} catch {$failure=$_} finally {
    foreach($owned in $ownedProcesses){try{StopBusy $owned}catch{if(!$failure){$failure=$_}}}
    $isolation=$true;try{AssertIsolation}catch{$isolation=$false;if(!$failure){$failure=$_}}
    $report=[ordered]@{schema=1;version='0.2.2';passed=($null-eq $failure);prepareOnly=[bool]$PrepareOnly;fixtureAppId=$fixtureGuid;formalRegistrationsUnchanged=$isolation;installer=[IO.Path]::GetFileName($InstallerPath);installerSha256=$(if(Test-Path -LiteralPath $InstallerPath){Hash $InstallerPath}else{$null});packageSha256=Hash $PackagePath;oldLegacyPackageSha256=Hash $OldLegacyPackagePath;oldModernPackageSha256=Hash $OldModernPackagePath;oldCurrentPackageSha256=Hash $OldCurrentPackagePath;frozenModernSourceSha256=Hash (Join-Path $projectRoot 'validation/installer-upgrade-fixtures/old-modern.iss');frozenLegacySourceSha256=Hash (Join-Path $projectRoot 'validation/installer-upgrade-fixtures/old-legacy.iss');upgrades=$upgrades.ToArray();tests=$results.ToArray();invocations=$invocations.ToArray();retainedFixtureRegistration=(FixtureRecord).exists;error=$(if($failure){$failure.Exception.Message}else{$null});scope='Synthetic OBS/Qt version binaries, separate fixture AppId, synthetic account/scene files only. No real OBS or credentials loaded. All fixtures/logs retained.';runDirectory=[IO.Path]::GetRelativePath($projectRoot,$runRoot).Replace('\','/')}
    $reportPath=Join-Path $projectRoot 'validation/installer-upgrade-0.2.2.json';$report|ConvertTo-Json -Depth 15|Set-Content -LiteralPath $reportPath -Encoding utf8
    Write-Host "Report: $reportPath";Write-Host "Retained fixtures: $runRoot"
}
if($failure){throw $failure}
Write-Host "Upgrade validation passed ($($results.Count) cases)."
