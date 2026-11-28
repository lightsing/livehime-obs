#requires -Version 7.0
[CmdletBinding()]
param(
    [string]$PluginPath = (Join-Path $PSScriptRoot '..\build\RelWithDebInfo\livehime-obs.dll'),
    [string]$ObsRoot = "$env:ProgramFiles\obs-studio"
)
$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
if (![Environment]::Is64BitProcess) { throw 'Use 64-bit PowerShell.' }
$PluginPath = [IO.Path]::GetFullPath($PluginPath)
$obsBin = [IO.Path]::GetFullPath((Join-Path $ObsRoot 'bin\64bit'))
if (!(Test-Path -LiteralPath $PluginPath)) { throw "Missing plugin: $PluginPath" }
# Optional import inspection complements the actual Windows loader check below.
$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
$dumpbinPath = if ($dumpbin) { $dumpbin.Source } else {
    $candidate = Get-ChildItem -Path "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | Select-Object -First 1
    if ($candidate) { $candidate.FullName } else { $null }
}
$codecImportsChecked = $false
if ($dumpbinPath) {
    $imports = & $dumpbinPath /dependents $PluginPath
    if ($LASTEXITCODE -ne 0) { throw 'Cannot inspect plugin imports.' }
    if (($imports -join "`n") -match '(?im)^\s*(?:zlib[^\s]*|brotli[^\s]*)\.dll\s*$') {
        throw 'Unexpected dynamic decompressor dependency; codecs must be statically linked.'
    }
    $codecImportsChecked = $true
}

Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class LivehimeLoaderProbe {
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr AddDllDirectory(string path);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool RemoveDllDirectory(IntPtr cookie);
  [DllImport("kernel32", CharSet=CharSet.Unicode, SetLastError=true)]
  public static extern IntPtr LoadLibraryEx(string path, IntPtr file, uint flags);
  [DllImport("kernel32", CharSet=CharSet.Ansi, ExactSpelling=true, SetLastError=true)]
  public static extern IntPtr GetProcAddress(IntPtr module, string name);
  [DllImport("kernel32", SetLastError=true)]
  public static extern bool FreeLibrary(IntPtr module);
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate uint VersionFunction();
  [UnmanagedFunctionPointer(CallingConvention.Cdecl)]
  public delegate IntPtr PluginVersionFunction();
}
'@
$cookie = [LivehimeLoaderProbe]::AddDllDirectory($obsBin)
if ($cookie -eq [IntPtr]::Zero) { throw "AddDllDirectory failed: $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
$module = [IntPtr]::Zero
try {
    # Resolve dependencies, run normal loader initialization, but do not invoke
    # obs_module_load outside a running OBS host or start any network operation.
    $module = [LivehimeLoaderProbe]::LoadLibraryEx($PluginPath, [IntPtr]::Zero, 0x1100)
    if ($module -eq [IntPtr]::Zero) { throw "LoadLibraryEx failed: Win32 $([Runtime.InteropServices.Marshal]::GetLastWin32Error())" }
    foreach ($export in @('obs_module_load', 'obs_module_unload', 'obs_module_ver', 'obs_module_name')) {
        if ([LivehimeLoaderProbe]::GetProcAddress($module, $export) -eq [IntPtr]::Zero) { throw "Missing OBS module export: $export" }
    }
    $address = [LivehimeLoaderProbe]::GetProcAddress($module, 'obs_module_ver')
    $getVersion = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($address, [LivehimeLoaderProbe+VersionFunction])
    $version = $getVersion.Invoke()
    if ($version -ne 0x21000001) { throw ('Expected OBS API 33.0.1; plugin reports 0x{0:x8}' -f $version) }
    $address = [LivehimeLoaderProbe]::GetProcAddress($module, 'livehime_obs_version')
    if ($address -eq [IntPtr]::Zero) { throw 'Missing plugin version export.' }
    $getPluginVersion = [Runtime.InteropServices.Marshal]::GetDelegateForFunctionPointer($address, [LivehimeLoaderProbe+PluginVersionFunction])
    $pluginVersion = [Runtime.InteropServices.Marshal]::PtrToStringAnsi($getPluginVersion.Invoke())
    [pscustomobject]@{
        plugin = $PluginPath; pluginVersion = $pluginVersion; obsBin = $obsBin; loaded = $true
        staticCodecImportsChecked = $codecImportsChecked
        moduleApi = '33.0.1'; scope = 'DLL import resolution and module exports; no OBS module initialization'
    } | ConvertTo-Json
} finally {
    if ($module -ne [IntPtr]::Zero) { [LivehimeLoaderProbe]::FreeLibrary($module) | Out-Null }
    [LivehimeLoaderProbe]::RemoveDllDirectory($cookie) | Out-Null
}
