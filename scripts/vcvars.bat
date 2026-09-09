@echo off
rem Reuse an x64 developer prompt, or discover the installed C++ toolchain.
if /i "%VSCMD_ARG_TGT_ARCH%"=="x64" exit /b 0
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
call "%PULSE_VS_PATH%\VC\Auxiliary\Build\vcvars64.bat"
exit /b %errorlevel%
