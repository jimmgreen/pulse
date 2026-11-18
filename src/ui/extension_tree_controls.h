#pragma once
#include "../app/extensions/ui_tree.h"
#include "fluent_components.h"
namespace pulse::ui {
struct ExtensionControlVisual {
    HWND host{}, window{};
    D2D1_RECT_F bounds{};
    fluent::ControlState state{};
    int hover_part = -1, pressed_part = -1;
    float scale = 1;
    bool dark = false;
};
bool DrawExtensionControl(const extensions::UiNode &node, const ExtensionControlVisual &visual, Compositor &compositor,
                          fluent::Painter &painter, const Theme &theme);
float MeasureExtensionControl(const extensions::UiNode &node, std::wstring_view text, float width, float scale,
                              fluent::Painter &painter);
int ExtensionControlPart(const extensions::UiNode &node, D2D1_RECT_F bounds, float x, float y, float scale,
                         fluent::Painter &painter);
bool IsExtensionBooleanControl(const extensions::UiNode &node);
bool IsExtensionDataControl(const extensions::UiNode &node);
} // namespace pulse::ui
