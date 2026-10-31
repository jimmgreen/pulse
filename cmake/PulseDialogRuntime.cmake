# Runtime bridge dependencies. Included once launcher/runtime sources are ready.
add_library(pulse_detours STATIC
    third_party/detours/src/detours.cpp
    third_party/detours/src/modules.cpp
    third_party/detours/src/disasm.cpp
    third_party/detours/src/image.cpp
    third_party/detours/src/creatwth.cpp
    third_party/detours/src/disolx86.cpp
    third_party/detours/src/disolx64.cpp
    third_party/detours/src/disolia64.cpp
    third_party/detours/src/disolarm.cpp
    third_party/detours/src/disolarm64.cpp)
target_include_directories(pulse_detours PUBLIC third_party/detours/src)
# Detours 4.0.1 uses the legacy MSVC extension for cleanup jumps. Keep its
# upstream sources intact and isolate compatibility flags from Pulse code.
get_target_property(_detours_options pulse_detours COMPILE_OPTIONS)
list(REMOVE_ITEM _detours_options /permissive- /W4)
set_property(TARGET pulse_detours PROPERTY COMPILE_OPTIONS "${_detours_options}")
set_property(TARGET pulse_detours PROPERTY CXX_STANDARD 14)
target_compile_options(pulse_detours PRIVATE /W0 /permissive)

add_library(pulse_dialog_bundle STATIC
    src/common/dialog_bundle.cpp src/common/json_value.cpp)
target_link_libraries(pulse_dialog_bundle PRIVATE bcrypt)
add_executable(pulse_dialog_bundle_test EXCLUDE_FROM_ALL
    src/bench/dialog_bundle_test.cpp src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_bundle_test PRIVATE pulse_dialog_bundle)
add_library(pulse_dialog_admin_trust STATIC src/common/dialog_admin_trust.cpp)
target_link_libraries(pulse_dialog_admin_trust PUBLIC advapi32)
add_executable(pulse_dialog_admin_trust_test EXCLUDE_FROM_ALL src/bench/dialog_admin_trust_test.cpp)
target_link_libraries(pulse_dialog_admin_trust_test PRIVATE advapi32)
add_library(pulse_dialog_session STATIC src/common/dialog_session.cpp)
target_link_libraries(pulse_dialog_session PRIVATE advapi32 ole32 pulse_dialog_admin_trust)
add_executable(pulse_dialog_management_controller_test EXCLUDE_FROM_ALL
    src/bench/dialog_management_controller_test.cpp
    src/app/dialog_management_controller.cpp
    src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_management_controller_test PRIVATE
    pulse_dialog_bundle pulse_dialog_session user32 ole32 advapi32)
if(TARGET pulse)
    target_sources(pulse PRIVATE src/dialog_launcher/managed_launch_profile.cpp)
    target_link_libraries(pulse PRIVATE pulse_dialog_bundle pulse_dialog_session)
endif()
add_executable(pulse_dialog_launcher src/dialog_launcher/main.cpp
    src/dialog_launcher/managed_launch_profile.cpp)
add_executable(pulse_managed_launch_profile_test EXCLUDE_FROM_ALL
    src/bench/managed_launch_profile_test.cpp
    src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_launcher PRIVATE pulse_detours advapi32 ole32 pulse_dialog_bundle pulse_dialog_session)
target_link_libraries(pulse_managed_launch_profile_test PRIVATE pulse_dialog_bundle)
set_target_properties(pulse_dialog_launcher PROPERTIES OUTPUT_NAME "PulseDialogLauncher")

add_library(pulse_file_dialog_runtime SHARED
    src/shell_bridge/file_dialog_runtime.cpp
    src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_dialog_error.cpp
    src/shell_bridge/file_dialog_ui_provider.cpp
    src/shell_bridge/runtime_child_process.cpp
    src/shell_bridge/runtime_child_injection.cpp
    src/shell_bridge/runtime_child_session.cpp
    src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/file_dialog_activation_ex.cpp
    src/shell_bridge/file_dialog_class_factory.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp
    src/shell_bridge/file_dialog_events.cpp
    src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp
    src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp
    src/ipc/picker_transport.cpp
    src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_runtime PRIVATE pulse_detours shell32 ole32 uuid advapi32 bcrypt user32)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(pulse_file_dialog_runtime PRIVATE src/shell_bridge/file_dialog_runtime_x86.def)
    set_target_properties(pulse_file_dialog_runtime PROPERTIES OUTPUT_NAME "PulseFileDialogRuntime32")
else()
    target_sources(pulse_file_dialog_runtime PRIVATE src/shell_bridge/file_dialog_runtime_x64.def)
    set_target_properties(pulse_file_dialog_runtime PROPERTIES OUTPUT_NAME "PulseFileDialogRuntime64")
endif()
add_executable(pulse_file_dialog_runtime_policy_test src/bench/file_dialog_runtime_policy_test.cpp
    src/shell_bridge/file_dialog_contract.cpp src/ipc/picker_protocol.cpp)

# Test the runtime routing functions without installing hooks in the test process.
get_target_property(_runtime_entry_sources pulse_file_dialog_runtime SOURCES)
list(FILTER _runtime_entry_sources EXCLUDE REGEX "file_dialog_runtime\\.(cpp|def)$|file_dialog_runtime_x(86|64)\\.def$")
add_executable(pulse_file_dialog_runtime_entry_test
    src/bench/file_dialog_runtime_entry_test.cpp ${_runtime_entry_sources})
target_include_directories(pulse_file_dialog_runtime_entry_test PRIVATE third_party/detours/src)
target_link_libraries(pulse_file_dialog_runtime_entry_test PRIVATE pulse_detours shell32 ole32 uuid advapi32 bcrypt user32)
add_executable(pulse_runtime_child_process_test
    src/bench/runtime_child_process_test.cpp
    src/bench/runtime_child_open_cancel.cpp
    src/shell_bridge/runtime_child_injection.cpp)
target_link_libraries(pulse_runtime_child_process_test PRIVATE pulse_detours advapi32 shell32 ole32 uuid)
# Explicit launcher-only in-process UI experiment. The ordinary runtime keeps
# its existing gates; this binary is selected by its separate DLL pathname.
get_target_property(_ui_proof_runtime_sources pulse_file_dialog_runtime SOURCES)
add_library(pulse_file_dialog_ui_proof_runtime SHARED EXCLUDE_FROM_ALL
    ${_ui_proof_runtime_sources})
target_compile_definitions(pulse_file_dialog_ui_proof_runtime PRIVATE PULSE_DIALOG_UI_PROOF=1)
target_link_libraries(pulse_file_dialog_ui_proof_runtime PRIVATE pulse_detours shell32 ole32 uuid advapi32 bcrypt user32)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set_target_properties(pulse_file_dialog_ui_proof_runtime PROPERTIES OUTPUT_NAME PulseFileDialogRuntimeUiProof32R8)
else()
    set_target_properties(pulse_file_dialog_ui_proof_runtime PROPERTIES OUTPUT_NAME PulseFileDialogRuntimeUiProof64R8)
endif()

# Pure call-through diagnostics: controlled seams never launch a process.
add_executable(pulse_runtime_child_diagnostic_test EXCLUDE_FROM_ALL
    src/bench/runtime_child_diagnostic_test.cpp
    src/shell_bridge/runtime_child_process.cpp)
target_sources(pulse_file_dialog_ui_proof_runtime PRIVATE
    src/shell_bridge/runtime_child_observer.cpp)
target_sources(pulse_runtime_child_diagnostic_test PRIVATE
    src/shell_bridge/runtime_child_observer.cpp)
target_compile_definitions(pulse_runtime_child_diagnostic_test PRIVATE PULSE_DIALOG_UI_PROOF=1)
target_link_libraries(pulse_runtime_child_diagnostic_test PRIVATE advapi32)
target_compile_definitions(pulse_runtime_child_diagnostic_test PRIVATE PULSE_RUNTIME_CHILD_ROUTING_TEST=1)

add_executable(pulse_runtime_parent_ready_test EXCLUDE_FROM_ALL src/bench/runtime_parent_ready_test.cpp)

# The COM wrapper returns a Windows-owned empty property store for the supported
# read-only collection contract. Link every target that compiles that wrapper.
get_property(_pulse_dialog_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(_pulse_dialog_target IN LISTS _pulse_dialog_targets)
    get_target_property(_pulse_dialog_sources ${_pulse_dialog_target} SOURCES)
    if("src/shell_bridge/file_dialog_wrapper.cpp" IN_LIST _pulse_dialog_sources)
        target_link_libraries(${_pulse_dialog_target} PRIVATE propsys advapi32)
        target_sources(${_pulse_dialog_target} PRIVATE src/shell_bridge/file_dialog_client_state.cpp src/shell_bridge/file_dialog_properties.cpp)
    endif()
endforeach()
unset(_pulse_dialog_targets)
unset(_pulse_dialog_target)
unset(_pulse_dialog_sources)

add_executable(pulse_process_launch_path_test EXCLUDE_FROM_ALL src/bench/process_launch_path_test.cpp)

add_executable(pulse_legacy_call_observer_test EXCLUDE_FROM_ALL src/bench/legacy_call_observer_test.cpp)
target_link_libraries(pulse_file_dialog_ui_proof_runtime PRIVATE comdlg32)
target_link_libraries(pulse_file_dialog_runtime_entry_test PRIVATE comdlg32)

add_executable(pulse_file_dialog_client_state_test EXCLUDE_FROM_ALL src/bench/file_dialog_client_state_test.cpp src/shell_bridge/file_dialog_client_state.cpp)
target_link_libraries(pulse_file_dialog_client_state_test PRIVATE advapi32)

add_executable(pulse_file_dialog_properties_test EXCLUDE_FROM_ALL src/bench/file_dialog_properties_test.cpp src/shell_bridge/file_dialog_properties.cpp)
target_link_libraries(pulse_file_dialog_properties_test PRIVATE propsys ole32 uuid)

target_link_libraries(pulse_file_dialog_runtime PRIVATE comdlg32)

add_executable(pulse_legacy_native_cwd_test EXCLUDE_FROM_ALL src/bench/legacy_native_cwd_test.cpp)
target_link_libraries(pulse_legacy_native_cwd_test PRIVATE comdlg32 user32)


add_executable(pulse_dialog_managed_entry_smoke EXCLUDE_FROM_ALL src/bench/dialog_managed_entry_smoke.cpp)
target_link_libraries(pulse_dialog_managed_entry_smoke PRIVATE ole32 shell32 uuid comdlg32 user32)

add_executable(pulse_process_token_restriction_test EXCLUDE_FROM_ALL src/bench/process_token_restriction_test.cpp)
target_link_libraries(pulse_process_token_restriction_test PRIVATE advapi32)

# Explicit protected administrator sessions share the existing bundle verifier.
foreach(_admin_target pulse_dialog_launcher pulse_file_dialog_runtime pulse_file_dialog_ui_proof_runtime pulse_file_dialog_runtime_entry_test)
    target_link_libraries(${_admin_target} PRIVATE pulse_dialog_admin_trust pulse_dialog_bundle)
endforeach()

add_executable(pulse_legacy_com_provider_test EXCLUDE_FROM_ALL
    src/bench/legacy_com_provider_test.cpp src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_dialog_error.cpp)
target_link_libraries(pulse_legacy_com_provider_test PRIVATE comdlg32 shell32 ole32 uuid user32)
