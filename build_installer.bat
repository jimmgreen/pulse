@echo off
rem Build Release binaries, then compile dist\PulseSetup-<version>.exe.
setlocal
cd /d "%~dp0"

call "%~dp0build_release.bat" || exit /b 1

for %%F in (build\pulse.exe build\Pulse.Index.exe build\Pulse.Preview.exe build\pulse_shell.exe) do (
    if not exist "%%F" (
        echo Missing build output: %%F
        exit /b 1
    )
)

set "ISCC="
where ISCC.exe >nul 2>&1 && set "ISCC=ISCC.exe"
if not defined ISCC if exist "C:\Program Files (x86)\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files (x86)\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "C:\Program Files\Inno Setup 6\ISCC.exe" set "ISCC=C:\Program Files\Inno Setup 6\ISCC.exe"
if not defined ISCC if exist "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe" set "ISCC=%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe"
if not defined ISCC (
    echo Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup
    exit /b 1
)

"%ISCC%" installer\PulseSetup.iss || exit /b 1
echo.
echo Installer written to dist\
