# Isolated production renderer for system-menu icon and nested-row visual checks.
get_target_property(context_menu_visual_sources pulse_menu_mnemonic_test SOURCES)
list(REMOVE_ITEM context_menu_visual_sources src/bench/menu_mnemonic_test.cpp)
add_executable(pulse_context_menu_visual_test EXCLUDE_FROM_ALL
    src/bench/context_menu_visual_test.cpp ${context_menu_visual_sources}
    src/app/context_menu_controller.cpp src/app/ctx_layout_cache.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp)
target_include_directories(pulse_context_menu_visual_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_options(pulse_context_menu_visual_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_context_menu_visual_test PRIVATE
    "$<TARGET_PROPERTY:pulse_menu_mnemonic_test,LINK_LIBRARIES>" shlwapi advapi32)
pulse_enable_lumatext(pulse_context_menu_visual_test)
add_executable(pulse_menu_measure_cache_test EXCLUDE_FROM_ALL
    src/bench/menu_measure_cache_test.cpp ${context_menu_visual_sources})
target_include_directories(pulse_menu_measure_cache_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_options(pulse_menu_measure_cache_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_menu_measure_cache_test PRIVATE
    "$<TARGET_PROPERTY:pulse_menu_mnemonic_test,LINK_LIBRARIES>")
pulse_enable_lumatext(pulse_menu_measure_cache_test)
add_executable(pulse_new_menu_state_test EXCLUDE_FROM_ALL src/bench/new_menu_state_test.cpp)
add_executable(pulse_shell_menu_icon_capture_test EXCLUDE_FROM_ALL src/bench/shell_menu_icon_capture_test.cpp)
target_link_libraries(pulse_shell_menu_icon_capture_test PRIVATE user32 gdi32)
if(PULSE_WITH_TEST_HOOKS)
    set_property(SOURCE src/app/app_commands.cpp APPEND PROPERTY
        COMPILE_DEFINITIONS PULSE_WITH_TEST_HOOKS=1)
    target_sources(pulse PRIVATE src/app/context_menu_speed_test.cpp)
    set_source_files_properties(src/app/context_menu_speed_test.cpp
        PROPERTIES COMPILE_DEFINITIONS PULSE_WITH_TEST_HOOKS=1)
endif()

add_executable(pulse_menu_icon_cache_test EXCLUDE_FROM_ALL
    src/bench/menu_icon_cache_test.cpp ${context_menu_visual_sources})
target_include_directories(pulse_menu_icon_cache_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_options(pulse_menu_icon_cache_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_menu_icon_cache_test PRIVATE
    "$<TARGET_PROPERTY:pulse_menu_mnemonic_test,LINK_LIBRARIES>")
pulse_enable_lumatext(pulse_menu_icon_cache_test)

add_executable(pulse_system_ctx_tree_scan_test EXCLUDE_FROM_ALL
    src/bench/system_ctx_tree_scan_test.cpp src/app/system_ctx_registry.cpp)
target_link_libraries(pulse_system_ctx_tree_scan_test PRIVATE advapi32 shell32 shlwapi ole32)

add_executable(pulse_settings_context_tree_test EXCLUDE_FROM_ALL
    src/bench/settings_context_tree_test.cpp src/app/settings_context_tree.cpp
    src/app/context_menu_prefs.cpp src/app/builtin_menu_items.cpp src/bench/localization_test.rc)
target_include_directories(pulse_settings_context_tree_test PRIVATE "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(pulse_settings_context_tree_test PRIVATE
    pulse_localization shell32 ole32 advapi32 shlwapi)

add_executable(pulse_static_cascade_tokens_test EXCLUDE_FROM_ALL
    src/bench/static_cascade_tokens_test.cpp src/app/shell_verbs.cpp src/common/menu_icon.cpp)
target_link_libraries(pulse_static_cascade_tokens_test PRIVATE
    pulse_localization ole32 shell32 shlwapi advapi32 user32 gdi32 version uuid)

get_target_property(menu_native_pixel_sources pulse_menu_frame_test SOURCES)
list(REMOVE_ITEM menu_native_pixel_sources src/bench/menu_frame_test.cpp)
add_executable(pulse_menu_native_pixels_test EXCLUDE_FROM_ALL
    src/bench/menu_native_pixels_test.cpp ${menu_native_pixel_sources})
target_include_directories(pulse_menu_native_pixels_test PRIVATE "${CMAKE_BINARY_DIR}/generated" src/ui)
target_link_options(pulse_menu_native_pixels_test PRIVATE /MANIFEST:NO)
target_link_libraries(pulse_menu_native_pixels_test PRIVATE
    "$<TARGET_PROPERTY:pulse_menu_frame_test,LINK_LIBRARIES>")
pulse_enable_lumatext(pulse_menu_native_pixels_test)

add_executable(pulse_native_explorer_test EXCLUDE_FROM_ALL
    src/bench/native_explorer_test.cpp src/app/native_explorer.cpp)
target_compile_definitions(pulse_native_explorer_test PRIVATE PULSE_NATIVE_EXPLORER_TEST=1)
target_link_libraries(pulse_native_explorer_test PRIVATE user32)

get_target_property(open_with_pref_sources pulse_ctx_menu_controller_test SOURCES)
list(REMOVE_ITEM open_with_pref_sources src/bench/ctx_menu_controller_test.cpp)
add_executable(pulse_open_with_preferences_test EXCLUDE_FROM_ALL
    src/bench/open_with_preferences_test.cpp ${open_with_pref_sources}
    src/app/settings_context_tree.cpp src/bench/localization_test.rc)
target_link_libraries(pulse_open_with_preferences_test PRIVATE
    "$<TARGET_PROPERTY:pulse_ctx_menu_controller_test,LINK_LIBRARIES>")
