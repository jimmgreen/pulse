@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
if not defined LUMATEXT_SOURCE_DIR if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
if defined LUMATEXT_SOURCE_DIR (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DPULSE_WITH_LUMATEXT=ON
) else (
    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
)
if errorlevel 1 exit /b 1
cmake --build build
if errorlevel 1 exit /b 1
