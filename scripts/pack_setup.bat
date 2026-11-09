@echo off
rem Pack a Release build into dist\PulseSetup-<version>[-win81].exe with the
rem Pulse setup (src\setup bootstrapper + tools\pack_installer\pack_installer.py).
rem usage: scripts\pack_setup.bat <build dir> <windows|win81>
setlocal
cd /d "%~dp0.."

set "PULSE_BUILD_DIR=%~1"
set "PULSE_CHANNEL=%~2"
if not defined PULSE_BUILD_DIR set "PULSE_BUILD_DIR=build"
if not defined PULSE_CHANNEL set "PULSE_CHANNEL=windows"
rem Accept a relative (to the repository) or absolute build directory.
for %%I in ("%PULSE_BUILD_DIR%") do set "PULSE_BUILD_DIR=%%~fI"
set "PULSE_SUFFIX="
if /i "%PULSE_CHANNEL%"=="win81" set "PULSE_SUFFIX=-win81"

set /p PULSE_VERSION=<version.txt
if not defined PULSE_VERSION (
    echo Missing version in version.txt
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\check_release_payload.ps1" -BuildDir "%PULSE_BUILD_DIR%" || exit /b 1

call "%CD%\scripts\vcvars.bat" >nul || exit /b 1
chcp 65001 >nul
set "VSLANG=1033"
cmake --build "%PULSE_BUILD_DIR%" --target pulse_launcher pulse_setup pulse_dialog_auto pulse_dialog_auto_control pulse_dialog_launcher pulse_file_dialog_ui_proof_runtime pulse_picker_ui_proof || exit /b 1

for %%F in (pulse.exe Pulse.AI.exe Pulse.Content.exe Pulse.Document.exe Pulse.Index.exe Pulse.Preview.exe pulse_shell.exe pulse_start_pin.dll pulse_elevated.exe pulse_hotkey_broker.exe pulse_integration.exe lumatext.dll pdfium.dll) do (
    if not exist "%PULSE_BUILD_DIR%\%%F" (
        echo Missing build output: %PULSE_BUILD_DIR%\%%F
        exit /b 1
    )
)

rem The payload carries no CRT DLLs, so every binary must link the static runtime.
powershell -NoProfile -ExecutionPolicy Bypass -File "%CD%\scripts\stage_installer_runtime.ps1" -BuildDir "%PULSE_BUILD_DIR%" -RequireStaticRuntime || exit /b 1

set "PULSE_PYTHON=python"
where python >nul 2>&1 || set "PULSE_PYTHON=py -3"
%PULSE_PYTHON% tools\l10n\check_binary_resources.py "%PULSE_BUILD_DIR%\PulsePickerUiProof.dll" || exit /b 1
%PULSE_PYTHON% tools\pack_installer\pack_installer.py --root . --build "%PULSE_BUILD_DIR%" --stub "%PULSE_BUILD_DIR%\pulse_setup.exe" --version %PULSE_VERSION% --channel %PULSE_CHANNEL% --out "dist\PulseSetup-%PULSE_VERSION%%PULSE_SUFFIX%.exe" || exit /b 1
%PULSE_PYTHON% tools\l10n\check_binary_resources.py --installer "dist\PulseSetup-%PULSE_VERSION%%PULSE_SUFFIX%.exe" || exit /b 1
echo.
echo Installer written to dist\PulseSetup-%PULSE_VERSION%%PULSE_SUFFIX%.exe
