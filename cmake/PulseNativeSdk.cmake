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
endif()

add_executable(pulse_native_commands_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/commands_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/commands_adapter.cpp")
target_include_directories(pulse_native_commands_test PRIVATE "${PULSE_NATIVE_ROOT}/sdk/native/include")
target_compile_features(pulse_native_commands_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_commands_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_commands_test PRIVATE /W4 /permissive- /utf-8)

add_executable(pulse_native_previews_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/previews_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/previews_adapter.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/ui_model.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/ui_tree.cpp"
    "${PULSE_NATIVE_ROOT}/src/common/json_value.cpp")
target_compile_features(pulse_native_previews_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_previews_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_previews_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_previews_test PRIVATE user32 shlwapi)

add_executable(pulse_native_tasks_test
    "${PULSE_NATIVE_ROOT}/src/bench/native_sdk/tasks_test.cpp"
    "${PULSE_NATIVE_ROOT}/src/app/extensions/native/tasks_adapter.cpp")
target_compile_features(pulse_native_tasks_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_tasks_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(pulse_native_tasks_test PRIVATE /W4 /permissive- /utf-8)

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
pulse_enable_lumatext(pulse_native_settings_pages_test)
