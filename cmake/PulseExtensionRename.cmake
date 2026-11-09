# Real custom rename plans and per-item identity execution.
add_executable(pulse_extension_rename_plan_test src/bench/extension_rename_plan_test.cpp
    src/app/extensions/rename_plan.cpp src/app/batch_rename_plan.cpp)
target_compile_features(pulse_extension_rename_plan_test PRIVATE cxx_std_20)
target_compile_options(pulse_extension_rename_plan_test PRIVATE /W4 /permissive- /utf-8)
get_target_property(extension_rename_sources pulse_rename_ops_test SOURCES)
list(FILTER extension_rename_sources EXCLUDE REGEX "(^|/)rename_ops_test\\.cpp$")
get_target_property(extension_rename_libraries pulse_rename_ops_test LINK_LIBRARIES)
add_executable(pulse_extension_rename_ops_test src/bench/extension_rename_ops_test.cpp ${extension_rename_sources})
target_link_libraries(pulse_extension_rename_ops_test PRIVATE ${extension_rename_libraries})
target_compile_features(pulse_extension_rename_ops_test PRIVATE cxx_std_20)
target_compile_options(pulse_extension_rename_ops_test PRIVATE /W4 /permissive- /utf-8)
add_dependencies(pulse_extension_rename_ops_test pulse_shell)
