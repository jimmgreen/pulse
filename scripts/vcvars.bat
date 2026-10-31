@echo off
rem Keep MSVC include diagnostics consistent with Ninja dependency parsing.
chcp 65001 >nul
set "VSLANG=1033"
rem Default to x64; pass x86 for the 32-bit dialog components.
set "PULSE_TARGET_ARCH=%~1"
if not defined PULSE_TARGET_ARCH set "PULSE_TARGET_ARCH=x64"
if /i not "%PULSE_TARGET_ARCH%"=="x64" if /i not "%PULSE_TARGET_ARCH%"=="x86" exit /b 2
if /i "%VSCMD_ARG_TGT_ARCH%"=="%PULSE_TARGET_ARCH%" exit /b 0
set "PULSE_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%PULSE_VSWHERE%" (
    echo Visual Studio C++ build tools are required.
    exit /b 1
)
set "PULSE_VS_PATH="
for /f "usebackq delims=" %%i in (`"%PULSE_VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "PULSE_VS_PATH=%%i"
if not defined PULSE_VS_PATH (
    echo No Visual Studio C++ toolchain found.
    exit /b 1
)
if /i "%PULSE_TARGET_ARCH%"=="x86" (
    call "%PULSE_VS_PATH%\VC\Auxiliary\Build\vcvarsall.bat" x64_x86
) else (
    call "%PULSE_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
)
exit /b %errorlevel%
