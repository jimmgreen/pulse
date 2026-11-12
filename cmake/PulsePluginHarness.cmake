# Headless Native Extension Contract & Smoke Test Harness.
# Designed for Low Integrity (S-1-16-4096) execution: zero USER32/GDI32/SHELL32 imports.
get_filename_component(PULSE_HARNESS_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_pulse_harness_saved_cxx_std_libs "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")

add_executable(pulse_plugin_harness EXCLUDE_FROM_ALL
    "${PULSE_HARNESS_ROOT}/src/bench/native_sdk/plugin_harness_main.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/module.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native_image_verify.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/commands_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/icons_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/diagnostics_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/dialogs_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/pickers_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/workbench_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/ui_model.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/ui_tree.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/tasks_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/native/fileops_adapter.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/authorization.cpp"
    "${PULSE_HARNESS_ROOT}/src/app/extensions/manifest.cpp"
    "${PULSE_HARNESS_ROOT}/src/common/json_value.cpp")

target_include_directories(pulse_plugin_harness PRIVATE
    "${PULSE_HARNESS_ROOT}/sdk/native/include")
target_compile_features(pulse_plugin_harness PRIVATE cxx_std_20)
target_compile_definitions(pulse_plugin_harness PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
set_target_properties(pulse_plugin_harness PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(pulse_plugin_harness PRIVATE
    kernel32 advapi32 wintrust crypt32 bcrypt)

if(MSVC)
    target_compile_options(pulse_plugin_harness PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_plugin_harness PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

set(CMAKE_CXX_STANDARD_LIBRARIES "${_pulse_harness_saved_cxx_std_libs}")
unset(_pulse_harness_saved_cxx_std_libs)
