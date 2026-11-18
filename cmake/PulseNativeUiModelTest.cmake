add_executable(pulse_native_ui_model_test
    src/bench/native_sdk/ui_model_test.cpp
    src/app/extensions/native/ui_model.cpp
    src/app/extensions/ui_tree.cpp src/common/json_value.cpp)
target_compile_features(pulse_native_ui_model_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_ui_model_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(pulse_native_ui_model_test PRIVATE /W4 /permissive- /utf-8)
endif()

add_executable(pulse_native_ui_model_catalog_test
    src/bench/native_sdk/ui_model_catalog_test.cpp
    src/app/extensions/native/ui_adapter.cpp src/app/extensions/native/ui_data_adapter.cpp src/app/extensions/native/ui_model.cpp
    ${PULSE_EXTENSION_PANEL_RENDER_SOURCES}
    src/app/extensions/ui_tree.cpp src/common/json_value.cpp)
target_compile_features(pulse_native_ui_model_catalog_test PRIVATE cxx_std_20)
target_compile_definitions(pulse_native_ui_model_catalog_test PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(pulse_native_ui_model_catalog_test PRIVATE
    d3d11 d2d1 dwrite dxgi dcomp dwmapi shcore ole32 oleacc oleaut32 windowscodecs comctl32 pulse_localization)
pulse_enable_lumatext(pulse_native_ui_model_catalog_test)
if(MSVC)
    target_compile_options(pulse_native_ui_model_catalog_test PRIVATE /W4 /permissive- /utf-8)
endif()
