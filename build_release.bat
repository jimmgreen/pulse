@echo off
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
rem Match CMake/Ninja dependency output encoding, including localized MSVC installs.
chcp 65001 >nul
set "VSLANG=1033"
if not defined LUMATEXT_SOURCE_DIR if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
set "PULSE_CMAKE_FRESH="
set "PULSE_CMAKE_CLEAN="
if /i "%~1"=="/clean" (
    set "PULSE_CMAKE_FRESH=--fresh"
    set "PULSE_CMAKE_CLEAN=--clean-first"
)
set "PULSE_DEPS="
set /p PULSE_UPDATE_KEY=<"%~dp0cmake\update-public-key.txt"
set PULSE_UPDATE_CONFIG=-DPULSE_UPDATE_MANIFEST_URL="https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json" -DPULSE_UPDATE_PUBLIC_KEY_HEX=%PULSE_UPDATE_KEY%
if exist "%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src\src\harfbuzz.cc" set PULSE_DEPS=-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src" -DFETCHCONTENT_SOURCE_DIR_FREETYPE="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\freetype-src"
if defined LUMATEXT_SOURCE_DIR (
    cmake %PULSE_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DPULSE_WITH_LUMATEXT=ON %PULSE_DEPS% %PULSE_UPDATE_CONFIG%
) else (
    cmake %PULSE_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release %PULSE_UPDATE_CONFIG%
)
if errorlevel 1 exit /b 1
cmake --build build %PULSE_CMAKE_CLEAN%
if errorlevel 1 exit /b 1
