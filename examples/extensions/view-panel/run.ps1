$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$pulseExe = Join-Path $repoRoot 'build\pulse.exe'
$manifest = Join-Path $PSScriptRoot 'manifest.json'
if (!(Test-Path -LiteralPath $pulseExe)) { throw 'Build the pulse target first.' }
$check = Start-Process -FilePath $pulseExe -ArgumentList @('--extension-check', ('"' + $manifest + '"')) -PassThru -Wait -WindowStyle Hidden
if ($check.ExitCode -ne 0) { throw "Manifest validation failed: $($check.ExitCode)" }
Start-Process -FilePath $pulseExe -ArgumentList @('--extension-manifest', ('"' + $manifest + '"')) -WorkingDirectory $repoRoot -WindowStyle Normal
