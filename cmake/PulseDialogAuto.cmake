# Current-user standard COM dialog entry; no injection or machine-wide registration.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_library(pulse_dialog_auto_registration STATIC src/common/dialog_auto_registration.cpp src/common/dialog_auto_transaction.cpp src/common/dialog_auto_repair.cpp)
    add_executable(pulse_dialog_registration_mutex_test EXCLUDE_FROM_ALL src/bench/dialog_registration_mutex_test.cpp)
    target_link_libraries(pulse_dialog_registration_mutex_test PRIVATE advapi32)
    add_executable(pulse_dialog_auto_transaction_test EXCLUDE_FROM_ALL src/bench/dialog_auto_transaction_test.cpp)
    target_link_libraries(pulse_dialog_auto_transaction_test PRIVATE pulse_dialog_auto_registration)
    add_executable(pulse_dialog_auto_repair_test EXCLUDE_FROM_ALL src/bench/dialog_auto_repair_test.cpp)
    target_link_libraries(pulse_dialog_auto_repair_test PRIVATE pulse_dialog_auto_registration)
    target_link_libraries(pulse_dialog_auto_registration PUBLIC advapi32 pulse_runtime_log)
    target_link_libraries(pulse PRIVATE pulse_dialog_auto_registration)
    target_link_libraries(pulse_dialog_management_controller_test PRIVATE pulse_dialog_auto_registration)
    add_executable(pulse_dialog_auto_control src/dialog_launcher/auto_main.cpp)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_auto_name PulseDialogAuto64)
        set(_auto_control_name PulseDialogAutoControl)
        set(_auto_def src/shell_bridge/file_dialog_auto_server.def)
    else()
        set(_auto_name PulseDialogAuto32)
        set(_auto_control_name PulseDialogAutoControl)
        set(_auto_def src/shell_bridge/file_dialog_auto_server_x86.def)
    endif()
    set_target_properties(pulse_dialog_auto_control PROPERTIES OUTPUT_NAME ${_auto_control_name})
    target_link_libraries(pulse_dialog_auto_control PRIVATE pulse_dialog_auto_registration pulse_dialog_bundle shell32 ole32)
    add_executable(pulse_dialog_auto_registration_test EXCLUDE_FROM_ALL src/bench/dialog_auto_registration_test.cpp)
    target_link_libraries(pulse_dialog_auto_registration_test PRIVATE pulse_dialog_auto_registration ole32)
    get_target_property(_auto_sources pulse_file_dialog_ui_proof_runtime SOURCES)
    list(FILTER _auto_sources EXCLUDE REGEX "file_dialog_runtime\\.(cpp|def)$|file_dialog_runtime_x(86|64)\\.def$|runtime_child_.*\\.cpp$|legacy_call_observer\\.cpp$")
    # Dialogs with events, custom controls or properties are shown by
    # pulse.exe --picker-remote-host through the complete V2 remote provider.
    list(APPEND _auto_sources src/shell_bridge/file_dialog_remote_provider.cpp src/ipc/picker_remote_protocol.cpp)
    list(REMOVE_DUPLICATES _auto_sources)
    add_library(pulse_dialog_auto SHARED EXCLUDE_FROM_ALL ${_auto_sources}
        src/shell_bridge/file_dialog_auto_server.cpp src/shell_bridge/direct_reveal_hook.cpp
        src/app/open_request_pipe.cpp ${_auto_def})
    set_target_properties(pulse_dialog_auto PROPERTIES OUTPUT_NAME ${_auto_name})
    target_link_libraries(pulse_dialog_auto PRIVATE pulse_detours pulse_dialog_sink_hooks pulse_dialog_bridge_state pulse_dialog_auto_registration pulse_runtime_log pulse_dialog_admin_trust pulse_dialog_bundle
        shell32 ole32 uuid advapi32 bcrypt user32 propsys comctl32)

    # Phase 1: Fixed-path thin Stub DLL (zero UI/shell32/user32 imports) + versioned Core DLL.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_stub_name PulseDialogStub64)
        set(_stub_def src/shell_bridge/dialog_stub64.def)
        set(_core_name PulseDialogCore64)
        set(_core_def src/shell_bridge/dialog_core64.def)
    else()
        set(_stub_name PulseDialogStub32)
        set(_stub_def src/shell_bridge/dialog_stub32.def)
        set(_core_name PulseDialogCore32)
        set(_core_def src/shell_bridge/dialog_core32.def)
    endif()
    add_library(pulse_dialog_stub SHARED EXCLUDE_FROM_ALL
        src/shell_bridge/dialog_stub_main.cpp src/common/json_value.cpp ${_stub_def})
    set_target_properties(pulse_dialog_stub PROPERTIES OUTPUT_NAME ${_stub_name})
    target_link_libraries(pulse_dialog_stub PRIVATE advapi32)

    add_library(pulse_dialog_core SHARED EXCLUDE_FROM_ALL ${_auto_sources}
        src/shell_bridge/file_dialog_auto_server.cpp src/shell_bridge/direct_reveal_hook.cpp
        src/app/open_request_pipe.cpp ${_core_def})
    set_target_properties(pulse_dialog_core PROPERTIES OUTPUT_NAME ${_core_name})
    target_link_libraries(pulse_dialog_core PRIVATE pulse_detours pulse_dialog_sink_hooks pulse_dialog_bridge_state pulse_dialog_auto_registration pulse_runtime_log pulse_dialog_admin_trust pulse_dialog_bundle
        shell32 ole32 uuid advapi32 bcrypt user32 propsys comctl32)

    add_executable(pulse_file_dialog_auto_server_test EXCLUDE_FROM_ALL
        src/bench/file_dialog_auto_server_test.cpp src/shell_bridge/direct_reveal_hook.cpp
        src/app/open_request_pipe.cpp ${_auto_sources})
    target_link_libraries(pulse_file_dialog_auto_server_test PRIVATE pulse_detours pulse_dialog_sink_hooks pulse_dialog_bridge_state pulse_runtime_log pulse_dialog_admin_trust pulse_dialog_bundle
        shell32 ole32 uuid advapi32 bcrypt user32 propsys comctl32)
    add_executable(pulse_file_dialog_diagnostic_test EXCLUDE_FROM_ALL src/bench/file_dialog_diagnostic_test.cpp)
    add_executable(pulse_file_dialog_generation_test EXCLUDE_FROM_ALL src/bench/file_dialog_generation_test.cpp)
    target_link_libraries(pulse_file_dialog_generation_test PRIVATE pulse_dialog_bundle ole32)
    add_executable(pulse_dialog_auto_com_activation_test EXCLUDE_FROM_ALL src/bench/dialog_auto_com_activation_test.cpp)
    target_link_libraries(pulse_dialog_auto_com_activation_test PRIVATE pulse_dialog_auto_registration ole32 uuid)
endif()
add_executable(pulse_dialog_thin_route_test EXCLUDE_FROM_ALL
    src/bench/dialog_thin_route_test.cpp src/common/dialog_auto_registration.cpp)
target_link_libraries(pulse_dialog_thin_route_test PRIVATE ole32 shell32 user32 advapi32 uuid)

# Dialog bridge binaries carry no renderer. Compile all of them (and their
# shared helper libs) with the same compact flags on both x64 and x86 so each
# PE stays small and sibling DLLs share identical machine code in solid LZMA2.
if(MSVC)
    foreach(_dialog_size_target
            pulse_dialog_auto pulse_dialog_stub pulse_dialog_core pulse_dialog_sink_hooks
            pulse_dialog_auto_control pulse_dialog_launcher
            pulse_file_dialog_ui_proof_runtime pulse_file_dialog_auto_entry_runtime
            pulse_dialog_admin_auto pulse_dialog_user_auto pulse_dialog_bundle
            pulse_dialog_auto_registration pulse_dialog_admin_trust
            pulse_dialog_session pulse_runtime_log)
        if(TARGET ${_dialog_size_target})
            target_compile_options(${_dialog_size_target} PRIVATE
                $<$<CONFIG:Release>:/Os /Oi- /Ob1 /Gy /Gw>)
            get_target_property(_dialog_target_type ${_dialog_size_target} TYPE)
            if(NOT _dialog_target_type STREQUAL "STATIC_LIBRARY")
                target_link_options(${_dialog_size_target} PRIVATE
                    $<$<CONFIG:Release>:/OPT:REF /OPT:ICF>)
            endif()
        endif()
    endforeach()
endif()

add_executable(pulse_direct_reveal_hook_test EXCLUDE_FROM_ALL
    src/bench/direct_reveal_hook_test.cpp src/shell_bridge/direct_reveal_hook.cpp src/app/open_request_pipe.cpp)
target_link_libraries(pulse_direct_reveal_hook_test PRIVATE pulse_detours shell32 ole32 uuid user32 advapi32)
