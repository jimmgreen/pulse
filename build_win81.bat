@echo off
setlocal
cd /d "%~dp0"
rem v143 is required: VS 2026 dropped the Windows 8.1 runtime target.
if not defined PULSE_VC_ROOT set "PULSE_VC_ROOT=%~dp0tools\win81-toolchain\VC\Tools\MSVC\14.44.35207"
if not exist "%PULSE_VC_ROOT%\bin\Hostx64\x64\cl.exe" (
    echo Set PULSE_VC_ROOT to a VS 2022 v143 toolset, or run tools\prepare_win81_toolchain.py.
    exit /b 1
)
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
set "PATH=%PULSE_VC_ROOT%\bin\Hostx64\x64;%PATH%"
set "INCLUDE=%PULSE_VC_ROOT%\include;%WindowsSdkDir%Include\%WindowsSDKVersion%ucrt;%WindowsSdkDir%Include\%WindowsSDKVersion%shared;%WindowsSdkDir%Include\%WindowsSDKVersion%um;%WindowsSdkDir%Include\%WindowsSDKVersion%winrt"
set "LIB=%PULSE_VC_ROOT%\lib\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%ucrt\x64;%WindowsSdkDir%Lib\%WindowsSDKVersion%um\x64"
set "VSLANG=1033"
if not defined CMAKE_BUILD_PARALLEL_LEVEL set "CMAKE_BUILD_PARALLEL_LEVEL=4"
if not defined LUMATEXT_SOURCE_DIR if exist "%~dp0..\lumatext\CMakeLists.txt" set "LUMATEXT_SOURCE_DIR=%~dp0..\lumatext"
set "PULSE_DEPS="
if exist "%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src\src\harfbuzz.cc" set PULSE_DEPS=-DFETCHCONTENT_SOURCE_DIR_HARFBUZZ="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\harfbuzz-src" -DFETCHCONTENT_SOURCE_DIR_FREETYPE="%LUMATEXT_SOURCE_DIR%\build-vs18\_deps\freetype-src"
cmake -S . -B build-win81 -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="%PULSE_VC_ROOT%\bin\Hostx64\x64\cl.exe" -DCMAKE_CXX_COMPILER="%PULSE_VC_ROOT%\bin\Hostx64\x64\cl.exe" -DCMAKE_LINKER="%PULSE_VC_ROOT%\bin\Hostx64\x64\link.exe" -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded -DPULSE_WITH_SELFTEST=ON -DPULSE_WIN81_CANDIDATE=ON -DLUMATEXT_SOURCE_DIR="%LUMATEXT_SOURCE_DIR%" -DPULSE_WITH_LUMATEXT=ON %PULSE_DEPS%
if errorlevel 1 exit /b 1
cmake --build build-win81
exit /b %errorlevel%
