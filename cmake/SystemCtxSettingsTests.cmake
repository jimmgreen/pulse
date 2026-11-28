add_executable(pulse_system_ctx_model_test EXCLUDE_FROM_ALL
    src/bench/system_ctx_model_test.cpp src/bench/localization_test.rc
    src/app/system_ctx_model.cpp src/app/system_ctx_registry.cpp
    src/shell_host/packaged_ctx_handlers.cpp)
target_link_libraries(pulse_system_ctx_model_test PRIVATE
    pulse_localization advapi32 shell32 shlwapi ole32 user32 gdi32 version uuid)
if(PULSE_WITH_TEST_HOOKS)
    target_sources(pulse PRIVATE src/app/system_ctx_ui_test.cpp)
    if(TARGET pulse_content_progress_ui_test)
        target_sources(pulse_content_progress_ui_test PRIVATE src/app/system_ctx_ui_test.cpp)
    endif()
    set_source_files_properties(src/app/system_ctx_ui_test.cpp
        PROPERTIES COMPILE_DEFINITIONS PULSE_WITH_TEST_HOOKS=1)
endif()

