@echo off
setlocal
cd /d "%~dp0"
if /i not "%~1"=="/skipbuild" call build_win81.bat
if errorlevel 1 exit /b 1
python tools\audit_win81_imports.py build-win81
if errorlevel 1 exit /b 1
set /p PULSE_VERSION=<version.txt
set "PULSE_ISCC="
for %%P in ("%ProgramFiles(x86)%\Inno Setup 6\ISCC.exe" "%ProgramFiles%\Inno Setup 6\ISCC.exe" "%LOCALAPPDATA%\Programs\Inno Setup 6\ISCC.exe") do if exist "%%~P" set "PULSE_ISCC=%%~P"
if not defined PULSE_ISCC (
    echo Inno Setup 6 is required.
    exit /b 1
)
"%PULSE_ISCC%" /DAppVersion=%PULSE_VERSION% /DBuildDir=build-win81 /DWin81Candidate=1 installer\PulseSetup.iss
exit /b %errorlevel%
