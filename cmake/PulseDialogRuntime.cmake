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

add_library(pulse_dialog_sink_hooks STATIC src/shell_bridge/dialog_sink_hooks.cpp)
target_link_libraries(pulse_dialog_sink_hooks PUBLIC pulse_detours)
add_executable(pulse_dialog_sink_hooks_test EXCLUDE_FROM_ALL src/bench/dialog_sink_hooks_test.cpp)
target_link_libraries(pulse_dialog_sink_hooks_test PRIVATE pulse_dialog_sink_hooks ole32 uuid user32)

add_library(pulse_dialog_core_fixture_v1 SHARED EXCLUDE_FROM_ALL src/bench/dialog_core_fixture.cpp)
target_compile_definitions(pulse_dialog_core_fixture_v1 PRIVATE PULSE_TEST_CORE_BUILD_ID=101)
target_link_libraries(pulse_dialog_core_fixture_v1 PRIVATE pulse_dialog_sink_hooks ole32 uuid)
add_library(pulse_dialog_core_fixture_v2 SHARED EXCLUDE_FROM_ALL src/bench/dialog_core_fixture.cpp)
target_compile_definitions(pulse_dialog_core_fixture_v2 PRIVATE PULSE_TEST_CORE_BUILD_ID=202)
target_link_libraries(pulse_dialog_core_fixture_v2 PRIVATE pulse_dialog_sink_hooks ole32 uuid)
add_executable(pulse_dialog_stub_hot_switch_test EXCLUDE_FROM_ALL src/bench/dialog_stub_hot_switch_test.cpp)
target_link_libraries(pulse_dialog_stub_hot_switch_test PRIVATE pulse_dialog_bridge_paths ole32 uuid)
# Real Core DLL: legacy A/W and SHBrowseForFolder sink routes (pass the Core path).
add_executable(pulse_dialog_core_legacy_route_test EXCLUDE_FROM_ALL src/bench/dialog_core_legacy_route_test.cpp
    src/ipc/picker_transport.cpp src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_dialog_core_legacy_route_test PRIVATE comdlg32 shell32 ole32 user32 advapi32 bcrypt)

add_library(pulse_dialog_bundle STATIC
    src/common/dialog_bundle.cpp src/common/json_value.cpp)
target_link_libraries(pulse_dialog_bundle PRIVATE bcrypt)
add_executable(pulse_dialog_bundle_test EXCLUDE_FROM_ALL
    src/bench/dialog_bundle_test.cpp src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_bundle_test PRIVATE pulse_dialog_bundle)
add_executable(pulse_dialog_bundle_arch_test EXCLUDE_FROM_ALL src/bench/dialog_bundle_arch_test.cpp)
target_link_libraries(pulse_dialog_bundle_arch_test PRIVATE pulse_dialog_bundle)
add_executable(pulse_dialog_thin_bundle_test EXCLUDE_FROM_ALL src/bench/dialog_thin_bundle_test.cpp)
target_link_libraries(pulse_dialog_thin_bundle_test PRIVATE pulse_dialog_bundle)
add_library(pulse_dialog_admin_trust STATIC src/common/dialog_admin_trust.cpp)
target_link_libraries(pulse_dialog_admin_trust PUBLIC advapi32)
add_executable(pulse_dialog_admin_trust_test EXCLUDE_FROM_ALL src/bench/dialog_admin_trust_test.cpp)
target_link_libraries(pulse_dialog_admin_trust_test PRIVATE advapi32)
add_library(pulse_dialog_bridge_paths STATIC src/common/dialog_bridge_paths.cpp)
target_link_libraries(pulse_dialog_bridge_paths PUBLIC pulse_dialog_bundle pulse_dialog_admin_trust advapi32 shell32)
add_executable(pulse_dialog_bridge_paths_test EXCLUDE_FROM_ALL src/bench/dialog_bridge_paths_test.cpp)
target_link_libraries(pulse_dialog_bridge_paths_test PRIVATE pulse_dialog_bridge_paths pulse_dialog_admin_trust)

# Phase 2: Shared memory state page, transactional HKLM COM registration, PulseDialogBroker service & WOW64 helper.
add_library(pulse_dialog_bridge_state STATIC src/common/dialog_bridge_state.cpp)
target_link_libraries(pulse_dialog_bridge_state PUBLIC advapi32)

add_library(pulse_dialog_com_hklm STATIC src/common/dialog_com_hklm.cpp)
target_link_libraries(pulse_dialog_com_hklm PUBLIC advapi32)

add_library(pulse_dialog_broker_lib STATIC
    src/dialog_broker/deferred_cleanup.cpp
    src/dialog_broker/process_admission.cpp
    src/dialog_broker/etw_process_watcher.cpp
    src/dialog_broker/stub_injector.cpp)
target_link_libraries(pulse_dialog_broker_lib PUBLIC pulse_dialog_bridge_state pulse_dialog_bridge_paths pulse_dialog_admin_trust advapi32 psapi)

add_executable(pulse_dialog_broker EXCLUDE_FROM_ALL src/dialog_broker/broker_service_main.cpp)
target_link_libraries(pulse_dialog_broker PRIVATE pulse_dialog_broker_lib pulse_dialog_bridge_paths pulse_dialog_com_hklm pulse_runtime_log advapi32 shell32)
set_target_properties(pulse_dialog_broker PROPERTIES OUTPUT_NAME "PulseDialogBroker")

add_executable(pulse_dialog_broker_cleanup_test EXCLUDE_FROM_ALL src/bench/dialog_broker_cleanup_test.cpp)
target_link_libraries(pulse_dialog_broker_cleanup_test PRIVATE pulse_dialog_broker_lib pulse_dialog_bridge_paths)

add_executable(pulse_dialog_inject32 EXCLUDE_FROM_ALL src/dialog_broker/inject32_main.cpp)
target_link_libraries(pulse_dialog_inject32 PRIVATE pulse_dialog_broker_lib pulse_dialog_admin_trust advapi32)
set_target_properties(pulse_dialog_inject32 PROPERTIES OUTPUT_NAME "PulseDialogInject32")

add_executable(pulse_dialog_phase2_test EXCLUDE_FROM_ALL
    src/bench/dialog_phase2_test.cpp
    src/ipc/picker_transport.cpp
    src/ipc/picker_protocol.cpp)
target_link_libraries(pulse_dialog_phase2_test PRIVATE
    pulse_dialog_broker_lib pulse_dialog_bridge_paths pulse_dialog_bridge_state
    pulse_dialog_com_hklm advapi32 shell32 user32 ole32 uuid)

# Phase 3: Layer 3 non-invasive window-level fallback (PulseDialogOverlay.exe).
option(PULSE_OVERLAY_UIACCESS "Embed uiAccess=true manifest in PulseDialogOverlay.exe (requires Authenticode signing + secure install location)" OFF)
add_library(pulse_dialog_overlay_lib STATIC
    src/dialog_overlay/dialog_classifier.cpp
    src/dialog_overlay/companion_bar.cpp)
target_link_libraries(pulse_dialog_overlay_lib PUBLIC pulse_dialog_bridge_state user32 gdi32 shell32 ole32)

add_executable(pulse_dialog_overlay WIN32 EXCLUDE_FROM_ALL
    src/dialog_overlay/overlay_main.cpp)
target_link_libraries(pulse_dialog_overlay PRIVATE pulse_dialog_overlay_lib pulse_dialog_bridge_state user32 gdi32 shell32 ole32)
set_target_properties(pulse_dialog_overlay PROPERTIES OUTPUT_NAME "PulseDialogOverlay")
if(MSVC)
    if(PULSE_OVERLAY_UIACCESS)
        set(_overlay_manifest "${CMAKE_SOURCE_DIR}/src/dialog_overlay/overlay_uiaccess.manifest")
    else()
        set(_overlay_manifest "${CMAKE_SOURCE_DIR}/src/dialog_overlay/overlay_dev.manifest")
    endif()
    target_link_options(pulse_dialog_overlay PRIVATE "/MANIFEST:EMBED" "/MANIFESTINPUT:${_overlay_manifest}")
endif()

add_executable(pulse_dialog_overlay_test EXCLUDE_FROM_ALL src/bench/dialog_overlay_test.cpp)
target_link_libraries(pulse_dialog_overlay_test PRIVATE pulse_dialog_overlay_lib pulse_dialog_bridge_state user32 gdi32 ole32)

# End-to-end verification: real pulse.exe warm vs cold V2 remote host + real Stub64 -> Core64 integration.
add_executable(pulse_warm_remote_e2e_test EXCLUDE_FROM_ALL
    src/bench/warm_remote_e2e_test.cpp
    src/shell_bridge/file_dialog_remote_provider.cpp
    src/ipc/picker_remote_protocol.cpp
    src/ipc/picker_transport.cpp
    src/ipc/picker_protocol.cpp
    src/common/picker_owner_guard.cpp)
target_link_libraries(pulse_warm_remote_e2e_test PRIVATE
    pulse_dialog_bridge_paths pulse_dialog_admin_trust ole32 uuid user32 advapi32 shell32)
# Administrator apps of the same user and session: the logon task (Task Scheduler,
# highest available, interactive token) that runs PulseDialogAdminAuto.exe.
add_library(pulse_dialog_admin_auto_task STATIC src/common/dialog_admin_auto_task.cpp)
target_link_libraries(pulse_dialog_admin_auto_task PUBLIC pulse_dialog_bundle pulse_dialog_admin_trust
    taskschd oleaut32 ole32 advapi32 shell32)
add_executable(pulse_dialog_admin_auto_task_test EXCLUDE_FROM_ALL src/bench/dialog_admin_auto_task_test.cpp)
target_link_libraries(pulse_dialog_admin_auto_task_test PRIVATE pulse_dialog_admin_auto_task)
add_library(pulse_dialog_session STATIC src/common/dialog_session.cpp)
target_link_libraries(pulse_dialog_session PRIVATE advapi32 ole32 pulse_dialog_admin_trust)
add_library(pulse_dialog_process_diagnostics STATIC src/common/dialog_process_diagnostics.cpp)
target_link_libraries(pulse_dialog_process_diagnostics PUBLIC advapi32 user32)
add_library(pulse_dialog_log_excerpt STATIC src/common/dialog_log_excerpt.cpp src/common/json_value.cpp)
add_executable(pulse_dialog_log_excerpt_test EXCLUDE_FROM_ALL src/bench/dialog_log_excerpt_test.cpp)
target_link_libraries(pulse_dialog_log_excerpt_test PRIVATE pulse_dialog_log_excerpt)
add_executable(pulse_dialog_process_diagnostics_test EXCLUDE_FROM_ALL src/bench/dialog_process_diagnostics_test.cpp)
target_link_libraries(pulse_dialog_process_diagnostics_test PRIVATE pulse_dialog_process_diagnostics)
add_executable(pulse_dialog_management_controller_test EXCLUDE_FROM_ALL
    src/bench/dialog_management_controller_test.cpp
    src/app/dialog_management_controller.cpp
    src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_management_controller_test PRIVATE
    pulse_dialog_bundle pulse_dialog_bridge_paths pulse_dialog_session pulse_dialog_process_diagnostics pulse_dialog_log_excerpt
    pulse_dialog_admin_auto_task pulse_crash_reporter pulse_dialog_user_auto user32 ole32 advapi32)
add_executable(pulse_dialog_controller_dual_test EXCLUDE_FROM_ALL
    src/bench/dialog_controller_dual_test.cpp src/app/dialog_management_controller.cpp
    src/common/dialog_auto_arch_status.cpp src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_controller_dual_test PRIVATE pulse_dialog_process_diagnostics pulse_dialog_log_excerpt
    pulse_dialog_admin_auto_task pulse_crash_reporter pulse_dialog_user_auto)
target_link_libraries(pulse_dialog_controller_dual_test PRIVATE
    pulse_dialog_bundle pulse_dialog_bridge_paths pulse_dialog_session pulse_dialog_auto_registration user32 ole32 advapi32)
if(TARGET pulse)
    target_link_libraries(pulse PRIVATE pulse_dialog_process_diagnostics pulse_dialog_log_excerpt pulse_dialog_admin_auto_task)
    target_sources(pulse PRIVATE src/dialog_launcher/managed_launch_profile.cpp)
    target_link_libraries(pulse PRIVATE pulse_dialog_bundle pulse_dialog_bridge_paths pulse_dialog_session)
endif()
add_executable(pulse_dialog_launcher src/dialog_launcher/main.cpp
    src/dialog_launcher/managed_launch_profile.cpp src/dialog_launcher/target_preflight.cpp)
add_executable(pulse_managed_launch_profile_test EXCLUDE_FROM_ALL
    src/bench/managed_launch_profile_test.cpp
    src/dialog_launcher/managed_launch_profile.cpp)
target_link_libraries(pulse_dialog_launcher PRIVATE pulse_detours advapi32 ole32 pulse_dialog_bundle pulse_dialog_session)
target_link_libraries(pulse_managed_launch_profile_test PRIVATE pulse_dialog_bundle)
set_target_properties(pulse_dialog_launcher PROPERTIES OUTPUT_NAME "PulseDialogLauncher")

add_library(pulse_file_dialog_runtime SHARED
    src/shell_bridge/file_dialog_runtime.cpp
    src/shell_bridge/browse_folder_adapter.cpp
    src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_hook_host.cpp src/shell_bridge/legacy_dialog_error.cpp
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
    target_sources(pulse_file_dialog_runtime PRIVATE src/shell_bridge/file_dialog_runtime_x86.def
        src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp)
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
# Experimental explicit bootstrap, isolated from installed/managed DLL output.
add_executable(pulse_dialog_auto_binding_test EXCLUDE_FROM_ALL
    src/bench/dialog_auto_binding_test.cpp src/common/dialog_auto_binding.cpp)
target_link_libraries(pulse_dialog_auto_binding_test PRIVATE advapi32)
set(_admin_auto_sources ${_ui_proof_runtime_sources})
list(FILTER _admin_auto_sources EXCLUDE REGEX "file_dialog_runtime.*\\.def$")
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND _admin_auto_sources src/shell_bridge/file_dialog_auto_runtime_x64.def)
else()
    list(APPEND _admin_auto_sources src/shell_bridge/file_dialog_auto_runtime_x86.def)
endif()
add_library(pulse_file_dialog_auto_entry_runtime SHARED EXCLUDE_FROM_ALL ${_admin_auto_sources})
target_sources(pulse_file_dialog_auto_entry_runtime PRIVATE src/shell_bridge/runtime_auto_imports.cpp
    src/shell_bridge/runtime_auto_lease.cpp src/common/dialog_auto_binding.cpp
    src/shell_bridge/direct_reveal_hook.cpp src/app/open_request_pipe.cpp)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(pulse_file_dialog_auto_entry_runtime PRIVATE
        src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp)
endif()
target_compile_definitions(pulse_file_dialog_auto_entry_runtime PRIVATE
    PULSE_RUNTIME_AUTO_ENTRY=1 PULSE_DIALOG_HISTORY_GLOBAL=1 PULSE_RUNTIME_REMOTE_PROVIDER=1)
target_link_libraries(pulse_file_dialog_auto_entry_runtime PRIVATE pulse_runtime_log pulse_detours pulse_dialog_sink_hooks shell32 ole32 uuid advapi32 bcrypt user32 comdlg32 pulse_dialog_admin_trust pulse_dialog_bundle)
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_auto_runtime_name "PulseFileDialogAutoRuntime64")
else()
    set(_auto_runtime_name "PulseFileDialogAutoRuntime32")
endif()
set_target_properties(pulse_file_dialog_auto_entry_runtime PROPERTIES OUTPUT_NAME "${_auto_runtime_name}")

# Non-elevated adaptive user-mode auto-attach library (shared by pulse.exe and PulseDialogAdminAuto.exe --user-adaptive).
add_library(pulse_dialog_user_auto STATIC
    src/common/dialog_adaptive_attach.cpp
    src/dialog_launcher/user_auto_attach.cpp
    src/dialog_launcher/window_show_watch.cpp
    src/common/dialog_auto_binding.cpp)
target_link_libraries(pulse_dialog_user_auto PUBLIC
    pulse_dialog_bundle pulse_dialog_admin_trust pulse_dialog_auto_registration
    advapi32 ole32 shell32 user32 version)
# Read-only field probe for native dialogs Pulse did not replace (not shipped).
add_executable(pulse_dialog_coverage_probe EXCLUDE_FROM_ALL src/bench/dialog_coverage_probe.cpp)
target_link_libraries(pulse_dialog_coverage_probe PRIVATE pulse_dialog_user_auto psapi)
add_executable(pulse_dialog_adaptive_attach_test EXCLUDE_FROM_ALL
    src/bench/dialog_adaptive_attach_test.cpp)
target_link_libraries(pulse_dialog_adaptive_attach_test PRIVATE pulse_dialog_user_auto comdlg32)
add_executable(pulse_dialog_window_watch_test EXCLUDE_FROM_ALL
    src/bench/dialog_window_watch_test.cpp src/dialog_launcher/window_show_watch.cpp)
target_link_libraries(pulse_dialog_window_watch_test PRIVATE user32 ole32)

# The elevated same-session helper the logon task runs: finds administrator apps of
# this user and session, publishes their binding and attaches the runtime above.
add_executable(pulse_dialog_admin_auto WIN32
    src/dialog_launcher/admin_auto_main.cpp src/dialog_launcher/admin_auto_attach.cpp
    src/dialog_launcher/admin_session_prune.cpp src/dialog_launcher/admin_version_retry.cpp)
target_link_libraries(pulse_dialog_admin_auto PRIVATE pulse_dialog_user_auto pulse_dialog_bundle pulse_dialog_admin_trust pulse_dialog_auto_registration
    pulse_runtime_log advapi32 ole32 shell32 bcrypt user32)
set_target_properties(pulse_dialog_admin_auto PROPERTIES OUTPUT_NAME "PulseDialogAdminAuto")
if(TARGET pulse)
    target_sources(pulse PRIVATE src/app/user_dialog_auto_controller.cpp)
    target_link_libraries(pulse PRIVATE pulse_dialog_user_auto)
endif()
add_executable(pulse_dialog_admin_auto_attach_test EXCLUDE_FROM_ALL
    src/bench/dialog_admin_auto_attach_test.cpp src/dialog_launcher/admin_auto_attach.cpp src/dialog_launcher/window_show_watch.cpp
    src/dialog_launcher/admin_session_prune.cpp src/dialog_launcher/admin_version_retry.cpp src/common/dialog_auto_binding.cpp)
target_link_libraries(pulse_dialog_admin_auto_attach_test PRIVATE pulse_dialog_bundle pulse_dialog_admin_trust
    pulse_dialog_auto_registration pulse_runtime_log advapi32 ole32 shell32 bcrypt user32)
add_executable(pulse_file_dialog_auto_entry_test EXCLUDE_FROM_ALL
    src/bench/file_dialog_auto_entry_test.cpp src/shell_bridge/runtime_auto_imports.cpp
    src/shell_bridge/runtime_auto_lease.cpp src/common/dialog_auto_binding.cpp
    src/shell_bridge/direct_reveal_hook.cpp src/app/open_request_pipe.cpp ${_runtime_entry_sources})
target_compile_definitions(pulse_file_dialog_auto_entry_test PRIVATE PULSE_RUNTIME_AUTO_ENTRY=1)
target_include_directories(pulse_file_dialog_auto_entry_test PRIVATE third_party/detours/src)
target_link_libraries(pulse_file_dialog_auto_entry_test PRIVATE comctl32 pulse_detours pulse_dialog_sink_hooks shell32 ole32 uuid advapi32 bcrypt user32 comdlg32 pulse_dialog_admin_trust pulse_dialog_bundle)

add_library(pulse_runtime_auto_imports_probe SHARED EXCLUDE_FROM_ALL
    src/bench/runtime_auto_imports_probe.cpp src/shell_bridge/runtime_auto_imports.cpp)
target_link_libraries(pulse_runtime_auto_imports_probe PRIVATE ole32 uuid)
set_target_properties(pulse_runtime_auto_imports_probe PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/auto-import-test")
add_executable(pulse_runtime_auto_imports_test EXCLUDE_FROM_ALL
    src/bench/runtime_auto_imports_test.cpp src/shell_bridge/runtime_auto_imports.cpp)
target_link_libraries(pulse_runtime_auto_imports_test PRIVATE ole32 uuid)

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
        target_sources(${_pulse_dialog_target} PRIVATE src/shell_bridge/file_dialog_client_state.cpp src/shell_bridge/file_dialog_properties.cpp src/shell_bridge/save_as_item_model.cpp)
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
    target_link_libraries(${_admin_target} PRIVATE pulse_dialog_admin_trust pulse_dialog_bundle pulse_dialog_sink_hooks)
endforeach()

add_executable(pulse_legacy_com_provider_test EXCLUDE_FROM_ALL
    src/bench/legacy_com_provider_test.cpp src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_hook_host.cpp src/shell_bridge/legacy_dialog_error.cpp)
target_link_libraries(pulse_legacy_com_provider_test PRIVATE comdlg32 shell32 ole32 uuid user32)

# Browse family uses public Detours entrypoints, including a real UI mode.
add_executable(pulse_browse_folder_runtime_test EXCLUDE_FROM_ALL
    src/bench/browse_folder_runtime_test.cpp ${_runtime_entry_sources})
target_include_directories(pulse_browse_folder_runtime_test PRIVATE third_party/detours/src)
target_sources(pulse_browse_folder_runtime_test PRIVATE
    src/shell_bridge/file_dialog_client_state.cpp src/shell_bridge/file_dialog_properties.cpp src/shell_bridge/save_as_item_model.cpp)
target_link_libraries(pulse_browse_folder_runtime_test PRIVATE pulse_detours pulse_dialog_sink_hooks shell32 ole32 uuid
    advapi32 bcrypt user32 comdlg32 comctl32 propsys pulse_dialog_admin_trust pulse_dialog_bundle)

add_executable(pulse_browse_folder_managed_smoke EXCLUDE_FROM_ALL src/bench/browse_folder_managed_smoke.cpp)
target_link_libraries(pulse_browse_folder_managed_smoke PRIVATE shell32 ole32 user32)

# Read-only dual-architecture support status; never activates or registers helpers.
add_executable(pulse_dialog_auto_arch_status_test EXCLUDE_FROM_ALL
    src/bench/dialog_auto_arch_status_test.cpp src/common/dialog_auto_arch_status.cpp)
target_link_libraries(pulse_dialog_auto_arch_status_test PRIVATE advapi32)
target_sources(pulse_dialog_management_controller_test PRIVATE src/common/dialog_auto_arch_status.cpp)
if(TARGET pulse)
    target_sources(pulse PRIVATE src/common/dialog_auto_arch_status.cpp)
endif()
# Continuation: real API entry routing, including loader-created imports.
add_executable(pulse_dialog_entry_matrix EXCLUDE_FROM_ALL src/bench/dialog_entry_matrix.cpp)
target_link_libraries(pulse_dialog_entry_matrix PRIVATE ole32 shell32 uuid user32 gdi32)
add_library(pulse_dialog_entry_plugin SHARED EXCLUDE_FROM_ALL src/bench/dialog_entry_plugin.cpp)
target_link_libraries(pulse_dialog_entry_plugin PRIVATE ole32 uuid)
set_target_properties(pulse_dialog_entry_plugin PROPERTIES OUTPUT_NAME PulseDialogEntryPlugin)
add_library(pulse_dialog_entry_delay SHARED EXCLUDE_FROM_ALL src/bench/dialog_entry_plugin.cpp)
target_compile_definitions(pulse_dialog_entry_delay PRIVATE PULSE_ENTRY_DELAY=1)
target_link_libraries(pulse_dialog_entry_delay PRIVATE ole32 uuid delayimp)
target_link_options(pulse_dialog_entry_delay PRIVATE /DELAYLOAD:ole32.dll)
set_target_properties(pulse_dialog_entry_delay PROPERTIES OUTPUT_NAME PulseDialogEntryDelay)
add_executable(pulse_file_dialog_save_as_item_test EXCLUDE_FROM_ALL src/bench/file_dialog_save_as_item_test.cpp)
target_sources(pulse_file_dialog_save_as_item_test PRIVATE src/shell_bridge/save_as_item_model.cpp src/shell_bridge/file_dialog_properties.cpp)
target_link_libraries(pulse_file_dialog_save_as_item_test PRIVATE shell32 ole32 uuid propsys user32)
# Read-only registry-context comparison and documented desktop launch relay.
add_executable(pulse_dialog_registry_context_test WIN32 EXCLUDE_FROM_ALL
    src/bench/dialog_registry_context_test.cpp src/common/json_value.cpp src/setup/setup_desktop_launch.cpp)
target_link_libraries(pulse_dialog_registry_context_test PRIVATE advapi32 shell32 ole32 oleaut32 uuid user32)

# Explicit, user-operated acceptance. No scripted input or automatic next Show.
add_executable(pulse_dialog_manual_acceptance_test WIN32 EXCLUDE_FROM_ALL
    src/bench/dialog_manual_acceptance_test.cpp)
target_link_libraries(pulse_dialog_manual_acceptance_test PRIVATE
    shell32 ole32 uuid propsys advapi32 user32 gdi32 bcrypt)

# Isolated registration backend; never writes the user's registration.
add_executable(pulse_dialog_auto_disabled_rebind_test EXCLUDE_FROM_ALL
    src/bench/dialog_auto_disabled_rebind_test.cpp)
target_link_libraries(pulse_dialog_auto_disabled_rebind_test PRIVATE advapi32)

add_executable(pulse_caller_callback_invocation_test EXCLUDE_FROM_ALL
    src/bench/caller_callback_invocation_test.cpp)
add_executable(pulse_picker_shell_filter_role_test EXCLUDE_FROM_ALL
    src/bench/picker_shell_filter_role_test.cpp)
add_executable(pulse_picker_shell_filtered_listing_test EXCLUDE_FROM_ALL
    src/bench/picker_shell_filtered_listing_test.cpp src/index/content_result_store.cpp)
target_link_libraries(pulse_picker_shell_filtered_listing_test PRIVATE
    pulse_sqlite_core advapi32 ole32 user32)

add_executable(pulse_launcher_preflight_test EXCLUDE_FROM_ALL
    src/bench/launcher_preflight_test.cpp src/dialog_launcher/target_preflight.cpp)

# Shared read-only target preflight for the settings launch confirmation.
foreach(_target pulse pulse_dialog_management_controller_test pulse_dialog_controller_dual_test)
    if(TARGET ${_target})
        target_sources(${_target} PRIVATE src/dialog_launcher/target_preflight.cpp)
    endif()
endforeach()

# Explicit managed admin launch uses the system Shell runas consent flow.
target_link_libraries(pulse_dialog_management_controller_test PRIVATE shell32)
target_link_libraries(pulse_dialog_controller_dual_test PRIVATE shell32)

# Managed dialog outcomes use the same bounded asynchronous local log as auto mode.
target_compile_definitions(pulse_file_dialog_runtime PRIVATE PULSE_DIALOG_HISTORY_GLOBAL=1)
target_link_libraries(pulse_file_dialog_runtime PRIVATE pulse_runtime_log shell32)

# Local application history reuses the bounded privacy-aware log collector.
add_library(pulse_dialog_takeover_history STATIC src/common/dialog_takeover_history.cpp)
target_link_libraries(pulse_dialog_takeover_history PUBLIC pulse_dialog_log_excerpt advapi32)
add_executable(pulse_dialog_takeover_history_test EXCLUDE_FROM_ALL src/bench/dialog_takeover_history_test.cpp)
target_link_libraries(pulse_dialog_takeover_history_test PRIVATE pulse_dialog_takeover_history)
add_executable(pulse_dialog_auto_global_history_test EXCLUDE_FROM_ALL
    src/bench/dialog_auto_global_history_test.cpp src/shell_bridge/runtime_auto_imports.cpp
    src/shell_bridge/file_dialog_client_state.cpp src/shell_bridge/file_dialog_properties.cpp src/shell_bridge/save_as_item_model.cpp
    src/shell_bridge/runtime_auto_lease.cpp src/common/dialog_auto_binding.cpp ${_runtime_entry_sources})
target_compile_definitions(pulse_dialog_auto_global_history_test PRIVATE PULSE_RUNTIME_AUTO_ENTRY=1 PULSE_DIALOG_HISTORY_GLOBAL=1)
target_include_directories(pulse_dialog_auto_global_history_test PRIVATE third_party/detours/src)
target_link_libraries(pulse_dialog_auto_global_history_test PRIVATE pulse_runtime_log pulse_dialog_takeover_history
    pulse_detours pulse_dialog_sink_hooks shell32 ole32 uuid advapi32 bcrypt user32 comdlg32 comctl32 propsys pulse_dialog_admin_trust pulse_dialog_bundle)
foreach(_history_target pulse pulse_dialog_management_controller_test pulse_dialog_controller_dual_test)
    if(TARGET ${_history_target})
        target_link_libraries(${_history_target} PRIVATE pulse_dialog_takeover_history)
    endif()
endforeach()
# The installed managed bridge currently uses the separately verified UI-proof DLL.
target_compile_definitions(pulse_file_dialog_ui_proof_runtime PRIVATE PULSE_DIALOG_HISTORY_GLOBAL=1)
target_link_libraries(pulse_file_dialog_ui_proof_runtime PRIVATE pulse_runtime_log shell32)

# Local-only observed loaded-component restart inventory.
add_library(pulse_dialog_restart_inventory STATIC src/common/dialog_restart_inventory.cpp)
target_link_libraries(pulse_dialog_restart_inventory PUBLIC pulse_dialog_process_diagnostics pulse_dialog_bundle)
add_library(pulse_dialog_restart_fixture SHARED EXCLUDE_FROM_ALL src/bench/dialog_restart_inventory_test.cpp)
target_compile_definitions(pulse_dialog_restart_fixture PRIVATE PULSE_RESTART_FIXTURE=1)
add_executable(pulse_dialog_restart_inventory_test EXCLUDE_FROM_ALL src/bench/dialog_restart_inventory_test.cpp)
target_link_libraries(pulse_dialog_restart_inventory_test PRIVATE pulse_dialog_restart_inventory)
add_dependencies(pulse_dialog_restart_inventory_test pulse_dialog_restart_fixture)
foreach(_restart_target pulse pulse_dialog_management_controller_test pulse_dialog_controller_dual_test)
    if(TARGET ${_restart_target})
        target_link_libraries(${_restart_target} PRIVATE pulse_dialog_restart_inventory)
    endif()
endforeach()

# Real native/local picker host coverage for legacy Explorer-style hooks.
get_target_property(_legacy_live_sources pulse_file_dialog_wrapper_test SOURCES)
list(REMOVE_ITEM _legacy_live_sources src/bench/file_dialog_wrapper_test.cpp)
add_executable(pulse_legacy_hook_provider_test EXCLUDE_FROM_ALL
    src/bench/legacy_hook_provider_test.cpp ${_legacy_live_sources}
    src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_hook_host.cpp
    src/shell_bridge/legacy_dialog_error.cpp src/shell_bridge/file_dialog_ui_provider.cpp)
target_compile_definitions(pulse_legacy_hook_provider_test PRIVATE PULSE_LEGACY_HOOK_LOCAL_UI_TEST=1)
target_link_libraries(pulse_legacy_hook_provider_test PRIVATE comdlg32 shell32 ole32 uuid advapi32 bcrypt user32 propsys)
# Legacy child hook routing uses the standard common-controls subclass chain.
target_link_libraries(pulse_file_dialog_runtime PRIVATE comctl32)
target_link_libraries(pulse_file_dialog_ui_proof_runtime PRIVATE comctl32)
target_link_libraries(pulse_file_dialog_auto_entry_runtime PRIVATE comctl32)
target_link_libraries(pulse_legacy_com_provider_test PRIVATE comctl32)
target_link_libraries(pulse_legacy_hook_provider_test PRIVATE comctl32)
add_executable(pulse_legacy_template_contract_test EXCLUDE_FROM_ALL
    src/bench/legacy_template_contract_test.cpp src/shell_bridge/legacy_hook_host.cpp)
target_link_libraries(pulse_legacy_template_contract_test PRIVATE user32 comctl32)
add_executable(pulse_legacy_template_provider_test EXCLUDE_FROM_ALL
    src/bench/legacy_template_provider_test.cpp src/bench/legacy_template_provider_test.rc ${_legacy_live_sources}
    src/shell_bridge/legacy_com_provider.cpp src/shell_bridge/legacy_hook_host.cpp
    src/shell_bridge/legacy_dialog_error.cpp src/shell_bridge/file_dialog_ui_provider.cpp)
target_compile_definitions(pulse_legacy_template_provider_test PRIVATE PULSE_LEGACY_HOOK_LOCAL_UI_TEST=1)
target_link_libraries(pulse_legacy_template_provider_test PRIVATE comdlg32 shell32 ole32 uuid advapi32 bcrypt user32 propsys comctl32)
add_executable(pulse_picker_property_presentation_test EXCLUDE_FROM_ALL
    src/bench/picker_property_presentation_test.cpp src/ipc/picker_remote_protocol.cpp)
target_link_libraries(pulse_picker_property_presentation_test PRIVATE user32)

# Header-only rebind policy regression; no elevated target or installed bundle needed.
add_executable(pulse_dialog_admin_auto_rebind_test EXCLUDE_FROM_ALL
    src/bench/dialog_admin_auto_rebind_test.cpp)
