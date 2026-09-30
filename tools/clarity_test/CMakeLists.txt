cmake_minimum_required(VERSION 3.25)
project(PulseClarityTest LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
if(NOT MSVC)
  message(FATAL_ERROR "This diagnostic requires Windows MSVC")
endif()
set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")
find_package(LumaText CONFIG REQUIRED PATHS "${ROOT}/third_party/lumatext" NO_DEFAULT_PATH)
add_executable(pulse_clarity main.cpp unused_compositor_guards.cpp "${ROOT}/src/ui/lumatext_renderer.cpp"
  "${ROOT}/src/ui/typography.cpp" "${ROOT}/src/common/localization.cpp")
target_compile_definitions(pulse_clarity PRIVATE PULSE_HAS_LUMATEXT=1 UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 WINVER=0x0A00)
target_compile_options(pulse_clarity PRIVATE /W4 /WX /permissive- /EHsc /utf-8 /Gy)
# Retain the production typography translation unit; unreachable full-app
# calls are guarded by throwing definitions in unused_compositor_guards.cpp.
target_link_options(pulse_clarity PRIVATE /OPT:REF /DELAYLOAD:lumatext.dll)
target_link_libraries(pulse_clarity PRIVATE LumaText::Shared d3d11 d2d1 dwrite
  dxgi windowscodecs ole32 user32 gdi32 imm32 comctl32 delayimp)
add_custom_command(TARGET pulse_clarity POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
  $<TARGET_FILE:LumaText::Shared> $<TARGET_FILE_DIR:pulse_clarity>)
