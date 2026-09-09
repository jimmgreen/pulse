@echo off
rem Build Release binaries, then compile dist\PulseSetup-<version>.exe.
setlocal
cd /d "%~dp0"

set /p PULSE_VERSION=<version.txt
if not defined PULSE_VERSION (
    echo Missing version in version.txt
    exit /b 1
)

if /i "%~1"=="/skipbuild" goto after_release_build
call "%~dp0build_release.bat" || exit /b 1
:after_release_build

for %%F in (build\pulse.exe build\Pulse.Index.exe build\Pulse.Preview.exe build\pulse_shell.exe build\lumatext.dll) do (
    if not exist "%%F" (
        echo Missing build output: %%F
        exit /b 1
    )
)

set "ISCC="
where ISCC.exe >nul 2>&1 && set "ISCC=ISCC.exe"
if not defined ISCC if exist "%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%ProgramFiles%\Inno Setup 6\ISCC.exe" set "ISCC=%ProgramFiles%\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup
    exit /b 1
)

"%ISCC%" /DAppVersion=%PULSE_VERSION% installer\PulseSetup.iss || exit /b 1
echo.
echo Installer written to dist\
