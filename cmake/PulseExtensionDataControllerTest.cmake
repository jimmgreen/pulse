add_executable(pulse_extension_data_controller_test
    src/bench/extension_data_controller_test.cpp src/ui/extension_data_controller.cpp)
target_compile_options(pulse_extension_data_controller_test PRIVATE /W4 /permissive- /utf-8)
