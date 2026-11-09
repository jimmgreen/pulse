add_executable(pulse_picker_user_storage_test src/bench/picker_user_storage_test.cpp)
target_link_libraries(pulse_picker_user_storage_test PRIVATE ole32)
# Isolated picker host and standard-dialog bridge verification targets.
add_executable(pulse_picker_protocol_test src/bench/picker_protocol_test.cpp src/ipc/picker_protocol.cpp)

add_executable(pulse_picker_transport_test src/bench/picker_transport_test.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_picker_transport_test PRIVATE advapi32 bcrypt)

add_executable(pulse_picker_session_test src/bench/picker_session_test.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_picker_session_test PRIVATE advapi32 bcrypt)

add_executable(pulse_file_save_model_test src/bench/file_save_model_test.cpp src/ui/file_save_model.cpp)

add_executable(pulse_file_save_revalidation_test src/bench/file_save_revalidation_test.cpp
    src/ui/file_save_revalidation.cpp src/ui/file_save_model.cpp)

add_executable(pulse_picker_bridge_model_test src/bench/picker_bridge_model_test.cpp
    src/app/picker_bridge_model.cpp src/ipc/picker_protocol.cpp)

add_executable(pulse_picker_host_test src/bench/picker_host_test.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_picker_host_test PRIVATE advapi32 bcrypt user32 ole32 uuid)

add_executable(pulse_picker_client_test src/bench/picker_client_test.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_picker_client_test PRIVATE advapi32 bcrypt user32)

add_executable(pulse_picker_shell_results_test src/bench/picker_shell_results_test.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp)
target_link_libraries(pulse_picker_shell_results_test PRIVATE shell32 ole32 uuid)

add_executable(pulse_native_file_dialog_contract_test src/bench/native_file_dialog_contract_test.cpp)
target_sources(pulse_native_file_dialog_contract_test PRIVATE
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_native_file_dialog_contract_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_picker_legacy_model_test src/bench/picker_legacy_model_test.cpp
    src/ipc/picker_legacy_model.cpp src/ipc/picker_protocol.cpp)

add_executable(pulse_file_dialog_contract_test src/bench/file_dialog_contract_test.cpp
    src/shell_bridge/file_dialog_contract.cpp src/ipc/picker_protocol.cpp)

add_executable(pulse_file_dialog_wrapper_test src/bench/file_dialog_wrapper_test.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_wrapper_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_file_dialog_events_test src/bench/file_dialog_events_test.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_events_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_file_dialog_control_live_test WIN32 src/bench/file_dialog_control_live_test.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_control_live_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_legacy_adapter_test src/bench/legacy_adapter_test.cpp
    src/shell_bridge/legacy_adapter.cpp src/ipc/picker_legacy_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_legacy_adapter_test PRIVATE comdlg32 ole32 uuid advapi32 bcrypt user32)

# Exercise the caller bridge against the real isolated picker host.
target_sources(pulse_picker_host_test PRIVATE src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp
    src/shell_bridge/file_dialog_class_factory.cpp
    src/shell_bridge/file_dialog_activation_ex.cpp
    src/shell_bridge/file_dialog_iat.cpp src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/legacy_ansi_adapter.cpp
    src/shell_bridge/legacy_adapter.cpp src/ipc/picker_legacy_model.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp)
target_link_libraries(pulse_picker_host_test PRIVATE shell32 ole32 uuid comdlg32)

# Explicit standard activation adapter; no installed interception or registration.
add_executable(pulse_file_dialog_activation_test src/bench/file_dialog_activation_test.cpp
    src/shell_bridge/file_dialog_activation.cpp src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp
    src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_activation_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

# Controlled main-EXE x86/x64 static-IAT experiment, never installed system-wide.
add_executable(pulse_file_dialog_iat_test src/bench/file_dialog_iat_test.cpp
    src/shell_bridge/file_dialog_iat.cpp src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp
    src/shell_bridge/file_dialog_contract.cpp src/shell_bridge/picker_shell_results.cpp
    src/ui/file_save_model.cpp src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_iat_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

# Explicit ANSI common-dialog adapter; no interception or registration.
add_executable(pulse_legacy_ansi_adapter_test src/bench/legacy_ansi_adapter_test.cpp
    src/shell_bridge/legacy_ansi_adapter.cpp src/ipc/picker_legacy_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_legacy_ansi_adapter_test PRIVATE comdlg32 ole32 uuid advapi32 bcrypt user32)

# Both explicit legacy adapters share one per-thread common-dialog error source.
target_sources(pulse_legacy_adapter_test PRIVATE src/shell_bridge/legacy_dialog_error.cpp)
target_sources(pulse_picker_host_test PRIVATE src/shell_bridge/legacy_dialog_error.cpp)
target_sources(pulse_legacy_ansi_adapter_test PRIVATE src/shell_bridge/legacy_dialog_error.cpp
    src/shell_bridge/legacy_adapter.cpp)

# Explicit MULTI_QI activation adapter; no installed Ex interception.
add_executable(pulse_file_dialog_activation_ex_test src/bench/file_dialog_activation_ex_test.cpp
    src/shell_bridge/file_dialog_activation_ex.cpp src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp
    src/shell_bridge/file_dialog_contract.cpp src/shell_bridge/picker_shell_results.cpp
    src/ui/file_save_model.cpp src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_activation_ex_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

# Windows 10 1903+ process-scoped UTF-8 ACP; no machine locale changes.
add_executable(pulse_legacy_ansi_utf8_test src/bench/legacy_ansi_adapter_test.cpp
    src/bench/legacy_ansi_utf8_test.manifest
    src/shell_bridge/legacy_ansi_adapter.cpp src/shell_bridge/legacy_adapter.cpp
    src/shell_bridge/legacy_dialog_error.cpp src/ipc/picker_legacy_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_compile_definitions(pulse_legacy_ansi_utf8_test PRIVATE PULSE_TEST_REQUIRE_UTF8_ACP=1)
target_link_libraries(pulse_legacy_ansi_utf8_test PRIVATE comdlg32 ole32 uuid advapi32 bcrypt user32)

# Explicit class-object activation adapter, not an installed class-factory hook.
add_executable(pulse_file_dialog_class_factory_test src/bench/file_dialog_class_factory_test.cpp
    src/shell_bridge/file_dialog_class_factory.cpp src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/file_dialog_activation_ex.cpp src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp
    src/shell_bridge/file_dialog_events.cpp src/shell_bridge/file_dialog_contract.cpp
    src/shell_bridge/picker_shell_results.cpp src/ui/file_save_model.cpp
    src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_file_dialog_class_factory_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

# Explicitly loaded, non-shipping DLL for bridge lifetime verification only.
add_library(pulse_picker_bridge_probe SHARED src/shell_bridge/file_dialog_probe.cpp
    src/shell_bridge/file_dialog_iat.cpp src/shell_bridge/file_dialog_activation.cpp
    src/shell_bridge/file_dialog_wrapper.cpp src/shell_bridge/file_dialog_inprocess_session.cpp src/shell_bridge/file_dialog_events.cpp
    src/shell_bridge/file_dialog_contract.cpp src/shell_bridge/picker_shell_results.cpp
    src/ui/file_save_model.cpp src/ipc/picker_client.cpp src/common/picker_owner_guard.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_compile_definitions(pulse_picker_bridge_probe PRIVATE PULSE_BRIDGE_PROBE_DLL=1)
set_target_properties(pulse_picker_bridge_probe PROPERTIES OUTPUT_NAME "PulsePickerBridgeProbe")
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    target_sources(pulse_picker_bridge_probe PRIVATE src/shell_bridge/file_dialog_probe_x86.def)
else()
    target_sources(pulse_picker_bridge_probe PRIVATE src/shell_bridge/file_dialog_probe_x64.def)
endif()
target_link_libraries(pulse_picker_bridge_probe PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

# C ABI loader only: deliberately does not link any bridge implementation.
add_executable(pulse_file_dialog_probe_load_test src/bench/file_dialog_probe_load_test.cpp)
target_link_libraries(pulse_file_dialog_probe_load_test PRIVATE ole32 uuid user32)

# Loader uses only the public wire codec to construct fixture registrations.
target_sources(pulse_file_dialog_probe_load_test PRIVATE src/ipc/picker_protocol.cpp)

# Restricted local Save access model, not a production bridge capability.
add_executable(pulse_file_save_access_policy_test
    src/bench/file_save_access_policy_test.cpp
    src/ui/file_save_access_policy.cpp
    src/ui/file_save_revalidation.cpp
    src/ui/file_save_model.cpp)
target_link_libraries(pulse_file_save_access_policy_test PRIVATE advapi32)
add_executable(pulse_native_file_type_index_test EXCLUDE_FROM_ALL src/bench/native_file_type_index_test.cpp)
target_link_libraries(pulse_native_file_type_index_test PRIVATE ole32 shell32 uuid)

add_executable(pulse_native_file_dialog_events_probe EXCLUDE_FROM_ALL src/bench/native_file_dialog_events_probe.cpp)
target_link_libraries(pulse_native_file_dialog_events_probe PRIVATE ole32 shell32 uuid user32)

add_executable(pulse_native_file_dialog_multisink_probe EXCLUDE_FROM_ALL src/bench/native_file_dialog_multisink_probe.cpp)
target_link_libraries(pulse_native_file_dialog_multisink_probe PRIVATE ole32 shell32 uuid user32)

get_target_property(_inprocess_wrapper_test_sources pulse_file_dialog_wrapper_test SOURCES)
list(REMOVE_ITEM _inprocess_wrapper_test_sources src/bench/file_dialog_wrapper_test.cpp)
add_executable(pulse_file_dialog_inprocess_wrapper_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_inprocess_wrapper_test.cpp ${_inprocess_wrapper_test_sources})
target_link_libraries(pulse_file_dialog_inprocess_wrapper_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_file_dialog_custom_button_live_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_custom_button_live_test.cpp
    src/shell_bridge/file_dialog_ui_provider.cpp ${_inprocess_wrapper_test_sources})
target_link_libraries(pulse_file_dialog_custom_button_live_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_file_dialog_multiselect_live_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_multiselect_live_test.cpp
    src/shell_bridge/file_dialog_ui_provider.cpp ${_inprocess_wrapper_test_sources})
target_link_libraries(pulse_file_dialog_multiselect_live_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)
target_sources(pulse_file_dialog_inprocess_wrapper_test PRIVATE
    src/shell_bridge/file_dialog_activation.cpp src/shell_bridge/file_dialog_class_factory.cpp)

add_executable(pulse_native_save_events_probe EXCLUDE_FROM_ALL src/bench/native_save_events_probe.cpp)
target_link_libraries(pulse_native_save_events_probe PRIVATE ole32 shell32 uuid user32)

add_executable(pulse_file_dialog_save_live_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_save_live_test.cpp
    src/shell_bridge/file_dialog_ui_provider.cpp ${_inprocess_wrapper_test_sources})
target_link_libraries(pulse_file_dialog_save_live_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)

add_executable(pulse_native_open_extension_probe EXCLUDE_FROM_ALL src/bench/native_open_extension_probe.cpp)
target_link_libraries(pulse_native_open_extension_probe PRIVATE ole32 shell32 uuid user32)

add_executable(pulse_native_empty_properties_probe EXCLUDE_FROM_ALL src/bench/native_empty_properties_probe.cpp)
target_link_libraries(pulse_native_empty_properties_probe PRIVATE ole32 shell32 uuid)

# Experimental duplex callbacks stay separate from production capability admission.
add_executable(pulse_picker_event_protocol_test src/bench/picker_event_protocol_test.cpp src/ipc/picker_protocol.cpp)
add_executable(pulse_picker_event_state_test src/bench/picker_event_state_test.cpp src/ipc/picker_protocol.cpp)
add_executable(pulse_picker_event_client_test src/bench/picker_event_client_test.cpp
    src/ipc/picker_event_client.cpp src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_picker_event_client_test PRIVATE advapi32 bcrypt user32 ole32)

add_executable(pulse_picker_event_adapter_test src/bench/picker_event_adapter_test.cpp src/app/picker_event_adapter.cpp src/ipc/picker_protocol.cpp)

# Complete ABI transport: share the existing picker implementation in pulse.exe.
target_sources(pulse PRIVATE src/picker_inprocess/picker_ui_bootstrap.cpp src/picker_inprocess/picker_ui_bootstrap_v2.cpp)
set_property(SOURCE src/picker_inprocess/picker_ui_bootstrap.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,pulse>:PULSE_PICKER_EXE_HOST=1>")
add_executable(pulse_picker_remote_protocol_test EXCLUDE_FROM_ALL src/bench/picker_remote_protocol_test.cpp src/ipc/picker_remote_protocol.cpp)
add_executable(pulse_picker_remote_v2_test EXCLUDE_FROM_ALL src/bench/picker_inprocess_v2_test.cpp
    src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp src/common/picker_owner_guard.cpp)
target_compile_definitions(pulse_picker_remote_v2_test PRIVATE PULSE_REMOTE_PROVIDER_TEST=1)
target_link_libraries(pulse_picker_remote_v2_test PRIVATE ole32 user32 windowscodecs advapi32 bcrypt)
add_executable(pulse_picker_open_bench EXCLUDE_FROM_ALL src/bench/picker_open_bench.cpp
    src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp src/common/picker_owner_guard.cpp)
target_link_libraries(pulse_picker_open_bench PRIVATE ole32 user32 shell32 advapi32 bcrypt)
add_executable(pulse_file_dialog_remote_multiselect_live_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_multiselect_live_test.cpp
    src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp
    ${_inprocess_wrapper_test_sources})
target_compile_definitions(pulse_file_dialog_remote_multiselect_live_test PRIVATE PULSE_REMOTE_PROVIDER_TEST=1)
target_link_libraries(pulse_file_dialog_remote_multiselect_live_test PRIVATE shell32 ole32 uuid advapi32 bcrypt user32)
