# Desktop WinRT async adapter; no AppContainer storage-grant claim.
add_library(pulse_winrt_picker_adapter STATIC EXCLUDE_FROM_ALL
    src/shell_bridge/winrt_picker_adapter.cpp
    src/shell_bridge/winrt_picker_activation.cpp
    src/ipc/picker_protocol.cpp)
target_compile_features(pulse_winrt_picker_adapter PUBLIC cxx_std_20)
target_compile_definitions(pulse_winrt_picker_adapter PUBLIC NOMINMAX UNICODE _UNICODE)
target_link_libraries(pulse_winrt_picker_adapter PUBLIC windowsapp ole32 advapi32 shell32)
add_executable(pulse_winrt_picker_adapter_test EXCLUDE_FROM_ALL src/bench/winrt_picker_adapter_test.cpp)
target_link_libraries(pulse_winrt_picker_adapter_test PRIVATE pulse_winrt_picker_adapter)

add_executable(pulse_winrt_picker_activation_test EXCLUDE_FROM_ALL src/bench/winrt_picker_activation_test.cpp)
target_link_libraries(pulse_winrt_picker_activation_test PRIVATE pulse_winrt_picker_adapter)
option(PULSE_EXPERIMENTAL_WINRT_PICKER "Enable unqualified desktop WinRT picker route (not production certified)" OFF)
if(PULSE_EXPERIMENTAL_WINRT_PICKER)
    foreach(target pulse_file_dialog_runtime pulse_file_dialog_ui_proof_runtime pulse_file_dialog_auto_entry_runtime)
        if(TARGET ${target})
            target_compile_definitions(${target} PRIVATE PULSE_EXPERIMENTAL_WINRT_PICKER=1)
            target_link_libraries(${target} PRIVATE pulse_winrt_picker_adapter)
        endif()
    endforeach()
endif()
