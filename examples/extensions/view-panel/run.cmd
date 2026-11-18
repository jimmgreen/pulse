@echo off
setlocal
set "PULSE_EXTENSION_EXE=%~dp0..\..\..\build\pulse.exe"
if not exist "%PULSE_EXTENSION_EXE%" (
    echo Build the pulse target first.
    pause
    exit /b 1
)
"%PULSE_EXTENSION_EXE%" --extension-check "%~dp0manifest.json"
if errorlevel 1 (
    echo Manifest validation failed.
    pause
    exit /b 1
)
start "" "%PULSE_EXTENSION_EXE%" --extension-manifest "%~dp0manifest.json"
