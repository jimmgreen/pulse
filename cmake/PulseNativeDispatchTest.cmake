get_filename_component(PULSE_NATIVE_DISPATCH_ROOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
add_executable(pulse_native_dispatch_test
    "${PULSE_NATIVE_DISPATCH_ROOT}/src/bench/native_dispatch_test.cpp"
    "${PULSE_NATIVE_DISPATCH_ROOT}/src/app/extensions/native/dispatch_adapter.cpp")
target_compile_features(pulse_native_dispatch_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_dispatch_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_dispatch_test PRIVATE user32)
if(MSVC)
    target_compile_options(pulse_native_dispatch_test PRIVATE /W4 /permissive- /utf-8)
endif()
