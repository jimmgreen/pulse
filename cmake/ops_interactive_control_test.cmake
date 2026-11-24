add_executable(pulse_ops_interactive_control_test EXCLUDE_FROM_ALL
    ${interactive_test_sources} src/bench/ops_interactive_control_test.cpp src/bench/localization_test.rc)
target_compile_options(pulse_ops_interactive_control_test PRIVATE /W4 /permissive- /utf-8 /Gy)
target_link_options(pulse_ops_interactive_control_test PRIVATE /OPT:REF)
target_link_libraries(pulse_ops_interactive_control_test PRIVATE ${interactive_test_libraries} advapi32)

add_executable(pulse_ops_notify_gate_test EXCLUDE_FROM_ALL
    ${interactive_test_sources} src/bench/ops_notify_gate_test.cpp src/bench/localization_test.rc)
target_compile_options(pulse_ops_notify_gate_test PRIVATE /W4 /permissive- /utf-8 /Gy)
target_link_options(pulse_ops_notify_gate_test PRIVATE /OPT:REF)
target_link_libraries(pulse_ops_notify_gate_test PRIVATE ${interactive_test_libraries} advapi32)

add_executable(pulse_delete_progress_test EXCLUDE_FROM_ALL src/bench/delete_progress_test.cpp src/ops/elevated_delete.cpp)
target_link_libraries(pulse_delete_progress_test PRIVATE ole32 shell32 uuid)
target_compile_features(pulse_delete_progress_test PRIVATE cxx_std_20)
target_compile_options(pulse_delete_progress_test PRIVATE /W4 /permissive- /utf-8)
