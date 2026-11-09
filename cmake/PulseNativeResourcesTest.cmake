add_executable(pulse_native_resources_test src/bench/native_resources_adapter_test.cpp
    src/app/extensions/native/resources_adapter.cpp src/common/json_value.cpp src/app/extensions/authorization.cpp)
target_compile_features(pulse_native_resources_test PRIVATE cxx_std_20)
target_compile_options(pulse_native_resources_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_resources_test PRIVATE user32 bcrypt)
