param(
    [string]$OutputDirectory,
    [switch]$PrepareMediumIntegrity,
    [switch]$Live,
    [int]$PulsePid = 0
)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
if (-not $OutputDirectory) {
    $OutputDirectory = Join-Path $root ('bench_data\explorer-launch-probe-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$out = [System.IO.Path]::GetFullPath($OutputDirectory)
$bench = [System.IO.Path]::GetFullPath((Join-Path $root 'bench_data')) + '\'
if (-not $out.StartsWith($bench, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputDirectory must be a new directory under this workspace bench_data.'
}
if (Test-Path -LiteralPath $out) { throw 'Use a new output directory; previous evidence is retained.' }
New-Item -ItemType Directory -Path $out | Out-Null
$build = Join-Path $out 'build'
$batch = Join-Path $out 'build-probe.cmd'
@"
@echo off
call "$root\scripts\vcvars.bat"
if errorlevel 1 exit /b 1
cmake -S "$PSScriptRoot" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release
if errorlevel 1 exit /b 1
cmake --build "$build"
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding utf8
& cmd.exe /d /c $batch 2>&1 | Tee-Object -FilePath (Join-Path $out 'build.log')
if ($LASTEXITCODE) { throw 'Probe build failed.' }
# Only explicit caller opt-in changes labels on these newly created probe EXEs.
# This does not elevate them or change any installed program/system policy.
if ($PrepareMediumIntegrity) {
    Get-ChildItem -LiteralPath $build -Filter '*.exe' -File | ForEach-Object {
        $before = (Get-FileHash -LiteralPath $_.FullName).Hash
        & icacls.exe $_.FullName /setintegritylevel M
        if ($LASTEXITCODE -or (Get-FileHash -LiteralPath $_.FullName).Hash -ne $before) { throw 'Probe label/hash check failed.' }
    }
}
& python (Join-Path $PSScriptRoot 'run_probe.py') --build $build --output (Join-Path $out 'isolated') 2>&1 |
    Tee-Object -FilePath (Join-Path $out 'isolated.log')
if ($LASTEXITCODE) { throw 'Isolated probe failed; do not run live checks.' }
if ($Live) {
    if ($PulsePid -le 0 -or (Get-Process -Id $PulsePid).ProcessName -ne 'pulse') { throw 'Specify the intended running Pulse PID.' }
    & python (Join-Path $PSScriptRoot 'run_live.py') --allow-live-window --build $build --output (Join-Path $out 'live') --pulse-pid $PulsePid 2>&1 |
        Tee-Object -FilePath (Join-Path $out 'live.log')
    if ($LASTEXITCODE) { throw 'Live probe failed.' }
}
Write-Output "Evidence: $out"
