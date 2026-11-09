# Controlled in-process proof only; include after shared Pulse libraries/functions.
# Explicit renderer dependency closure; never includes app_main or app source glob.
set(PULSE_PICKER_INPROCESS_SOURCES
    src/picker_inprocess/picker_ui_bootstrap.cpp
    src/picker_inprocess/picker_ui_bootstrap_v2.cpp
    src/app/new_item_name.cpp src/app/create_rename_intent.cpp
    src/app/picker_appearance.cpp
    src/app/frame_pump.cpp
    src/picker_inprocess/picker_ui.rc
    src/ui/folder_picker_dialog.cpp src/common/picker_owner_guard.cpp
    src/common/picker_layout_settings.cpp
    src/ui/folder_picker_loader.cpp
    src/ui/folder_picker_model.cpp
    src/ui/details_column_menu_model.cpp
    src/ui/folder_picker_view.cpp
    src/ui/folder_picker_art.cpp
    src/ui/folder_picker_operations.cpp
    src/ui/drag_drop.cpp
    src/ui/file_save_model.cpp
    src/ui/file_save_revalidation.cpp
    src/ui/file_save_access_policy.cpp
    src/ui/confirm_dialog.cpp
    src/ui/confirm_dialog_view.cpp
    src/ui/file_operation_dialog.cpp
    src/ui/dialog_lifecycle.cpp
    src/ui/edit_host.cpp
    src/ui/fluent_components.cpp
    src/ui/fluent_menu.cpp
    src/ui/tab_shape.cpp
    src/ui/typography.cpp
    src/ui/ui_compositor.cpp
    src/ui/window_helpers.cpp
    src/ui/window_material.cpp
    src/ui/ui_pane_zoom_motion.cpp
    src/ui/ui_renderer.cpp
    src/ui/ui_column_strip.cpp
    src/ui/ui_address_search.cpp
    src/ui/ui_pane_header_icons.cpp
    src/ui/ui_hover_preview.cpp
    src/ui/ui_hit_test.cpp
    src/ui/ui_fluent_svg.cpp
    src/ui/ui_details_panel.cpp
    src/ui/ui_pane_list.cpp
    src/ui/ui_tray_shelf.cpp
    src/ui/ui_toolbar.cpp
    src/ui/ui_sidebar.cpp
    src/ui/ui_settings_core.cpp
    src/ui/ui_settings_assistant.cpp
    src/ui/ui_settings_ai.cpp
    src/ui/ui_settings_context.cpp
    src/ui/ui_settings_view.cpp
    src/ui/ui_settings_sysctx.cpp
    src/ui/ui_settings_shortcuts.cpp
    src/ui/ui_settings_packs.cpp
    src/ui/ui_settings_layout.cpp
    src/ui/shell_icons.cpp
    src/ui/open_with_icons.cpp
    src/ui/thumbnail_cache.cpp
    src/ui/thumbnail_disk_cache.cpp
    src/ui/folder_thumbnail_cache.cpp
    src/ui/video_preview.cpp
    src/ui/ffmpeg_playback.cpp
    src/ui/quick_preview_window.cpp
    src/picker_inprocess/native_preview_view_unavailable.cpp
    src/ui/quick_preview_chrome.cpp
    src/ui/quick_preview_text_draw.cpp
    src/ui/quick_preview_pages.cpp
    src/ui/quick_preview_playback.cpp
    src/ui/quick_preview_audio.cpp
    src/ui/audio_waveform.cpp
    src/ui/markdown_view.cpp
    src/ui/markdown_image_metadata.cpp
    src/ui/visible_text_renderer.cpp
    src/ui/math_formula.cpp
    src/ui/math_syntax.cpp
    src/ui/math_layout.cpp
    src/ui/math_macro.cpp
    src/ui/math_alphabet.cpp
    src/ui/math_font_metrics.cpp
    src/ui/math_font_face.cpp
    src/ui/table_view.cpp
    src/ui/tree_view.cpp
    src/ui/preview_handler_host.cpp
    src/ui/preview_format_catalog.cpp
    src/ui/name_highlight.cpp
    src/ui/bloom_accent_picker.cpp
    src/ui/column_strip_layout.cpp
    src/ui/view_layout.cpp
    src/ui/snack_style.cpp
    src/ui/svg_bitmap.cpp
    src/app/places.cpp
    src/app/device_registry.cpp
    src/app/builtin_menu_items.cpp
    src/app/search_query.cpp
    src/ops/clipboard.cpp
    src/ops/ops_manager.cpp
    src/ops/duplicate_cleanup_guard.cpp
    src/ops/file_lock_owner.cpp
    src/ipc/shell_client.cpp
    src/common/ffmpeg_tool.cpp
    src/ui/ai_panel_view.cpp
    src/ui/ai_panel_peek.cpp
    src/ui/ai_panel_conversation.cpp
    src/ui/ai_panel_history.cpp
    src/ui/ai_panel_rich.cpp
    src/ui/ai_rich_text.cpp
    src/ui/syntax_highlight.cpp
    src/preview_host/markdown_document.cpp
    third_party/md4c/md4c.c
    src/ui/ai_panel_voice.cpp
    src/ui/ai_panel_icons.cpp
    src/ui/ai_list_overlay.cpp
    src/ui/group_wheel.cpp
    src/ui/window_overview_view.cpp
    src/ui/window_switcher_view.cpp
    src/ui/archive_preview.cpp
    src/ui/wall_aspects.cpp
    src/ui/toolbar_picker.cpp
    src/fs/device_path.cpp
    src/fs/device_shell.cpp
    src/fs/device_fake.cpp
    src/fs/device_worker.cpp
    src/index/content_result_store.cpp
    src/index/index_query.cpp
    src/index/search_kinds.cpp
    src/index/filename_pinyin.cpp
    src/app/cloud_sync_probe.cpp
    src/app/keymap.cpp
    src/app/tag_ads_sync.cpp
    src/app/search_query_rewrite.cpp
    src/app/session.cpp
    src/app/git_status.cpp
    src/app/tag_query.cpp
    src/app/app_model.cpp
    src/app/window_identity.cpp
    src/app/session_tab_model.cpp
    src/app/tab_group_model.cpp
    src/app/cloud_roots.cpp
    src/app/entry_group.cpp
    src/app/entry_sort.cpp
    src/app/pane_layout.cpp
    src/app/column_view_model.cpp
    src/ops/ops_manager_authorization.cpp
    src/ops/ops_manager_authorized_delete.cpp
    src/ops/ops_manager_shell_transfer.cpp
    src/ops/elevated_delete.cpp
    src/ops/elevated_transfer.cpp
    src/ops/elevated_transfer_client.cpp
    src/ops/transfer_engine.cpp
    src/ops/shell_command.cpp
    src/ops/shortcut_launch.cpp
    src/ipc/elevated_transfer_transport.cpp
)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    list(APPEND PULSE_PICKER_INPROCESS_SOURCES src/picker_inprocess/picker_ui_x86.def)
else()
    list(APPEND PULSE_PICKER_INPROCESS_SOURCES src/picker_inprocess/picker_ui.def)
endif()
add_library(pulse_picker_ui_proof SHARED EXCLUDE_FROM_ALL ${PULSE_PICKER_INPROCESS_SOURCES})
# Ninja's RC rule does not discover nested resource includes. Recompile the
# DLL's string tables whenever the generated catalog or its IDs change.
set_property(SOURCE src/picker_inprocess/picker_ui.rc APPEND PROPERTY OBJECT_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/app/resource.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/string_ids.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/common/strings_zh_tw.rcinc"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/picker_inprocess/picker_ui.manifest")
set_target_properties(pulse_picker_ui_proof PROPERTIES OUTPUT_NAME PulsePickerUiProof)
target_compile_definitions(pulse_picker_ui_proof PRIVATE MD4C_USE_UTF16)
target_link_libraries(pulse_picker_ui_proof PRIVATE
    pulse_localization pulse_runtime_log pulse_fs_watch pulse_text pulse_sqlite_core pulse_virtual_disk
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 oleaut32 shell32 uuid
    comctl32 advapi32 shlwapi version windowscodecs gdi32 gdiplus uxtheme
    netapi32 mpr bcrypt propsys cfgmgr32 winhttp crypt32 secur32
    mf mfplat mfreadwrite mfuuid mfplay winmm psapi)
target_link_options(pulse_picker_ui_proof PRIVATE /MANIFEST:NO)
# Built for size like Release pulse.exe (PULSE_SIZE_DEFAULT in CMakeLists.txt).
if(PULSE_SIZE_DEFAULT)
    target_compile_options(pulse_picker_ui_proof PRIVATE $<$<CONFIG:Release>:/Os /Oi- /Ob1 /Gw>)
endif()
pulse_enable_lumatext(pulse_picker_ui_proof)

add_executable(pulse_picker_inprocess_test EXCLUDE_FROM_ALL src/bench/picker_inprocess_test.cpp)
target_link_libraries(pulse_picker_inprocess_test PRIVATE ole32 user32)
add_executable(pulse_layered_child_style_probe EXCLUDE_FROM_ALL src/bench/layered_child_style_probe.cpp)
target_link_libraries(pulse_layered_child_style_probe PRIVATE user32 gdi32)

add_executable(pulse_picker_appearance_test EXCLUDE_FROM_ALL src/bench/picker_appearance_test.cpp src/app/picker_appearance.cpp)
target_link_libraries(pulse_picker_appearance_test PRIVATE shell32 ole32 uuid)

add_library(pulse_luma_expected_fixture SHARED EXCLUDE_FROM_ALL src/bench/optional_luma_fixture.cpp)
add_library(pulse_luma_foreign_fixture SHARED EXCLUDE_FROM_ALL src/bench/optional_luma_fixture.cpp)
target_compile_definitions(pulse_luma_expected_fixture PRIVATE PULSE_LUMA_FIXTURE_VALUE=71)
target_compile_definitions(pulse_luma_foreign_fixture PRIVATE PULSE_LUMA_FIXTURE_VALUE=93)
set_target_properties(pulse_luma_expected_fixture PROPERTIES OUTPUT_NAME lumatext
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luma-binding/expected"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luma-binding/expected")
set_target_properties(pulse_luma_foreign_fixture PROPERTIES OUTPUT_NAME lumatext
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luma-binding/foreign"
    ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luma-binding/foreign")
add_library(pulse_luma_binding_fixture SHARED EXCLUDE_FROM_ALL src/bench/optional_luma_binding_fixture.cpp)
set_target_properties(pulse_luma_binding_fixture PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/luma-binding/expected")
target_compile_definitions(pulse_luma_binding_fixture PRIVATE PULSE_HAS_LUMATEXT=1)
target_link_libraries(pulse_luma_binding_fixture PRIVATE pulse_luma_expected_fixture delayimp user32)
target_link_options(pulse_luma_binding_fixture PRIVATE /DELAYLOAD:lumatext.dll)
add_executable(pulse_luma_binding_test EXCLUDE_FROM_ALL src/bench/optional_luma_binding_test.cpp)

# Keep proof-only phase diagnostics confined to the affected translation units.
set_property(SOURCE src/ui/folder_picker_dialog.cpp src/ui/quick_preview_window.cpp src/ui/lumatext_renderer.cpp src/ui/ui_compositor.cpp src/picker_inprocess/picker_ui_bootstrap_v2.cpp APPEND PROPERTY COMPILE_DEFINITIONS
    "$<$<STREQUAL:$<TARGET_PROPERTY:NAME>,pulse_picker_ui_proof>:PULSE_PICKER_INPROCESS_PROOF=1>")
add_library(pulse_module_location_fixture SHARED EXCLUDE_FROM_ALL src/bench/module_location_fixture.cpp)
add_executable(pulse_module_location_test EXCLUDE_FROM_ALL src/bench/module_location_test.cpp)

add_executable(pulse_picker_inprocess_v2_test EXCLUDE_FROM_ALL src/bench/picker_inprocess_v2_test.cpp)
target_link_libraries(pulse_picker_inprocess_v2_test PRIVATE ole32 user32 windowscodecs)

add_executable(pulse_picker_inprocess_v2_input_test EXCLUDE_FROM_ALL src/bench/picker_inprocess_v2_input_test.cpp)
target_link_libraries(pulse_picker_inprocess_v2_input_test PRIVATE ole32 user32)

add_executable(pulse_file_dialog_ui_provider_test EXCLUDE_FROM_ALL src/bench/file_dialog_ui_provider_test.cpp src/shell_bridge/file_dialog_ui_provider.cpp)

add_executable(pulse_picker_activation_stack_test EXCLUDE_FROM_ALL src/bench/picker_activation_stack_test.cpp)

add_executable(pulse_picker_low_integrity_owner_test EXCLUDE_FROM_ALL src/bench/picker_low_integrity_owner_test.cpp)
target_link_libraries(pulse_picker_low_integrity_owner_test PRIVATE advapi32 ole32 user32)


add_executable(pulse_picker_native_options_ui_test EXCLUDE_FROM_ALL src/bench/picker_native_options_ui_test.cpp)
target_link_libraries(pulse_picker_native_options_ui_test PRIVATE ole32 user32)
