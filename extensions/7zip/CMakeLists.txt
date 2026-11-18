cmake_minimum_required(VERSION 3.25)
project(Pulse7Zip LANGUAGES CXX)
# The official Pulse 7-Zip extension. Build in an x64 MSVC environment:
#   cmake -S extensions/7zip -B build-extensions/7zip -G Ninja -DPULSE_7ZIP_DIR=C:/path/with/7z.dll
#   cmake --build build-extensions/7zip
set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../sdk/native/include" CACHE PATH "Public SDK headers")
set(PULSE_7ZIP_DIR "" CACHE PATH "Directory containing x64 7z.dll and License.txt (7-Zip 26.00)")
foreach(file 7z.dll License.txt)
    if(NOT EXISTS "${PULSE_7ZIP_DIR}/${file}")
        message(FATAL_ERROR "PULSE_7ZIP_DIR must contain ${file}")
    endif()
    configure_file("${PULSE_7ZIP_DIR}/${file}" "${CMAKE_CURRENT_BINARY_DIR}/${file}" COPYONLY)
endforeach()
configure_file("${CMAKE_CURRENT_LIST_DIR}/NOTICE.txt" "${CMAKE_CURRENT_BINARY_DIR}/NOTICE.txt" COPYONLY)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/7z.lib"
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_LIST_DIR}/7z.def" "/out:${CMAKE_CURRENT_BINARY_DIR}/7z.lib"
    DEPENDS 7z.def VERBATIM)
add_custom_target(pulse_7zip_import DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/7z.lib")

file(GLOB PULSE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PULSE_SDK_INCLUDE}/pulse/*.h" "${PULSE_SDK_INCLUDE}/pulse/*.hpp")
set(ENGINE_SOURCES engine.cpp)
set(PLUGIN_SOURCES plugin.cpp text.cpp settings.cpp jobs.cpp dialogs.cpp)

add_library(pulse_7zip_engine STATIC ${ENGINE_SOURCES})
add_dependencies(pulse_7zip_engine pulse_7zip_import)
target_compile_features(pulse_7zip_engine PUBLIC cxx_std_20)
target_compile_options(pulse_7zip_engine PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_7zip_engine PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_7zip_engine PUBLIC "${CMAKE_CURRENT_BINARY_DIR}/7z.lib" ole32 oleaut32 uuid bcrypt)

add_library(pulse_7zip SHARED ${PLUGIN_SOURCES})
set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS}")
target_include_directories(pulse_7zip PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_options(pulse_7zip PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_7zip PRIVATE pulse_7zip_engine user32 shell32)

option(PULSE_7ZIP_TESTS "Build the engine round-trip test" ON)
if(PULSE_7ZIP_TESTS)
    add_executable(pulse_7zip_engine_test engine_test.cpp)
    target_compile_options(pulse_7zip_engine_test PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(pulse_7zip_engine_test PRIVATE pulse_7zip_engine)
endif()
