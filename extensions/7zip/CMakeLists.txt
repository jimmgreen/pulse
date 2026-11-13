cmake_minimum_required(VERSION 3.25)
project(Pulse7Zip LANGUAGES CXX RC)
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

file(GLOB PULSE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PULSE_SDK_INCLUDE}/pulse/*.h" "${PULSE_SDK_INCLUDE}/pulse/*.hpp")

# 7z.dll engine: linked ONLY into pulse_7zip_worker.exe (and engine_test), NEVER into pulse_7zip.dll.
add_library(pulse_7zip_engine STATIC engine.cpp)
target_compile_features(pulse_7zip_engine PUBLIC cxx_std_20)
target_compile_options(pulse_7zip_engine PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_7zip_engine PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_7zip_engine PUBLIC uuid bcrypt)

# Authenticated Named Pipe transport shared by pulse_7zip.dll and pulse_7zip_worker.exe.
add_library(pulse_7zip_ipc STATIC worker_ipc.cpp)
target_compile_features(pulse_7zip_ipc PUBLIC cxx_std_20)
target_compile_options(pulse_7zip_ipc PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_7zip_ipc PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_7zip_ipc PUBLIC advapi32 bcrypt)

# Out-of-process worker hosting 7z.dll under a restricted Windows Job Object.
add_executable(pulse_7zip_worker worker_main.cpp worker.manifest)
target_compile_options(pulse_7zip_worker PRIVATE /W4 /permissive- /utf-8)
target_link_options(pulse_7zip_worker PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_7zip_worker PRIVATE pulse_7zip_engine pulse_7zip_ipc)

# Host-facing UI/command/preview/task extension DLL (zero 7z.dll linkage).
set(PLUGIN_SOURCES plugin.cpp text.cpp settings.cpp association.cpp jobs.cpp dialogs.cpp worker_client.cpp)
add_library(pulse_7zip SHARED ${PLUGIN_SOURCES} plugin.rc)
set_source_files_properties(${PLUGIN_SOURCES} PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS}")
file(GLOB PULSE_7ZIP_ICONS CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/icons/*.ico")
set_source_files_properties(plugin.rc PROPERTIES OBJECT_DEPENDS "${PULSE_7ZIP_ICONS}")
target_include_directories(pulse_7zip PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_features(pulse_7zip PRIVATE cxx_std_20)
target_compile_options(pulse_7zip PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/permissive-;/utf-8>")
target_compile_definitions(pulse_7zip PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_options(pulse_7zip PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_7zip PRIVATE pulse_7zip_ipc user32 shell32 ole32 shlwapi advapi32)
add_dependencies(pulse_7zip pulse_7zip_worker)

option(PULSE_7ZIP_TESTS "Build the engine and worker isolation tests" ON)
if(PULSE_7ZIP_TESTS)
    add_executable(pulse_7zip_engine_test engine_test.cpp worker.manifest)
    target_compile_options(pulse_7zip_engine_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(pulse_7zip_engine_test PRIVATE /INCREMENTAL:NO /OPT:REF)
    target_link_libraries(pulse_7zip_engine_test PRIVATE pulse_7zip_engine)

    add_executable(pulse_7zip_worker_isolation_test worker_isolation_test.cpp worker_client.cpp worker.manifest)
    target_compile_options(pulse_7zip_worker_isolation_test PRIVATE /W4 /permissive- /utf-8)
    target_link_options(pulse_7zip_worker_isolation_test PRIVATE /INCREMENTAL:NO /OPT:REF)
    target_link_libraries(pulse_7zip_worker_isolation_test PRIVATE pulse_7zip_ipc)
    add_dependencies(pulse_7zip_worker_isolation_test pulse_7zip pulse_7zip_worker)

    # 接管压缩包: registry writes and their undo, against a redirected HKEY_CURRENT_USER.
    add_executable(pulse_7zip_association_test association_test.cpp association.cpp text.cpp)
    target_compile_features(pulse_7zip_association_test PRIVATE cxx_std_20)
    target_compile_options(pulse_7zip_association_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(pulse_7zip_association_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(pulse_7zip_association_test PRIVATE shell32 ole32 shlwapi advapi32)
endif()
