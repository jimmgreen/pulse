# May be included by Pulse or a small independent native SDK validation project.
get_filename_component(PULSE_NATIVE_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_library(pulse_native_module STATIC "${PULSE_NATIVE_ROOT}/src/app/extensions/native/module.cpp")
target_sources(pulse_native_module PRIVATE "${PULSE_NATIVE_ROOT}/src/app/extensions/native_image_verify.cpp")
target_link_libraries(pulse_native_module PRIVATE bcrypt)
target_compile_features(pulse_native_module PUBLIC cxx_std_20)
target_compile_definitions(pulse_native_module PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_include_directories(pulse_native_module PUBLIC "${PULSE_NATIVE_ROOT}/sdk/native/include")
foreach(pair IN ITEMS "native_good:0" "native_bad_abi:1" "native_missing:2" "native_init_fail:3" "native_with_dependency:4" "native_no_preview:5" "native_old_preview:6")
    string(REPLACE ":" ";" fields "${pair}")
    list(GET fields 0 name)
    list(GET fields 1 mode)
    add_library(${name} SHARED "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/fixture.cpp")
    target_include_directories(${name} PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
    target_compile_definitions(${name} PRIVATE FIXTURE_MODE=${mode})
    # Exercise allocation/destruction in a distinct static plugin CRT.
    set_target_properties(${name} PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endforeach()
add_library(native_private_dependency SHARED "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/dependency_fixture.cpp")
set_target_properties(native_private_dependency PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(native_with_dependency PRIVATE native_private_dependency)
add_executable(pulse_native_loader_test "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/loader_test.cpp")
target_link_libraries(pulse_native_loader_test PRIVATE pulse_native_module)
add_dependencies(pulse_native_loader_test native_good native_bad_abi native_missing native_init_fail native_with_dependency native_no_preview native_old_preview)
if(MSVC)
    foreach(target IN ITEMS pulse_native_module pulse_native_loader_test native_good native_bad_abi native_missing native_init_fail native_with_dependency native_no_preview native_old_preview native_private_dependency)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
    endforeach()
endif()

if(TARGET pulse_crash_reporter)
    foreach(mode IN ITEMS activate callback)
        add_library(native_crash_${mode} SHARED "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/crash_fixture.cpp")
        target_include_directories(native_crash_${mode} PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
        target_compile_definitions(native_crash_${mode} PRIVATE CRASH_ON_ACTIVATE=$<STREQUAL:${mode},activate>)
    endforeach()
    add_executable(pulse_native_crash_guard_test
        "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/crash_guard_test.cpp"
        "${PULSE_NATIVE_ROOT}/src/app/extensions/native/crash_guard.cpp")
    target_link_libraries(pulse_native_crash_guard_test PRIVATE pulse_native_module pulse_crash_reporter psapi user32)
    target_compile_features(pulse_native_crash_guard_test PRIVATE cxx_std_20)
    target_compile_definitions(pulse_native_crash_guard_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    add_dependencies(pulse_native_crash_guard_test native_crash_activate native_crash_callback)
    if(MSVC)
        foreach(target IN ITEMS pulse_native_crash_guard_test native_crash_activate native_crash_callback)
            target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        endforeach()
    endif()

    add_executable(pulse_native_runtime_overhaul_test EXCLUDE_FROM_ALL
        "${PULSE_NATIVE_ROOT}/src/bench/native_runtime_overhaul_test.cpp"
        "${PULSE_NATIVE_ROOT}/src/app/extensions/native/worker_budget.cpp"
        "${PULSE_NATIVE_ROOT}/src/app/extensions/native/tasks_adapter.cpp"
        "${PULSE_NATIVE_ROOT}/src/app/extensions/native/commands_adapter.cpp"
        "${PULSE_NATIVE_ROOT}/src/app/extensions/native/crash_guard.cpp")
    target_include_directories(pulse_native_runtime_overhaul_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
    target_compile_features(pulse_native_runtime_overhaul_test PRIVATE cxx_std_20)
    target_compile_definitions(pulse_native_runtime_overhaul_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
    target_compile_options(pulse_native_runtime_overhaul_test PRIVATE /W4 /permissive- /utf-8)
    target_link_libraries(pulse_native_runtime_overhaul_test PRIVATE pulse_crash_reporter psapi delayimp)
    target_link_options(pulse_native_runtime_overhaul_test PRIVATE /DELAYLOAD:shell32.dll)
endif()

add_executable(pulse_native_commands_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/commands_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/commands_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/icons_adapter.cpp")
target_include_directories(pulse_native_commands_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_commands_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_commands_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_commands_test PRIVATE /W4 /permissive- /utf-8)

set(_pulse_ui_svc_saved_cxx_std_libs "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")
add_executable(pulse_native_ui_services_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/ui_services_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/dialogs_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/pickers_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/workbench_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/ui_model.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/ui_tree.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_include_directories(pulse_native_ui_services_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_ui_services_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_ui_services_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_ui_services_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_ui_services_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_ui_services_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_command_projection_cache_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/command_projection_cache_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/command_projection_cache.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/commands_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/icons_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp"
    "${PULSE_NATIVE_ROOT}/extensions/7zip/text.cpp")
target_include_directories(pulse_command_projection_cache_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_command_projection_cache_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_command_projection_cache_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_command_projection_cache_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_command_projection_cache_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_command_projection_cache_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_native_associations_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/associations_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/associations_registry.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/associations_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/manifest.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_include_directories(pulse_native_associations_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_associations_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_associations_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_associations_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_associations_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_associations_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_extension_quick_settings_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/quick_settings_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/manifest.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_include_directories(pulse_extension_quick_settings_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_extension_quick_settings_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_extension_quick_settings_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_extension_quick_settings_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_extension_quick_settings_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_extension_quick_settings_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_sdk_worker_echo_helper WIN32
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/worker_echo_helper.cpp")
target_include_directories(pulse_sdk_worker_echo_helper PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_sdk_worker_echo_helper PRIVATE cxx_std_20)
target_compile_definitions(pulse_sdk_worker_echo_helper PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_sdk_worker_echo_helper PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_sdk_worker_echo_helper PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_sdk_worker_echo_helper PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_sdk_worker_helper_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/worker_helper_test.cpp")
target_include_directories(pulse_sdk_worker_helper_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_sdk_worker_helper_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_sdk_worker_helper_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_sdk_worker_helper_test PRIVATE kernel32)
add_dependencies(pulse_sdk_worker_helper_test pulse_sdk_worker_echo_helper)
if(MSVC)
    target_compile_options(pulse_sdk_worker_helper_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_sdk_worker_helper_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_native_vfs_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/vfs_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/vfs_registry.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/vfs_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp")
target_include_directories(pulse_native_vfs_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_vfs_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_vfs_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_vfs_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_vfs_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_vfs_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()
add_executable(pulse_native_columns_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/columns_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/column_registry.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/columns_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp")
target_include_directories(pulse_native_columns_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_columns_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_columns_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_columns_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_columns_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_columns_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()
add_executable(pulse_native_statusbar_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/statusbar_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/statusbar_registry.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/statusbar_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp")
target_include_directories(pulse_native_statusbar_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_statusbar_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_statusbar_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_statusbar_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_statusbar_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_statusbar_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()
add_executable(pulse_native_extcapsules_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/extcapsules_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/extcapsule_registry.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/extcapsules_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp")
target_include_directories(pulse_native_extcapsules_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_extcapsules_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_extcapsules_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_extcapsules_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_extcapsules_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_extcapsules_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()
add_executable(pulse_native_fileevents_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/fileevents_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/file_event_hub.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/fileevents_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp")
target_include_directories(pulse_native_fileevents_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_fileevents_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_fileevents_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_fileevents_test PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_native_fileevents_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_native_fileevents_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()
set(CMAKE_CXX_STANDARD_LIBRARIES "${_pulse_ui_svc_saved_cxx_std_libs}")
unset(_pulse_ui_svc_saved_cxx_std_libs)

add_executable(pulse_native_previews_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/previews_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/previews_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/ui_model.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/ui_tree.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_compile_features(pulse_native_previews_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_previews_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_previews_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_previews_test PRIVATE user32 shlwapi)

add_executable(pulse_native_preview_thumbnails_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/preview_thumbnails_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/previews_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/ui_model.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/ui_tree.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_include_directories(pulse_native_preview_thumbnails_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_preview_thumbnails_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_preview_thumbnails_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_preview_thumbnails_test PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(pulse_native_preview_thumbnails_test PRIVATE user32 shlwapi)

# pulse::sdk::App against an in-process fake host (no Pulse sources), plus a header
# generated from app_strings.tsv by tools/extensions/native_strings.py.
add_executable(pulse_native_app_test "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/app_test.cpp")
target_include_directories(pulse_native_app_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_app_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_app_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_app_test PRIVATE user32)
if(MSVC)
    target_compile_options(pulse_native_app_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    set_target_properties(pulse_native_app_test PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

add_executable(pulse_native_tasks_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/tasks_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/tasks_adapter.cpp")
target_compile_features(pulse_native_tasks_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_tasks_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_tasks_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_native_worker_budget_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_worker_budget_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/tasks_adapter.cpp")
target_include_directories(pulse_native_worker_budget_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_worker_budget_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_worker_budget_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_worker_budget_test PRIVATE /W4 /WX /permissive- /utf-8)

add_executable(pulse_native_staging_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/staging_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/staging_adapter.cpp")
target_compile_features(pulse_native_staging_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_staging_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_staging_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_native_settings_pages_test
    src/bench/native_sdk/settings_pages_test.cpp
    src/app/extensions/native/settings_pages_adapter.cpp
    src/app/extensions/native/ui_adapter.cpp src/app/extensions/native/ui_data_adapter.cpp
    src/app/extensions/native/ui_model.cpp
    ${PULSE_EXTENSION_PANEL_RENDER_SOURCES}
    src/app/extensions/ui_tree.cpp src/common/json_value.cpp)
target_compile_features(pulse_native_settings_pages_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_settings_pages_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_settings_pages_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_settings_pages_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 oleacc oleaut32 windowscodecs comctl32 pulse_localization)
# The hosted controls use Common Controls v6 exports.
target_link_options(pulse_native_settings_pages_test PRIVATE
    "/MANIFESTDEPENDENCY:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'")
pulse_enable_lumatext(pulse_native_settings_pages_test)