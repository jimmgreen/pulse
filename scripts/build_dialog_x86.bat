@echo off
rem Build the x86 dialog companion that the installer packs next to the x64 build.
rem usage: scripts\build_dialog_x86.bat [build dir]   (default build-x86)
rem First use configures the folder; PDFium must be an x86 build (PULSE_PDFIUM_X86_ROOT).
setlocal
cd /d "%~dp0.."

set "PULSE_X86_DIR=%~1"
if not defined PULSE_X86_DIR set "PULSE_X86_DIR=build-x86"

call "%CD%\scripts\vcvars.bat" x86 || exit /b 1

if exist "%PULSE_X86_DIR%\build.ninja" goto build
if not defined PULSE_PDFIUM_X86_ROOT if exist "%CD%\build-dialog-x86-next\pdfium-x86\bin\pdfium.dll" set "PULSE_PDFIUM_X86_ROOT=%CD%\build-dialog-x86-next\pdfium-x86"
if not defined PULSE_PDFIUM_X86_ROOT (
    echo The x86 dialog companion needs an x86 PDFium; set PULSE_PDFIUM_X86_ROOT.
    exit /b 1
)
set /p PULSE_UPDATE_KEY=<"%CD%\cmake\update-public-key.txt"
cmake -S . -B "%PULSE_X86_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded ^
    -DPULSE_PDFIUM_ROOT="%PULSE_PDFIUM_X86_ROOT%" -DPULSE_WITH_LUMATEXT=OFF -DLUMATEXT_SOURCE_DIR= ^
    -DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_TEST_HOOKS=OFF -DPULSE_RELEASE_SYMBOLS=OFF ^
    -DPULSE_UPDATE_MANIFEST_URL="https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json" ^
    -DPULSE_UPDATE_PUBLIC_KEY_HEX=%PULSE_UPDATE_KEY% || exit /b 1

:build
cmake --build "%PULSE_X86_DIR%" --target pulse_dialog_stub pulse_dialog_core pulse_dialog_inject32 ^
    pulse_dialog_launcher pulse_dialog_auto pulse_dialog_auto_control ^
    pulse_file_dialog_ui_proof_runtime pulse_picker_ui_proof pulse_dialog_admin_auto ^
    pulse_file_dialog_auto_entry_runtime || exit /b 1
exit /b 0
