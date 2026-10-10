cmake_minimum_required(VERSION 3.25)
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" PULSE_VERSION LIMIT_COUNT 1)
project(Pulse VERSION ${PULSE_VERSION} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Self-contained releases, including bundled static dependencies.
if(NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "Pulse requires MSVC (Windows SDK / D2D / NT API)")
endif()

option(PULSE_WIN81_CANDIDATE "Require a Windows 8.1-capable release toolset" OFF)
option(PULSE_RELEASE_SYMBOLS "Generate matching PDBs for signed release archives" OFF)
if(PULSE_WIN81_CANDIDATE AND MSVC_VERSION GREATER_EQUAL 1950)
    message(FATAL_ERROR "Windows 8.1 candidate builds require VS 2022 v143, not VS 2026")
endif()

add_compile_options(/utf-8 /permissive- /W4 /EHsc)
# Expose optional SDK COM interfaces; runtime calls are capability-checked.
add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603 WINVER=0x0603 _WIN32_IE=0x0A00 NTDDI_VERSION=NTDDI_WIN10_NI)

option(PULSE_WITH_LUMATEXT "Use LumaText for Pulse-owned single-line text" ON)
set(LUMATEXT_SOURCE_DIR "" CACHE PATH
    "Optional LumaText source tree used for development builds")
set(PULSE_LUMATEXT_TARGET "")
set(PULSE_LUMATEXT_RUNTIME_TARGET "")
if(PULSE_WITH_LUMATEXT)
    # The committed prebuilt package keeps everyday builds from rebuilding the
    # LumaText source tree; an explicit CMAKE_PREFIX_PATH (release SDK) still wins.
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/bin/lumatext.dll")
        list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext")
    endif()
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
        if(MSVC)
            foreach(runtime_target lumatext_shared lumatext_harfbuzz freetype)
                if(TARGET ${runtime_target})
                    set_property(TARGET ${runtime_target} PROPERTY MSVC_RUNTIME_LIBRARY
                        "${CMAKE_MSVC_RUNTIME_LIBRARY}")
                endif()
            endforeach()
        endif()
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
        target_link_libraries(${target} PRIVATE ${PULSE_LUMATEXT_TARGET} delayimp)
        target_link_options(${target} PRIVATE /DELAYLOAD:lumatext.dll)
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
            elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/share/LumaText/licenses")
                add_custom_command(TARGET ${target} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E make_directory
                        $<TARGET_FILE_DIR:${target}>/licenses/LumaText
                    COMMAND ${CMAKE_COMMAND} -E copy_directory
                        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/lumatext/share/LumaText/licenses"
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

set(PULSE_DEFAULT_UPDATE_MANIFEST "https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest.json")
if(PULSE_WIN81_CANDIDATE)
    set(PULSE_DEFAULT_UPDATE_MANIFEST "https://github.com/jimmgreen/pulse/releases/latest/download/update-manifest-win81.json")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/cmake/update-public-key.txt" PULSE_DEFAULT_UPDATE_PUBLIC_KEY LIMIT_COUNT 1)
set(PULSE_UPDATE_MANIFEST_URL "${PULSE_DEFAULT_UPDATE_MANIFEST}" CACHE STRING
    "HTTPS URL for the signed update manifest (empty disables update checks)")
set(PULSE_UPDATE_PUBLIC_KEY_HEX "${PULSE_DEFAULT_UPDATE_PUBLIC_KEY}" CACHE STRING
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

add_library(pulse_runtime_log STATIC src/common/runtime_log.cpp)
target_include_directories(pulse_runtime_log PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_runtime_log PUBLIC psapi)

add_library(pulse_crash_reporter STATIC
    src/common/crash_reporter.cpp
    src/common/diagnostics_exporter.cpp
    src/common/diagnostics_report.cpp
    src/common/diagnostics_archive.cpp)
target_include_directories(pulse_crash_reporter PUBLIC "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_crash_reporter PUBLIC dbghelp shell32 bcrypt version advapi32 pulse_localization pulse_runtime_log)

add_library(pulse_text STATIC src/common/text_decode.cpp)

add_library(pulse_localization STATIC src/common/localization.cpp src/common/zh_hant.cpp
    src/common/service_text.cpp)
target_link_libraries(pulse_localization PUBLIC user32)

add_executable(pulse_localization_test
    src/bench/localization_test.cpp
    src/bench/localization_test.rc)
set_property(SOURCE src/bench/localization_test.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings_zh_tw.rcinc")
target_link_libraries(pulse_localization_test PRIVATE pulse_localization)

add_executable(pulse_crash_test src/bench/crash_reporter_test.cpp)
target_link_libraries(pulse_crash_test PRIVATE pulse_crash_reporter)

add_executable(pulse_diagnostics_recovery_test src/bench/diagnostics_recovery_test.cpp)
target_link_libraries(pulse_diagnostics_recovery_test PRIVATE pulse_crash_reporter)

# ---- Stage 0 benchmarks (console) ----
add_executable(pulse_bench_enum src/bench/measure_enum.cpp)
add_executable(pulse_bench_shell src/bench/measure_shell.cpp)
target_link_libraries(pulse_bench_shell PRIVATE ole32 shell32 uuid comctl32)
add_executable(pulse_bench_mft src/bench/measure_mft.cpp)
target_link_libraries(pulse_bench_mft PRIVATE shell32)

add_executable(pulse_index_test
    src/bench/index_test_main.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp
    src/index/change_tracking.cpp)
target_link_libraries(pulse_index_test PRIVATE shell32 mpr advapi32)

add_executable(pulse_index_migration_test
    src/bench/index_migration_test.cpp
    src/index/index_migration.cpp src/index/index_config.cpp)
target_link_libraries(pulse_index_migration_test PRIVATE shell32 advapi32)
add_executable(pulse_index_security_test src/bench/index_security_test.cpp src/index/index_migration.cpp src/index/index_config.cpp)
target_link_libraries(pulse_index_security_test PRIVATE advapi32)
add_executable(pulse_ffmpeg_process_audit_test src/bench/ffmpeg_process_audit_test.cpp)
target_link_libraries(pulse_ffmpeg_process_audit_test PRIVATE shell32)

add_executable(pulse_index_engine_test
    src/bench/index_engine_test.cpp
    src/index/index_engine.cpp
    src/index/change_tracking.cpp
    src/index/index_visibility.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_mft.cpp
    src/index/index_paths.cpp
    src/index/index_delta.cpp
    src/index/index_shard.cpp
    src/fs/fs_enum.cpp)
target_link_libraries(pulse_index_engine_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization)

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

add_executable(pulse_duplicate_scan_test
    src/bench/duplicate_scan_test.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/app/duplicate_scan.cpp)
target_link_libraries(pulse_duplicate_scan_test PRIVATE ole32)

add_executable(pulse_drag_drop_test src/bench/drag_drop_test_main.cpp)
target_link_libraries(pulse_drag_drop_test PRIVATE ole32 shell32 uuid user32)

add_executable(pulse_drop_staging_test src/bench/drop_staging_test.cpp src/app/drop_staging.cpp)
add_executable(pulse_drop_staging_failure_test src/bench/drop_staging_failure_test.cpp)

add_executable(pulse_saved_search_test
    src/bench/saved_search_test.cpp
    src/app/saved_search.cpp)
target_link_libraries(pulse_saved_search_test PRIVATE shell32 ole32)

add_executable(pulse_search_query_test
    src/bench/search_query_test.cpp
    src/app/search_query.cpp
    src/app/search_query_rewrite.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp)

add_executable(pulse_update_test
    src/bench/update_checker_test.cpp
    src/app/update_checker.cpp
    src/app/update_transport.cpp)
target_include_directories(pulse_update_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_update_test PRIVATE bcrypt crypt32 winhttp user32)

add_executable(pulse_update_installer_test
    src/bench/update_installer_test.cpp
    src/app/update_installer.cpp
    src/app/update_transport.cpp)
target_include_directories(pulse_update_installer_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_update_installer_test PRIVATE bcrypt shell32 user32 winhttp)

add_executable(pulse_pack_installer_test
    src/bench/pack_installer_test.cpp
    src/app/pack_installer.cpp
    src/app/update_installer.cpp
    src/app/update_transport.cpp)
target_include_directories(pulse_pack_installer_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_pack_installer_test PRIVATE pulse_runtime_log bcrypt cabinet shell32 user32 winhttp)

add_executable(pulse_update_session_test src/bench/update_session_test.cpp src/app/startup_location.cpp)

# ---- Stage 0 visual/perf sample: D2D virtual list (GUI) ----
add_executable(pulse_d2d_list WIN32 src/bench/d2d_list.cpp)
target_link_libraries(pulse_d2d_list PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore)

# Fluent component visual contract: dark/light/high-contrast regression PNGs.
add_executable(pulse_fluent_gallery WIN32
    src/bench/fluent_components_gallery.cpp
    src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp)
target_link_libraries(pulse_fluent_gallery PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32
    pulse_localization)
pulse_enable_lumatext(pulse_fluent_gallery)

# Pulse's own dialogs: offscreen PNGs in light / dark / high contrast at 100%
# and 150%, plus --live real windows. The open dialog hosts the main window's
# renderer, so it runs inside pulse.exe (pulse.exe --picker-live file|folder|image).
add_executable(pulse_dialog_gallery WIN32
    src/bench/dialog_gallery.cpp
    src/bench/dialog_gallery.rc
    src/ui/confirm_dialog.cpp
    src/ui/confirm_dialog_view.cpp
    src/ui/dialog_lifecycle.cpp
    src/ui/edit_host.cpp
    src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp)
target_link_libraries(pulse_dialog_gallery PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme shell32
    pulse_localization)
# Pulse's manifest: layered child EDITs need the Windows 8+ compatibility id.
set_property(SOURCE src/bench/dialog_gallery.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings_zh_tw.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/pulse.manifest")
target_link_options(pulse_dialog_gallery PRIVATE /MANIFEST:NO)
pulse_enable_lumatext(pulse_dialog_gallery)

target_link_libraries(pulse_dialog_gallery PRIVATE shlwapi uuid)
# The open dialog's own chrome (title strip and footer): layout and hit tests.
add_executable(pulse_file_picker_view_test src/bench/file_picker_view_test.cpp
    src/bench/dialog_gallery.rc
    src/ui/dialog_lifecycle.cpp
    src/ui/edit_host.cpp
    src/ui/fluent_components.cpp
    src/ui/folder_picker_view.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp)
target_link_libraries(pulse_file_picker_view_test PRIVATE d3d11 d2d1 dwrite dxgi dcomp
    dwmapi shcore ole32 windowscodecs comctl32 uxtheme shell32 shlwapi uuid pulse_localization)
target_link_options(pulse_file_picker_view_test PRIVATE /MANIFEST:NO)
pulse_enable_lumatext(pulse_file_picker_view_test)
add_executable(pulse_file_picker_model_test src/bench/file_picker_model_test.cpp
    src/bench/localization_test.rc src/ui/folder_picker_model.cpp src/ui/folder_picker_loader.cpp)
target_link_libraries(pulse_file_picker_model_test PRIVATE shell32 ole32 uuid pulse_localization)
# The loader enumerates with the main window's fs::EnumerateDirectory.
target_link_libraries(pulse_file_picker_model_test PRIVATE pulse_fs_watch)

add_executable(pulse_dialogs_test
    src/bench/dialogs_test.cpp
    src/bench/localization_test.rc
    src/ui/confirm_dialog_view.cpp
    src/ui/fluent_components.cpp
    src/ui/folder_picker_loader.cpp
    src/ui/folder_picker_model.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp
    src/ui/dialog_lifecycle.cpp)
target_link_libraries(pulse_dialogs_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme
    pulse_localization)
pulse_enable_lumatext(pulse_dialogs_test)
target_link_libraries(pulse_dialogs_test PRIVATE uuid pulse_fs_watch)

# Conflict dialog: hovering a cut-off path shows it whole (#55).
add_executable(pulse_conflict_dialog_test
    src/bench/conflict_dialog_test.cpp
    src/bench/localization_test.rc
    src/ui/file_operation_dialog.cpp
    src/ui/confirm_dialog_view.cpp
    src/ui/dialog_lifecycle.cpp
    src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp
)
target_link_libraries(pulse_conflict_dialog_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme gdiplus
    pulse_localization)
pulse_enable_lumatext(pulse_conflict_dialog_test)
get_target_property(PULSE_PERMISSION_UI_SOURCES pulse_conflict_dialog_test SOURCES)
list(REMOVE_ITEM PULSE_PERMISSION_UI_SOURCES src/bench/conflict_dialog_test.cpp)
add_executable(pulse_file_operation_authorization_dialog_test
    src/bench/file_operation_authorization_dialog_test.cpp ${PULSE_PERMISSION_UI_SOURCES})
target_link_libraries(pulse_file_operation_authorization_dialog_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme gdiplus pulse_localization)
pulse_enable_lumatext(pulse_file_operation_authorization_dialog_test)

add_executable(pulse_file_picker_operations_test
    src/bench/file_picker_operations_test.cpp
    src/ui/folder_picker_operations.cpp ${PULSE_PERMISSION_UI_SOURCES}
    src/ops/ops_manager.cpp src/ops/duplicate_cleanup_guard.cpp
    src/ops/file_lock_owner.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_file_picker_operations_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme
    gdiplus shell32 uuid bcrypt pulse_localization)
pulse_enable_lumatext(pulse_file_picker_operations_test)

# Real batch-rename host on an isolated, never activated desktop.
add_executable(pulse_batch_rename_present_test
    src/bench/batch_rename_present_test.cpp src/bench/child_edit_test.rc
    src/ui/batch_rename_dialog.cpp src/app/batch_rename.cpp src/fs/fs_enum.cpp
    src/ui/dialog_lifecycle.cpp src/ui/fluent_components.cpp src/ui/tab_shape.cpp
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/window_helpers.cpp src/ui/edit_host.cpp)
target_compile_definitions(pulse_batch_rename_present_test PRIVATE PULSE_TEST_BATCH_RENAME_PRESENT=1)
target_link_options(pulse_batch_rename_present_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_batch_rename_present_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme gdiplus
    shell32 shlwapi netapi32 pulse_localization)
pulse_enable_lumatext(pulse_batch_rename_present_test)

get_target_property(PULSE_PERMISSION_UI_SOURCES pulse_conflict_dialog_test SOURCES)
list(REMOVE_ITEM PULSE_PERMISSION_UI_SOURCES src/bench/conflict_dialog_test.cpp)
target_link_libraries(pulse_file_operation_authorization_dialog_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs comctl32 uxtheme gdiplus pulse_localization)
pulse_enable_lumatext(pulse_file_operation_authorization_dialog_test)

# ---- Focused visual regression: offscreen commands and theme tokens ----
add_executable(pulse_command_icon_test src/bench/command_icon_test.cpp)
target_link_libraries(pulse_command_icon_test PRIVATE d2d1 dwrite dwmapi ole32 windowscodecs user32)

# ---- Pulse.exe: main UI process ----
file(GLOB_RECURSE PULSE_APP_SOURCES CONFIGURE_DEPENDS
    src/app/*.cpp src/ui/*.cpp src/fs/*.cpp src/ops/*.cpp src/ipc/*.cpp src/index/*.cpp)
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_host\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "network_agent_main\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_(agent|index|search|task_search|hit_aliases|instant_session)\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "filename_timing\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "text_task_reader\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "content_timing\\.cpp$")

list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "lumatext_renderer\\.cpp$")
list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "(document_(host_main|reader|filter)|office_ooxml|pdfium_text)\\.cpp$")

# The local-volume engine runs in Pulse.Index.exe. Production keeps only the
# lightweight query parser and per-user network index inside pulse.exe.
option(PULSE_WITH_SELFTEST "Embed the legacy --selftest suite in Pulse.exe" OFF)
if(NOT PULSE_WITH_SELFTEST)
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "selftest_1b2\\.cpp$")
    list(FILTER PULSE_APP_SOURCES EXCLUDE REGEX "index_(engine|engine_pinyin|visibility|mft|paths|delta|folder_sizes)\\.cpp$")
endif()
list(APPEND PULSE_APP_SOURCES src/index/network_agent_client.cpp)
if(PULSE_APP_SOURCES)
    add_executable(pulse WIN32 ${PULSE_APP_SOURCES})
    if(PULSE_WITH_SELFTEST)
        target_compile_definitions(pulse PRIVATE PULSE_WITH_SELFTEST=1)
        target_sources(pulse PRIVATE src/bench/startup_rollback_test.cpp)
        target_sources(pulse PRIVATE src/bench/ui_module_audit_test.cpp)
        target_sources(pulse PRIVATE src/bench/quick_preview_audit_test.cpp)
        target_sources(pulse PRIVATE src/bench/tray_completion_test.cpp)
    endif()
    target_sources(pulse PRIVATE src/app/pulse.rc src/common/ffmpeg_tool.cpp src/bench/file_picker_dialog_test.cpp src/bench/file_picker_operations_test.cpp)
    include(cmake/release_notes.cmake)
    pulse_embed_release_notes(pulse)
    set_property(SOURCE src/app/pulse.rc APPEND PROPERTY OBJECT_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/global-search-logo.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/app/pulse.ico"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings_zh_tw.rcinc"
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
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/search_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/pc_24_color.svg"
        "${CMAKE_CURRENT_SOURCE_DIR}/assets/fluent/tag_24_color.svg")
    target_link_options(pulse PRIVATE /MANIFEST:NO)
# pulse.map resolves --hang-watch stack RVAs (Release builds carry no PDB).
target_link_options(pulse PRIVATE /MAP)
    target_link_libraries(pulse PRIVATE
        d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 shell32 uuid comctl32 advapi32 shlwapi version windowscodecs gdi32 netapi32 mpr bcrypt pulse_crash_reporter)
    target_link_libraries(pulse PRIVATE propsys oleaut32)  # shell_window_registry.cpp
    target_link_libraries(pulse PRIVATE winhttp crypt32 secur32)
    target_link_libraries(pulse PRIVATE pulse_localization pulse_text)
    pulse_enable_lumatext(pulse)
    pulse_add_version_resource(pulse "Pulse file manager" "pulse" "pulse.exe")
endif()

# ---- Pulse.Index.exe: index + query host (session helper or Windows service) ----
add_executable(pulse_index WIN32
    src/index/index_host.cpp
    src/index/index_path_service.cpp
    src/index/index_migration.cpp
    src/index/network_agent_main.cpp
    src/index/content_agent.cpp
    src/index/content_instant_session.cpp
    src/index/content_search.cpp
    src/index/index_engine.cpp
    src/index/change_tracking.cpp
    src/index/index_visibility.cpp
    src/index/index_config.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/index_mft.cpp
    src/index/index_paths.cpp
    src/index/index_delta.cpp
    src/index/index_shard.cpp
    src/index/network_index.cpp
    src/fs/fs_enum.cpp)
target_sources(pulse_index PRIVATE src/index/index_host.rc)
target_link_options(pulse_index PRIVATE /MANIFEST:NO)
set_target_properties(pulse_index PROPERTIES OUTPUT_NAME "Pulse.Index")
target_link_libraries(pulse_index PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr bcrypt pulse_localization
    pulse_crash_reporter pulse_text)
pulse_add_version_resource(pulse_index "Pulse index host" "Pulse.Index" "Pulse.Index.exe")
add_dependencies(pulse_index_host_stress pulse_index)
add_executable(pulse_index_caller_filter_test src/bench/index_caller_filter_test.cpp)
target_link_libraries(pulse_index_caller_filter_test PRIVATE advapi32)
add_dependencies(pulse_index_caller_filter_test pulse_index)

# ---- Stage 1B-1 ops self test (console; drives pulse_shell.exe over the pipe) ----
add_executable(pulse_operation_presentation_test src/bench/operation_presentation_test.cpp)
add_executable(pulse_column_strip_test src/bench/column_strip_test.cpp src/ui/column_strip_layout.cpp)
add_executable(pulse_playback_controls_test src/bench/playback_controls_test.cpp src/ui/video_preview.cpp
    src/ui/ffmpeg_playback.cpp src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_playback_controls_test PRIVATE user32 gdi32 ole32 mfuuid winmm)

# Quick Look playback through the FFmpeg pack; a real FFmpeg stands in for the
# pack (PULSE_TEST_FFMPEG or C:\ffmpeg\bin), else the integration part is skipped.
add_executable(pulse_ffmpeg_playback_test src/bench/ffmpeg_playback_test.cpp
    src/ui/ffmpeg_playback.cpp src/ui/audio_waveform.cpp src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_ffmpeg_playback_test PRIVATE user32 winmm ole32 mfuuid)
add_executable(pulse_cover_palette_test src/bench/cover_palette_test.cpp)

add_executable(pulse_ops_test
    src/bench/ops_test_main.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/ops/ops_manager.cpp
    src/ops/file_lock_owner.cpp
    src/ops/clipboard.cpp
    src/ipc/shell_client.cpp)
target_link_libraries(pulse_ops_test PRIVATE ole32 shell32 uuid bcrypt pulse_localization)
add_executable(pulse_authorization_flow_test src/bench/authorization_flow_test.cpp
    src/ops/ops_manager.cpp src/ops/duplicate_cleanup_guard.cpp src/ops/file_lock_owner.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_authorization_flow_test PRIVATE ole32 shell32 uuid bcrypt advapi32 pulse_localization)

add_executable(pulse_rename_ops_test src/bench/rename_ops_test.cpp
    src/app/batch_rename.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/ops/ops_manager.cpp src/ops/file_lock_owner.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_rename_ops_test PRIVATE ole32 shell32 uuid bcrypt pulse_localization pulse_fs_watch)

add_executable(pulse_create_rename_intent_test src/bench/create_rename_intent_test.cpp
    src/app/create_rename_intent.cpp src/ops/duplicate_cleanup_guard.cpp
    src/ops/ops_manager.cpp src/ops/file_lock_owner.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_create_rename_intent_test PRIVATE ole32 shell32 uuid bcrypt pulse_localization pulse_fs_watch)

# Locked-item lookup (Restart Manager) + end-process-and-retry through OpsManager.
add_executable(pulse_file_lock_test src/bench/file_lock_owner_test.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/ops/file_lock_owner.cpp src/ops/ops_manager.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_file_lock_test PRIVATE ole32 shell32 uuid bcrypt rstrtmgr pulse_localization)

# ---- Pulse.Shell.exe: COM isolation process (stage 1: recycle bin / IFileOperation) ----
set(PULSE_ELEVATED_HOST_SOURCES
    src/elevated_host/main.cpp
    src/ops/elevated_delete.cpp
    src/ipc/elevated_transfer_transport.cpp
    src/ops/ops_manager.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/ops/file_lock_owner.cpp
    src/ipc/shell_client.cpp)
add_executable(pulse_elevated WIN32 ${PULSE_ELEVATED_HOST_SOURCES})
target_compile_definitions(pulse_elevated PRIVATE PULSE_ELEVATED_HOST=1)
target_link_libraries(pulse_elevated PRIVATE ole32 shell32 uuid advapi32 bcrypt
    rstrtmgr pulse_localization)
pulse_add_version_resource(pulse_elevated "Pulse authorized file operations" "pulse_elevated" "pulse_elevated.exe")
if(TARGET pulse)
    add_dependencies(pulse pulse_elevated)
endif()
add_executable(pulse_elevated_test_host WIN32 ${PULSE_ELEVATED_HOST_SOURCES})
target_compile_definitions(pulse_elevated_test_host PRIVATE PULSE_ELEVATED_HOST=1 PULSE_ELEVATED_TEST_HOST=1)
target_link_libraries(pulse_elevated_test_host PRIVATE ole32 shell32 uuid advapi32 bcrypt
    rstrtmgr pulse_localization)
add_executable(pulse_elevated_transfer_protocol_test src/bench/elevated_transfer_protocol_test.cpp)
add_executable(pulse_elevated_delete_test src/bench/elevated_delete_test.cpp src/ops/elevated_delete.cpp)
target_link_libraries(pulse_elevated_delete_test PRIVATE ole32 shell32 uuid)
add_executable(pulse_elevated_session_test src/bench/elevated_session_test.cpp
    src/ops/elevated_transfer_client.cpp src/ipc/elevated_transfer_transport.cpp)
target_compile_definitions(pulse_elevated_session_test PRIVATE PULSE_ELEVATED_TEST_CLIENT=1)
target_sources(pulse_elevated_session_test PRIVATE src/bench/review_transfer_test.cpp
    src/ops/ops_manager.cpp src/ops/duplicate_cleanup_guard.cpp
    src/ops/file_lock_owner.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_elevated_session_test PRIVATE ole32 shell32 advapi32 bcrypt
    uuid rstrtmgr pulse_localization)
add_dependencies(pulse_elevated_session_test pulse_elevated_test_host)

file(GLOB_RECURSE PULSE_SHELL_SOURCES CONFIGURE_DEPENDS src/shell_host/*.cpp)
if(PULSE_SHELL_SOURCES)
    add_executable(pulse_shell WIN32 ${PULSE_SHELL_SOURCES})
target_link_libraries(pulse_shell PRIVATE ole32 shell32 uuid advapi32 shlwapi version pulse_crash_reporter)
    pulse_add_version_resource(pulse_shell "Pulse Shell host" "pulse_shell" "pulse_shell.exe")
endif()

# ---- Pulse.Preview.exe: thumbnail decoding / Shell provider isolation ----
add_executable(pulse_preview WIN32 src/preview_host/main.cpp src/preview_host/video_codec.cpp
    src/preview_host/media_pack.cpp src/preview_host/image_pack.cpp
    src/preview_host/raw_pack.cpp src/preview_host/archive_pack.cpp src/common/ffmpeg_tool.cpp
    src/preview_host/folder_thumbnail.cpp
    src/preview_host/svg_raster.cpp src/preview_host/metafile_raster.cpp
    src/preview_host/pdf_raster.cpp src/preview_host/dwg_thumb.cpp
    src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp src/preview_host/psd_raster.cpp
    src/preview_host/font_raster.cpp src/preview_host/preview_decoders.cpp
    src/preview_host/preview_properties.cpp src/preview_host/preview_file_utils.cpp
    src/preview_host/zip_entry.cpp src/preview_host/office_model.cpp src/preview_host/office_sketch.cpp
    src/preview_host/folder_listing.cpp src/preview_host/markdown_document.cpp third_party/md4c/md4c.c
    src/preview_host/office_doc_model.cpp src/preview_host/office_slide_model.cpp
    src/preview_host/office_sheet_model.cpp
    src/preview_host/content_sniff.cpp src/preview_host/table_document.cpp
    src/preview_host/tree_document.cpp src/preview_host/notebook_document.cpp
    src/preview_host/doc_payload.cpp src/preview_host/docx_document.cpp src/preview_host/epub_document.cpp
    src/preview_host/image_frames.cpp)
set_target_properties(pulse_preview PROPERTIES OUTPUT_NAME "Pulse.Preview")
# md4c (MIT, third_party/md4c) parses Markdown for Quick Look, on UTF-16 text.
target_compile_definitions(pulse_preview PRIVATE MD4C_USE_UTF16)
set_source_files_properties(third_party/md4c/md4c.c PROPERTIES COMPILE_OPTIONS "/W0")
# PDF / AI previews render with PDFium, loaded on demand (LoadLibraryExW) from
# the executable's directory - the same pdfium.dll Pulse.Document.exe uses.
add_custom_command(TARGET pulse_preview POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll" "$<TARGET_FILE_DIR:pulse_preview>/pdfium.dll")
# gdiplus renders WMF/EMF, including the EMF+ records Office clip art uses.
target_link_libraries(pulse_preview PRIVATE ole32 shell32 gdi32 uuid windowscodecs propsys advapi32 gdiplus shlwapi)
# Keep system renderers out of host startup: SVG loads Direct2D on
# demand; Office sketches likewise resolve d2d1/dwrite/xmllite dynamically.
target_link_libraries(pulse_preview PRIVATE pulse_crash_reporter)
pulse_add_version_resource(pulse_preview "Pulse preview host" "Pulse.Preview" "Pulse.Preview.exe")

# Preview protocol self test (console; drives Pulse.Preview.exe over the pipe)
add_executable(pulse_preview_test src/bench/preview_test_main.cpp src/bench/preview_coverage.cpp
    src/bench/thumbnail_cache_test.cpp src/ui/thumbnail_cache.cpp src/preview_host/video_codec.cpp
    src/preview_host/zip_entry.cpp src/preview_host/office_model.cpp
    src/preview_host/office_doc_model.cpp src/preview_host/office_slide_model.cpp
    src/preview_host/office_sheet_model.cpp
    src/preview_host/content_sniff.cpp)
target_link_libraries(pulse_preview_test PRIVATE ole32 gdi32 gdiplus shlwapi shell32 advapi32 uuid)
add_dependencies(pulse_preview_test pulse_preview)

# FFmpeg preview pack: parsing units and an end-to-end run through
# Pulse.Preview.exe. A real FFmpeg stands in for the pack (PULSE_TEST_FFMPEG or
# C:\ffmpeg\bin); without one the integration part is skipped.
add_executable(pulse_media_pack_test src/bench/media_pack_test.cpp src/preview_host/media_pack.cpp
    src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_media_pack_test PRIVATE pulse_runtime_log ole32 shell32 advapi32)
add_dependencies(pulse_media_pack_test pulse_preview)

# Image preview pack: parsing units and an end-to-end run of a built pack
# (PULSE_TEST_IMGPACK, see packs/images/build.ps1) directly and through
# Pulse.Preview.exe; without one the integration part is skipped.
add_executable(pulse_image_pack_test src/bench/image_pack_test.cpp src/preview_host/image_pack.cpp
    src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_image_pack_test PRIVATE pulse_runtime_log ole32 shell32 advapi32)
add_dependencies(pulse_image_pack_test pulse_preview)

add_executable(pulse_raw_pack_test src/bench/raw_pack_test.cpp src/preview_host/raw_pack.cpp
    src/preview_host/image_pack.cpp src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_raw_pack_test PRIVATE pulse_runtime_log ole32 shell32 advapi32)
add_dependencies(pulse_raw_pack_test pulse_preview)

add_executable(pulse_raw_pack_host_test src/bench/raw_pack_host_test.cpp)
target_link_libraries(pulse_raw_pack_host_test PRIVATE user32)
add_dependencies(pulse_raw_pack_host_test pulse_preview)

add_executable(pulse_archive_pack_test src/bench/archive_pack_test.cpp src/preview_host/archive_pack.cpp
    src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_archive_pack_test PRIVATE pulse_runtime_log ole32 shell32 shlwapi advapi32)
add_dependencies(pulse_archive_pack_test pulse_preview)

add_executable(pulse_archive_pack_host_test src/bench/archive_pack_host_test.cpp)
target_link_libraries(pulse_archive_pack_host_test PRIVATE user32)
add_dependencies(pulse_archive_pack_host_test pulse_preview)

add_executable(pulse_preview_regression_test src/bench/preview_regression_test.cpp
    src/preview_host/doc_payload.cpp src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp)
target_link_libraries(pulse_preview_regression_test PRIVATE ole32 shell32 shlwapi)
add_dependencies(pulse_preview_regression_test pulse_preview)

# Native Markdown math: focused parsing, layout and offscreen rendering checks.
add_executable(pulse_math_syntax_test src/bench/math_syntax_test.cpp
    src/ui/math_syntax.cpp src/ui/math_macro.cpp src/ui/math_alphabet.cpp src/ui/math_font_metrics.cpp)

add_executable(pulse_math_test src/bench/math_formula_test.cpp
    src/bench/math_formula_stage3_test.cpp src/bench/math_formula_stage4_test.cpp
    src/bench/math_formula_stage5_test.cpp
    src/bench/math_formula_architecture_test.cpp
    src/bench/math_formula_structures_test.cpp
    src/bench/math_formula_reliable_test.cpp
    src/ui/math_formula.cpp src/ui/math_syntax.cpp src/ui/math_layout.cpp
    src/ui/math_macro.cpp src/ui/math_alphabet.cpp
    src/ui/math_font_metrics.cpp src/ui/math_font_face.cpp
    src/ui/markdown_view.cpp src/ui/syntax_highlight.cpp src/ui/typography.cpp
    src/ui/thumbnail_cache.cpp src/ui/ui_compositor.cpp
    src/preview_host/markdown_document.cpp third_party/md4c/md4c.c)
target_compile_definitions(pulse_math_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_math_test PRIVATE dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_math_test)

add_executable(pulse_shell_icons_test src/bench/shell_icons_test.cpp src/ui/shell_icons.cpp)
target_link_libraries(pulse_shell_icons_test PRIVATE ole32 shell32 comctl32 uuid)

# Packaged (MSIX / sparse) File Explorer context-menu verbs (#48).
add_executable(pulse_packaged_ctx_test src/bench/packaged_ctx_test.cpp
    src/shell_host/packaged_ctx_handlers.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_packaged_ctx_test PRIVATE ole32 shell32 uuid shlwapi bcrypt pulse_localization)
# A context-menu handler that never returns: pulse_shell recycles itself (#65).
add_executable(pulse_shell_recycle_test src/bench/shell_recycle_test.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_shell_recycle_test PRIVATE ole32 shell32 uuid shlwapi bcrypt pulse_localization)

add_executable(pulse_ctx_handlers_test src/bench/ctx_handlers_test.cpp
    src/shell_host/ctx_handlers.cpp src/shell_host/packaged_ctx_handlers.cpp)
target_link_libraries(pulse_ctx_handlers_test PRIVATE ole32 shell32 uuid shlwapi)

# Explorer parity diagnosis: native menu vs Pulse pipeline on the real machine.
add_executable(pulse_ctx_parity_probe src/bench/ctx_parity_probe.cpp
    src/shell_host/ctx_handlers.cpp src/shell_host/packaged_ctx_handlers.cpp src/app/shell_verbs.cpp)
target_link_libraries(pulse_ctx_parity_probe PRIVATE ole32 oleaut32 shell32 uuid shlwapi advapi32
    version user32 pulse_localization)

add_executable(pulse_integration src/integration_host/main.cpp src/app/shell_integration_registry.cpp)
target_link_libraries(pulse_integration PRIVATE advapi32 shell32 shlwapi)

add_executable(pulse_open_handoff_test src/bench/open_handoff_test.cpp src/app/single_instance_coordinator.cpp src/app/this_pc_argument.cpp)
target_link_libraries(pulse_open_handoff_test PRIVATE user32 ole32 advapi32)

add_executable(pulse_integration_registry_test src/bench/integration_registry_test.cpp
    src/app/shell_integration_registry.cpp)
target_link_libraries(pulse_integration_registry_test PRIVATE advapi32 shlwapi)
target_compile_definitions(pulse_integration_registry_test PRIVATE PULSE_INTEGRATION_TEST=1)

add_executable(pulse_preview_handler_probe
    src/bench/preview_handler_probe.cpp
    src/ui/preview_handler_host.cpp)
target_compile_definitions(pulse_preview_handler_probe PRIVATE
    PULSE_PREVIEW_HANDLER_TESTING=1)
target_link_libraries(pulse_preview_handler_probe PRIVATE
    ole32 shell32 uuid shlwapi oleaut32 d2d1 gdi32 user32)

add_executable(pulse_preview_handler_pan_test src/bench/preview_handler_pan_test.cpp)
add_executable(pulse_directory_test src/bench/directory_regression_test.cpp)
add_executable(pulse_fs_audit_test src/bench/fs_audit_test.cpp)
target_link_libraries(pulse_fs_audit_test PRIVATE pulse_localization user32 shell32 shlwapi netapi32 advapi32)
add_executable(pulse_enum_benchmark_test src/bench/enum_benchmark_test.cpp)
target_link_libraries(pulse_enum_benchmark_test PRIVATE shell32 shlwapi)
add_executable(pulse_network_agent_client_test src/bench/network_agent_client_test.cpp
    src/index/network_agent_client.cpp)
target_link_libraries(pulse_network_agent_client_test PRIVATE shell32 advapi32)
target_link_libraries(pulse_directory_test PRIVATE pulse_localization user32 shell32 shlwapi netapi32)
add_executable(pulse_persistence_test src/bench/persistence_regression_test.cpp
    src/app/app_prefs.cpp src/app/places.cpp src/app/session_tab_model.cpp
    src/app/tab_group_model.cpp src/app/default_file_manager.cpp
    src/app/shell_integration_registry.cpp src/app/startup_launch.cpp
    src/app/folder_view_prefs.cpp src/app/folder_sort_prefs.cpp
    src/ui/view_layout.cpp src/fs/fs_enum.cpp src/app/entry_group.cpp src/app/tag_ads_sync.cpp)
target_link_libraries(pulse_persistence_test PRIVATE
    user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization)
add_executable(pulse_preview_handler_stale_test src/bench/preview_handler_stale_test.cpp
    src/ui/preview_handler_host.cpp)
target_compile_definitions(pulse_preview_handler_stale_test PRIVATE PULSE_PREVIEW_HANDLER_TESTING=1)
target_link_libraries(pulse_preview_handler_stale_test PRIVATE
    ole32 shell32 uuid shlwapi oleaut32 d2d1 gdi32 user32 advapi32)
target_link_libraries(pulse_preview_handler_pan_test PRIVATE gdi32 user32)

add_executable(pulse_material_test
    src/bench/window_material_test.cpp
    src/ui/window_material.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp)
target_compile_definitions(pulse_material_test PRIVATE
    PULSE_WINDOW_MATERIAL_TESTING=1)
target_link_libraries(pulse_material_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32
    comctl32 pulse_localization)
pulse_enable_lumatext(pulse_material_test)

add_executable(pulse_search_history_test
    src/bench/search_history_test.cpp
    src/app/search_history.cpp)

add_executable(pulse_app_controllers_test
    src/bench/app_controllers_test.cpp
    src/bench/localization_test.rc
    src/app/settings_controller.cpp
    src/app/pack_installer.cpp
    src/app/update_installer.cpp
    src/app/update_transport.cpp
    src/app/context_menu_controller.cpp
    src/app/session_tab_model.cpp
    src/app/tab_group_model.cpp
    src/app/single_instance_coordinator.cpp
    src/app/tray_controller.cpp
    src/app/app_prefs.cpp
    src/app/default_file_manager.cpp
    src/app/shell_integration_registry.cpp
    src/app/shell_window_plan.cpp
    src/app/entry_group.cpp
    src/app/entry_sort.cpp
    src/app/folder_view_prefs.cpp
    src/app/folder_sort_prefs.cpp
    src/app/startup_location.cpp
    src/app/context_menu_prefs.cpp
    src/app/builtin_menu_items.cpp
    src/app/address_bar_command.cpp
    src/app/last_tab_close.cpp
    src/app/startup_launch.cpp
    src/ui/view_layout.cpp
    src/index/index_client.cpp
    src/index/network_agent_client.cpp)
target_include_directories(pulse_app_controllers_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_app_controllers_test PRIVATE
    user32 shell32 ole32 uuid advapi32 shlwapi pulse_localization pulse_runtime_log bcrypt winhttp)

# Shell-only operations (recycle bin, Explorer verbs) go through pulse_shell.exe.
# Copy/move use the in-process scheduler plus CopyFile2.
# Filename search goes through Pulse.Index.exe (never Engine::Start in the UI).
if(TARGET pulse AND TARGET pulse_shell)
    add_dependencies(pulse pulse_shell)
endif()
if(TARGET pulse AND TARGET pulse_integration)
    add_dependencies(pulse pulse_integration)
endif()
if(TARGET pulse AND TARGET pulse_preview)
    add_dependencies(pulse pulse_preview)
endif()
if(TARGET pulse AND TARGET pulse_index)
    add_dependencies(pulse pulse_index)
endif()
if(TARGET pulse_ops_test AND TARGET pulse_shell)
    add_dependencies(pulse_ops_test pulse_shell)
    add_dependencies(pulse_file_picker_operations_test pulse_shell)
    add_dependencies(pulse_rename_ops_test pulse_shell)
    add_dependencies(pulse_create_rename_intent_test pulse_shell)
    add_dependencies(pulse_file_lock_test pulse_shell)
    add_dependencies(pulse_packaged_ctx_test pulse_shell)
    add_dependencies(pulse_shell_recycle_test pulse_shell)
endif()

# Headless modal lifecycle checks: no screenshots and no foreground activation.
add_executable(pulse_dialog_close_test src/bench/dialog_close_test.cpp src/ui/dialog_lifecycle.cpp)
target_compile_features(pulse_dialog_close_test PRIVATE cxx_std_20)
target_compile_options(pulse_dialog_close_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_dialog_close_test PRIVATE UNICODE _UNICODE NOMINMAX)
target_link_libraries(pulse_dialog_close_test PRIVATE dwmapi user32)

add_executable(pulse_child_edit_test
    src/bench/child_edit_test.cpp src/bench/child_edit_test.rc
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/edit_host.cpp)
target_compile_options(pulse_child_edit_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_child_edit_test PRIVATE PULSE_TEST_LUMATEXT_INIT_FAILURE=1)
target_link_options(pulse_child_edit_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_child_edit_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_child_edit_test)

# Focused folder change tracking regression tests.
add_executable(pulse_change_tracking_polling_test src/bench/change_tracking_polling_test.cpp)
target_link_libraries(pulse_change_tracking_polling_test PRIVATE user32)
add_executable(pulse_change_tracking_test src/bench/change_tracking_test.cpp src/index/change_tracking.cpp)
add_executable(pulse_change_tracking_memory_test src/bench/change_tracking_memory_test.cpp)
add_executable(pulse_change_feed_memory_test src/bench/change_feed_memory_test.cpp)
add_executable(pulse_usn_packet_queue_test src/bench/usn_packet_queue_test.cpp)
add_executable(pulse_change_tracking_client_test src/bench/change_tracking_client_test.cpp)
add_executable(pulse_global_search_hotkey_test src/bench/global_search_hotkey_test.cpp src/app/global_search_hotkey.cpp)
target_link_libraries(pulse_global_search_hotkey_test PRIVATE user32)
add_executable(pulse_frame_pump_test src/bench/frame_pump_test.cpp src/app/frame_pump.cpp)
target_link_libraries(pulse_frame_pump_test PRIVATE dwmapi user32)

# Search enhancement: native pinyin data and per-user full-text index.
# Mapping remains disabled by default. Only the short-lived task snapshot opts
# into a RAM-budgeted mapping; the 2 GiB default cap cannot cover large caches.
# Size trims: no SQL JSON functions, extensions, shared cache, column decltype
# or deprecated APIs are used anywhere.
set(PULSE_SQLITE_DEFINITIONS SQLITE_THREADSAFE=1 SQLITE_DQS=0 SQLITE_MAX_MMAP_SIZE=17179869184LL
    SQLITE_OMIT_JSON SQLITE_OMIT_LOAD_EXTENSION SQLITE_OMIT_SHARED_CACHE SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_DEPRECATED)
add_library(pulse_sqlite STATIC third_party/sqlite/sqlite3.c)
target_compile_definitions(pulse_sqlite PRIVATE SQLITE_ENABLE_FTS5 ${PULSE_SQLITE_DEFINITIONS})
target_include_directories(pulse_sqlite PUBLIC third_party/sqlite)
# pulse.exe only spools content results and reads the legacy configuration
# tables; the FTS5 index lives in Pulse.Index.exe.
add_library(pulse_sqlite_core STATIC third_party/sqlite/sqlite3.c)
target_compile_definitions(pulse_sqlite_core PRIVATE ${PULSE_SQLITE_DEFINITIONS})
target_include_directories(pulse_sqlite_core PUBLIC third_party/sqlite)
set_source_files_properties(third_party/sqlite/sqlite3.c PROPERTIES COMPILE_OPTIONS "/W0;/TC")

add_executable(pulse_filename_pinyin_test
    src/bench/filename_pinyin_test.cpp src/index/index_query.cpp src/index/search_kinds.cpp)
add_executable(pulse_index_pinyin_test
    src/bench/index_pinyin_test.cpp src/index/index_engine.cpp
    src/index/change_tracking.cpp src/index/index_visibility.cpp src/index/index_config.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/index_mft.cpp
    src/index/index_paths.cpp src/index/index_delta.cpp src/index/index_shard.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_index_pinyin_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization)
foreach(folder_size_target pulse_index pulse_index_engine_test pulse_index_pinyin_test)
    target_sources(${folder_size_target} PRIVATE src/index/index_folder_sizes.cpp)
endforeach()

foreach(search_target pulse_index pulse_content_search_test)
    target_sources(${search_target} PRIVATE src/index/content_index.cpp)
    target_link_libraries(${search_target} PRIVATE pulse_sqlite)
endforeach()
target_link_libraries(pulse PRIVATE pulse_sqlite_core)
target_sources(pulse_content_search_test PRIVATE src/index/index_query.cpp src/index/search_kinds.cpp)
get_property(search_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(search_target IN LISTS search_targets)
    get_target_property(search_sources ${search_target} SOURCES)
    if("${search_sources}" MATCHES "index_query\\.cpp" AND NOT "${search_sources}" MATCHES "filename_pinyin\\.cpp")
        target_sources(${search_target} PRIVATE src/index/filename_pinyin.cpp)
    endif()
    if("${search_sources}" MATCHES "index_engine\\.cpp")
        if(NOT "${search_sources}" MATCHES "index_engine_pinyin\\.cpp")
            target_sources(${search_target} PRIVATE src/index/index_engine_pinyin.cpp)
            target_include_directories(${search_target} PRIVATE "${CMAKE_BINARY_DIR}/generated")
        endif()
        if(NOT "${search_sources}" MATCHES "filename_timing\\.cpp")
            target_sources(${search_target} PRIVATE src/index/filename_timing.cpp)
        endif()
    endif()
endforeach()
add_executable(pulse_content_index_test src/bench/content_index_test.cpp src/bench/content_instant_test.cpp
    src/index/content_instant_session.cpp
    src/index/content_index.cpp src/index/content_search.cpp src/index/content_search_client.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_content_index_test PRIVATE pulse_sqlite pulse_text bcrypt ole32)
add_dependencies(pulse_content_index_test pulse_index)
add_executable(pulse_search_enhancement_ui_test src/bench/search_enhancement_ui_test.cpp
    src/ui/name_highlight.cpp src/app/search_query.cpp src/app/search_query_rewrite.cpp src/index/index_query.cpp
    src/index/search_kinds.cpp src/index/filename_pinyin.cpp)

add_executable(pulse_search_query_preservation_test
    src/bench/search_query_preservation_test.cpp
    src/app/search_query.cpp src/app/search_query_rewrite.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)

foreach(paged_target pulse_content_search_test pulse_content_index_test)
    if(TARGET ${paged_target})
        target_sources(${paged_target} PRIVATE src/index/content_result_store.cpp)
        target_link_libraries(${paged_target} PRIVATE pulse_sqlite)
    endif()
endforeach()

add_executable(pulse_content_paging_test src/bench/content_paging_test.cpp
    src/index/content_result_store.cpp src/index/content_index.cpp src/index/content_search.cpp
    src/index/content_search_client.cpp src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_content_paging_test PRIVATE pulse_sqlite pulse_text bcrypt ole32)
add_dependencies(pulse_content_paging_test pulse_index)

foreach(content_client_target pulse pulse_content_search_test pulse_content_index_test pulse_content_paging_test)
    target_sources(${content_client_target} PRIVATE src/index/content_search_session.cpp)
endforeach()

# Document parsers never load into the UI or the persistent index writer.
add_library(pulse_document_reader STATIC src/index/document_reader.cpp)
target_link_libraries(pulse_document_reader PUBLIC pulse_text shell32 ole32 pulse_runtime_log)
add_executable(pulse_document src/index/document_host_main.cpp
    src/index/document_filter.cpp src/index/office_ooxml.cpp src/index/pdfium_text.cpp)
add_custom_command(TARGET pulse_document POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/bin/pdfium.dll" "$<TARGET_FILE_DIR:pulse_document>/pdfium.dll"
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/licenses" "$<TARGET_FILE_DIR:pulse_document>/licenses/PDFium"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/third_party/pdfium/LICENSE" "$<TARGET_FILE_DIR:pulse_document>/licenses/PDFium/binary-distribution.txt")
set_target_properties(pulse_document PROPERTIES OUTPUT_NAME "Pulse.Document")
target_link_libraries(pulse_document PRIVATE ole32 uuid xmllite shlwapi advapi32)
add_executable(pulse_pdf_bench src/bench/pdf_bench.cpp)
add_executable(pulse_document_admission_test src/bench/document_admission_test.cpp)
add_executable(pulse_document_failure_fixture src/bench/document_failure_host.cpp)
add_executable(pulse_document_pool_bench src/bench/document_pool_bench.cpp)
target_link_libraries(pulse_document_pool_bench PRIVATE pulse_document_reader psapi)
add_dependencies(pulse_document_pool_bench pulse_document)
target_link_libraries(pulse_pdf_bench PRIVATE pulse_document_reader ole32 psapi)
add_dependencies(pulse_pdf_bench pulse_document)
pulse_add_version_resource(pulse_document "Pulse document text extractor" "Pulse.Document" "Pulse.Document.exe")
foreach(document_target pulse pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test)
    target_link_libraries(${document_target} PRIVATE pulse_document_reader)
    add_dependencies(${document_target} pulse_document)
endforeach()
add_executable(pulse_office_ooxml_test src/bench/office_ooxml_test.cpp src/index/office_ooxml.cpp)
target_include_directories(pulse_office_ooxml_test PRIVATE src)
target_link_libraries(pulse_office_ooxml_test PRIVATE ole32 uuid xmllite)
add_executable(pulse_office_index_test src/bench/office_index_test.cpp
    src/index/content_index.cpp src/index/content_search.cpp src/index/content_result_store.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_office_index_test PRIVATE pulse_document_reader pulse_sqlite ole32 uuid bcrypt psapi)
add_dependencies(pulse_office_index_test pulse_document)
add_library(pulse_content_task STATIC src/index/content_task_search.cpp src/index/text_task_reader.cpp src/index/content_timing.cpp)
target_link_libraries(pulse_content_task PUBLIC pulse_document_reader pulse_text)
add_executable(pulse_content_timing_test src/bench/content_timing_test.cpp src/index/content_timing.cpp)
add_executable(pulse_text_task_test src/bench/text_task_reader_test.cpp src/index/text_task_reader.cpp)
target_link_libraries(pulse_text_task_test PRIVATE pulse_text)
foreach(task_target pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test pulse_office_index_test)
    target_link_libraries(${task_target} PRIVATE pulse_content_task)
endforeach()

add_library(pulse_fs_watch STATIC src/fs/fs_watch.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_fs_watch PUBLIC shell32 shlwapi netapi32 pulse_localization)
foreach(watch_target pulse pulse_index pulse_content_search_test pulse_content_index_test pulse_content_paging_test pulse_office_index_test)
    target_link_libraries(${watch_target} PRIVATE pulse_fs_watch)
endforeach()

# Configuration storage is shared without linking the body extraction engine into the UI.
get_property(configuration_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(configuration_target IN LISTS configuration_targets)
    get_target_property(configuration_sources ${configuration_target} SOURCES)
    if("${configuration_sources}" MATCHES "content_(index|search_client)\\.cpp" AND NOT "${configuration_sources}" MATCHES "content_configuration\\.cpp")
        target_sources(${configuration_target} PRIVATE src/index/content_configuration.cpp)
    endif()
endforeach()

# Opt-in integration fixture: real view model/renderer, isolated preferences, no installer execution.
option(PULSE_BUILD_PROGRESS_UI_TESTS "Build the content/update status-bar renderer fixture" OFF)

# Focused cross-volume WAL replay regression, sharing the engine fixture dependencies.
get_target_property(delta_replay_sources pulse_index_engine_test SOURCES)
list(REMOVE_ITEM delta_replay_sources src/bench/index_engine_test.cpp)
get_target_property(delta_replay_libraries pulse_index_engine_test LINK_LIBRARIES)
add_executable(pulse_index_delta_replay_test src/bench/index_delta_replay_test.cpp ${delta_replay_sources})
target_link_libraries(pulse_index_delta_replay_test PRIVATE ${delta_replay_libraries})
foreach(delta_replay_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(delta_replay_value pulse_index_engine_test ${delta_replay_property})
    if(delta_replay_value)
        set_property(TARGET pulse_index_delta_replay_test PROPERTY ${delta_replay_property} "${delta_replay_value}")
    endif()
endforeach()

add_executable(pulse_runtime_log_test src/bench/runtime_log_test.cpp)
add_executable(pulse_elevated_transfer_test src/bench/elevated_transfer_test.cpp src/ops/elevated_transfer.cpp)
target_link_libraries(pulse_elevated_transfer_test PRIVATE ole32 shell32 uuid advapi32)
add_executable(pulse_preview_resource_probe src/bench/preview_resource_probe.cpp)
target_link_libraries(pulse_preview_resource_probe PRIVATE psapi)
add_executable(pulse_pdf_budget_test src/bench/pdf_budget_test.cpp src/preview_host/pdf_raster.cpp)
target_link_libraries(pulse_pdf_budget_test PRIVATE psapi)
target_link_libraries(pulse_runtime_log_test PRIVATE pulse_crash_reporter)
add_executable(pulse_diagnostics_export_test src/bench/diagnostics_export_test.cpp)
target_link_libraries(pulse_diagnostics_export_test PRIVATE pulse_crash_reporter)
# Standalone fixtures compile these instrumented modules directly.
get_property(runtime_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(runtime_target IN LISTS runtime_targets)
    get_target_property(runtime_sources ${runtime_target} SOURCES)
    if("${runtime_sources}" MATCHES "ops_manager\\.cpp")
        if(NOT "${runtime_sources}" MATCHES "elevated_transfer\\.cpp")
            target_sources(${runtime_target} PRIVATE src/ops/elevated_transfer.cpp)
        endif()
        if(NOT runtime_target MATCHES "^pulse_elevated(_test_host)?$" AND NOT "${runtime_sources}" MATCHES "ops_manager_shell_transfer\\.cpp")
            target_sources(${runtime_target} PRIVATE src/ops/ops_manager_shell_transfer.cpp)
        endif()
        if(NOT runtime_target MATCHES "^pulse_elevated(_test_host)?$")
            foreach(permission_source ops_manager_authorization ops_manager_authorized_delete elevated_delete)
                if(NOT "${runtime_sources}" MATCHES "${permission_source}\\.cpp")
                    target_sources(${runtime_target} PRIVATE src/ops/${permission_source}.cpp)
                endif()
            endforeach()
            if(NOT "${runtime_sources}" MATCHES "elevated_transfer_client\\.cpp")
                target_sources(${runtime_target} PRIVATE src/ops/elevated_transfer_client.cpp)
            endif()
            if(NOT "${runtime_sources}" MATCHES "elevated_transfer_transport\\.cpp")
                target_sources(${runtime_target} PRIVATE src/ipc/elevated_transfer_transport.cpp)
            endif()
            target_link_libraries(${runtime_target} PRIVATE advapi32 bcrypt)
        endif()
    endif()
    if("${runtime_sources}" MATCHES "ops_manager\\.cpp" AND NOT "${runtime_sources}" MATCHES "shell_command\\.cpp")
        target_sources(${runtime_target} PRIVATE src/ops/shell_command.cpp)
    endif()
    if("${runtime_sources}" MATCHES "(index_engine|index_delta|filename_timing|update_checker|update_installer|app_worker|app_prefs|ops_manager)\\.cpp")
        target_link_libraries(${runtime_target} PRIVATE pulse_runtime_log)
    endif()
endforeach()

add_executable(pulse_shell_command_test src/bench/shell_command_test.cpp src/ops/shell_command.cpp)
target_link_libraries(pulse_shell_command_test PRIVATE shell32 user32)
add_executable(pulse_reparse_entry_test src/bench/reparse_entry_test.cpp)
target_link_libraries(pulse_reparse_entry_test PRIVATE pulse_localization user32 shell32 shlwapi netapi32)

add_executable(pulse_link_destination_test src/bench/link_destination_test.cpp src/app/link_resolve.cpp src/fs/fs_enum.cpp
    src/app/snapshot_patch.cpp src/app/entry_order_hold.cpp src/app/entry_group.cpp src/app/entry_sort.cpp)
target_link_libraries(pulse_link_destination_test PRIVATE pulse_localization shell32 shlwapi netapi32 ole32 uuid)
add_executable(pulse_entry_sort_key_test src/bench/entry_sort_key_test.cpp src/app/entry_group.cpp src/app/entry_sort.cpp)
target_link_libraries(pulse_entry_sort_key_test PRIVATE pulse_localization shlwapi)
add_executable(pulse_link_pill_test src/bench/link_pill_test.cpp)
target_link_libraries(pulse_link_pill_test PRIVATE user32)

add_executable(pulse_network_locations_test src/bench/network_locations_test.cpp src/app/network_locations.cpp)
target_link_libraries(pulse_network_locations_test PRIVATE shell32 ole32 uuid)

add_executable(pulse_archive_integrity_test src/bench/archive_integrity_test.cpp
    src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp)
add_dependencies(pulse_archive_integrity_test pulse_preview)
add_executable(pulse_document_completeness_test src/bench/document_completeness_test.cpp
    src/preview_host/table_document.cpp src/preview_host/epub_document.cpp
    src/preview_host/docx_document.cpp src/preview_host/doc_payload.cpp src/preview_host/zip_entry.cpp)
add_dependencies(pulse_document_completeness_test pulse_preview)
add_executable(pulse_preview_command_test src/bench/preview_command_test.cpp)
add_executable(pulse_selection_input_test src/bench/selection_input_test.cpp)
add_executable(pulse_sidebar_refresh_test src/bench/sidebar_refresh_test.cpp)
add_executable(pulse_folder_thumbnail_test src/bench/folder_thumbnail_test.cpp src/preview_host/folder_thumbnail.cpp)
add_executable(pulse_index_writer_idle_test src/bench/index_writer_idle_test.cpp)
target_link_libraries(pulse_index_writer_idle_test PRIVATE user32 shell32 advapi32)
set(PULSE_FOLDER_SIZE_SOURCES src/app/folder_sizes.cpp src/app/folder_size_scanner.cpp src/app/folder_size_store.cpp)
add_executable(pulse_folder_sizes_experience_test src/bench/folder_sizes_experience_test.cpp ${PULSE_FOLDER_SIZE_SOURCES})
target_link_libraries(pulse_folder_sizes_experience_test PRIVATE pulse_fs_watch)
add_executable(pulse_folder_sizes_perf_test src/bench/folder_sizes_perf_test.cpp ${PULSE_FOLDER_SIZE_SOURCES})
target_link_libraries(pulse_folder_sizes_perf_test PRIVATE pulse_fs_watch)
add_executable(pulse_folder_sizes_navigation_test src/bench/folder_sizes_navigation_perf.cpp ${PULSE_FOLDER_SIZE_SOURCES})
target_link_libraries(pulse_folder_sizes_navigation_test PRIVATE pulse_fs_watch)
add_executable(pulse_folder_sizes_refactor_test src/bench/folder_sizes_refactor_test.cpp
    src/app/folder_sizes_index_test.cpp ${PULSE_FOLDER_SIZE_SOURCES})
target_compile_definitions(pulse_folder_sizes_refactor_test PRIVATE PULSE_WITH_SELFTEST)
target_link_libraries(pulse_folder_sizes_refactor_test PRIVATE pulse_fs_watch)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench_data/folder_size_refactor_baseline/folder_sizes.cpp")
    add_executable(pulse_folder_sizes_pre_refactor
        bench_data/folder_size_refactor_baseline/folder_sizes_perf_test.cpp
        bench_data/folder_size_refactor_baseline/folder_sizes.cpp)
    target_include_directories(pulse_folder_sizes_pre_refactor PRIVATE src/app)
    target_link_libraries(pulse_folder_sizes_pre_refactor PRIVATE pulse_fs_watch)
    add_executable(pulse_folder_sizes_navigation_before src/bench/folder_sizes_navigation_perf.cpp
        bench_data/folder_size_refactor_baseline/folder_sizes.cpp)
    target_include_directories(pulse_folder_sizes_navigation_before PRIVATE src/app)
    target_compile_definitions(pulse_folder_sizes_navigation_before PRIVATE PULSE_FOLDER_SIZE_REFACTOR_BASELINE)
    target_link_libraries(pulse_folder_sizes_navigation_before PRIVATE pulse_fs_watch)
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/bench_data/folder_sizes_baseline.cpp")
    add_executable(pulse_folder_sizes_baseline_test src/bench/folder_sizes_perf_test.cpp bench_data/folder_sizes_baseline.cpp)
    target_include_directories(pulse_folder_sizes_baseline_test PRIVATE src/app)
    target_compile_definitions(pulse_folder_sizes_baseline_test PRIVATE PULSE_FOLDER_SIZE_BASELINE)
    target_link_libraries(pulse_folder_sizes_baseline_test PRIVATE pulse_fs_watch)
endif()
target_compile_definitions(pulse_folder_thumbnail_test PRIVATE PULSE_FOLDER_THUMBNAIL_TESTING)
target_link_libraries(pulse_folder_thumbnail_test PRIVATE ole32 shell32 windowscodecs gdiplus gdi32 uuid)
add_executable(pulse_folder_thumbnail_ipc_test src/bench/folder_thumbnail_ipc_test.cpp)
target_link_libraries(pulse_folder_thumbnail_ipc_test PRIVATE ole32 windowscodecs uuid)
add_dependencies(pulse_folder_thumbnail_ipc_test pulse_preview)
add_executable(pulse_folder_thumbnail_cache_test src/bench/folder_thumbnail_cache_test.cpp
    src/ui/thumbnail_cache.cpp src/ui/folder_thumbnail_cache.cpp)
target_link_libraries(pulse_folder_thumbnail_cache_test PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs ole32 shell32 shlwapi uuid)
add_executable(pulse_thumbnail_cache_unit_test src/bench/thumbnail_cache_unit_main.cpp
    src/bench/thumbnail_cache_test.cpp src/ui/thumbnail_cache.cpp)
target_link_libraries(pulse_thumbnail_cache_unit_test PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs ole32 shell32 shlwapi uuid)
target_link_libraries(pulse_preview_command_test PRIVATE pulse_localization)
add_executable(pulse_image_preview_integrity_test src/bench/image_preview_integrity_test.cpp
    src/preview_host/image_frames.cpp src/preview_host/svg_raster.cpp)
target_link_libraries(pulse_image_preview_integrity_test PRIVATE ole32 windowscodecs uuid)
add_dependencies(pulse_image_preview_integrity_test pulse_preview)
add_executable(pulse_preview_integrity_ui_test src/bench/preview_integrity_ui_test.cpp
    src/bench/preview_integrity_strings.rc
    src/ui/archive_preview.cpp src/ui/table_view.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_link_libraries(pulse_preview_integrity_ui_test PRIVATE dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_preview_integrity_ui_test)

add_executable(pulse_open_with_icons_test src/bench/open_with_icons_test.cpp src/ui/open_with_icons.cpp)
target_link_libraries(pulse_open_with_icons_test PRIVATE ole32 shell32 shlwapi uuid)

add_executable(pulse_udf_listing_test src/bench/udf_listing_test.cpp src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp)
add_dependencies(pulse_udf_listing_test pulse_preview)
add_executable(pulse_preview_integrity_test src/bench/preview_integrity_test.cpp)
# Focused regressions for the October review; no full-suite dependency.
add_executable(pulse_audit_persistence_test src/bench/audit_persistence_test.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp src/index/index_config.cpp)
target_link_libraries(pulse_audit_persistence_test PRIVATE user32 shell32 ole32 advapi32 uuid shlwapi pulse_localization)

add_executable(pulse_places_persistence_audit_test src/bench/places_persistence_audit_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp)
target_link_libraries(pulse_places_persistence_audit_test PRIVATE pulse_localization pulse_runtime_log user32 shell32 ole32 uuid advapi32 shlwapi netapi32 psapi)

add_executable(pulse_mft_completion_test src/bench/mft_completion_test.cpp src/index/index_mft.cpp)
add_executable(pulse_usn_enum_page_test src/bench/usn_enum_page_test.cpp)

add_executable(pulse_preview_format_audit_test src/bench/preview_format_audit_test.cpp
    src/preview_host/psd_raster.cpp src/preview_host/office_doc_model.cpp
    src/preview_host/table_document.cpp src/preview_host/notebook_document.cpp
    src/preview_host/archive_listing.cpp src/preview_host/udf_listing.cpp
    src/preview_host/zip_entry.cpp src/preview_host/markdown_document.cpp
    src/preview_host/doc_payload.cpp src/preview_host/office_model.cpp
    src/preview_host/gif_frames.cpp src/preview_host/preview_file_utils.cpp third_party/md4c/md4c.c)
target_compile_definitions(pulse_preview_format_audit_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_preview_format_audit_test PRIVATE ole32 windowscodecs uuid shell32 shlwapi advapi32)
target_sources(pulse_preview PRIVATE src/preview_host/gif_frames.cpp)

add_executable(pulse_preview_pipeline_audit_test src/bench/preview_pipeline_audit_test.cpp
    src/preview_host/gif_frames.cpp src/preview_host/folder_listing.cpp src/preview_host/preview_file_utils.cpp)
target_link_libraries(pulse_preview_pipeline_audit_test PRIVATE ole32 windowscodecs uuid shlwapi)
add_executable(pulse_tray_metadata_audit_test src/bench/tray_metadata_audit_test.cpp)
add_executable(pulse_network_worker_audit_test src/bench/network_worker_audit_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/index_shard.cpp src/index/change_tracking.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_network_worker_audit_test PRIVATE shell32 mpr advapi32 pulse_runtime_log)
add_executable(pulse_pdf_unicode_test src/bench/pdf_unicode_test.cpp src/index/pdfium_text.cpp)

get_target_property(transfer_audit_sources pulse_ops_test SOURCES)
list(FILTER transfer_audit_sources EXCLUDE REGEX "ops_test_main.cpp$")
get_target_property(transfer_audit_libraries pulse_ops_test LINK_LIBRARIES)
add_executable(pulse_transfer_audit_test ${transfer_audit_sources} src/bench/transfer_audit_test.cpp)
target_link_libraries(pulse_transfer_audit_test PRIVATE ${transfer_audit_libraries} advapi32)
target_compile_definitions(pulse_transfer_audit_test PRIVATE PULSE_ELEVATED_TEST_CLIENT=1)
add_dependencies(pulse_transfer_audit_test pulse_elevated_test_host)

add_executable(pulse_typography_policy_test src/bench/typography_policy_test.cpp src/bench/child_edit_test.rc
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/edit_host.cpp
    src/ui/fluent_components.cpp src/ui/fluent_menu.cpp src/ui/tab_shape.cpp)
target_compile_definitions(pulse_typography_policy_test PRIVATE PULSE_TEST_LUMATEXT_PRESENT_FAILURE=1)
target_link_options(pulse_typography_policy_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_typography_policy_test PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi
    shcore ole32 windowscodecs user32 gdi32 comctl32 uxtheme shell32 pulse_localization)
pulse_enable_lumatext(pulse_typography_policy_test)

# Context-menu access keys: pure matching rules plus the real Fluent modal loop.
add_executable(pulse_menu_mnemonic_test src/bench/menu_mnemonic_test.cpp src/bench/child_edit_test.rc
    src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/edit_host.cpp
    src/ui/fluent_components.cpp src/ui/fluent_menu.cpp src/ui/tab_shape.cpp)
target_include_directories(pulse_menu_mnemonic_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_options(pulse_menu_mnemonic_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_menu_mnemonic_test PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi
    shcore ole32 windowscodecs user32 gdi32 comctl32 uxtheme shell32 imm32 pulse_localization)
pulse_enable_lumatext(pulse_menu_mnemonic_test)

get_target_property(audit_content_sources pulse_content_index_test SOURCES)
list(REMOVE_ITEM audit_content_sources src/bench/content_index_test.cpp src/bench/content_instant_test.cpp)
get_target_property(audit_content_libraries pulse_content_index_test LINK_LIBRARIES)
add_executable(pulse_search_audit_test ${audit_content_sources} src/bench/search_audit_test.cpp
    src/app/search_query.cpp src/app/global_search_handoff_query.cpp src/index/network_index.cpp
    src/index/index_config.cpp src/index/index_shard.cpp src/index/change_tracking.cpp)
target_link_libraries(pulse_search_audit_test PRIVATE ${audit_content_libraries} shell32 mpr advapi32)
target_sources(pulse PRIVATE src/app/global_search_handoff_query.cpp)
add_executable(pulse_audit_preview_test src/bench/audit_preview_test.cpp
    src/bench/thumbnail_cache_test.cpp src/ui/thumbnail_cache.cpp
    src/preview_host/preview_properties.cpp src/preview_host/preview_file_utils.cpp
    src/preview_host/video_codec.cpp src/preview_host/media_pack.cpp
    src/preview_host/image_pack.cpp src/preview_host/raw_pack.cpp src/common/ffmpeg_tool.cpp)
target_link_libraries(pulse_audit_preview_test PRIVATE pulse_runtime_log ole32 propsys shell32 shlwapi advapi32 uuid
    gdi32 d3d11 d2d1 dxgi)

add_executable(pulse_quicklook_catalog_test src/bench/quicklook_catalog_test.cpp
    src/ui/preview_format_catalog.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_link_libraries(pulse_quicklook_catalog_test PRIVATE pulse_localization d3d11 d2d1 dwrite
    dxgi dcomp dwmapi shcore ole32 windowscodecs user32 gdi32 shell32 advapi32 comctl32)
pulse_enable_lumatext(pulse_quicklook_catalog_test)
add_executable(pulse_diagnostics_retention_test src/bench/diagnostics_retention_test.cpp)
target_link_libraries(pulse_diagnostics_retention_test PRIVATE pulse_crash_reporter)
add_executable(pulse_net_cache_audit_test src/bench/net_cache_audit_test.cpp src/fs/fs_net_cache.cpp)
target_compile_definitions(pulse_net_cache_audit_test PRIVATE PULSE_TEST_NET_CACHE=1)
target_link_libraries(pulse_net_cache_audit_test PRIVATE pulse_fs_watch shell32 user32)
add_executable(pulse_common_persistence_test src/bench/common_persistence_test.cpp src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_common_persistence_test PRIVATE pulse_localization pulse_runtime_log user32 shell32 ole32 uuid advapi32 shlwapi netapi32 psapi)

if(PULSE_RELEASE_SYMBOLS)
    foreach(payload_target pulse pulse_index pulse_document pulse_preview pulse_shell pulse_integration pulse_elevated)
        target_compile_options(${payload_target} PRIVATE /Z7)
        target_link_options(${payload_target} PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
    endforeach()
endif()

add_executable(pulse_deadline_pipe_test src/bench/deadline_pipe_test.cpp)
add_executable(pulse_shell_client_generation_test src/bench/shell_client_generation_test.cpp)
get_target_property(shell_lifetime_sources pulse_shell SOURCES)
list(FILTER shell_lifetime_sources EXCLUDE REGEX "src/shell_host/main.cpp$")
get_target_property(shell_lifetime_libraries pulse_shell LINK_LIBRARIES)
add_executable(pulse_shell_context_lifetime_audit_test ${shell_lifetime_sources} src/bench/shell_context_lifetime_audit_test.cpp)
target_link_libraries(pulse_shell_context_lifetime_audit_test PRIVATE ${shell_lifetime_libraries})
target_include_directories(pulse_shell_context_lifetime_audit_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
add_executable(pulse_folder_budget_audit_test src/bench/folder_budget_audit_test.cpp)
target_link_libraries(pulse_folder_budget_audit_test PRIVATE shlwapi)
add_executable(pulse_shell_registry_retry_test src/bench/shell_registry_retry_test.cpp
    src/app/shell_window_registry.cpp src/app/shell_window_plan.cpp)
target_link_libraries(pulse_shell_registry_retry_test PRIVATE ole32 oleaut32 shell32 shlwapi propsys uuid user32)
add_executable(pulse_shell_tag_registry_test src/bench/shell_tag_registry_test.cpp src/app/shell_tag_registry.cpp src/app/shell_tag_protocol.cpp)
target_link_libraries(pulse_shell_tag_registry_test PRIVATE advapi32 shell32 shlwapi ole32)
add_executable(pulse_shell_tag_com_test
    src/bench/shell_tag_com_test.cpp src/app/shell_tag_com.cpp
    src/app/shell_tag_protocol.cpp src/app/shell_tag_batch.cpp
    src/app/shell_tag_registry.cpp src/app/places.cpp src/app/tag_ads_sync.cpp
    src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_shell_tag_com_test PRIVATE pulse_localization pulse_runtime_log
    user32 shell32 ole32 uuid advapi32 shlwapi netapi32 psapi)
add_executable(pulse_single_instance_audit_test src/bench/single_instance_audit_test.cpp src/app/single_instance_coordinator.cpp)
target_link_libraries(pulse_single_instance_audit_test PRIVATE ole32 user32)
add_executable(pulse_preview_pan_lifetime_audit_test src/bench/preview_pan_lifetime_audit_test.cpp)
target_link_libraries(pulse_preview_pan_lifetime_audit_test PRIVATE gdi32 user32)
add_executable(pulse_markdown_metadata_audit_test src/bench/markdown_metadata_audit_test.cpp src/ui/markdown_image_metadata.cpp)
target_link_libraries(pulse_markdown_metadata_audit_test PRIVATE user32)
add_executable(pulse_update_stage_audit_test src/bench/update_stage_audit_test.cpp src/app/update_stage.cpp)
target_link_libraries(pulse_update_stage_audit_test PRIVATE shell32 bcrypt)
get_target_property(preview_view_sources pulse_math_test SOURCES)
list(FILTER preview_view_sources EXCLUDE REGEX "src/bench/|lumatext_renderer.cpp$")
get_target_property(preview_view_libraries pulse_math_test LINK_LIBRARIES)
add_executable(pulse_preview_view_audit_test ${preview_view_sources} src/bench/preview_view_audit_test.cpp
    src/ui/tree_view.cpp src/ui/table_view.cpp src/preview_host/table_document.cpp src/preview_host/zip_entry.cpp
    src/preview_host/tree_document.cpp src/preview_host/preview_file_utils.cpp)
target_compile_definitions(pulse_preview_view_audit_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_preview_view_audit_test PRIVATE ${preview_view_libraries} xmllite)
pulse_enable_lumatext(pulse_preview_view_audit_test)
get_target_property(waveform_audit_sources pulse_ffmpeg_playback_test SOURCES)
list(FILTER waveform_audit_sources EXCLUDE REGEX "ffmpeg_playback_test.cpp$")
get_target_property(waveform_audit_libraries pulse_ffmpeg_playback_test LINK_LIBRARIES)
add_executable(pulse_waveform_cancel_audit_test ${waveform_audit_sources} src/bench/waveform_cancel_audit_test.cpp)
target_link_libraries(pulse_waveform_cancel_audit_test PRIVATE ${waveform_audit_libraries})
add_executable(pulse_global_search_state_test src/bench/global_search_state_test.cpp)
add_executable(pulse_network_agent_deadline_test src/bench/network_agent_deadline_test.cpp)
target_link_libraries(pulse_network_agent_deadline_test PRIVATE advapi32)
add_dependencies(pulse_network_agent_deadline_test pulse_index)
get_target_property(preview_ack_sources pulse_preview SOURCES)
list(FILTER preview_ack_sources EXCLUDE REGEX "src/preview_host/main.cpp$")
get_target_property(preview_ack_libraries pulse_preview LINK_LIBRARIES)
add_executable(pulse_preview_ack_audit_test ${preview_ack_sources} src/bench/preview_ack_audit_test.cpp)
target_compile_definitions(pulse_preview_ack_audit_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_preview_ack_audit_test PRIVATE ${preview_ack_libraries})
target_include_directories(pulse_preview_ack_audit_test PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")
get_property(audit_dependency_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(audit_target IN LISTS audit_dependency_targets)
    get_target_property(audit_sources ${audit_target} SOURCES)
    if("${audit_sources}" MATCHES "markdown_view\\.cpp")
        target_sources(${audit_target} PRIVATE src/ui/markdown_image_metadata.cpp)
    endif()
    if("${audit_sources}" MATCHES "update_installer\\.cpp" AND NOT "${audit_sources}" MATCHES "update_stage\\.cpp")
        target_sources(${audit_target} PRIVATE src/app/update_stage.cpp)
    endif()
endforeach()

# Focused review P1 security contract tests; no service or user configuration changes.
add_executable(pulse_review_security_test src/bench/review_security_test.cpp
    src/index/index_config.cpp src/index/index_migration.cpp)
target_link_libraries(pulse_review_security_test PRIVATE shell32 advapi32)

# Isolated Shell startup regression: stub never invokes Shell verbs or file operations.
add_executable(pulse_shell_reconnect_stub src/bench/shell_reconnect_stub.cpp)
add_executable(pulse_shell_reconnect_test src/bench/shell_reconnect_test.cpp src/ipc/shell_client.cpp)
target_link_libraries(pulse_shell_reconnect_test PRIVATE pulse_localization shell32 advapi32)
add_dependencies(pulse_shell_reconnect_test pulse_shell_reconnect_stub)
target_compile_definitions(pulse_shell_reconnect_test PRIVATE PULSE_SHELL_RECONNECT_TEST_CLIENT=1)
# Exercise the real host worker-acquisition and join code with controlled OS
# allocation failures; never run the host entry point or installed providers.
set(PULSE_WORKER_TEST_SOURCES ${PULSE_SHELL_SOURCES})
list(REMOVE_ITEM PULSE_WORKER_TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/shell_host/main.cpp")
add_executable(pulse_shell_worker_lifecycle_test src/bench/shell_worker_lifecycle_test.cpp
    ${PULSE_WORKER_TEST_SOURCES})
target_link_libraries(pulse_shell_worker_lifecycle_test PRIVATE ole32 shell32 uuid advapi32
    shlwapi version pulse_crash_reporter)
# Real metadata readers and the validated-payload restore step, tested only on
# privately created $I/$R pairs; never use the user's real Recycle Bin.
add_executable(pulse_recycle_metadata_test src/bench/recycle_metadata_test.cpp
    src/fs/fs_recycle.cpp src/fs/fs_enum.cpp ${PULSE_WORKER_TEST_SOURCES})
target_link_libraries(pulse_recycle_metadata_test PRIVATE ole32 shell32 uuid advapi32
    shlwapi version netapi32 pulse_crash_reporter pulse_localization)
target_compile_definitions(pulse_recycle_metadata_test PRIVATE PULSE_RECYCLE_METADATA_TEST=1)
# Ordinary Win32 path semantics and owned-directory enumeration only.
add_executable(pulse_path_normalize_test src/bench/path_normalize_test.cpp src/fs/fs_enum.cpp
    src/fs/fs_snapshot.cpp)
target_link_libraries(pulse_path_normalize_test PRIVATE netapi32 shlwapi pulse_localization)

add_executable(pulse_unc_probe_sync_test src/bench/unc_probe_sync_test.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_unc_probe_sync_test PRIVATE netapi32 shlwapi pulse_localization)

add_executable(pulse_net_cache_commit_test src/bench/net_cache_commit_test.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_net_cache_commit_test PRIVATE netapi32 shlwapi pulse_localization)

add_executable(pulse_watch_retry_test src/bench/watch_retry_test.cpp src/fs/fs_enum.cpp)
target_link_libraries(pulse_watch_retry_test PRIVATE netapi32 shlwapi pulse_localization)

add_executable(pulse_unc_probe_order_test src/bench/unc_probe_order_test.cpp)

add_executable(pulse_enum_cancel_test src/bench/enum_cancel_test.cpp)
target_link_libraries(pulse_enum_cancel_test PRIVATE pulse_localization shell32 shlwapi netapi32)

add_executable(pulse_worker_enum_cancel_test src/bench/worker_enum_cancel_test.cpp)

add_executable(pulse_shell_benchmark_test src/bench/shell_benchmark_test.cpp)
target_link_libraries(pulse_shell_benchmark_test PRIVATE ole32 shell32 uuid comctl32)

get_target_property(query_narrow_sources pulse_index_pinyin_test SOURCES)
list(REMOVE_ITEM query_narrow_sources src/bench/index_pinyin_test.cpp)
add_executable(pulse_query_narrow_test src/bench/query_narrow_test.cpp ${query_narrow_sources})
target_include_directories(pulse_query_narrow_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_query_narrow_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization)
target_link_libraries(pulse_query_narrow_test PRIVATE pulse_runtime_log)

add_executable(pulse_index_scope_move_test src/bench/index_scope_move_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_scope_move_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_scope_move_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_walk_wal_test src/bench/index_walk_wal_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_walk_wal_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_walk_wal_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_mft_read_test src/bench/index_mft_read_test.cpp)

add_executable(pulse_index_prefix_bounds_test src/bench/index_prefix_bounds_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_prefix_bounds_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_prefix_bounds_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_patch_unique_test src/bench/index_patch_unique_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_patch_unique_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_patch_unique_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_deep_path_test src/bench/index_deep_path_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_deep_path_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_deep_path_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_host_backpressure_test src/bench/index_host_backpressure_test.cpp)
add_dependencies(pulse_index_host_backpressure_test pulse_index)

add_executable(pulse_mft_enum_page_test src/bench/mft_enum_page_test.cpp)

add_executable(pulse_network_agent_listener_test src/bench/network_agent_listener_test.cpp)
target_link_libraries(pulse_network_agent_listener_test PRIVATE advapi32)

add_executable(pulse_network_walk_failure_test src/bench/network_walk_failure_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp src/index/change_tracking.cpp)
target_link_libraries(pulse_network_walk_failure_test PRIVATE shell32 mpr advapi32 shlwapi pulse_localization pulse_runtime_log)
target_sources(pulse_network_walk_failure_test PRIVATE src/index/filename_pinyin.cpp)

add_dependencies(pulse_pdf_unicode_test pulse_document)

add_executable(pulse_network_overlay_race_test src/bench/network_overlay_race_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/change_tracking.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_network_overlay_race_test PRIVATE shell32 mpr advapi32 shlwapi pulse_localization pulse_runtime_log)
target_link_libraries(pulse_pdf_unicode_test PRIVATE pulse_document_reader)

get_target_property(content_saved_sources pulse_content_paging_test SOURCES)
get_target_property(content_saved_libraries pulse_content_paging_test LINK_LIBRARIES)
list(REMOVE_ITEM content_saved_sources src/bench/content_paging_test.cpp)
add_executable(pulse_content_saved_sort_test src/bench/content_saved_sort_test.cpp ${content_saved_sources})
target_link_libraries(pulse_content_saved_sort_test PRIVATE ${content_saved_libraries})

add_executable(pulse_network_config_integrity_test src/bench/network_config_integrity_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/change_tracking.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_network_config_integrity_test PRIVATE shell32 mpr advapi32 shlwapi pulse_localization pulse_runtime_log)

add_executable(pulse_content_enumeration_test src/bench/content_enumeration_test.cpp
    src/index/content_search.cpp src/index/content_task_search.cpp src/index/text_task_reader.cpp
    src/index/content_timing.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/filename_pinyin.cpp)
target_compile_definitions(pulse_content_enumeration_test PRIVATE PULSE_CONTENT_ENUMERATION_TEST)
target_link_libraries(pulse_content_enumeration_test PRIVATE pulse_document_reader pulse_text bcrypt ole32)

get_target_property(content_scope_sources pulse_content_enumeration_test SOURCES)
list(REMOVE_ITEM content_scope_sources src/bench/content_enumeration_test.cpp)
add_executable(pulse_content_scope_enumeration_test src/bench/content_scope_enumeration_test.cpp ${content_scope_sources})
target_compile_definitions(pulse_content_scope_enumeration_test PRIVATE PULSE_CONTENT_ENUMERATION_TEST)
target_link_libraries(pulse_content_scope_enumeration_test PRIVATE pulse_document_reader pulse_text bcrypt ole32)

add_executable(pulse_network_overlap_query_test src/bench/network_overlap_query_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/change_tracking.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_network_overlap_query_test PRIVATE shell32 mpr advapi32 shlwapi pulse_localization pulse_runtime_log)

add_executable(pulse_network_live_topk_test src/bench/network_live_topk_test.cpp
    src/index/index_config.cpp src/index/index_query.cpp src/index/search_kinds.cpp
    src/index/change_tracking.cpp src/index/filename_pinyin.cpp)
target_link_libraries(pulse_network_live_topk_test PRIVATE shell32 mpr advapi32 shlwapi pulse_localization pulse_runtime_log)

get_target_property(content_reconcile_sources pulse_content_index_test SOURCES)
get_target_property(content_reconcile_libraries pulse_content_index_test LINK_LIBRARIES)
list(REMOVE_ITEM content_reconcile_sources src/bench/content_index_test.cpp src/bench/content_instant_test.cpp)
add_executable(pulse_content_reconcile_retry_test src/bench/content_reconcile_retry_test.cpp ${content_reconcile_sources})
target_link_libraries(pulse_content_reconcile_retry_test PRIVATE ${content_reconcile_libraries})

add_executable(pulse_content_stream_work_test src/bench/content_stream_work_test.cpp src/index/content_search_session.cpp)
target_link_libraries(pulse_content_stream_work_test PRIVATE pulse_sqlite ole32 advapi32)

add_executable(pulse_context_menu_identity_test src/bench/context_menu_identity_test.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp)
target_link_libraries(pulse_context_menu_identity_test PRIVATE pulse_localization shell32 advapi32)

add_executable(pulse_places_save_order_test src/bench/places_save_order_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_compile_definitions(pulse_places_save_order_test PRIVATE PULSE_PLACES_SAVE_TEST=1)
target_link_libraries(pulse_places_save_order_test PRIVATE user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization pulse_runtime_log)

add_executable(pulse_places_tag_recovery_test src/bench/places_tag_recovery_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_places_tag_recovery_test PRIVATE user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization pulse_runtime_log)

add_executable(pulse_config_capacity_test src/bench/config_capacity_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_config_capacity_test PRIVATE user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization pulse_runtime_log)

add_executable(pulse_virtual_worker_consumers_test src/bench/virtual_worker_consumers_test.cpp)

target_sources(pulse_persistence_test PRIVATE src/app/this_pc_argument.cpp)
target_sources(pulse_app_controllers_test PRIVATE src/app/this_pc_argument.cpp src/app/update_download_cleanup.cpp)
target_link_libraries(pulse_app_controllers_test PRIVATE crypt32)
add_executable(pulse_single_instance_endpoint_test src/bench/single_instance_endpoint_test.cpp
    src/app/single_instance_coordinator.cpp src/app/shell_tag_command.cpp src/app/this_pc_argument.cpp)
target_link_libraries(pulse_single_instance_endpoint_test PRIVATE user32 advapi32 ole32)
add_executable(pulse_shell_window_registry_retry_test src/bench/shell_window_registry_retry_test.cpp
    src/app/shell_window_registry.cpp)
target_compile_definitions(pulse_shell_window_registry_retry_test PRIVATE PULSE_SHELL_WINDOW_REGISTRY_TEST=1)
target_link_libraries(pulse_shell_window_registry_retry_test PRIVATE ole32 oleaut32 shell32 shlwapi propsys uuid user32)
target_link_libraries(pulse_shell_tag_registry_test PRIVATE advapi32 shell32 shlwapi)
add_executable(pulse_shell_tag_actions_test src/bench/shell_tag_actions_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_shell_tag_actions_test PRIVATE user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization pulse_runtime_log)
add_executable(pulse_integration_registry_recovery_test src/bench/integration_registry_recovery_test.cpp)
target_link_libraries(pulse_integration_registry_recovery_test PRIVATE advapi32 shlwapi)
add_executable(pulse_bloom_factory_test src/bench/bloom_factory_test.cpp src/ui/bloom_accent_picker.cpp)
target_link_libraries(pulse_bloom_factory_test PRIVATE d2d1 d3d11 dxgi uuid)
if(PULSE_WITH_SELFTEST)
    target_sources(pulse PRIVATE src/bench/layout_regression_probe.cpp)
endif()
if(PULSE_WITH_SELFTEST)
    target_sources(pulse PRIVATE src/bench/dialog_recovery_test.cpp)
    target_sources(pulse PRIVATE src/bench/tag_color_command_test.cpp)
endif()
add_executable(pulse_view_layout_hit_test src/bench/view_layout_hit_test.cpp src/ui/view_layout.cpp)
add_executable(pulse_pane_marquee_layout_test src/bench/pane_marquee_layout_test.cpp
    src/app/pane_layout.cpp src/ui/view_layout.cpp)

add_executable(pulse_preview_folder_budget_test src/bench/preview_folder_budget_test.cpp
    src/preview_host/preview_file_utils.cpp)
target_link_libraries(pulse_preview_folder_budget_test PRIVATE shlwapi)
add_executable(pulse_notebook_clipping_test src/bench/notebook_clipping_test.cpp
    src/preview_host/notebook_document.cpp src/preview_host/table_document.cpp
    src/preview_host/zip_entry.cpp src/preview_host/doc_payload.cpp
    src/preview_host/markdown_document.cpp third_party/md4c/md4c.c)
target_compile_definitions(pulse_notebook_clipping_test PRIVATE MD4C_USE_UTF16)

add_executable(pulse_preview_data_boundaries_test src/bench/preview_data_boundaries_test.cpp
    src/preview_host/psd_raster.cpp src/preview_host/table_document.cpp src/preview_host/zip_entry.cpp)
target_link_libraries(pulse_preview_data_boundaries_test PRIVATE ole32 windowscodecs)

add_executable(pulse_rtf_page_text_test src/bench/rtf_page_text_test.cpp
    src/preview_host/office_model.cpp src/preview_host/zip_entry.cpp)
target_link_libraries(pulse_rtf_page_text_test PRIVATE ole32 shlwapi uuid)
add_dependencies(pulse_rtf_page_text_test pulse_preview)
add_executable(pulse_zip_eocd_test src/bench/zip_eocd_test.cpp src/preview_host/udf_listing.cpp)
target_link_libraries(pulse_zip_eocd_test PRIVATE ole32 shell32 shlwapi)

# M15: bounded GIF composition and same-connection preview mapping ACK regression.
target_sources(pulse_preview PRIVATE src/preview_host/gif_decoder.cpp)
add_executable(pulse_gif_decode_test src/bench/gif_decode_test.cpp src/preview_host/gif_decoder.cpp)
target_link_libraries(pulse_gif_decode_test PRIVATE ole32 windowscodecs uuid)
if(PULSE_WITH_SELFTEST)
    target_compile_definitions(pulse_preview PRIVATE PULSE_PREVIEW_MAPPING_TEST)
    add_executable(pulse_preview_mapping_ack_test src/bench/preview_mapping_ack_test.cpp)
    add_dependencies(pulse_preview_mapping_ack_test pulse_preview)
endif()

add_executable(pulse_preview_interaction_test src/bench/preview_interaction_test.cpp
    src/ui/math_formula.cpp src/ui/math_syntax.cpp src/ui/math_layout.cpp
    src/ui/math_macro.cpp src/ui/math_alphabet.cpp
    src/ui/math_font_metrics.cpp src/ui/math_font_face.cpp
    src/ui/table_view.cpp src/ui/markdown_view.cpp src/ui/syntax_highlight.cpp src/ui/typography.cpp
    src/ui/thumbnail_cache.cpp src/ui/ui_compositor.cpp
    src/preview_host/markdown_document.cpp third_party/md4c/md4c.c)
target_compile_definitions(pulse_preview_interaction_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_preview_interaction_test PRIVATE dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_preview_interaction_test)

add_executable(pulse_math_dpi_test src/bench/math_dpi_test.cpp
    src/ui/math_formula.cpp src/ui/math_syntax.cpp src/ui/math_layout.cpp
    src/ui/math_macro.cpp src/ui/math_alphabet.cpp
    src/ui/math_font_metrics.cpp src/ui/math_font_face.cpp)
target_link_libraries(pulse_math_dpi_test PRIVATE dwrite d2d1 ole32 uuid)


# M16-004: real host payload -> TreeView copy -> independent XmlLite parse.
add_executable(pulse_xml_subtree_copy_test src/bench/xml_subtree_copy_test.cpp
    src/ui/tree_view.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp)
target_link_libraries(pulse_xml_subtree_copy_test PRIVATE xmllite dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_xml_subtree_copy_test)
add_dependencies(pulse_xml_subtree_copy_test pulse_preview)

add_executable(pulse_document_image_cache_test src/bench/document_image_cache_test.cpp src/ui/thumbnail_cache.cpp)
target_link_libraries(pulse_document_image_cache_test PRIVATE d2d1 d3d11 dxgi dwrite windowscodecs ole32 shell32 shlwapi uuid)
add_dependencies(pulse_document_image_cache_test pulse_preview)
get_target_property(PULSE_DOCUMENT_IMAGE_VIEW_SOURCES pulse_preview_interaction_test SOURCES)
list(FILTER PULSE_DOCUMENT_IMAGE_VIEW_SOURCES EXCLUDE REGEX "src/bench/")
add_executable(pulse_document_image_view_test src/bench/document_image_view_test.cpp ${PULSE_DOCUMENT_IMAGE_VIEW_SOURCES})
target_compile_definitions(pulse_document_image_view_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_document_image_view_test PRIVATE dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_document_image_view_test)

# M17-005: real input-thread/message-loop lifecycle with fake hook APIs.
add_executable(pulse_preview_handler_pan_thread_test src/bench/preview_handler_pan_thread_test.cpp)
target_link_libraries(pulse_preview_handler_pan_thread_test PRIVATE gdi32 user32)

if(PULSE_WITH_SELFTEST)
    target_sources(pulse PRIVATE src/bench/tray_tags_ui_test.cpp)
endif()

# M18-002: durable, identity-bound update package recovery across process exits.
target_sources(pulse_update_installer_test PRIVATE src/app/update_download_cleanup.cpp)
target_link_libraries(pulse_update_installer_test PRIVATE crypt32 advapi32)
add_executable(pulse_update_download_cleanup_test src/bench/update_download_cleanup_test.cpp
    src/app/update_download_cleanup.cpp)
target_link_libraries(pulse_update_download_cleanup_test PRIVATE bcrypt crypt32 advapi32 shell32)

add_executable(pulse_places_json_roundtrip_test src/bench/places_json_roundtrip_test.cpp
    src/app/places.cpp src/app/tag_ads_sync.cpp src/fs/fs_enum.cpp src/ui/view_layout.cpp)
target_link_libraries(pulse_places_json_roundtrip_test PRIVATE user32 shell32 ole32 uuid advapi32 shlwapi netapi32 pulse_localization)

add_executable(pulse_index_exclusion_concurrency_test src/bench/index_exclusion_concurrency_test.cpp ${query_narrow_sources})
target_include_directories(pulse_index_exclusion_concurrency_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_index_exclusion_concurrency_test PRIVATE advapi32 shlwapi ole32 shell32 netapi32 mpr pulse_localization pulse_runtime_log)

add_executable(pulse_index_volume_fault_test src/bench/index_volume_fault_test.cpp ${delta_replay_sources})
target_link_libraries(pulse_index_volume_fault_test PRIVATE ${delta_replay_libraries} pulse_runtime_log)
foreach(volume_fault_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(volume_fault_value pulse_index_engine_test ${volume_fault_property})
    if(volume_fault_value)
        set_property(TARGET pulse_index_volume_fault_test PROPERTY ${volume_fault_property} "${volume_fault_value}")
    endif()
endforeach()
target_compile_definitions(pulse_index_volume_fault_test PRIVATE PULSE_INDEX_VOLUME_FAULT_TEST=1)

add_executable(pulse_syntax_span_bounds_test
    src/bench/syntax_span_bounds_test.cpp src/ui/syntax_highlight.cpp)
add_executable(pulse_preview_orientation_test src/bench/preview_orientation_test.cpp)
target_link_libraries(pulse_preview_orientation_test PRIVATE ole32 windowscodecs)
add_dependencies(pulse_preview_orientation_test pulse_preview)

add_executable(pulse_content_search_snapshot_test src/bench/content_search_snapshot_test.cpp)

add_executable(pulse_advanced_search_query_test src/bench/advanced_search_query_test.cpp
    src/bench/dialog_gallery.rc
    src/ui/dialog_lifecycle.cpp
    src/ui/advanced_search_dialog.cpp src/ui/edit_host.cpp src/ui/fluent_components.cpp
    src/ui/tab_shape.cpp src/ui/typography.cpp src/ui/ui_compositor.cpp src/ui/window_helpers.cpp
    src/app/search_query.cpp src/app/search_query_rewrite.cpp
    src/index/index_query.cpp src/index/search_kinds.cpp src/index/filename_pinyin.cpp)
target_compile_definitions(pulse_advanced_search_query_test PRIVATE PULSE_ADVANCED_QUERY_TEST)
target_link_options(pulse_advanced_search_query_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_advanced_search_query_test PRIVATE d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore
    ole32 windowscodecs comctl32 shell32 shlwapi uxtheme pulse_localization)
pulse_enable_lumatext(pulse_advanced_search_query_test)

add_executable(pulse_markdown_image_uri_test src/bench/markdown_image_uri_test.cpp ${PULSE_DOCUMENT_IMAGE_VIEW_SOURCES})
target_compile_definitions(pulse_markdown_image_uri_test PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_markdown_image_uri_test PRIVATE dwrite d2d1 d3d11 dxgi dcomp dwmapi shcore
    windowscodecs ole32 shell32 shlwapi user32 gdi32 comctl32 pulse_localization)
pulse_enable_lumatext(pulse_markdown_image_uri_test)

add_executable(pulse_preview_image_cache_race_test src/bench/preview_image_cache_race_test.cpp src/preview_host/doc_payload.cpp)
target_compile_definitions(pulse_preview_image_cache_race_test PRIVATE PULSE_PREVIEW_IMAGE_CACHE_RACE_TEST=1)

if(PULSE_WITH_SELFTEST)
    target_sources(pulse PRIVATE src/bench/tab_pin_boundary_test.cpp)
endif()

add_executable(pulse_enum_benchmark_audit_test src/bench/enum_benchmark_audit_test.cpp)

# Merged targets inherit the same document-image lifetime and GIF decoder sources.
target_compile_definitions(pulse_file_picker_operations_test PRIVATE PULSE_FILE_PICKER_OPERATIONS_STANDALONE=1)
get_property(unified_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(unified_target IN LISTS unified_targets)
    get_target_property(unified_sources ${unified_target} SOURCES)
    if("${unified_sources}" MATCHES "markdown_view[.]cpp" AND NOT "${unified_sources}" MATCHES "markdown_image_metadata[.]cpp")
        target_sources(${unified_target} PRIVATE src/ui/markdown_image_metadata.cpp)
    endif()
    if("${unified_sources}" MATCHES "preview_decoders[.]cpp" AND NOT "${unified_sources}" MATCHES "gif_decoder[.]cpp")
        target_sources(${unified_target} PRIVATE src/preview_host/gif_decoder.cpp)
    endif()
endforeach()

# Capture the completed Pulse target, including optional selftest dependencies.
if(PULSE_BUILD_PROGRESS_UI_TESTS)
    get_target_property(progress_ui_sources pulse SOURCES)
    get_target_property(progress_ui_libraries pulse LINK_LIBRARIES)
    add_executable(pulse_content_progress_ui_test src/bench/content_progress_ui_test.cpp ${progress_ui_sources})
    target_link_libraries(pulse_content_progress_ui_test PRIVATE ${progress_ui_libraries})
    foreach(progress_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
        get_target_property(progress_value pulse ${progress_property})
        if(progress_value)
            set_property(TARGET pulse_content_progress_ui_test PROPERTY ${progress_property} "${progress_value}")
        endif()
    endforeach()
endif()

include(cmake/setup.cmake)

# Large legitimate indexes must round-trip beyond the former five-million cap.
add_executable(pulse_index_capacity_test src/bench/index_capacity_test.cpp ${delta_replay_sources})
target_link_libraries(pulse_index_capacity_test PRIVATE ${delta_replay_libraries} pulse_runtime_log)
foreach(capacity_property INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS)
    get_target_property(capacity_value pulse_index_engine_test ${capacity_property})
    if(capacity_value)
        set_property(TARGET pulse_index_capacity_test PROPERTY ${capacity_property} "${capacity_value}")
    endif()
endforeach()

# Diagnostic events are used by both production preview sources and their isolated probes.
get_property(diagnostics_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(diagnostics_target IN LISTS diagnostics_targets)
    get_target_property(diagnostics_sources ${diagnostics_target} SOURCES)
    if(diagnostics_sources MATCHES "thumbnail_cache[.]cpp|preview_handler_host[.]cpp")
        target_link_libraries(${diagnostics_target} PRIVATE pulse_runtime_log)
    endif()
endforeach()

add_executable(pulse_support_report_test src/bench/support_report_test.cpp)
target_link_libraries(pulse_support_report_test PRIVATE pulse_crash_reporter)
