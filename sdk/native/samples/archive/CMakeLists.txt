cmake_minimum_required(VERSION 3.25)
project(PulseArchive LANGUAGES CXX)
set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../include" CACHE PATH "Public SDK headers")
set(PULSE_7ZIP_DIR "" CACHE PATH "Directory containing x64 7z.dll, License.txt and NOTICE.txt")
# SDK updates must rebuild the plugin even when a localized /showIncludes
# prefix is misdetected by an author's CMake/Ninja installation.
file(GLOB PULSE_PUBLIC_HEADERS CONFIGURE_DEPENDS "${PULSE_SDK_INCLUDE}/pulse/*.h" "${PULSE_SDK_INCLUDE}/pulse/*.hpp")
set_source_files_properties(plugin.cpp archive_tasks.cpp archive_settings.cpp archive_writer.cpp PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_HEADERS};${CMAKE_CURRENT_LIST_DIR}/archive_settings.h;${CMAKE_CURRENT_LIST_DIR}/archive_tasks.h;${CMAKE_CURRENT_LIST_DIR}/archive_reader.h;${CMAKE_CURRENT_LIST_DIR}/archive_writer.h")
set_source_files_properties(archive_reader.cpp archive_reader_test.cpp PROPERTIES
    OBJECT_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/archive_reader.h")
foreach(file 7z.dll License.txt NOTICE.txt)
    if(NOT EXISTS "${PULSE_7ZIP_DIR}/${file}")
        message(FATAL_ERROR "PULSE_7ZIP_DIR must contain ${file}")
    endif()
    configure_file("${PULSE_7ZIP_DIR}/${file}" "${CMAKE_CURRENT_BINARY_DIR}/${file}" COPYONLY)
endforeach()
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/7z.lib"
    COMMAND "${CMAKE_AR}" /nologo /machine:x64 "/def:${CMAKE_CURRENT_LIST_DIR}/7z.def" "/out:${CMAKE_CURRENT_BINARY_DIR}/7z.lib"
    DEPENDS 7z.def VERBATIM)
add_library(pulse_archive SHARED plugin.cpp archive_reader.cpp archive_writer.cpp archive_tasks.cpp archive_settings.cpp "${CMAKE_CURRENT_BINARY_DIR}/7z.lib")
target_link_libraries(pulse_archive PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/7z.lib" ole32 oleaut32 uuid)
target_include_directories(pulse_archive PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_features(pulse_archive PRIVATE cxx_std_20)
target_compile_options(pulse_archive PRIVATE /W4 /permissive- /utf-8)

option(PULSE_ARCHIVE_READER_TEST "Build the standalone archive listing test" OFF)
if(PULSE_ARCHIVE_READER_TEST)
    add_executable(archive_reader_test archive_reader_test.cpp archive_reader.cpp "${CMAKE_CURRENT_BINARY_DIR}/7z.lib")
    target_link_libraries(archive_reader_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/7z.lib" ole32 oleaut32 uuid)
    target_compile_features(archive_reader_test PRIVATE cxx_std_20)
    target_compile_options(archive_reader_test PRIVATE /W4 /permissive- /utf-8)
endif()

option(PULSE_ARCHIVE_WRITER_TEST "Build the standalone archive writer test" OFF)
if(PULSE_ARCHIVE_WRITER_TEST)
    add_executable(archive_writer_test archive_writer_test.cpp archive_writer.cpp archive_reader.cpp "${CMAKE_CURRENT_BINARY_DIR}/7z.lib")
    target_link_libraries(archive_writer_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/7z.lib" ole32 oleaut32 uuid)
    target_compile_features(archive_writer_test PRIVATE cxx_std_20)
    target_compile_options(archive_writer_test PRIVATE /W4 /permissive- /utf-8)
endif()

