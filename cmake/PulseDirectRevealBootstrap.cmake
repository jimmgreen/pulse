# Include after PulseDialogAuto and the existing dialog bundle/trust targets.
target_sources(pulse PRIVATE src/app/direct_reveal_bootstrap.cpp)
target_link_libraries(pulse PRIVATE pulse_dialog_bundle pulse_dialog_bridge_paths pulse_dialog_admin_trust)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    add_library(pulse_direct_reveal_bootstrap_fixture SHARED EXCLUDE_FROM_ALL
        src/bench/direct_reveal_bootstrap_fixture.cpp)
    add_executable(pulse_direct_reveal_bootstrap_test EXCLUDE_FROM_ALL
        src/bench/direct_reveal_bootstrap_test.cpp src/app/direct_reveal_bootstrap.cpp
        src/app/direct_reveal_targets.cpp src/app/explorer_launch_origin.cpp)
    target_compile_definitions(pulse_direct_reveal_bootstrap_test PRIVATE PULSE_DIRECT_REVEAL_BOOTSTRAP_TEST=1)
    target_link_libraries(pulse_direct_reveal_bootstrap_test PRIVATE pulse_dialog_bundle pulse_dialog_bridge_paths pulse_dialog_admin_trust
        pulse_dialog_user_auto pulse_runtime_log shell32 ole32 advapi32 user32)
endif()
