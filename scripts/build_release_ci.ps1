#requires -Version 7.2
param(
    [ValidateSet('windows', 'win81')][string]$Channel = 'windows',
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
$toolsetEnvironment = ''
$compilerArguments = ''
if (-not $vs) {
    # A local extracted v143 toolset can share the installed Windows SDK.
    $toolset = Join-Path $repo 'tools/win81-toolchain/VC/Tools/MSVC/14.44.35207'
    if (-not (Test-Path "$toolset/bin/Hostx64/x64/cl.exe")) { throw 'Visual Studio 2022 C++ tools are required' }
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if (-not $vs) { throw 'An installed Windows SDK developer environment is required' }
    $toolsetEnvironment = @"
set "PATH=$toolset\bin\Hostx64\x64;%PATH%"
set "INCLUDE=$toolset\include;%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt;%WindowsSdkDir%Include\%WindowsSDKVersion%shared;%WindowsSdkDir%Include\%WindowsSDKVersion%um;%WindowsSdkDir%Include\%WindowsSDKVersion%winrt"
set "LIB=$toolset\lib\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64"
"@
    $compilerArguments = "-DCMAKE_C_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_CXX_COMPILER=`"$toolset/bin/Hostx64/x64/cl.exe`" -DCMAKE_LINKER=`"$toolset/bin/Hostx64/x64/link.exe`""
}
$vcvars = Join-Path $vs 'VC\Auxiliary\Build\vcvars64.bat'
$sdkRoot = Join-Path $repo '.release-sdk'
$sdkArchive = Join-Path $repo '.release-sdk.zip'
$sdk = Get-Content -Raw (Join-Path $repo 'cmake/lumatext-sdk.json') | ConvertFrom-Json
if (-not (Test-Path $sdkArchive) -or (Get-FileHash $sdkArchive -Algorithm SHA256).Hash -ne $sdk.sha256) {
    Invoke-WebRequest -Uri $sdk.url -OutFile $sdkArchive
}
if ((Get-FileHash $sdkArchive -Algorithm SHA256).Hash -ne $sdk.sha256) { throw 'LumaText SDK checksum mismatch' }
Expand-Archive -LiteralPath $sdkArchive -DestinationPath $sdkRoot -Force
# ZIP timestamps have no timezone; normalize extracted inputs before Ninja runs.
$extractedAt = [DateTime]::UtcNow
Get-ChildItem -LiteralPath $sdkRoot -Recurse -File | ForEach-Object { $_.LastWriteTimeUtc = $extractedAt }
$candidate = if ($Channel -eq 'win81') { 'ON' } else { 'OFF' }
$manifest = if ($Channel -eq 'win81') { 'update-manifest-win81.json' } else { 'update-manifest.json' }
$publicKey = (Get-Content (Join-Path $repo 'cmake/update-public-key.txt') -Raw).Trim()
New-Item -ItemType Directory -Path $build -Force | Out-Null
# Release verification is scoped to the changes being shipped. pulse already
# depends on all three packaged hosts; standalone tests need explicit targets.
$testNames = @('pulse_rename_ops_test', 'pulse_child_edit_test', 'pulse_localization_test',
    'pulse_update_test', 'pulse_update_installer_test')
$testTargets = (@('pulse', 'pulse_index_engine_test', 'pulse_index_host_stress') + $testNames) -join ' '
$batch = Join-Path $build 'compile-release.bat'
@"
@echo off
call "$vcvars"
if errorlevel 1 exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
$toolsetEnvironment
cmake -S "$repo" -B "$build" -G Ninja $compilerArguments -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_WIN81_CANDIDATE=$candidate -DPULSE_WITH_SELFTEST=ON -DPULSE_WITH_LUMATEXT=ON -DLUMATEXT_SOURCE_DIR= -DCMAKE_PREFIX_PATH="$sdkRoot" -DPULSE_UPDATE_MANIFEST_URL="https://github.com/jimmgreen/pulse/releases/latest/download/$manifest" -DPULSE_UPDATE_PUBLIC_KEY_HEX=$publicKey
if errorlevel 1 exit /b 1
cmake --build "$build" --parallel 4 --target $testTargets
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Release compilation failed' }
foreach ($testName in $testNames) {
    & (Join-Path $build "$testName.exe")
    if ($LASTEXITCODE -ne 0) { throw "$testName failed" }
}
foreach ($mode in @('--startup-stop', '--shell-roundtrip')) {
    & (Join-Path $build 'pulse_rename_ops_test.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Rename lifecycle check $mode failed" }
}
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
& (Join-Path $build 'pulse_index_engine_test.exe') --parent-cycle-only
if ($LASTEXITCODE -ne 0) { throw 'Index parent-cycle regression failed' }
foreach ($mode in @('--service-start-only', '--shutdown-only')) {
    & (Join-Path $build 'pulse_index_host_stress.exe') $mode
    if ($LASTEXITCODE -ne 0) { throw "Index lifecycle check $mode failed" }
}
$selftestCases = @('rename-editor', 'rename-editor-native', 'operation-toast',
    'filter-controls', 'rename-outside', 'address-editor', 'address-editor-native')
$selftestLogs = @{
    'rename-editor' = 'bench_data/rename-editor/results.log'
    'rename-editor-native' = 'bench_data/rename-editor/results.log'
    'operation-toast' = 'bench_data/operation-toast/results.log'
    'address-editor' = 'bench_data/address-editor/results.log'
    'address-editor-native' = 'bench_data/address-editor/results.log'
}
foreach ($testCase in $selftestCases) {
    $env:PULSE_SELFTEST_CASE = $testCase -replace '-native$', ''
    $env:PULSE_LUMATEXT = if ($testCase.EndsWith('-native')) { '0' } else { '1' }
    $selftest = Start-Process -FilePath (Join-Path $build 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
    $finished = $selftest.WaitForExit(120000)
    if (-not $finished) { $selftest.Kill(); $selftest.WaitForExit() }
    $selftest.Refresh()
    $caseLog = Join-Path $build "selftest-$testCase.log"
    $sourceLog = if ($selftestLogs.ContainsKey($testCase)) { $selftestLogs[$testCase] } else { 'bench_data/selftest_1b2_last.log' }
    Copy-Item $sourceLog $caseLog -ErrorAction SilentlyContinue
    if (-not $finished) { throw "Selftest $testCase timed out" }
    if ($selftest.ExitCode -ne 0) {
        Get-Content $caseLog -ErrorAction SilentlyContinue | Select-String '\[FAIL\]'
        throw "Selftest $testCase failed: $($selftest.ExitCode)"
    }
}
Remove-Item Env:PULSE_SELFTEST_CASE
Remove-Item Env:PULSE_LUMATEXT
# Strip the embedded test suite from the shipped executable after verification.
(Get-Content -LiteralPath $batch -Raw).Replace('-DPULSE_WITH_SELFTEST=ON', '-DPULSE_WITH_SELFTEST=OFF').Replace("--target $testTargets", '--target pulse') |
    Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Production compilation failed' }
if ($Channel -eq 'win81') {
    python tools/audit_win81_imports.py $build
    if ($LASTEXITCODE -ne 0) { throw 'Windows 8.1 startup import guard failed' }
}
$licenses = Join-Path $build 'licenses/LumaText'
New-Item -ItemType Directory -Path $licenses -Force | Out-Null
Copy-Item -Path (Join-Path $sdkRoot 'share/LumaText/licenses/*') -Destination $licenses
$iscc = @("${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe", "$env:ProgramFiles\Inno Setup 6\ISCC.exe",
    "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $iscc) { throw 'Inno Setup 6 is required' }
$version = (Get-Content version.txt -Raw).Trim()
$arguments = @("/DAppVersion=$version", "/DBuildDir=$build")
if ($Channel -eq 'win81') { $arguments += '/DWin81Candidate=1' }
& $iscc @arguments installer/PulseSetup.iss
if ($LASTEXITCODE -ne 0) { throw 'Installer compilation failed' }
