#include "extension_tree_controls.h"
#include "edit_host.h"
#include "extension_tree_extra_controls.h"
#include <algorithm>
#include <charconv>
#include <commctrl.h>
namespace pulse::ui {
using extensions::UiNode;
using extensions::UiNodeType;
namespace {
std::wstring Wide(std::string_view text) {
    int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(count, 0);
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), count);
    return out;
}
std::wstring WindowText(HWND window) {
    int count = GetWindowTextLengthW(window);
    std::wstring out(count + 1, 0);
    GetWindowTextW(window, out.data(), count + 1);
    out.resize(count);
    return out;
}
bool Contains(D2D1_RECT_F bounds, float x, float y) {
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom;
}
fluent::ButtonKind ButtonKind(const std::string &kind) {
    return kind == "primary"             ? fluent::ButtonKind::Primary
           : kind == "danger"            ? fluent::ButtonKind::Danger
           : kind == "transparent"       ? fluent::ButtonKind::Transparent
           : kind == "toggle"            ? fluent::ButtonKind::Toggle
           : kind == "transparentToggle" ? fluent::ButtonKind::TransparentToggle
                                         : fluent::ButtonKind::Standard;
}
fluent::InfoBarKind InfoKind(const std::string &kind) {
    return kind == "success"   ? fluent::InfoBarKind::Success
           : kind == "warning" ? fluent::InfoBarKind::Warning
           : kind == "error"   ? fluent::InfoBarKind::Error
           : kind == "neutral" ? fluent::InfoBarKind::Neutral
                               : fluent::InfoBarKind::Informational;
}
struct InfoSpec {
    std::wstring title, message, glyph, actions[2];
    fluent::InfoBarSpec spec;
    InfoSpec(const UiNode &node, D2D1_RECT_F bounds) {
        title = Wide(node.text);
        message = Wide(node.message);
        glyph = Wide(node.glyph);
        spec.bounds = bounds;
        spec.title = title;
        spec.message = message;
        spec.glyph = glyph;
        spec.kind = InfoKind(node.info_kind);
        spec.show_close = !node.close_command.empty();
        for (size_t i = 0; i < std::min<size_t>(2, node.actions.size()); ++i) {
            actions[i] = Wide(node.actions[i].text);
            spec.actions[i] = actions[i];
        }
    }
};
} // namespace
bool IsExtensionBooleanControl(const UiNode &node) {
    return node.type == UiNodeType::Checkbox || node.type == UiNodeType::Switch || node.type == UiNodeType::Radio ||
           node.type == UiNodeType::SidebarSection || node.type == UiNodeType::StagingTray ||
           (node.type == UiNodeType::Button &&
            (node.button_kind == "toggle" || node.button_kind == "transparentToggle"));
}
bool IsExtensionDataControl(const UiNode &node) {
    return node.type == UiNodeType::VirtualList || node.type == UiNodeType::Tree || node.type == UiNodeType::DataTable;
}
float MeasureExtensionControl(const UiNode &node, std::wstring_view text, float width, float scale,
                              fluent::Painter &painter) {
    float height = std::max(32, node.min_height) * scale;
    height = std::max(height, MeasureExtensionExtraControl(node, width, scale, painter));
    if (node.type == UiNodeType::SplitPane)
        height = std::max(height, 240 * scale);
    if (node.type == UiNodeType::Text && !node.min_height)
        height = 0;  // text takes the height of its lines; a control's 32 DIP floor only padded it
    if (node.type == UiNodeType::Text)
        height = std::max(height, node.text_role == "caption"   ? painter.MeasureWrappedCaptionHeight(text, width)
                                  : node.text_role == "section" ? painter.MeasureWrappedSectionHeight(text, width)
                                                                : painter.MeasureWrappedBodyHeight(text, width));
    if (node.type == UiNodeType::List || IsExtensionDataControl(node))
        height = std::max(height, (IsExtensionDataControl(node) ? 240 : 120) * scale);
    if (node.type == UiNodeType::InfoBar) {
        InfoSpec info(node, {0, 0, width, height});
        height = std::max(height, painter.InfoBarStripHeight(info.spec, width));
    }
    if (node.type == UiNodeType::EmptyState)
        height = std::max(height, 180 * scale);
    if (node.type == UiNodeType::Slider)
        height = std::max(height, fluent::StepSliderHeight(scale) +
                                      (node.options.empty() ? 0.0f : painter.MeasureWrappedCaptionHeight(L"0", width)));
    return height;
}
int ExtensionControlPart(const UiNode &node, D2D1_RECT_F bounds, float x, float y, float scale,
                         fluent::Painter &painter) {
    if (!Contains(bounds, x, y))
        return -1;
    if (node.type == UiNodeType::Tabs || node.type == UiNodeType::Segmented) {
        int count = static_cast<int>(node.type == UiNodeType::Tabs ? node.children.size() : node.options.size());
        return std::clamp(static_cast<int>((x - bounds.left) * count / std::max(1.0f, bounds.right - bounds.left)), 0,
                          std::max(0, count - 1));
    }
    if (node.type == UiNodeType::SplitButton)
        return x >= bounds.right - 34 * scale ? 1 : 0;
    if (node.type == UiNodeType::InfoBar) {
        InfoSpec info(node, bounds);
        auto layout = painter.LayoutInfoBar(info.spec);
        for (int i = 0; i < 2; ++i)
            if (Contains(layout.actions[i], x, y))
                return i;
        if (Contains(layout.close, x, y))
            return 2;
        return -1;
    }
    int extra = HitExtensionExtraControlPart(node, bounds, x, y, scale);
    if (node.type == UiNodeType::PaneHeader || node.type == UiNodeType::StagingTray)
        return extra;
    return extra >= 0 ? extra : 0;
}
bool DrawExtensionControl(const UiNode &node, const ExtensionControlVisual &value, Compositor &compositor,
                          fluent::Painter &painter, const Theme &theme) {
    auto state = value.state;
    auto bounds = value.bounds;
    auto *dc = compositor.Dc();
    const float scale = value.scale;
    auto text = WindowText(value.window), label = Wide(node.text), glyph = Wide(node.glyph), keycap = Wide(node.keycap),
         detail = Wide(node.detail), message = Wide(node.message);
    state.checked = IsExtensionBooleanControl(node) && SendMessageW(value.window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    auto effective = value;
    effective.state = state;
    switch (node.type) {
    case UiNodeType::Button:
    case UiNodeType::MenuButton:
    case UiNodeType::Select: {
        fluent::ButtonSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.glyph = glyph;
        spec.keycap = keycap;
        spec.kind = ButtonKind(node.button_kind);
        spec.state = state;
        spec.icon_only = node.icon_only;
        spec.bordered = node.bordered;
        spec.drop_down = node.type != UiNodeType::Button;
        if (node.type == UiNodeType::Select) {
            int selected = static_cast<int>(SendMessageW(value.window, CB_GETCURSEL, 0, 0));
            label = selected >= 0 && selected < static_cast<int>(node.options.size()) ? Wide(node.options[selected])
                                                                                      : std::wstring{};
            spec.text = label;
        }
        painter.DrawButton(spec);
        break;
    }
    case UiNodeType::SplitButton: {
        fluent::SplitButtonSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.glyph = glyph;
        spec.primary_state = state;
        spec.menu_state = state;
        spec.primary_state.hovered = state.hovered && value.hover_part == 0;
        spec.menu_state.hovered = state.hovered && value.hover_part == 1;
        spec.primary_state.pressed = state.pressed && value.pressed_part == 0;
        spec.menu_state.pressed = state.pressed && value.pressed_part == 1;
        painter.DrawSplitButton(spec);
        break;
    }
    case UiNodeType::TextInput: {
        fluent::TextFieldSpec spec;
        spec.bounds = bounds;
        spec.state = state;
        spec.hosted_edit = true;
        auto placeholder = Wide(node.placeholder);
        spec.placeholder = placeholder;
        spec.leading_glyph = glyph;
        auto trailing = Wide(node.trailing_glyph), badge = Wide(node.badge_text);
        spec.trailing_glyph = trailing;
        spec.trailing_badge = badge;
        spec.trailing_keycap = keycap;
        bool show_placeholder = text.empty() && !state.focused && !placeholder.empty();
        spec.suppress_text = !show_placeholder;
        painter.DrawTextField(spec);
        if (show_placeholder)
            break;
        RECT edit{};
        GetClientRect(value.window, &edit);
        MapWindowPoints(value.window, value.host, reinterpret_cast<POINT *>(&edit), 2);
        return DrawPulsePaintedChildEdit(compositor, compositor.TextFormat(), ColorFromRef(EditTextColor(value.dark)),
                                         ColorFromRef(EditBackColor(value.dark)), value.window,
                                         {static_cast<float>(edit.left), static_cast<float>(edit.top),
                                          static_cast<float>(edit.right), static_cast<float>(edit.bottom)});
    }
    case UiNodeType::Checkbox:
        painter.DrawCheckBox(bounds, label, state);
        break;
    case UiNodeType::Radio:
        painter.DrawRadioButton(bounds, label, state);
        break;
    case UiNodeType::Switch:
        painter.DrawSwitch(bounds, label, state);
        break;
    case UiNodeType::Slider: {
        fluent::StepSliderSpec spec;
        spec.bounds = bounds;
        spec.state = state;
        spec.stops = node.stops;
        spec.position = static_cast<float>(SendMessageW(value.window, TBM_GETPOS, 0, 0));
        // Options name the stops: a caption under each one, the current stop in the text color.
        if (!node.options.empty())
            spec.bounds.bottom = std::min(bounds.bottom, bounds.top + fluent::StepSliderHeight(scale));
        painter.DrawStepSlider(spec);
        const int stops = std::max(node.stops, 2);
        const int current = static_cast<int>(std::lround(spec.position));
        for (int index = 0; index < stops && index < static_cast<int>(node.options.size()); ++index) {
            const float x = fluent::StepSliderStopX(spec.bounds, stops, static_cast<float>(index), scale);
            const float half = 60 * scale;
            auto cell = D2D1::RectF(x - half, spec.bounds.bottom, x + half, bounds.bottom);
            auto align = fluent::HorizontalAlignment::Center;
            if (index == 0) { cell.left = bounds.left; cell.right = bounds.left + 2 * half; align = fluent::HorizontalAlignment::Left; }
            if (index + 1 == stops) { cell.right = bounds.right; cell.left = bounds.right - 2 * half; align = fluent::HorizontalAlignment::Right; }
            painter.DrawText(Wide(node.options[static_cast<size_t>(index)]), cell, compositor.SmallFormat(),
                             !state.enabled ? theme.text_disabled : index == current ? theme.text : theme.text_secondary,
                             align);
        }
        break;
    }
    case UiNodeType::Tabs:
    case UiNodeType::Segmented: {
        size_t count = node.type == UiNodeType::Tabs ? node.children.size() : node.options.size();
        float width = (bounds.right - bounds.left) / std::max(1.0f, static_cast<float>(count));
        int selected = TabCtrl_GetCurSel(value.window);
        if (node.type == UiNodeType::Segmented)
            painter.DrawSegmentedTrack(bounds);
        for (size_t index = 0; index < count; ++index) {
            auto cell = D2D1::RectF(bounds.left + width * static_cast<float>(index), bounds.top,
                                    bounds.left + width * static_cast<float>(index + 1), bounds.bottom);
            auto caption = Wide(node.type == UiNodeType::Tabs ? node.children[index].text : node.options[index]);
            auto part = state;
            part.selected = selected == static_cast<int>(index);
            part.hovered = state.hovered && value.hover_part == static_cast<int>(index);
            part.pressed = state.pressed && value.pressed_part == static_cast<int>(index);
            if (node.type == UiNodeType::Tabs) {
                fluent::TabSpec spec;
                spec.bounds = cell;
                spec.text = caption;
                auto tab_glyph = Wide(node.children[index].glyph);
                spec.glyph = tab_glyph;
                spec.state = part;
                spec.show_close = !node.children[index].close_command.empty();
                painter.DrawTab(spec);
            } else {
                fluent::SegmentedItemSpec spec;
                spec.bounds = cell;
                spec.text = caption;
                spec.state = part;
                spec.shared_track = true;
                spec.position = count == 1           ? fluent::SegmentPosition::Single
                                : index == 0         ? fluent::SegmentPosition::First
                                : index + 1 == count ? fluent::SegmentPosition::Last
                                                     : fluent::SegmentPosition::Middle;
                painter.DrawSegmentedItem(spec);
            }
        }
        break;
    }
    case UiNodeType::List: {
        int selected = static_cast<int>(SendMessageW(value.window, LB_GETCURSEL, 0, 0)),
            first = static_cast<int>(SendMessageW(value.window, LB_GETTOPINDEX, 0, 0));
        dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (int index = std::max(0, first); index < static_cast<int>(node.options.size()); ++index) {
            fluent::ListRowSpec row;
            row.bounds = {bounds.left, bounds.top + (index - first) * 32 * scale, bounds.right,
                          bounds.top + (index - first + 1) * 32 * scale};
            row.state = state;
            row.state.selected = index == selected;
            row.state.hovered = state.hovered && value.hover_part == index;
            row.state.pressed = state.pressed && value.pressed_part == index;
            painter.DrawListRowBackground(row);
            auto content = row.bounds;
            content.left += 12 * scale;
            painter.DrawText(Wide(node.options[index]), content, compositor.TextFormat(), theme.text);
        }
        dc->PopAxisAlignedClip();
        break;
    }
    case UiNodeType::Progress: {
        fluent::ProgressSpec spec;
        spec.bounds = bounds;
        spec.value = static_cast<float>(SendMessageW(value.window, PBM_GETPOS, 0, 0)) / 100;
        spec.paused = node.progress_state == "paused";
        spec.error = node.progress_state == "error";
        spec.indeterminate = node.progress_state == "indeterminate";
        spec.animation_progress = static_cast<float>(GetTickCount64() % 100000) / 1000;
        if (spec.indeterminate)
            SetTimer(value.host, 1, 16, nullptr);
        if (node.progress_kind == "ring")
            painter.DrawProgressRing(spec);
        else
            painter.DrawProgressBar(spec);
        break;
    }
    case UiNodeType::Badge: {
        fluent::BadgeSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.dot = node.dot;
        spec.kind = node.badge_kind == "accent"    ? fluent::BadgeKind::Accent
                    : node.badge_kind == "success" ? fluent::BadgeKind::Success
                    : node.badge_kind == "warning" ? fluent::BadgeKind::Warning
                    : node.badge_kind == "danger"  ? fluent::BadgeKind::Danger
                    : node.badge_kind == "keycap"  ? fluent::BadgeKind::Keycap
                                                   : fluent::BadgeKind::Neutral;
        painter.DrawBadge(spec);
        break;
    }
    case UiNodeType::Tag: {
        fluent::TagSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.color = theme.accent;
        spec.bordered = node.bordered;
        if (node.color.size() == 7) {
            unsigned color = 0;
            auto result = std::from_chars(node.color.data() + 1, node.color.data() + 7, color, 16);
            if (result.ec == std::errc{})
                spec.color = HexColor(color);
        }
        painter.DrawTag(spec);
        break;
    }
    case UiNodeType::InfoBar: {
        InfoSpec info(node, bounds);
        info.spec.state = state;
        info.spec.hovered_action = value.hover_part < 2 ? value.hover_part : -1;
        painter.DrawInfoBar(info.spec);
        break;
    }
    case UiNodeType::SidebarItem: {
        fluent::SidebarItemSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.glyph = glyph;
        spec.detail = detail;
        auto badge = Wide(node.badge_text);
        spec.badge_text = badge;
        spec.state = state;
        spec.state.selected = node.selected;
        painter.DrawSidebarItem(spec);
        break;
    }
    case UiNodeType::Breadcrumb: {
        fluent::BreadcrumbSegmentSpec spec;
        spec.bounds = bounds;
        spec.text = label;
        spec.glyph = glyph;
        spec.state = state;
        spec.current = node.current;
        spec.has_separator = node.separator;
        spec.has_menu = !node.menu.empty();
        painter.DrawBreadcrumbSegment(spec);
        break;
    }
    case UiNodeType::EmptyState: {
        fluent::EmptyStateSpec spec;
        spec.bounds = bounds;
        spec.glyph = glyph;
        spec.title = label;
        spec.message = message;
        painter.DrawEmptyState(spec);
        break;
    }
    case UiNodeType::Divider:
        painter.FillRoundedRect({bounds.left, bounds.top + (bounds.bottom - bounds.top) / 2, bounds.right,
                                 bounds.top + (bounds.bottom - bounds.top) / 2 + scale},
                                0, theme.stroke_divider);
        break;
    case UiNodeType::Text:
        if (node.alignment != "left") {
            painter.DrawText(text, bounds,
                             node.text_role == "caption"   ? compositor.SmallFormat()
                             : node.text_role == "section" ? compositor.HeaderFormat()
                                                           : compositor.TextFormat(),
                             theme.text,
                             node.alignment == "center" ? fluent::HorizontalAlignment::Center
                                                        : fluent::HorizontalAlignment::Right);
        } else {
            // Text shorter than its control-height box is centred in it, so a label lines up with
            // the field beside it in a grid row.
            const float width = bounds.right - bounds.left;
            const float measured = node.text_role == "caption"   ? painter.MeasureWrappedCaptionHeight(text, width)
                                   : node.text_role == "section" ? painter.MeasureWrappedSectionHeight(text, width)
                                                                 : painter.MeasureWrappedBodyHeight(text, width);
            const D2D1_POINT_2F origin{bounds.left,
                                       bounds.top + std::max(0.0f, (bounds.bottom - bounds.top - measured) / 2)};
            if (node.text_role == "caption")
                painter.DrawWrappedCaption(text, origin, width, theme.text);
            else if (node.text_role == "section")
                painter.DrawWrappedSection(text, origin, width, theme.text);
            else
                painter.DrawWrappedBody(text, origin, width, theme.text);
        }
        break;
    case UiNodeType::SplitPane: {
        float ratio = static_cast<float>(SendMessageW(value.window, TBM_GETPOS, 0, 0)) / 100;
        bool horizontal = node.orientation == "horizontal";
        float at = (horizontal ? bounds.left : bounds.top) +
                   ((horizontal ? bounds.right - bounds.left : bounds.bottom - bounds.top) - 8 * scale) * ratio;
        fluent::SplitterSpec spec;
        spec.bounds = horizontal ? D2D1::RectF(at, bounds.top, at + 8 * scale, bounds.bottom)
                                 : D2D1::RectF(bounds.left, at, bounds.right, at + 8 * scale);
        spec.vertical = horizontal;
        spec.state = state;
        painter.DrawSplitter(spec);
        break;
    }
    case UiNodeType::CommandSearch: {
        DrawExtensionExtraControl(node, effective, compositor, painter, theme);
        if (text.empty() && !state.focused)
            break;
        RECT edit{};
        GetClientRect(value.window, &edit);
        MapWindowPoints(value.window, value.host, reinterpret_cast<POINT *>(&edit), 2);
        return DrawPulsePaintedChildEdit(compositor, compositor.TextFormat(), ColorFromRef(EditTextColor(value.dark)),
                                         ColorFromRef(EditBackColor(value.dark)), value.window,
                                         {static_cast<float>(edit.left), static_cast<float>(edit.top),
                                          static_cast<float>(edit.right), static_cast<float>(edit.bottom)});
    }
    default:
        if (DrawExtensionExtraControl(node, effective, compositor, painter, theme))
            break;
        if (node.type == UiNodeType::Group && node.bordered) {
            // A settings card: the bounds cover the whole card (see ExtensionTreeLayout).
            const float radius = 8 * scale;
            painter.FillRoundedRect(bounds, radius, theme.surface_card);
            painter.StrokeRoundedRect(bounds, radius, theme.stroke_card);
            if (!text.empty())
                painter.DrawText(text, D2D1::RectF(bounds.left + 16 * scale, bounds.top + 12 * scale,
                                                   bounds.right - 16 * scale, bounds.top + 44 * scale),
                                 compositor.HeaderFormat(), theme.text);
            break;
        }
        painter.DrawText(text, bounds,
                         node.type == UiNodeType::Group ? compositor.HeaderFormat() : compositor.TextFormat(),
                         theme.text);
        break;
    }
    return true;
}
} // namespace pulse::ui
