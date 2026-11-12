# Headless Local .pulse-ext Verification, Install Service & Permission Localization Suite.
# Designed for Low Integrity (S-1-16-4096) execution: zero USER32/GDI32/SHELL32 imports.
get_filename_component(PULSE_LOCAL_INSTALL_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

set(_pulse_local_inst_saved_cxx_std_libs "${CMAKE_CXX_STANDARD_LIBRARIES}")
set(CMAKE_CXX_STANDARD_LIBRARIES "kernel32.lib")

add_executable(pulse_extension_local_install_test EXCLUDE_FROM_ALL
    "${PULSE_LOCAL_INSTALL_ROOT}/src/bench/extension_local_install_test.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/bench/localization_test.rc"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/store/install_service.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/store/install_transaction.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/store/package_download.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/store/package_archive.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/package_store.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/manifest.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/package_signature.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/native_image_verify.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/extensions/resource_settings.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/app/update_transport.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/common/json_value.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/common/localization.cpp"
    "${PULSE_LOCAL_INSTALL_ROOT}/src/common/zh_hant.cpp")
target_include_directories(pulse_extension_local_install_test PRIVATE
    "${PULSE_LOCAL_INSTALL_ROOT}/sdk/native/include"
    "${CMAKE_BINARY_DIR}/generated")
target_compile_features(pulse_extension_local_install_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_extension_local_install_test PRIVATE
    UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0603
    PULSE_EXTENSION_INSTALL_SERVICE_TESTING
    PULSE_EXTENSION_LOCAL_PACKAGE_TESTING)
set_target_properties(pulse_extension_local_install_test PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_link_libraries(pulse_extension_local_install_test PRIVATE
    kernel32 bcrypt winhttp)
if(MSVC)
    target_compile_options(pulse_extension_local_install_test PRIVATE /W4 /WX /permissive- /utf-8 /EHsc)
    target_link_options(pulse_extension_local_install_test PRIVATE
        /NODEFAULTLIB:user32.lib
        /NODEFAULTLIB:gdi32.lib
        /NODEFAULTLIB:shell32.lib)
endif()

set(CMAKE_CXX_STANDARD_LIBRARIES "${_pulse_local_inst_saved_cxx_std_libs}")
unset(_pulse_local_inst_saved_cxx_std_libs)
