cmake_minimum_required(VERSION 3.25)
project(PulseSandboxedWorker LANGUAGES CXX)

set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH "Public SDK headers")
file(GLOB PULSE_PUBLIC_HEADERS CONFIGURE_DEPENDS
    "${PULSE_SDK_INCLUDE}/pulse/*.h"
    "${PULSE_SDK_INCLUDE}/pulse/*.hpp")

set(_pulse_sw_saved_cxx_std_libs "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")

add_executable(sandboxed_worker WIN32 worker_main.cpp)
target_compile_features(sandboxed_worker PRIVATE cxx_std_20)
target_compile_definitions(sandboxed_worker PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
set_target_properties(sandboxed_worker PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(sandboxed_worker PRIVATE kernel32 bcrypt)

add_library(pulse_sandboxed_worker SHARED plugin.cpp)
set_source_files_properties(plugin.cpp PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS}")
target_include_directories(pulse_sandboxed_worker PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_features(pulse_sandboxed_worker PRIVATE cxx_std_20)
target_compile_definitions(pulse_sandboxed_worker PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
set_target_properties(pulse_sandboxed_worker PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(pulse_sandboxed_worker PRIVATE kernel32)
add_dependencies(pulse_sandboxed_worker sandboxed_worker)

if(MSVC)
    foreach(_tgt sandboxed_worker pulse_sandboxed_worker)
        target_compile_options(${_tgt} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
        target_link_options(${_tgt} PRIVATE
            /NODEFAULTLIB:user32.lib
            /NODEFAULTLIB:gdi32.lib
            /NODEFAULTLIB:shell32.lib)
    endforeach()
endif()

set(CMAKE_CXX_STANDARD_LIBRARIES "${_pulse_sw_saved_cxx_std_libs}")
unset(_pulse_sw_saved_cxx_std_libs)
