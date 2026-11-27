if(TARGET pulse)
    # Reuse the real --test-instance / window-management entry; no fake main HWND.
    target_sources(pulse PRIVATE src/bench/animation_integration_probe.cpp)
endif()
