#include "extension_tree_extra_controls.h"
#include <algorithm>
#include <charconv>

namespace pulse::ui {
using extensions::UiNodeType;
namespace {
std::wstring WideExtra(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<size_t>(count), 0);
    if (count) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), count);
    return result;
}
fluent::BadgeKind BadgeKind(const std::string& value) {
    if (value == "accent") return fluent::BadgeKind::Accent;
    if (value == "success") return fluent::BadgeKind::Success;
    if (value == "warning") return fluent::BadgeKind::Warning;
    if (value == "danger") return fluent::BadgeKind::Danger;
    if (value == "keycap") return fluent::BadgeKind::Keycap;
    return fluent::BadgeKind::Neutral;
}
}
float MeasureExtensionExtraControl(const extensions::UiNode& node, float width, float scale,
    fluent::Painter& painter) {
    (void)width;
    (void)painter;
    float height = 0;
    switch (node.type) {
    case UiNodeType::TrayCard: height = 76 * scale; break;
    case UiNodeType::CommandSearch: height = 40 * scale; break;
    case UiNodeType::SidebarSection: height = 32 * scale; break;
    case UiNodeType::DriveItem: height = 76 * scale; break;
    case UiNodeType::PaneHeader: height = 48 * scale; break;
    case UiNodeType::FileRow: height = 40 * scale; break;
    case UiNodeType::StagingTray: height = (node.expanded ? 100.0f : 40.0f) * scale; break;
    case UiNodeType::StagingItem: height = 36 * scale; break;
    case UiNodeType::StatusBar: height = 32 * scale; break;
    case UiNodeType::Tooltip: height = 32 * scale; break;
    case UiNodeType::TitleBarButton: height = 32 * scale; break;
    default: return 0;
    }
    return (std::max)(height, node.min_height * scale);
}
int HitExtensionExtraControlPart(const extensions::UiNode& node, D2D1_RECT_F bounds,
    float x, float y, float scale) {
    if (x < bounds.left || x >= bounds.right || y < bounds.top || y >= bounds.bottom) return -1;
    if (node.type == UiNodeType::PaneHeader) {
        if (x >= bounds.right - 31 * scale && x < bounds.right - 3 * scale &&
            y >= bounds.top + 3 * scale && y < bounds.bottom - 3 * scale) return 1;
        const float width = (std::min)(260 * scale, (bounds.right - bounds.left) * 0.38f);
        if (x >= bounds.right - 42 * scale - width && x < bounds.right - 42 * scale &&
            y >= bounds.top + 5 * scale && y < bounds.bottom - 5 * scale) return 0;
        return -1;
    }
    if (node.type == UiNodeType::StagingTray) {
        return !node.action_text.empty() && x >= bounds.right - 82 * scale && x < bounds.right - 10 * scale &&
            y >= bounds.top + 7.5f * scale && y < bounds.top + 26.5f * scale ? 0 : -1;
    }
    return 0;
}
bool DrawExtensionExtraControl(const extensions::UiNode& node, const ExtensionControlVisual& visual,
    Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    (void)compositor;
    const auto text = WideExtra(node.text), glyph = WideExtra(node.glyph), detail = WideExtra(node.detail);
    const auto summary = WideExtra(node.summary);
    const auto bounds = visual.bounds;
    auto state = visual.state;
    switch (node.type) {
    case UiNodeType::TrayCard: {
        fluent::TrayCardSpec spec;
        spec.bounds = bounds; spec.summary = summary.empty() ? text : summary; spec.detail = detail;
        spec.item_count = node.item_count; spec.move_intent = node.move_intent; spec.state = state;
        painter.DrawTrayCard(spec);
        return true;
    }
    case UiNodeType::CommandSearch: {
        const auto placeholder = WideExtra(node.placeholder), keycap = WideExtra(node.keycap);
        fluent::CommandSearchBoxSpec spec;
        spec.bounds = bounds; spec.placeholder = placeholder; spec.keycap = keycap; spec.state = state;
        spec.hosted_edit = true;
        spec.suppress_text = visual.window && (GetWindowTextLengthW(visual.window) > 0 || state.focused);
        painter.DrawCommandSearchBox(spec);
        return true;
    }
    case UiNodeType::SidebarSection: {
        fluent::SidebarSectionHeaderSpec spec;
        spec.bounds = bounds; spec.text = text; spec.glyph = glyph; spec.state = state;
        spec.expanded = state.checked;
        painter.DrawSidebarSectionHeader(spec);
        return true;
    }
    case UiNodeType::DriveItem: {
        fluent::DriveSidebarItemSpec spec;
        spec.bounds = bounds; spec.name = text; spec.detail = detail; spec.glyph = glyph; spec.state = state;
        spec.capacity = node.capacity / 100.0f;
        spec.free_bytes = node.free_bytes < 0 ? kCapacityUnknownFree : static_cast<uint64_t>(node.free_bytes);
        spec.total_bytes = static_cast<uint64_t>(node.total_bytes);
        spec.icon_color = theme.text_secondary; spec.bar_color = theme.accent;
        painter.DrawDriveSidebarItem(spec);
        return true;
    }
    case UiNodeType::PaneHeader: {
        const auto filter = WideExtra(node.filter_text);
        fluent::PaneHeaderSpec spec;
        spec.bounds = bounds; spec.title = text; spec.summary = summary; spec.filter_text = filter;
        spec.filter_state = spec.sort_state = state;
        spec.filter_state.enabled = state.enabled && !node.filter_command.empty();
        spec.sort_state.enabled = state.enabled && !node.sort_command.empty();
        spec.filter_state.hovered = state.hovered && visual.hover_part == 0;
        spec.sort_state.hovered = state.hovered && visual.hover_part == 1;
        spec.filter_state.pressed = state.pressed && visual.pressed_part == 0;
        spec.sort_state.pressed = state.pressed && visual.pressed_part == 1;
        painter.DrawPaneHeader(spec);
        return true;
    }
    case UiNodeType::FileRow: {
        const auto modified = WideExtra(node.modified), type = WideExtra(node.file_type);
        const auto size = WideExtra(node.file_size), status = WideExtra(node.status_text);
        fluent::FileRowContentSpec spec;
        spec.bounds = bounds; spec.name = text; spec.modified = modified; spec.type = type;
        spec.size = size; spec.glyph = glyph; spec.status = status; spec.state = state;
        spec.state.selected = state.selected || node.selected; spec.cut = node.cut; spec.status_kind = BadgeKind(node.badge_kind);
        for (const auto& hex : node.tag_colors) {
            if (spec.tag_count == spec.tag_colors.size()) break;
            unsigned value = 0;
            if (hex.size() == 7 && std::from_chars(hex.data() + 1, hex.data() + 7, value, 16).ec == std::errc{})
                spec.tag_colors[spec.tag_count++] = HexColor(value);
        }
        painter.DrawFileRowContent(spec);
        return true;
    }
    case UiNodeType::StagingTray: {
        const auto helper = WideExtra(node.helper), count = WideExtra(node.count_label), action = WideExtra(node.action_text);
        fluent::StagingTrayPanelSpec spec;
        spec.bounds = bounds; spec.title = text; spec.helper = helper; spec.count_label = count;
        spec.action_text = action; spec.item_count = node.item_count; spec.state = state;
        spec.action_hovered = state.hovered && visual.hover_part == 0;
        spec.expanded = state.checked;
        painter.DrawStagingTrayPanel(spec);
        return true;
    }
    case UiNodeType::StagingItem: {
        fluent::StagingItemSpec spec;
        spec.bounds = bounds; spec.name = text; spec.state = state; spec.missing = node.missing;
        painter.DrawStagingItem(spec);
        return true;
    }
    case UiNodeType::StatusBar: {
        const auto center = WideExtra(node.center_text), right = WideExtra(node.right_text);
        fluent::StatusBarSpec spec;
        spec.bounds = bounds; spec.left_text = text; spec.center_text = center; spec.right_text = right;
        spec.ready = node.ready;
        painter.DrawStatusBar(spec);
        return true;
    }
    case UiNodeType::Tooltip:
        painter.DrawTooltip(bounds, text);
        return true;
    case UiNodeType::TitleBarButton: {
        const auto role = node.title_bar_role == "minimize" ? fluent::TitleBarButtonRole::Minimize
            : node.title_bar_role == "maximize" ? fluent::TitleBarButtonRole::Maximize
            : node.title_bar_role == "restore" ? fluent::TitleBarButtonRole::Restore
            : node.title_bar_role == "close" ? fluent::TitleBarButtonRole::Close : fluent::TitleBarButtonRole::Custom;
        painter.DrawTitleBarButton(bounds, role, glyph, state);
        return true;
    }
    default: return false;
    }
}
}
