# Focused regression targets; none are part of the shipping package.
set(PULSE_SEARCH_WINDOW_TEST_SOURCE "" CACHE FILEPATH "Optional source snapshot for the focused search-window test peer")
add_executable(pulse_search_monitor_policy_test EXCLUDE_FROM_ALL src/bench/global_search_monitor_policy_test.cpp)
target_link_libraries(pulse_search_monitor_policy_test PRIVATE user32)
add_executable(pulse_search_monitor_native_test EXCLUDE_FROM_ALL src/bench/global_search_monitor_native_test.cpp)
target_link_libraries(pulse_search_monitor_native_test PRIVATE user32)
add_executable(pulse_search_activation_state_test EXCLUDE_FROM_ALL
    src/bench/global_search_activation_test.cpp)
add_executable(pulse_double_ctrl_input_test EXCLUDE_FROM_ALL
    src/bench/double_ctrl_input_test.cpp src/app/double_ctrl.cpp)
target_link_libraries(pulse_double_ctrl_input_test PRIVATE user32 shell32)
add_executable(pulse_drag_shake_watcher_test EXCLUDE_FROM_ALL
    src/bench/drag_shake_watcher_test_main.cpp src/app/drag_shake_watcher.cpp)
target_link_libraries(pulse_drag_shake_watcher_test PRIVATE user32 shell32 shcore)
target_include_directories(pulse_drag_shake_watcher_test PRIVATE src/app)
add_executable(pulse_ctrl_search_process_probe EXCLUDE_FROM_ALL src/bench/ctrl_search_process_probe.cpp)
target_link_libraries(pulse_ctrl_search_process_probe PRIVATE user32 advapi32)

# Compile the real search window implementation into its existing test peer.
# Other production objects are reused; no frozen historical object directory.
foreach(variant IN ITEMS modified baseline)
    set(target pulse_search_activation_${variant}_test)
    set(original "${CMAKE_SOURCE_DIR}/bench_data/ctrl-search-refactor-20261125/original/src/app")
    if(variant STREQUAL "baseline" AND NOT EXISTS "${original}/global_search_window.cpp")
        continue()
    endif()
    add_executable(${target} EXCLUDE_FROM_ALL src/bench/global_search_window_test.cpp
        src/bench/ctrl_search_profile_fixture.cpp
        $<FILTER:$<TARGET_OBJECTS:pulse>,EXCLUDE,global_search_window\\.cpp\\.obj$>)
    target_link_libraries(${target} PRIVATE "$<TARGET_PROPERTY:pulse,LINK_LIBRARIES>")
    foreach(prop INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS LINK_OPTIONS)
        get_target_property(value pulse ${prop})
        if(value)
            set_property(TARGET ${target} PROPERTY ${prop} "${value}")
        endif()
    endforeach()
    target_include_directories(${target} PRIVATE "${CMAKE_SOURCE_DIR}/src/app")
    target_compile_definitions(${target} PRIVATE PULSE_SEARCH_ISOLATED_PROFILE=1)
    if(variant STREQUAL "modified" AND PULSE_SEARCH_WINDOW_TEST_SOURCE)
        target_compile_definitions(${target} PRIVATE PULSE_SEARCH_WINDOW_SOURCE="${PULSE_SEARCH_WINDOW_TEST_SOURCE}")
    endif()
    if(variant STREQUAL "baseline")
        # Keep old implementation logic intact. Only adapt declarations/calls for
        # new controller symbols retained by the shared production object graph.
        # The baseline regression itself calls the original six-argument Show.
        set(bridge "${CMAKE_CURRENT_BINARY_DIR}/ctrl-search-baseline")
        file(MAKE_DIRECTORY "${bridge}")
        file(READ "${original}/global_search_window.h" baseline_header)
        string(REPLACE "#pragma once" "#pragma once\n#include \"global_search_activation.h\"" baseline_header "${baseline_header}")
        string(REPLACE "    void SetAppearance(" "    bool Show(HWND owner, bool dark, float scale, const std::wstring& folder, bool pinyin, const std::wstring& query, app::SearchTrigger) { return Show(owner, dark, scale, folder, pinyin, query); }\n    app::SearchVisibility Visibility() const { return Visible() ? app::SearchVisibility::VisibleFocused : app::SearchVisibility::Hidden; }\n    void SetAppearance(" baseline_header "${baseline_header}")
        # Out-of-line ABI adapters must be emitted even if this TU doesn't call them.
        string(REPLACE "bool Show(HWND owner, bool dark, float scale, const std::wstring& folder, bool pinyin, const std::wstring& query, app::SearchTrigger) { return Show(owner, dark, scale, folder, pinyin, query); }" "bool Show(HWND owner, bool dark, float scale, const std::wstring& folder, bool pinyin, const std::wstring& query, app::SearchTrigger);" baseline_header "${baseline_header}")
        string(REPLACE "app::SearchVisibility Visibility() const { return Visible() ? app::SearchVisibility::VisibleFocused : app::SearchVisibility::Hidden; }" "app::SearchVisibility Visibility() const;" baseline_header "${baseline_header}")
        # Concurrent animation diagnostics added this ABI entry point. It is not
        # exercised by the activation baseline; retain its real snapshot logic.
        string(REPLACE "private:" "    bool SavePresentedSnapshot(const std::wstring& path) const;\nprivate:" baseline_header "${baseline_header}")
        file(WRITE "${bridge}/global_search_window.h" "${baseline_header}")
        file(READ "${original}/global_search_window.cpp" baseline_source)
        string(APPEND baseline_source "\nnamespace pulse {\nbool GlobalSearchWindow::Show(HWND owner, bool dark, float scale, const std::wstring& folder, bool pinyin, const std::wstring& query, app::SearchTrigger) { return Show(owner, dark, scale, folder, pinyin, query); }\napp::SearchVisibility GlobalSearchWindow::Visibility() const { return Visible() ? app::SearchVisibility::VisibleFocused : app::SearchVisibility::Hidden; }\n}\n")
        string(APPEND baseline_source "\nnamespace pulse {\nbool GlobalSearchWindow::SavePresentedSnapshot(const std::wstring& path) const { return impl_ && ui::Compositor::FrameCopyEnabled() && impl_->compositor.FrameCopy() && impl_->compositor.SaveSnapshot(path.c_str()); }\n}\n")
        file(WRITE "${bridge}/global_search_window.cpp" "${baseline_source}")
        target_compile_definitions(${target} PRIVATE
            PULSE_SEARCH_WINDOW_SOURCE="${bridge}/global_search_window.cpp"
            PULSE_SEARCH_WINDOW_HEADER="${bridge}/global_search_window.h")
    endif()
endforeach()
