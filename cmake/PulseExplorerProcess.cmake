# Production Explorer launch replacement shares one parser and route across hooks.
add_library(pulse_explorer_launch STATIC src/common/explorer_launch_request.cpp)
target_link_libraries(pulse_explorer_launch PUBLIC shell32)

add_library(pulse_explorer_process STATIC src/shell_bridge/explorer_process_route.cpp src/app/open_request_pipe.cpp)
target_link_libraries(pulse_explorer_process PUBLIC pulse_explorer_launch
    pulse_dialog_bundle pulse_dialog_admin_trust pulse_dialog_bridge_paths bcrypt user32 advapi32 ole32)

add_executable(pulse_explorer_bridge WIN32
    src/explorer_bridge/main.cpp src/explorer_bridge/dispatch.cpp src/explorer_bridge/cold_start.cpp
    src/explorer_bridge/cold_start_win32.cpp src/app/open_request_pipe.cpp)
target_link_libraries(pulse_explorer_bridge PRIVATE pulse_explorer_launch pulse_dialog_bundle
    pulse_dialog_admin_trust pulse_dialog_bridge_paths user32 ole32 advapi32 shell32)
set_target_properties(pulse_explorer_launch pulse_explorer_process pulse_explorer_bridge
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(PULSE_RELEASE_SYMBOLS)
    target_compile_options(pulse_explorer_bridge PRIVATE /Z7)
    target_link_options(pulse_explorer_bridge PRIVATE /DEBUG:FULL /OPT:REF /OPT:ICF)
endif()

# Included after production and focused test targets are declared. Match the owning
# source exactly, so new consumers receive the dependency without unrelated targets.
get_property(_explorer_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_explorer_target IN LISTS _explorer_targets)
    get_target_property(_explorer_sources ${_explorer_target} SOURCES)
    foreach(_explorer_source IN LISTS _explorer_sources)
        if(_explorer_source MATCHES "(^|/)src/shell_bridge/(direct_reveal_hook|runtime_child_process|dialog_sink_hooks)\\.cpp$")
            target_link_libraries(${_explorer_target} PRIVATE pulse_explorer_process)
            break()
        endif()
    endforeach()
endforeach()

add_dependencies(pulse_package pulse_explorer_bridge)

add_executable(pulse_explorer_launch_request_test EXCLUDE_FROM_ALL src/bench/explorer_launch_request_test.cpp)
target_link_libraries(pulse_explorer_launch_request_test PRIVATE pulse_explorer_launch)
add_executable(pulse_explorer_process_test EXCLUDE_FROM_ALL src/bench/explorer_process_test.cpp
    src/shell_bridge/explorer_process_route.cpp src/explorer_bridge/dispatch.cpp
    src/explorer_bridge/cold_start.cpp src/explorer_bridge/cold_start_win32.cpp src/app/open_request_pipe.cpp)
target_compile_definitions(pulse_explorer_process_test PRIVATE PULSE_EXPLORER_PROCESS_TEST=1)
target_link_libraries(pulse_explorer_process_test PRIVATE pulse_explorer_launch pulse_dialog_bundle
    pulse_dialog_admin_trust pulse_dialog_bridge_paths bcrypt user32 ole32 advapi32 shell32)
add_library(pulse_explorer_process_node MODULE EXCLUDE_FROM_ALL src/bench/explorer_process_node.cpp
    src/shell_bridge/explorer_process_route.cpp src/app/open_request_pipe.cpp)
set_target_properties(pulse_explorer_process_node PROPERTIES PREFIX "" SUFFIX ".node")
target_compile_definitions(pulse_explorer_process_node PRIVATE PULSE_EXPLORER_PROCESS_TEST=1)
target_link_libraries(pulse_explorer_process_node PRIVATE pulse_explorer_launch pulse_dialog_bundle
    pulse_dialog_admin_trust pulse_dialog_bridge_paths pulse_detours bcrypt uuid user32 ole32 advapi32 shell32)
add_executable(pulse_explorer_takeover_wake_test EXCLUDE_FROM_ALL
    src/bench/explorer_takeover_wake_test.cpp src/app/shell_window_plan.cpp)
target_include_directories(pulse_explorer_takeover_wake_test PRIVATE src/app)
target_link_libraries(pulse_explorer_takeover_wake_test PRIVATE shlwapi)

add_executable(pulse_explorer_bridge_deploy EXCLUDE_FROM_ALL
    src/bench/explorer_bridge_deploy.cpp src/setup/setup_dialog_bridge.cpp src/setup/setup_service.cpp)
target_link_libraries(pulse_explorer_bridge_deploy PRIVATE pulse_dialog_bridge_paths
    pulse_dialog_com_hklm pulse_dialog_admin_trust pulse_dialog_bundle bcrypt advapi32 shell32 ole32)

add_executable(pulse_browse_cold_start_test EXCLUDE_FROM_ALL
    src/bench/browse_cold_start_test.cpp src/explorer_bridge/cold_start.cpp)
add_executable(pulse_browse_bridge_dispatch_test EXCLUDE_FROM_ALL
    src/bench/browse_bridge_dispatch_test.cpp src/explorer_bridge/dispatch.cpp
    src/explorer_bridge/cold_start.cpp src/explorer_bridge/cold_start_win32.cpp src/app/open_request_pipe.cpp)
target_link_libraries(pulse_browse_bridge_dispatch_test PRIVATE pulse_explorer_launch pulse_dialog_bundle
    pulse_dialog_admin_trust pulse_dialog_bridge_paths user32 ole32 advapi32 shell32)

# Native-fallback retention is separate from Explorer presentation tests.
add_executable(pulse_native_browse_intent_test EXCLUDE_FROM_ALL src/bench/native_browse_intent_test.cpp)
target_compile_definitions(pulse_native_browse_intent_test PRIVATE PULSE_NATIVE_BROWSE_INTENT_TEST=1)
target_link_libraries(pulse_native_browse_intent_test PRIVATE user32 advapi32)
add_executable(pulse_native_browse_launch_test EXCLUDE_FROM_ALL src/bench/native_browse_launch_test.cpp)
target_link_libraries(pulse_native_browse_launch_test PRIVATE user32 advapi32)
set_target_properties(pulse_native_browse_intent_test pulse_native_browse_launch_test
    PROPERTIES MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
