cmake_minimum_required(VERSION 3.25)
project(PulseDesktop LANGUAGES CXX RC)

set(PULSE_SDK_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../sdk/native/include" CACHE PATH "Public SDK headers")
configure_file("${CMAKE_CURRENT_LIST_DIR}/manifest.json" "${CMAKE_CURRENT_BINARY_DIR}/manifest.json" COPYONLY)

file(GLOB PULSE_PUBLIC_C_HEADERS CONFIGURE_DEPENDS "${PULSE_SDK_INCLUDE}/pulse/*.h")

# Authenticated overlapped Named Pipe IPC shared by pulse_desktop.dll and pulse_desktop_host.exe.
add_library(pulse_desktop_ipc STATIC
    ipc/desktop_ipc.cpp
    ipc/assistant_availability.cpp
    ../../src/common/json_value.cpp
)
target_compile_features(pulse_desktop_ipc PUBLIC cxx_std_20)
target_compile_options(pulse_desktop_ipc PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_ipc PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_ipc PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pulse_desktop_ipc PUBLIC advapi32 bcrypt)

# Pure C++20 model engine + native AI organizer (Pulse AI Assistant DPAPI + WinHTTP bridge).
add_library(pulse_desktop_model STATIC
    host/model/layout_groups.cpp
    host/model/group_tabs.cpp
    host/model/group_drop.cpp
    host/model/loose_icons.cpp
    host/model/tile_board.cpp
    host/model/tile_reorder.cpp
    host/model/rules_and_health.cpp
    host/model/undo_history.cpp
    host/ai/desktop_ai_organizer.cpp
)
target_compile_features(pulse_desktop_model PUBLIC cxx_std_20)
target_compile_options(pulse_desktop_model PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_model PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_model PUBLIC "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pulse_desktop_model PUBLIC pulse_desktop_ipc advapi32 crypt32 winhttp shell32 ole32 shlwapi)

# Build-time HLSL compilation for physical Liquid Glass D2D1 custom effect (ps_4_0).
find_program(PULSE_FXC_EXECUTABLE NAMES fxc fxc.exe)
if(NOT PULSE_FXC_EXECUTABLE AND DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}/bin/$ENV{WindowsSDKVersion}/x64" _pulse_sdk_bin)
    find_program(PULSE_FXC_EXECUTABLE NAMES fxc fxc.exe HINTS "${_pulse_sdk_bin}" NO_DEFAULT_PATH)
endif()
if(NOT PULSE_FXC_EXECUTABLE)
    file(GLOB _pulse_fxc_glob "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
    if(_pulse_fxc_glob)
        list(SORT _pulse_fxc_glob)
        list(POP_BACK _pulse_fxc_glob PULSE_FXC_EXECUTABLE)
    endif()
endif()
if(NOT PULSE_FXC_EXECUTABLE)
    message(FATAL_ERROR "fxc.exe was not found in Windows SDK; required to compile tile_liquid_glass.hlsl")
endif()

find_path(PULSE_D2D_SHADER_INCLUDE_DIR NAMES d2d1effecthelpers.hlsli)
if(NOT PULSE_D2D_SHADER_INCLUDE_DIR AND DEFINED ENV{WindowsSdkDir} AND DEFINED ENV{WindowsSDKVersion})
    file(TO_CMAKE_PATH "$ENV{WindowsSdkDir}/Include/$ENV{WindowsSDKVersion}/um" _pulse_sdk_um)
    find_path(PULSE_D2D_SHADER_INCLUDE_DIR NAMES d2d1effecthelpers.hlsli HINTS "${_pulse_sdk_um}" NO_DEFAULT_PATH)
endif()
if(NOT PULSE_D2D_SHADER_INCLUDE_DIR)
    file(GLOB _pulse_hlsli_glob "C:/Program Files (x86)/Windows Kits/10/Include/10.*/um/d2d1effecthelpers.hlsli")
    if(_pulse_hlsli_glob)
        list(SORT _pulse_hlsli_glob)
        list(POP_BACK _pulse_hlsli_glob _pulse_hlsli_file)
        get_filename_component(PULSE_D2D_SHADER_INCLUDE_DIR "${_pulse_hlsli_file}" DIRECTORY)
    endif()
endif()
if(NOT PULSE_D2D_SHADER_INCLUDE_DIR)
    message(FATAL_ERROR "d2d1effecthelpers.hlsli was not found in Windows SDK Include/<ver>/um")
endif()

set(PULSE_GENERATED_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(PULSE_TILE_LIQUID_GLASS_SHADER_HEADER "${PULSE_GENERATED_SHADER_DIR}/tile_liquid_glass_ps.h")
add_custom_command(
    OUTPUT "${PULSE_TILE_LIQUID_GLASS_SHADER_HEADER}"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${PULSE_GENERATED_SHADER_DIR}"
    COMMAND "${PULSE_FXC_EXECUTABLE}" /nologo /T ps_4_0 /E main /O3 /WX /Qstrip_debug
            /D D2D_FULL_SHADER /D D2D_ENTRY=main
            /I "${PULSE_D2D_SHADER_INCLUDE_DIR}"
            /I "${CMAKE_CURRENT_LIST_DIR}/host/render/shaders"
            /Fh "${PULSE_TILE_LIQUID_GLASS_SHADER_HEADER}"
            /Vn g_tile_liquid_glass_ps
            "${CMAKE_CURRENT_LIST_DIR}/host/render/shaders/tile_liquid_glass.hlsl"
    DEPENDS
        "${CMAKE_CURRENT_LIST_DIR}/host/render/shaders/tile_liquid_glass.hlsl"
    COMMENT "Compiling tile_liquid_glass.hlsl (ps_4_0) with fxc.exe"
    VERBATIM
)
set(PULSE_MENU_FROST_SHADER_HEADERS)
foreach(_stage IN ITEMS vs ps)
    if(_stage STREQUAL "vs")
        set(_entry VSMain)
    else()
        set(_entry PSMain)
    endif()
    set(_header "${PULSE_GENERATED_SHADER_DIR}/menu_underlay_frost_${_stage}.h")
    add_custom_command(
        OUTPUT "${_header}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${PULSE_GENERATED_SHADER_DIR}"
        COMMAND "${PULSE_FXC_EXECUTABLE}" /nologo /T ${_stage}_4_0 /E ${_entry} /O3 /WX /Gis /Qstrip_debug
                /Fh "${_header}" /Vn g_menu_underlay_frost_${_stage}
                "${CMAKE_CURRENT_LIST_DIR}/host/render/shaders/menu_underlay_frost.hlsl"
        DEPENDS "${CMAKE_CURRENT_LIST_DIR}/host/render/shaders/menu_underlay_frost.hlsl"
        VERBATIM
    )
    list(APPEND PULSE_MENU_FROST_SHADER_HEADERS "${_header}")
endforeach()
add_custom_target(pulse_desktop_shaders DEPENDS "${PULSE_TILE_LIQUID_GLASS_SHADER_HEADER}" ${PULSE_MENU_FROST_SHADER_HEADERS})

# Desktop mount, lossless Shell COM snapshot/restore, 1/4-res wallpaper blur pyramid,
# D3D11/D2D/DComp renderer, on-demand overlays, and OLE IDropTarget/IDropSource.
add_library(pulse_desktop_mount_gfx STATIC
    host/desktop_mount.cpp
    host/live_wallpaper_detector.cpp
    host/live/widget_live_extras.cpp
    host/live/dock_system_provider.cpp
    host/render/wallpaper_backdrop.cpp
    host/render/desktop_renderer.cpp
    host/render/desktop_text_renderer.cpp
    host/render/host_backdrop_layer.cpp
    host/render/tile_liquid_glass.cpp
    host/render/live_wallpaper_source.cpp
    host/video/wallpaper_policy.cpp
    host/video/video_prober.cpp
    host/video/video_wallpaper_player.cpp
    host/video/transcode_manager.cpp
    host/video/video_wallpaper_controller.cpp
    host/render/vector_glyphs.cpp
    host/render/widget_painters.cpp
    host/render/widget_faces.cpp
    host/render/dock_tile.cpp
    host/render/liquid_dock_bar.cpp
    host/render/dock_genie.cpp
    host/overlays/desktop_overlays.cpp
    host/overlays/dock_stack_popup.cpp
    host/overlays/dock_system_panel.cpp
    host/overlays/desktop_menu_policy.cpp
    host/overlays/widget_icons.cpp
    host/overlays/widget_detail.cpp
    host/overlays/lunar_calendar.cpp
    host/overlays/desktop_shared_menu.cpp
    host/overlays/ole_drag_drop.cpp
    "${PULSE_TILE_LIQUID_GLASS_SHADER_HEADER}"
)
add_dependencies(pulse_desktop_mount_gfx pulse_desktop_shaders)
target_sources(pulse_desktop_mount_gfx INTERFACE "${CMAKE_CURRENT_LIST_DIR}/host/desktop_strings.rc")
target_compile_features(pulse_desktop_mount_gfx PUBLIC cxx_std_20)
target_compile_options(pulse_desktop_mount_gfx PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_mount_gfx PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_mount_gfx PUBLIC "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_include_directories(pulse_desktop_mount_gfx PRIVATE "${PULSE_GENERATED_SHADER_DIR}")
target_link_libraries(pulse_desktop_mount_gfx PUBLIC
    pulse_desktop_model
    pulse_desktop_ipc
    windowsapp
    runtimeobject
    d3d11
    d3dcompiler
    dxgi
    dxguid
    d2d1
    dwrite
    dcomp
    mfplat
    mfuuid
    mf
    mfreadwrite
    windowscodecs
    dwmapi
    shlwapi
    shell32
    ole32
    oleaut32
    uuid
    user32
    gdi32
    advapi32
    bcrypt
    psapi
    version
    shcore
    imm32
    iphlpapi
    ws2_32
)

# Reuse Pulse's actual menu implementation, not a desktop-specific imitation.
set(PULSE_SHARED_SRC "${CMAKE_CURRENT_LIST_DIR}/../../src")
add_library(pulse_desktop_shared_menu STATIC
    ${PULSE_SHARED_SRC}/ui/store_icons.cpp
    ${PULSE_SHARED_SRC}/ui/product_tiles.cpp
    ${PULSE_SHARED_SRC}/ui/fluent_menu.cpp
    ${PULSE_SHARED_SRC}/ui/native_menu_surface.cpp
    ${PULSE_SHARED_SRC}/ui/fluent_components.cpp
    ${PULSE_SHARED_SRC}/ui/ui_compositor.cpp
    ${PULSE_SHARED_SRC}/ui/typography.cpp
    ${PULSE_SHARED_SRC}/ui/edit_host.cpp
    ${PULSE_SHARED_SRC}/ui/tab_shape.cpp
    ${PULSE_SHARED_SRC}/ui/window_material.cpp
    ${PULSE_SHARED_SRC}/ui/lumatext_renderer.cpp
    ${PULSE_SHARED_SRC}/common/localization.cpp
    ${PULSE_SHARED_SRC}/common/zh_hant.cpp
    ${PULSE_SHARED_SRC}/common/service_text.cpp
)
target_include_directories(pulse_desktop_shared_menu PRIVATE "${PULSE_SDK_INCLUDE}")
target_compile_features(pulse_desktop_shared_menu PUBLIC cxx_std_20)
target_compile_options(pulse_desktop_shared_menu PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_shared_menu PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_desktop_shared_menu PUBLIC d3d11 d2d1 dwrite dxgi dcomp dwmapi
    shcore ole32 windowscodecs user32 gdi32 comctl32 uxtheme shell32 imm32 advapi32)

set(PULSE_LUMATEXT_ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/lumatext")
if(EXISTS "${PULSE_LUMATEXT_ROOT}/bin/lumatext.dll" AND EXISTS "${PULSE_LUMATEXT_ROOT}/lib/lumatext.lib")
    target_compile_definitions(pulse_desktop_shared_menu PUBLIC PULSE_HAS_LUMATEXT=1)
    target_include_directories(pulse_desktop_shared_menu PUBLIC "${PULSE_LUMATEXT_ROOT}/include")
    target_link_libraries(pulse_desktop_shared_menu PUBLIC "${PULSE_LUMATEXT_ROOT}/lib/lumatext.lib" delayimp)
    target_link_options(pulse_desktop_shared_menu PUBLIC /DELAYLOAD:lumatext.dll)
endif()
target_link_libraries(pulse_desktop_mount_gfx PUBLIC pulse_desktop_shared_menu)

# Host-facing Pulse plugin DLL (PulsePluginEntryV1 + PulsePluginSdkPreview).
set(PULSE_DESKTOP_PLUGIN_SOURCES
    plugin/plugin.cpp
    plugin/settings_page.cpp
)
add_library(pulse_desktop SHARED ${PULSE_DESKTOP_PLUGIN_SOURCES})
target_sources(pulse_desktop PRIVATE "${PULSE_SHARED_SRC}/common/localization.cpp" host/desktop_strings.rc)
set_source_files_properties(${PULSE_DESKTOP_PLUGIN_SOURCES} PROPERTIES OBJECT_DEPENDS "${PULSE_PUBLIC_C_HEADERS}")
target_compile_features(pulse_desktop PRIVATE cxx_std_20)
target_compile_options(pulse_desktop PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_options(pulse_desktop PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_desktop PRIVATE
    pulse_desktop_ipc
    user32
    shell32
    ole32
    oleaut32
    shlwapi
    advapi32
    bcrypt
)

# Out-of-process desktop host + unit tests.
add_executable(pulse_desktop_host
    host/host_main.cpp
    host/live/widget_data.cpp
)
target_compile_features(pulse_desktop_host PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_host PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_host PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_options(pulse_desktop_host PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_desktop_host PRIVATE
    pulse_desktop_mount_gfx
    pulse_desktop_model
    pulse_desktop_ipc
    winhttp
    iphlpapi
    pdh
)
add_dependencies(pulse_desktop pulse_desktop_host)

# Standalone memory-capped background video wallpaper transcoder (Media Foundation only, zero FFmpeg).
add_executable(pulse_wallpaper_transcoder
    transcoder/transcoder_main.cpp
)
target_compile_features(pulse_wallpaper_transcoder PRIVATE cxx_std_20)
target_compile_options(pulse_wallpaper_transcoder PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_wallpaper_transcoder PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_wallpaper_transcoder PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_options(pulse_wallpaper_transcoder PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_wallpaper_transcoder PRIVATE
    pulse_desktop_ipc
    d3d11
    dxgi
    dxguid
    mfplat
    mfuuid
    mf
    mfreadwrite
    ole32
    oleaut32
)
add_dependencies(pulse_desktop pulse_wallpaper_transcoder)
add_dependencies(pulse_desktop_host pulse_wallpaper_transcoder)

# Unit + real D3D11/DComp/MF integration tests for video wallpaper policy, cache, pause, player & transcoder.
add_executable(pulse_desktop_video_wallpaper_test
    tests/video_wallpaper_test.cpp
)
target_compile_features(pulse_desktop_video_wallpaper_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_wallpaper_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_wallpaper_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_video_wallpaper_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_video_wallpaper_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc)
add_dependencies(pulse_desktop_video_wallpaper_test pulse_wallpaper_transcoder)

# Hardware benchmark & quality/coexistence verification suite (not shipped).
add_executable(pulse_desktop_video_wallpaper_bench EXCLUDE_FROM_ALL
    tests/video_wallpaper_bench.cpp
)
target_compile_features(pulse_desktop_video_wallpaper_bench PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_wallpaper_bench PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_wallpaper_bench PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_video_wallpaper_bench PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_video_wallpaper_bench PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc pdh psapi)
add_dependencies(pulse_desktop_video_wallpaper_bench pulse_wallpaper_transcoder)
if(EXISTS "${PULSE_LUMATEXT_ROOT}/bin/lumatext.dll")
    add_custom_command(TARGET pulse_desktop_host POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PULSE_LUMATEXT_ROOT}/bin/lumatext.dll"
            "$<TARGET_FILE_DIR:pulse_desktop_host>/lumatext.dll")
endif()

add_executable(pulse_desktop_model_test
    tests/model_layout_test.cpp
)
target_compile_features(pulse_desktop_model_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_model_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_model_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_model_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_options(pulse_desktop_model_test PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_desktop_model_test PRIVATE pulse_desktop_model)

option(PULSE_DESKTOP_TESTS "Build desktop mount/restore/IPC/4K-renderer verification suite" ON)
if(PULSE_DESKTOP_TESTS)
    add_executable(pulse_desktop_mount_restore_test
        tests/mount_restore_test.cpp
    )
    target_compile_features(pulse_desktop_mount_restore_test PRIVATE cxx_std_20)
    target_compile_options(pulse_desktop_mount_restore_test PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(pulse_desktop_mount_restore_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_include_directories(pulse_desktop_mount_restore_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
    target_link_options(pulse_desktop_mount_restore_test PRIVATE /INCREMENTAL:NO /OPT:REF)
    target_link_libraries(pulse_desktop_mount_restore_test PRIVATE
        pulse_desktop_mount_gfx
        pulse_desktop_ipc
    )
endif()

add_executable(pulse_desktop_tile_layout_test tests/tile_layout_regression_test.cpp)
target_compile_features(pulse_desktop_tile_layout_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_layout_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_tile_layout_test PRIVATE pulse_desktop_model)
add_executable(pulse_desktop_menu_test tests/shared_menu_test.cpp)
target_compile_features(pulse_desktop_menu_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_menu_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_menu_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_material_live_test tests/material_live_test.cpp)
target_compile_features(pulse_desktop_material_live_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_material_live_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_material_live_test PRIVATE pulse_desktop_mount_gfx)
add_executable(pulse_desktop_mica_graph_test tests/mica_graph_test.cpp)
target_compile_features(pulse_desktop_mica_graph_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_mica_graph_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_mica_graph_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_settings_test tests/settings_page_test.cpp plugin/settings_page.cpp)
target_sources(pulse_desktop_settings_test PRIVATE "${PULSE_SHARED_SRC}/common/localization.cpp" host/desktop_strings.rc
    # Pulse's own validator: every tree the page submits must pass what the real settings host accepts.
    "${PULSE_SHARED_SRC}/app/extensions/native/ui_model.cpp" "${PULSE_SHARED_SRC}/app/extensions/ui_tree.cpp"
    "${PULSE_SHARED_SRC}/common/json_value.cpp")
target_compile_features(pulse_desktop_settings_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_settings_test PRIVATE /W4 /permissive- /utf-8)
target_include_directories(pulse_desktop_settings_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_settings_test PRIVATE pulse_desktop_ipc)

# The settings page painted by Pulse's own ExtensionTreeView (visual check; not part of the package).
add_executable(pulse_desktop_settings_render_test EXCLUDE_FROM_ALL tests/settings_render_test.cpp plugin/settings_page.cpp
    host/desktop_strings.rc "${PULSE_SHARED_SRC}/bench/child_edit_test.rc"
    "${PULSE_SHARED_SRC}/common/localization.cpp" "${PULSE_SHARED_SRC}/common/zh_hant.cpp"
    "${PULSE_SHARED_SRC}/common/service_text.cpp"
    "${PULSE_SHARED_SRC}/app/extensions/native/ui_model.cpp" "${PULSE_SHARED_SRC}/app/extensions/ui_tree.cpp"
    "${PULSE_SHARED_SRC}/app/frame_pump.cpp" "${PULSE_SHARED_SRC}/ui/extension_panel.cpp"
    "${PULSE_SHARED_SRC}/ui/extension_tree.cpp" "${PULSE_SHARED_SRC}/ui/extension_tree_controls.cpp"
    "${PULSE_SHARED_SRC}/ui/extension_tree_extra_controls.cpp" "${PULSE_SHARED_SRC}/ui/extension_tree_layout.cpp"
    "${PULSE_SHARED_SRC}/ui/extension_data_view.cpp" "${PULSE_SHARED_SRC}/ui/extension_data_controller.cpp"
    "${PULSE_SHARED_SRC}/ui/extension_data_accessibility.cpp" "${PULSE_SHARED_SRC}/ui/edit_host.cpp"
    "${PULSE_SHARED_SRC}/ui/fluent_menu.cpp" "${PULSE_SHARED_SRC}/ui/native_menu_surface.cpp"
    "${PULSE_SHARED_SRC}/ui/window_material.cpp" "${PULSE_SHARED_SRC}/ui/fluent_components.cpp"
    "${PULSE_SHARED_SRC}/ui/tab_shape.cpp" "${PULSE_SHARED_SRC}/ui/typography.cpp"
    "${PULSE_SHARED_SRC}/ui/ui_compositor.cpp" "${PULSE_SHARED_SRC}/ui/window_helpers.cpp"
    "${PULSE_SHARED_SRC}/ui/lumatext_renderer.cpp" "${PULSE_SHARED_SRC}/ui/store_icons.cpp"
    "${PULSE_SHARED_SRC}/ui/product_tiles.cpp")
target_compile_features(pulse_desktop_settings_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_settings_render_test PRIVATE /W4 /permissive- /utf-8)
target_include_directories(pulse_desktop_settings_render_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_settings_render_test PRIVATE pulse_desktop_ipc d3d11 d2d1 dwrite dxgi dcomp dwmapi
    shcore ole32 windowscodecs comctl32 uxtheme oleacc oleaut32 imm32 shell32 user32)
target_link_options(pulse_desktop_settings_render_test PRIVATE /MANIFEST:NO)

# Prototype: video wallpaper cost (Media Foundation hardware decode + DirectComposition), not shipped.
add_executable(pulse_desktop_video_wallpaper_probe EXCLUDE_FROM_ALL tests/video_wallpaper_probe.cpp tests/hevc_dxva.cpp)
target_compile_features(pulse_desktop_video_wallpaper_probe PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_wallpaper_probe PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_wallpaper_probe PRIVATE UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(pulse_desktop_video_wallpaper_probe PRIVATE d3d11 dxgi dcomp mfplat mfuuid mf mfreadwrite psapi windowscodecs pdh dxguid ole32
    oleaut32 user32)

# Diagnostic: what the Dock's tray scan sees (run at the user's integrity level).
add_executable(pulse_desktop_tray_probe EXCLUDE_FROM_ALL tests/tray_probe.cpp host/live/widget_live_extras.cpp)
target_compile_features(pulse_desktop_tray_probe PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tray_probe PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_tray_probe PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_tray_probe PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_tray_probe PRIVATE pulse_desktop_model pulse_desktop_ipc windowsapp runtimeobject
    shlwapi shell32 ole32 oleaut32 uuid user32 gdi32 advapi32 psapi iphlpapi ws2_32 wlanapi powrprof imm32 comctl32
    bthprops version)

add_executable(pulse_desktop_edit_test tests/hosted_edit_test.cpp)
target_compile_features(pulse_desktop_edit_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_edit_test PRIVATE /W4 /permissive- /utf-8)
target_include_directories(pulse_desktop_edit_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_options(pulse_desktop_edit_test PRIVATE /INCREMENTAL:NO /OPT:REF)
target_link_libraries(pulse_desktop_edit_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_feedback_test tests/feedback_regression_test.cpp)
target_compile_features(pulse_desktop_feedback_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_feedback_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_feedback_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_tile_hover_render_test tests/tile_hover_render_test.cpp)
target_compile_features(pulse_desktop_tile_hover_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_hover_render_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_tile_hover_render_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_widget_gallery_render_test tests/widget_gallery_render_test.cpp)
target_compile_features(pulse_desktop_widget_gallery_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_widget_gallery_render_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_widget_gallery_render_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_drag_damage_test tests/drag_damage_test.cpp)
target_compile_features(pulse_desktop_drag_damage_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_drag_damage_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_drag_damage_test PRIVATE pulse_desktop_model)

add_executable(pulse_desktop_drag_render_bench tests/drag_render_bench.cpp)
target_compile_features(pulse_desktop_drag_render_bench PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_drag_render_bench PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_drag_render_bench PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_drag_pixels_test tests/drag_pixels_test.cpp)
target_compile_features(pulse_desktop_drag_pixels_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_drag_pixels_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_drag_pixels_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_hover_native_surface_test tests/hover_native_surface_test.cpp)
target_compile_features(pulse_desktop_hover_native_surface_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_hover_native_surface_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_hover_native_surface_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_tile_reorder_test tests/tile_reorder_test.cpp)
target_compile_features(pulse_desktop_tile_reorder_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_reorder_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_tile_reorder_test PRIVATE pulse_desktop_model)

add_executable(pulse_desktop_group_features_test tests/group_features_test.cpp)
target_compile_features(pulse_desktop_group_features_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_group_features_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_group_features_test PRIVATE pulse_desktop_model)

add_executable(pulse_desktop_menu_policy_test tests/menu_policy_test.cpp)
target_compile_features(pulse_desktop_menu_policy_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_menu_policy_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_menu_policy_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_host_job_test tests/host_job_test.cpp)
target_compile_features(pulse_desktop_host_job_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_host_job_test PRIVATE /W4 /permissive- /utf-8)
target_include_directories(pulse_desktop_host_job_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pulse_desktop_host_job_test PRIVATE shell32)

add_executable(pulse_desktop_icon_label_test tests/icon_label_test.cpp)
target_compile_features(pulse_desktop_icon_label_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_icon_label_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_icon_label_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_text_contrast_test tests/text_contrast_test.cpp)
target_compile_features(pulse_desktop_text_contrast_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_text_contrast_test PRIVATE /W4 /WX /permissive- /utf-8)
target_include_directories(pulse_desktop_text_contrast_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

add_executable(pulse_desktop_text_contrast_render_test tests/text_contrast_render_test.cpp)
target_compile_features(pulse_desktop_text_contrast_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_text_contrast_render_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_text_contrast_render_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_external_drag_test tests/external_drag_test.cpp host/overlays/ole_drag_drop.cpp)
target_compile_features(pulse_desktop_external_drag_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_external_drag_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_external_drag_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_external_drag_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pulse_desktop_external_drag_test PRIVATE shell32 ole32 user32 uuid)

add_executable(pulse_desktop_tile_drag_session_test tests/tile_drag_session_test.cpp)
target_compile_features(pulse_desktop_tile_drag_session_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_drag_session_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_tile_drag_session_test PRIVATE pulse_desktop_model)

add_executable(pulse_desktop_tile_drag_host_test tests/tile_drag_host_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_tile_drag_host_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_drag_host_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_tile_drag_host_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_tile_drag_host_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_tile_drag_host_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)

add_executable(pulse_desktop_tile_reorder_render_test tests/tile_reorder_render_test.cpp)
target_compile_features(pulse_desktop_tile_reorder_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_reorder_render_test PRIVATE /W4 /permissive- /utf-8)
target_link_libraries(pulse_desktop_tile_reorder_render_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_tile_drag_contract_test tests/tile_drag_contract_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_tile_drag_contract_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_drag_contract_test PRIVATE /W4 /permissive- /utf-8)
target_compile_definitions(pulse_desktop_tile_drag_contract_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_tile_drag_contract_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_tile_drag_contract_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)

add_executable(pulse_desktop_group_render_test tests/group_render_test.cpp)
target_compile_features(pulse_desktop_group_render_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_group_render_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_group_render_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_group_menu_test tests/group_menu_test.cpp)
target_compile_features(pulse_desktop_group_menu_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_group_menu_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_group_menu_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_group_host_test tests/group_host_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_group_host_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_group_host_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_group_host_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_group_host_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_group_host_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)

add_executable(pulse_desktop_liquid_glass_test tests/liquid_glass_test.cpp)
target_compile_features(pulse_desktop_liquid_glass_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_liquid_glass_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_liquid_glass_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_liquid_glass_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_liquid_glass_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model)

add_executable(pulse_desktop_tile_persistence_host_test tests/tile_persistence_host_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_tile_persistence_host_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_tile_persistence_host_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_tile_persistence_host_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_tile_persistence_host_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_tile_persistence_host_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)

add_executable(pulse_desktop_text_route_test tests/text_route_test.cpp)
target_compile_features(pulse_desktop_text_route_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_text_route_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_text_route_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_preview_host_test tests/preview_host_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_preview_host_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_preview_host_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_preview_host_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_preview_host_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_preview_host_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)

add_executable(pulse_desktop_preview_dispatch_test tests/preview_dispatch_test.cpp)
target_compile_features(pulse_desktop_preview_dispatch_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_preview_dispatch_test PRIVATE /W4 /permissive- /utf-8)
target_include_directories(pulse_desktop_preview_dispatch_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_preview_dispatch_test PRIVATE pulse_desktop_ipc)

add_executable(pulse_desktop_widget_text_route_test tests/widget_text_route_test.cpp)
target_compile_features(pulse_desktop_widget_text_route_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_widget_text_route_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_widget_text_route_test PRIVATE pulse_desktop_mount_gfx)

# RC does not reliably emit dependencies for transitive generated string includes.
set_source_files_properties("${CMAKE_CURRENT_LIST_DIR}/host/desktop_strings.rc" PROPERTIES
    OBJECT_DEPENDS "${PULSE_SHARED_SRC}/common/strings.rcinc;${PULSE_SHARED_SRC}/common/strings_zh_tw.rcinc;${PULSE_SHARED_SRC}/common/string_ids.h")

add_executable(pulse_desktop_video_glass_latency_test tests/video_glass_latency_test.cpp)
target_compile_features(pulse_desktop_video_glass_latency_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_glass_latency_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_glass_latency_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_video_glass_latency_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_video_glass_latency_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model)

add_executable(pulse_desktop_video_glass_mailbox_test tests/video_glass_mailbox_test.cpp)
target_compile_features(pulse_desktop_video_glass_mailbox_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_glass_mailbox_test PRIVATE /W4 /WX /permissive- /utf-8)
target_include_directories(pulse_desktop_video_glass_mailbox_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")

add_executable(pulse_desktop_video_probe_job_test tests/video_probe_job_test.cpp)
target_compile_features(pulse_desktop_video_probe_job_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_probe_job_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_probe_job_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_desktop_video_probe_job_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_video_glass_playback_test tests/video_glass_playback_test.cpp)
target_compile_features(pulse_desktop_video_glass_playback_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_glass_playback_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_glass_playback_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_desktop_video_glass_playback_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_video_glass_retry_test tests/video_glass_retry_test.cpp)
target_compile_features(pulse_desktop_video_glass_retry_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_glass_retry_test PRIVATE /W4 /WX /permissive- /utf-8)
target_include_directories(pulse_desktop_video_glass_retry_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}")
target_link_libraries(pulse_desktop_video_glass_retry_test PRIVATE user32)

add_executable(pulse_desktop_video_visibility_test tests/video_visibility_test.cpp)
target_compile_features(pulse_desktop_video_visibility_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_visibility_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_visibility_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(pulse_desktop_video_visibility_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_ipc)

add_executable(pulse_desktop_video_pause_host_test tests/video_pause_host_test.cpp host/live/widget_data.cpp)
target_compile_features(pulse_desktop_video_pause_host_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_video_pause_host_test PRIVATE /W4 /WX /permissive- /utf-8)
target_compile_definitions(pulse_desktop_video_pause_host_test PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_include_directories(pulse_desktop_video_pause_host_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}" "${PULSE_SDK_INCLUDE}")
target_link_libraries(pulse_desktop_video_pause_host_test PRIVATE pulse_desktop_mount_gfx pulse_desktop_model pulse_desktop_ipc winhttp iphlpapi pdh)
add_executable(pulse_desktop_menu_underlay_gpu_test tests/menu_underlay_gpu_test.cpp)
target_compile_features(pulse_desktop_menu_underlay_gpu_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_menu_underlay_gpu_test PRIVATE /W4 /WX /permissive- /utf-8)
target_link_libraries(pulse_desktop_menu_underlay_gpu_test PRIVATE pulse_desktop_mount_gfx)

add_executable(pulse_desktop_menu_material_retry_test tests/menu_material_retry_test.cpp)
target_compile_features(pulse_desktop_menu_material_retry_test PRIVATE cxx_std_20)
target_compile_options(pulse_desktop_menu_material_retry_test PRIVATE /W4 /WX /permissive- /utf-8)
target_include_directories(pulse_desktop_menu_material_retry_test PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../../src")
target_link_libraries(pulse_desktop_menu_material_retry_test PRIVATE pulse_desktop_mount_gfx)
