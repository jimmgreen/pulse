# Current-user standard COM dialog entry; no injection or machine-wide registration.
if(CMAKE_SIZEOF_VOID_P EQUAL 8 OR CMAKE_SIZEOF_VOID_P EQUAL 4)
    add_library(pulse_dialog_auto_registration STATIC src/common/dialog_auto_registration.cpp)
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
        src/shell_bridge/file_dialog_auto_server.cpp ${_auto_def})
    set_target_properties(pulse_dialog_auto PROPERTIES OUTPUT_NAME ${_auto_name})
    target_link_libraries(pulse_dialog_auto PRIVATE pulse_dialog_auto_registration pulse_runtime_log pulse_dialog_admin_trust
        shell32 ole32 uuid advapi32 bcrypt user32 propsys)
    add_executable(pulse_file_dialog_auto_server_test EXCLUDE_FROM_ALL
        src/bench/file_dialog_auto_server_test.cpp ${_auto_sources})
    target_link_libraries(pulse_file_dialog_auto_server_test PRIVATE pulse_runtime_log pulse_dialog_admin_trust
        shell32 ole32 uuid advapi32 bcrypt user32 propsys)
    add_executable(pulse_file_dialog_diagnostic_test EXCLUDE_FROM_ALL src/bench/file_dialog_diagnostic_test.cpp)
    add_executable(pulse_dialog_auto_com_activation_test EXCLUDE_FROM_ALL src/bench/dialog_auto_com_activation_test.cpp)
    target_link_libraries(pulse_dialog_auto_com_activation_test PRIVATE pulse_dialog_auto_registration ole32 uuid)
endif()
