#pragma once
#include "extension_tree_controls.h"
namespace pulse::ui {
inline float ExtensionSettingsRowControlWidth(const extensions::UiNode& child, float scale) {
    if (child.max_width > 0) return static_cast<float>(child.max_width) * scale;
    if (child.min_width > 0) return static_cast<float>(child.min_width) * scale;
    if (child.type == extensions::UiNodeType::Switch ||
        child.type == extensions::UiNodeType::Checkbox ||
        child.type == extensions::UiNodeType::Radio)
        return 48.0f * scale;
    if (child.type == extensions::UiNodeType::Tag) return 96.0f * scale;
    return 140.0f * scale;
}
// A settings row keeps its controls on the label's line while the label keeps 140 DIP (gap
// included): at their max_width when that fits, else narrowed toward their min_width. Returns the
// total width (gaps included) and fills widths, or returns 0 when the controls go under the label.
template <class Visible>
float ExtensionSettingsRowInlineWidths(const extensions::UiNode& node, float inner_w, float gap, float scale,
                                       Visible&& visible, std::vector<float>& widths) {
    widths.assign(node.children.size(), 0.0f);
    std::vector<float> least(node.children.size(), 0.0f);
    float preferred_sum = 0.0f, least_sum = 0.0f;
    size_t count = 0;
    for (size_t i = 0; i < node.children.size(); ++i) {
        if (!visible(i)) continue;
        const auto& child = node.children[i];
        widths[i] = (std::min)(inner_w, ExtensionSettingsRowControlWidth(child, scale));
        least[i] = child.min_width > 0 ? (std::min)(widths[i], static_cast<float>(child.min_width) * scale) : widths[i];
        preferred_sum += widths[i];
        least_sum += least[i];
        ++count;
    }
    if (count == 0) return 0.0f;
    const float gaps = gap * static_cast<float>(count - 1);
    const float room = inner_w - 140.0f * scale - gaps;
    if (least_sum > room) return 0.0f;
    if (preferred_sum > room) {
        const float t = preferred_sum > least_sum ? (room - least_sum) / (preferred_sum - least_sum) : 1.0f;
        preferred_sum = 0.0f;
        for (size_t i = 0; i < widths.size(); ++i) {
            if (widths[i] <= 0.0f) continue;
            widths[i] = least[i] + (widths[i] - least[i]) * t;
            preferred_sum += widths[i];
        }
    }
    return preferred_sum + gaps;
}
bool DrawExtensionExtraControl(const extensions::UiNode& node, const ExtensionControlVisual& visual,
    Compositor& compositor, fluent::Painter& painter, const Theme& theme);
float MeasureExtensionExtraControl(const extensions::UiNode& node, float width, float scale,
    fluent::Painter& painter);
// Theme-aware colour token (text, secondary, accent, success, warning, danger, #RRGGBB...).
bool ResolveExtensionColorToken(std::string_view token, const Theme& theme, bool dark, D2D1_COLOR_F& out) noexcept;
// Maps a host-pixel point inside a canvas to its script viewBox units (clamped to the box).
bool ExtensionCanvasPoint(const extensions::UiNode& node, D2D1_RECT_F bounds, float scale, float x, float y,
                          float& view_x, float& view_y) noexcept;
int HitExtensionExtraControlPart(const extensions::UiNode& node, D2D1_RECT_F bounds,
    float x, float y, float scale);
}