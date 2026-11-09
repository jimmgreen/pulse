#pragma once
#include "extension_tree_controls.h"
namespace pulse::ui {
bool DrawExtensionExtraControl(const extensions::UiNode& node, const ExtensionControlVisual& visual,
    Compositor& compositor, fluent::Painter& painter, const Theme& theme);
float MeasureExtensionExtraControl(const extensions::UiNode& node, float width, float scale,
    fluent::Painter& painter);
int HitExtensionExtraControlPart(const extensions::UiNode& node, D2D1_RECT_F bounds,
    float x, float y, float scale);
}
