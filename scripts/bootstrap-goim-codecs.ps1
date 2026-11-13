#requires -Version 7.0
[CmdletBinding()]
param([string]$DependenciesRoot = (Join-Path $PSScriptRoot '..\..\livehime-obs-deps\goim'), [switch]$Offline)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$DependenciesRoot = [IO.Path]::GetFullPath($DependenciesRoot)
New-Item -ItemType Directory -Force -Path $DependenciesRoot | Out-Null
$lock = Get-Content -Raw -LiteralPath (Join-Path $PSScriptRoot '..\deps\goim-codecs.lock.json') | ConvertFrom-Json
foreach ($artifact in $lock.artifacts) {
 $archive = Join-Path $DependenciesRoot $artifact.file
 if (!(Test-Path -LiteralPath $archive)) {
  if ($Offline) { throw "Missing codec archive: $archive" }
  Invoke-WebRequest -Uri $artifact.url -OutFile $archive
 }
 if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $artifact.sha256) { throw "Codec archive checksum mismatch: $archive" }
 # Always extract verified bytes; stale modified source trees cannot be silently reused.
 & tar -xf $archive -C $DependenciesRoot
 if ($LASTEXITCODE -ne 0) { throw "Codec extraction failed: $archive" }
}
Write-Output $DependenciesRoot
