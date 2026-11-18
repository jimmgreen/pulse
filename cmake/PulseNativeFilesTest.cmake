add_executable(pulse_native_files_test src/bench/native_files_adapter_test.cpp
    src/app/extensions/native/files_adapter.cpp src/common/json_value.cpp src/app/extensions/authorization.cpp)
target_compile_features(pulse_native_files_test PRIVATE cxx_std_20)
target_compile_options(pulse_native_files_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_files_test PRIVATE user32 bcrypt)

add_executable(pulse_native_fileops_test src/bench/native_fileops_adapter_test.cpp
    src/app/extensions/native/fileops_adapter.cpp src/app/extensions/authorization.cpp)
target_compile_features(pulse_native_fileops_test PRIVATE cxx_std_20)
target_compile_options(pulse_native_fileops_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_native_fileops_test PRIVATE user32 bcrypt)
