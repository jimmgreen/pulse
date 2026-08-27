cmake_minimum_required(VERSION 3.25)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PULSE_VERSION LIMIT_COUNT 1)
project(Pulse VERSION ${PULSE_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT MSVC)
    message(FATAL_ERROR "Pulse requires MSVC (Windows SDK / D2D / NT API)")
endif()

add_compile_options(/utf-8 /permissive- /W4 /EHsc)
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 _WIN32_IE=0x0A00 NTDDI_VERSION=NTDDI_WIN10_NI)

option(PULSE_WITH_LUMATEXT "Use LumaText for Pulse-owned single-line text" ON)
set(LUMATEXT_SOURCE_DIR "" CACHE PATH
    "Optional LumaText source tree used for development builds")
set(PULSE_LUMATEXT_TARGET "")
set(PULSE_LUMATEXT_RUNTIME_TARGET "")
if(PULSE_WITH_LUMATEXT)
    if(LUMATEXT_SOURCE_DIR)
        if(NOT EXISTS "${LUMATEXT_SOURCE_DIR}/CMakeLists.txt")
            message(FATAL_ERROR "LUMATEXT_SOURCE_DIR does not contain CMakeLists.txt")
        endif()
        set(LUMATEXT_BUILD_STATIC OFF)
        set(LUMATEXT_BUILD_SAMPLES OFF)
        set(LUMATEXT_BUILD_TESTS OFF)
        set(LUMATEXT_BUILD_REFERENCE_TOOLS OFF)
        add_subdirectory("${LUMATEXT_SOURCE_DIR}"
            "${CMAKE_BINARY_DIR}/_deps/lumatext" EXCLUDE_FROM_ALL)
    else()
        find_package(LumaText 0.1 CONFIG QUIET)
    endif()

    if(TARGET lumatext_shared)
        set(PULSE_LUMATEXT_TARGET LumaText::D2D)
        set(PULSE_LUMATEXT_RUNTIME_TARGET lumatext_shared)
        message(STATUS "Pulse text renderer: LumaText shared library")
    elseif(TARGET LumaText::Shared)
        set(PULSE_LUMATEXT_TARGET LumaText::Shared)
        set(PULSE_LUMATEXT_RUNTIME_TARGET LumaText::Shared)
        message(STATUS "Pulse text renderer: installed LumaText shared library")
    elseif(TARGET LumaText::D2D)
        set(PULSE_LUMATEXT_TARGET LumaText::D2D)
        message(STATUS "Pulse text renderer: LumaText package")
    else()
        message(STATUS "Pulse text renderer: DirectWrite (LumaText was not found)")
    endif()
endif()

function(pulse_enable_lumatext target)
    target_sources(${target} PRIVATE src/ui/lumatext_renderer.cpp)
    if(PULSE_LUMATEXT_TARGET)
        target_compile_definitions(${target} PRIVATE PULSE_HAS_LUMATEXT=1)
        target_link_libraries(${target} PRIVATE ${PULSE_LUMATEXT_TARGET})
        if(PULSE_LUMATEXT_RUNTIME_TARGET)
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    $<TARGET_FILE:${PULSE_LUMATEXT_RUNTIME_TARGET}>
                    $<TARGET_FILE_DIR:${target}>)
            if(LUMATEXT_SOURCE_DIR)
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${LUMATEXT_SOURCE_DIR}/LICENSE"
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText/LumaText.txt
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${LUMATEXT_SOURCE_DIR}/LICENSES"
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText)
            endif()
        endif()
    endif()
endfunction()

set(PULSE_BUILD_ID_FILE "${CMAKE_BINARY_DIR}/pulse_build_id.txt")
if(DEFINED PULSE_BUILD_ID AND NOT PULSE_BUILD_ID STREQUAL "")
    file(WRITE "${PULSE_BUILD_ID_FILE}" "${PULSE_BUILD_ID}\n")
elseif(EXISTS "${PULSE_BUILD_ID_FILE}")
    file(STRINGS "${PULSE_BUILD_ID_FILE}" PULSE_BUILD_ID LIMIT_COUNT 1)
else()
    string(TIMESTAMP PULSE_BUILD_TIME "%Y%m%dT%H%M%SZ" UTC)
    string(RANDOM LENGTH 8 ALPHABET 0123456789ABCDEF PULSE_BUILD_RANDOM)
    set(PULSE_BUILD_ID "${PULSE_BUILD_TIME}-${PULSE_BUILD_RANDOM}")
    file(WRITE "${PULSE_BUILD_ID_FILE}" "${PULSE_BUILD_ID}\n")
endif()

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/generated")
configure_file(cmake/pulse_version.h.in generated/pulse_version.h @ONLY)

set(PULSE_UPDATE_MANIFEST_URL "" CACHE STRING
    "HTTPS URL for the signed update manifest (empty disables update checks)")
set(PULSE_UPDATE_PUBLIC_KEY_HEX "" CACHE STRING
    "ECDSA P-256 public key as an uncompressed SEC1 hex point")
set(PULSE_UPDATE_ENABLED_VALUE 0)
if(NOT PULSE_UPDATE_MANIFEST_URL STREQUAL "" OR
   NOT PULSE_UPDATE_PUBLIC_KEY_HEX STREQUAL "")
    if(NOT PULSE_UPDATE_MANIFEST_URL MATCHES "^https://[^\"]+$")
        message(FATAL_ERROR "PULSE_UPDATE_MANIFEST_URL must be an HTTPS URL")
    endif()
    string(LENGTH "${PULSE_UPDATE_PUBLIC_KEY_HEX}" PULSE_UPDATE_PUBLIC_KEY_LENGTH)
    if(NOT PULSE_UPDATE_PUBLIC_KEY_LENGTH EQUAL 130 OR
       NOT PULSE_UPDATE_PUBLIC_KEY_HEX MATCHES "^04[0-9A-Fa-f]+$")
        message(FATAL_ERROR
            "PULSE_UPDATE_PUBLIC_KEY_HEX must be a 65-byte uncompressed P-256 point")
    endif()
    set(PULSE_UPDATE_ENABLED_VALUE 1)
endif()
configure_file(cmake/pulse_update_config.h.in generated/pulse_update_config.h @ONLY)

function(pulse_add_version_resource target description internal_name original_filename)
    set(PULSE_FILE_DESCRIPTION "${description}")
    set(PULSE_INTERNAL_NAME "${internal_name}")
    set(PULSE_ORIGINAL_FILENAME "${original_filename}")
    configure_file(cmake/version.rc.in "generated/${target}_version.rc" @ONLY)
    target_sources(${target} PRIVATE "${CMAKE_BINARY_DIR}/generated/${target}_version.rc")
endfunction()

add_library(pulse_crash_reporter STATIC
    src/common/crash_reporter.cpp
    src/common/diagnostics_exporter.cpp)
target_include_directories(pulse_crash_reporter PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_crash_reporter PUBLIC dbghelp shell32)

add_library(pulse_text STATIC src/common/text_decode.cpp)

add_library(pulse_localization STATIC src/common/localization.cpp)
target_link_libraries(pulse_localization PUBLIC user32)

add_executable(pulse_localization_test
    src/bench/localization_test.cpp
    src/bench/localization_test.rc)
set_property(SOURCE src/bench/localization_test.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc")
target_link_libraries(pulse_localization_test PRIVATE pulse_localization)

add_executable(pulse_crash_test src/bench/crash_reporter_test.cpp)
target_link_libraries(pulse_crash_test PRIVATE pulse_crash_reporter)

# ---- Stage 0 benchmarks (console) ----
add_executable(pulse_bench_enum src/bench/measure_enum.cpp)
add_executable(pulse_bench_shell src/bench/measure_shell.cpp)
target_link_libraries(pulse_bench_shell PRIVATE ole32 shell32 uuid comctl32)
add_executable(pulse_bench_mft src/bench/measure_mft.cpp)

add_executable(pulse_index_test
    src/bench/index_test_main.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp)
target_link_libraries(pulse_index_test PRIVATE shell32 mpr advapi32)

# End-to-end index latency probe. It talks to the installed/running service so
# timings include the production pipe, query engine, paging, and serialization.
add_executable(pulse_index_perf src/bench/index_perf_main.cpp)

add_executable(pulse_index_host_stress src/bench/index_host_stress.cpp)

add_executable(pulse_content_search_test
    src/bench/content_search_test.cpp
    src/index/content_search.cpp
    src/index/content_search_client.cpp)
target_link_libraries(pulse_content_search_test PRIVATE pulse_text bcrypt ole32)
add_dependencies(pulse_content_search_test pulse_index)

add_executable(pulse_saved_search_test
    src/bench/saved_search_test.cpp
    src/app/saved_search.cpp)
target_link_libraries(pulse_saved_search_test PRIVATE shell32 ole32)

add_executable(pulse_update_test
    src/bench/update_checker_test.cpp
    src/app/update_checker.cpp)
target_include_directories(pulse_update_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_update_test PRIVATE bcrypt crypt32 winhttp)

# ---- Stage 0 visual/perf sample: D2D virtual list (GUI) ----
add_executable(pulse_d2d_list WIN32 src/bench/d2d_list.cpp)
target_link_libraries(pulse_d2d_list PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore)

# Fluent component visual contract: dark/light/high-contrast regression PNGs.
add_executable(pulse_fluent_gallery WIN32
    src/bench/fluent_components_gallery.cpp
    src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp)
target_link_libraries(pulse_fluent_gallery PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs
    pulse_localization)
pulse_enable_lumatext(pulse_fluent_gallery)

# ---- Pulse.exe: main UI process ----
file(GLOB_RECURSE PULSE_APP_SOURCES CONFIGURE_DEPENDS
    src/app/*.cpp src/ui/*.cpp src/fs/*.cpp src/ops/*.cpp src/ipc/*.cpp src/index/*.cpp)
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_host\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "network_agent_main\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_agent\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_search\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "lumatext_renderer\\.cpp$")

# The local-volume engine runs in Pulse.Index.exe. Production keeps only the
# lightweight query parser and per-user network index inside pulse.exe.
option(PULSE_WITH_SELFTEST "Embed the legacy --selftest suite in Pulse.exe" OFF)
if(NOT PULSE_WITH_SELFTEST)
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "selftest_1b2\\.cpp$")
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_(engine|mft|paths|delta)\\.cpp$")
endif()
list(APPEND PULSE_APP_SOURCES src/index/network_agent_client.cpp)
if(PULSE_APP_SOURCES)
    add_executable(pulse WIN32 ${PULSE_APP_SOURCES})
    if(PULSE_WITH_SELFTEST)
        target_compile_definitions(pulse PRIVATE PULSE_WITH_SELFTEST=1)
    endif()
    target_sources(pulse PRIVATE src/app/pulse.rc)
    set_property(SOURCE src/app/pulse.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/cut_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/copy_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/clipboard_paste_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/rename_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/delete_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/star_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/history_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/desktop_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/arrow_square_down_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/recycle_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/drive_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/document_folder_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/globe_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/search_visual_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/apps_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/panel_right_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/panel_right_contract_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/add_circle_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/search_24_color.svg")
    target_link_options(pulse PRIVATE /MANIFEST:NO)
    target_link_libraries(pulse PRIVATE
        d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid comctl32 advapi32 shlwapi version windowscodecs gdi32 netapi32 mpr bcrypt pulse_crash_reporter)
    target_link_libraries(pulse PRIVATE winhttp crypt32)
    target_link_libraries(pulse PRIVATE pulse_localization)
    pulse_enable_lumatext(pulse)
    pulse_add_version_resource(pulse "Pulse file manager" "pulse" "pulse.exe")
endif()

# ---- Pulse.Index.exe: index + query host (session helper or Windows service) ----
add_executable(pulse_index WIN32
    src/index/index_host.cpp
    src/index/network_agent_main.cpp
    src/index/content_agent.cpp
    src/index/content_search.cpp
    src/index/index_engine.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/index_mft.cpp
    src/index/index_paths.cpp
    src/index/index_delta.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp
    src/fs/fs_enum.cpp)
target_sources(pulse_index PRIVATE src/index/index_host.rc)
target_link_options(pulse_index PRIVATE /MANIFEST:NO)
set_target_properties(pulse_index PROPERTIES OUTPUT_NAME "Pulse.Index")
target_link_libraries(pulse_index PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr bcrypt
    pulse_crash_reporter pulse_text)
pulse_add_version_resource(pulse_index "Pulse index host" "Pulse.Index" "Pulse.Index.exe")
add_dependencies(pulse_index_host_stress pulse_index)

# ---- Stage 1B-1 ops self test (console; drives pulse_shell.exe over the pipe) ----
add_executable(pulse_ops_test
    src/bench/ops_test_main.cpp
    src/ops/ops_manager.cpp
    src/ops/clipboard.cpp
    src/ipc/shell_client.cpp)
target_link_libraries(pulse_ops_test PRIVATE ole32 shell32 uuid bcrypt)

# ---- Pulse.Shell.exe: COM isolation process (stage 1: recycle bin / IFileOperation) ----
file(GLOB_RECURSE PULSE_SHELL_SOURCES CONFIGURE_DEPENDS src/shell_host/*.cpp)
if(PULSE_SHELL_SOURCES)
    add_executable(pulse_shell WIN32 ${PULSE_SHELL_SOURCES})
target_link_libraries(pulse_shell PRIVATE ole32 shell32 uuid advapi32 shlwapi version pulse_crash_reporter)
    pulse_add_version_resource(pulse_shell "Pulse Shell host" "pulse_shell" "pulse_shell.exe")
endif()

# ---- Pulse.Preview.exe: thumbnail decoding / Shell provider isolation ----
add_executable(pulse_preview WIN32 src/preview_host/main.cpp)
set_target_properties(pulse_preview PROPERTIES OUTPUT_NAME "Pulse.Preview")
target_link_libraries(pulse_preview PRIVATE ole32 shell32 gdi32 uuid windowscodecs propsys advapi32)
target_link_libraries(pulse_preview PRIVATE pulse_crash_reporter)
pulse_add_version_resource(pulse_preview "Pulse preview host" "Pulse.Preview" "Pulse.Preview.exe")

# Preview protocol self test (console; drives Pulse.Preview.exe over the pipe)
add_executable(pulse_preview_test src/bench/preview_test_main.cpp)
add_dependencies(pulse_preview_test pulse_preview)

add_executable(pulse_preview_handler_probe
    src/bench/preview_handler_probe.cpp
    src/ui/preview_handler_host.cpp)
target_compile_definitions(pulse_preview_handler_probe PRIVATE
    PULSE_PREVIEW_HANDLER_TESTING=1)
target_link_libraries(pulse_preview_handler_probe PRIVATE
    ole32 shell32 uuid shlwapi oleaut32 d2d1 gdi32 user32)

add_executable(pulse_material_test
    src/bench/window_material_test.cpp
    src/ui/window_material.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp)
target_compile_definitions(pulse_material_test PRIVATE
    PULSE_WINDOW_MATERIAL_TESTING=1)
target_link_libraries(pulse_material_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32
    pulse_localization)
pulse_enable_lumatext(pulse_material_test)

add_executable(pulse_app_controllers_test
    src/bench/app_controllers_test.cpp
    src/app/settings_controller.cpp
    src/app/context_menu_controller.cpp
    src/app/session_tab_model.cpp
    src/app/tab_group_model.cpp
    src/app/single_instance_coordinator.cpp
    src/app/tray_controller.cpp
    src/app/app_prefs.cpp
    src/app/context_menu_prefs.cpp
    src/ui/view_layout.cpp
    src/index/index_client.cpp
    src/index/network_agent_client.cpp)
target_link_libraries(pulse_app_controllers_test PRIVATE
    user32 shell32 ole32 uuid advapi32 shlwapi)

# Shell-only operations (recycle bin, Explorer verbs) go through pulse_shell.exe.
# Copy/move use the in-process scheduler plus CopyFile2.
# Filename search goes through Pulse.Index.exe (never Engine::Start in the UI).
if(TARGET pulse AND TARGET pulse_shell)
    add_dependencies(pulse pulse_shell)
endif()
if(TARGET pulse AND TARGET pulse_preview)
    add_dependencies(pulse pulse_preview)
endif()
if(TARGET pulse AND TARGET pulse_index)
    add_dependencies(pulse pulse_index)
endif()
if(TARGET pulse_ops_test AND TARGET pulse_shell)
    add_dependencies(pulse_ops_test pulse_shell)
endif()
