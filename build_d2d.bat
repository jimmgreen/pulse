@echo off
call "%~dp0scripts\vcvars.bat"
if errorlevel 1 exit /b 1
cmake -S . -B build-d2d -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-d2d --target pulse_d2d_list
