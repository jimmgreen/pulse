if(TARGET pulse)
    # Reuse the real --test-instance / window-management entry; no fake main HWND.
    target_sources(pulse PRIVATE src/bench/animation_integration_probe.cpp)
    # pulse_content_progress_ui_test copied pulse's sources before this file ran.
    if(TARGET pulse_content_progress_ui_test)
        target_sources(pulse_content_progress_ui_test PRIVATE src/bench/animation_integration_probe.cpp)
    endif()
endif()
