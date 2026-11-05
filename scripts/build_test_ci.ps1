#requires -Version 7.2
param(
    [string]$BuildDir = 'build-ci'
)

$ErrorActionPreference = 'Stop'
$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$build = [IO.Path]::GetFullPath((Join-Path $repo $BuildDir))
if (-not $build.StartsWith($repo + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'BuildDir must be inside the repository'
}

$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -version '[17.0,18.0)' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) {
    throw 'Visual Studio 2022 C++ tools are required'
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'

$repository = if ($env:GITHUB_REPOSITORY) { $env:GITHUB_REPOSITORY } else { 'fei98625/pulse' }
if ($repository -notmatch '^[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+$') {
    throw 'GITHUB_REPOSITORY must be an owner/repository name'
}

New-Item -ItemType Directory -Path $build -Force | Out-Null

# Keep this intentionally small. These targets are enough to launch Pulse from
# the standalone artifact; the two focused tests catch the current column and
# session regressions without compiling the complete release test matrix.
$targets = @(
    'pulse',
    'pulse_index',
    'pulse_preview',
    'pulse_shell',
    'pulse_integration',
    'pulse_elevated',
    'pulse_column_strip_test',
    'pulse_session_window_test'
) -join ' '

$batch = Join-Path $build 'compile-fast-test.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
cmake -S "$repo" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_WIN81_CANDIDATE=OFF -DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_TEST_HOOKS=OFF -DPULSE_BUILD_PROGRESS_UI_TESTS=OFF -DPULSE_WITH_LUMATEXT=ON -DPULSE_RELEASE_REPOSITORY="$repository"
if errorlevel 1 exit /b 1
cmake --build "$build" --parallel 4 --target $targets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii

& $batch
if ($LASTEXITCODE -ne 0) {
    throw 'Fast Windows test build failed'
}

foreach ($testName in @('pulse_column_strip_test', 'pulse_session_window_test')) {
    & (Join-Path $build "$testName.exe")
    if ($LASTEXITCODE -ne 0) {
        throw "$testName failed"
    }
}

. (Join-Path $PSScriptRoot 'release_payload.ps1')
foreach ($file in $PulseReleasePayload) {
    if (-not (Test-Path -LiteralPath (Join-Path $build $file))) {
        throw "Fast test build is missing runtime payload: $file"
    }
}

Write-Host 'Fast Windows test build passed; runnable payload is ready.' -ForegroundColor Green
