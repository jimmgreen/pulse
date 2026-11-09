add_executable(pulse_native_context_test src/bench/native_sdk/context_test.cpp
    src/app/extensions/native/context_adapter.cpp src/app/extensions/authorization.cpp)
target_compile_features(pulse_native_context_test PRIVATE cxx_std_20)
target_compile_options(pulse_native_context_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_context_test PRIVATE user32 bcrypt)
