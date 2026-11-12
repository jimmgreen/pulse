@echo off
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
rem Match CMake/Ninja dependency output encoding, including localized MSVC installs.
chcp 65001 >nul
set "VSLANG=1033"
set "PULSE_LUMATEXT_PREBUILT="
if exist "%~dp0third_party\lumatext\bin\lumatext.dll" set "PULSE_LUMATEXT_PREBUILT=1"
if not defined LUMATEXT_SOURCE_DIR if not defined PULSE_LUMATEXT_PREBUILT if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
set "PULSE_CMAKE_FRESH="
set "PULSE_CMAKE_CLEAN="
set "PULSE_BUILD_TYPE=Release"
rem Signed packages archive PDBs; optimized RelWithDebInfo emits matching symbols.
if /i "%~1"=="/symbols" set "PULSE_BUILD_TYPE=RelWithDebInfo"
if /i "%~1"=="/clean" (
    set "PULSE_CMAKE_FRESH=--fresh"
    set "PULSE_CMAKE_CLEAN=--clean-first"
)
set "PULSE_DEPS="
set "PULSE_SYMBOLS=OFF"
if /i "%~1"=="/symbols" set "PULSE_SYMBOLS=ON"
set /p PULSE_UPDATE_KEY=<"%~dp0cmake\update-public-key.txt"
set PULSE_UPDATE_CONFIG=-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_WITH_SELFTEST=OFF -DPULSE_WITH_TEST_HOOKS=OFF -DPULSE_RELEASE_SYMBOLS=%PULSE_SYMBOLS% -DPULSE_UPDATE_MANIFEST_URL="https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json" -DPULSE_UPDATE_PUBLIC_KEY_HEX=%PULSE_UPDATE_KEY%
if exist "%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src\src\harfbuzz.cc" set PULSE_DEPS=-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src" -DFETCHCONTENT_SOURCE_DIR_FREETYPE="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\freetype-src"
if defined LUMATEXT_SOURCE_DIR (
    cmake %PULSE_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%PULSE_BUILD_TYPE% -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DPULSE_WITH_LUMATEXT=ON %PULSE_DEPS% %PULSE_UPDATE_CONFIG%
) else if defined PULSE_LUMATEXT_PREBUILT (
    rem -DLUMATEXT_SOURCE_DIR= clears a cached source tree; CMakeLists.txt picks
    rem up the committed third_party\lumatext package on its own.
    cmake %PULSE_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%PULSE_BUILD_TYPE% -DLUMATEXT_SOURCE_DIR= -DPULSE_WITH_LUMATEXT=ON %PULSE_UPDATE_CONFIG%
) else (
    cmake %PULSE_CMAKE_FRESH% -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=%PULSE_BUILD_TYPE% %PULSE_UPDATE_CONFIG%
)
if errorlevel 1 exit /b 1
rem Only what the installer ships (pulse_package, CMakeLists.txt); test programs
rem build by name when a test needs them, so a broken test never blocks a package.
cmake --build build --target pulse_package %PULSE_CMAKE_CLEAN%
if errorlevel 1 exit /b 1
