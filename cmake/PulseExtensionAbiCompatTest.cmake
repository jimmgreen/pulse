# Headless Native Extension ABI Compatibility & Ownership Verification Suite.
# Designed for Low Integrity (S-1-16-4096) execution: zero USER32/GDI32/SHELL32 imports.
get_filename_component(PULSE_COMPAT_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_pulse_compat_saved_cxx_std_libs "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")

add_library(pulse_extension_abi_compat_fixture SHARED EXCLUDE_FROM_ALL
    "${PULSE_COMPAT_ROOT}/src/bench/native_sdk/compat_fixture.cpp")
target_include_directories(pulse_extension_abi_compat_fixture PRIVATE
    "${PULSE_COMPAT_ROOT}/sdk/native/include")
target_compile_features(pulse_extension_abi_compat_fixture PRIVATE cxx_std_20)
target_compile_definitions(pulse_extension_abi_compat_fixture PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
set_target_properties(pulse_extension_abi_compat_fixture PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(pulse_extension_abi_compat_fixture PRIVATE kernel32)
if(MSVC)
    target_compile_options(pulse_extension_abi_compat_fixture PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_extension_abi_compat_fixture PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

add_executable(pulse_extension_abi_compat_test EXCLUDE_FROM_ALL
    "${PULSE_COMPAT_ROOT}/src/bench/extension_abi_compat_test.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native/module.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native_image_verify.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native/fileops_adapter.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native/tasks_adapter.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native/worker_budget.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/native/commands_adapter.cpp"
    "${PULSE_COMPAT_ROOT}/src/app/extensions/authorization.cpp")
target_include_directories(pulse_extension_abi_compat_test PRIVATE
    "${PULSE_COMPAT_ROOT}/sdk/native/include")
target_compile_features(pulse_extension_abi_compat_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_extension_abi_compat_test PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603)
set_target_properties(pulse_extension_abi_compat_test PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(pulse_extension_abi_compat_test PRIVATE
    kernel32 advapi32 wintrust crypt32 bcrypt)
add_dependencies(pulse_extension_abi_compat_test pulse_extension_abi_compat_fixture)
if(MSVC)
    target_compile_options(pulse_extension_abi_compat_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_extension_abi_compat_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

set(CMAKE_CXX_STANDARD_LIBRARIES "${_pulse_compat_saved_cxx_std_libs}")
unset(_pulse_compat_saved_cxx_std_libs)
