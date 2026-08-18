@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat"
cmake -S . -B build-d2d -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-d2d --target pulse_d2d_list
