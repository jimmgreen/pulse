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
cmake --build "$build" --parallel 4
exit /b %errorlevel%
"@ | Set-Content -LiteralPath $batch -Encoding ascii
& $batch
if ($LASTEXITCODE -ne 0) { throw 'Release compilation failed' }
$testNames = @('pulse_index_migration_test','pulse_index_test','pulse_index_engine_test',
    'pulse_content_search_test','pulse_duplicate_scan_test','pulse_saved_search_test',
    'pulse_search_query_test','pulse_update_test','pulse_update_installer_test',
    'pulse_ops_test','pulse_preview_test','pulse_preview_handler_pan_test','pulse_app_controllers_test',
    'pulse_localization_test','pulse_shell_icons_test','pulse_material_test',
    'pulse_search_history_test','pulse_child_edit_test','pulse_dialog_close_test')
foreach ($testName in $testNames) {
    & (Join-Path $build "$testName.exe")
    if ($LASTEXITCODE -ne 0) { throw "$testName failed" }
}
$env:PULSE_SELFTEST_NO_SCREENSHOTS = '1'
$selftest = Start-Process -FilePath (Join-Path $build 'pulse.exe') -ArgumentList '--selftest' -WindowStyle Hidden -PassThru
if (-not $selftest.WaitForExit(120000)) { $selftest.Kill(); throw 'Selftest timed out' }
if ($selftest.ExitCode -ne 0) {
    Get-Content 'bench_data/selftest_1b2_last.log' -ErrorAction SilentlyContinue | Select-String '\[FAIL\]'
    throw "Selftest failed: $($selftest.ExitCode)"
}
# Strip the embedded test suite from the shipped executable after verification.
(Get-Content -LiteralPath $batch -Raw).Replace('-DPULSE_WITH_SELFTEST=ON', '-DPULSE_WITH_SELFTEST=OFF') |
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
