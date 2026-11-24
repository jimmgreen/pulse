get_target_property(interactive_test_sources pulse_ops_test SOURCES)
list(FILTER interactive_test_sources EXCLUDE REGEX "(ops_test_main|retry_template_test).cpp$")
get_target_property(interactive_test_libraries pulse_ops_test LINK_LIBRARIES)
add_executable(pulse_ops_interactive_test EXCLUDE_FROM_ALL
    ${interactive_test_sources} src/bench/ops_interactive_test.cpp src/bench/localization_test.rc)
target_compile_options(pulse_ops_interactive_test PRIVATE /W4 /permissive- /utf-8 /Gy)
target_link_options(pulse_ops_interactive_test PRIVATE /OPT:REF)
target_link_libraries(pulse_ops_interactive_test PRIVATE ${interactive_test_libraries} advapi32)

include(${CMAKE_CURRENT_LIST_DIR}/ops_interactive_control_test.cmake)

add_executable(pulse_ops_interactive_names_test EXCLUDE_FROM_ALL
    ${interactive_test_sources} src/bench/ops_interactive_names_test.cpp src/bench/localization_test.rc)
target_compile_options(pulse_ops_interactive_names_test PRIVATE /W4 /permissive- /utf-8 /Gy)
target_link_options(pulse_ops_interactive_names_test PRIVATE /OPT:REF)
target_link_libraries(pulse_ops_interactive_names_test PRIVATE ${interactive_test_libraries} advapi32)
