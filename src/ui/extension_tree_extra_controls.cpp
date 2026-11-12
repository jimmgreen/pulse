#include "extension_tree_extra_controls.h"
#include "command_icons.h"
#include "store_icons.h"
#include "../app/extensions/icon_registry.h"
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::ui {
using extensions::UiNodeType;
namespace {
std::wstring WideExtra(std::string_view value) {
    if (value.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(count > 0 ? static_cast<size_t>(count) : 0, 0);
    if (count > 0) MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
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

bool ParseCanvasFloat(std::string_view token, float& out) noexcept {
    if (token.empty()) return false;
    if (token.front() == '+') token.remove_prefix(1);
    if (token.empty()) return false;
    const auto res = std::from_chars(token.data(), token.data() + token.size(), out);
    return res.ec == std::errc{} && res.ptr == token.data() + token.size() &&
        std::isfinite(out) && std::fabs(out) <= 32768.0f;
}

bool ResolveCanvasColor(std::string_view token, const Theme& theme, bool dark, float alpha_mul,
                        D2D1_COLOR_F& out) noexcept {
    if (token.empty() || token == "none" || token == "transparent") return false;
    D2D1_COLOR_F color{};
    if (token.front() == '#') {
        const auto hex = token.substr(1);
        uint32_t v = 0;
        if (hex.size() == 3) {
            if (std::from_chars(hex.data(), hex.data() + 3, v, 16).ec != std::errc{}) return false;
            const uint32_t r = (v >> 8) & 0xFu, g = (v >> 4) & 0xFu, b = v & 0xFu;
            color = HexColor(((r * 17u) << 16) | ((g * 17u) << 8) | (b * 17u), 1.0f);
        } else if (hex.size() == 6) {
            if (std::from_chars(hex.data(), hex.data() + 6, v, 16).ec != std::errc{}) return false;
            color = HexColor(v, 1.0f);
        } else if (hex.size() == 8) {
            if (std::from_chars(hex.data(), hex.data() + 8, v, 16).ec != std::errc{}) return false;
            color = HexColor(v & 0x00FFFFFFu, static_cast<float>((v >> 24) & 0xFFu) / 255.0f);
        } else {
            return false;
        }
    } else if (token == "text" || token == "primary" || token == "foreground") {
        color = theme.text;
    } else if (token == "secondary" || token == "muted") {
        color = theme.text_secondary;
    } else if (token == "disabled") {
        color = theme.text_disabled;
    } else if (token == "accent") {
        color = theme.accent;
    } else if (token == "success") {
        color = dark ? HexColor(0x5FD4A0) : HexColor(0x0F7B4F);
    } else if (token == "warning" || token == "warn") {
        color = dark ? HexColor(0xF2C14E) : HexColor(0xC98A00);
    } else if (token == "danger" || token == "error") {
        color = theme.danger;
    } else if (token == "card" || token == "surface") {
        color = theme.surface_card;
    } else if (token == "bg" || token == "background") {
        color = theme.bg;
    } else if (token == "divider" || token == "border") {
        color = theme.stroke_divider;
    } else if (token == "hover") {
        color = theme.fill_hover;
    } else if (token == "selected") {
        color = theme.fill_selected;
    } else {
        return false;
    }
    color.a = std::clamp(color.a * alpha_mul, 0.0f, 1.0f);
    out = color;
    return color.a > 0.001f;
}

struct CanvasToken {
    std::string key;   // empty for positional tokens
    std::string value;
    bool quoted = false;
};

std::vector<CanvasToken> TokenizeCanvasCommand(std::string_view line) {
    std::vector<CanvasToken> tokens;
    size_t i = 0;
    const auto skip_ws = [&]() {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
    };
    while (i < line.size() && tokens.size() < 24) {
        skip_ws();
        if (i >= line.size()) break;
        if (line[i] == '"') {
            ++i;
            std::string val;
            while (i < line.size() && line[i] != '"') {
                if (line[i] == '\\' && i + 1 < line.size()) ++i;
                val.push_back(line[i++]);
            }
            if (i < line.size() && line[i] == '"') ++i;
            tokens.push_back({{}, std::move(val), true});
            continue;
        }
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r' && line[i] != '"') {
            ++i;
        }
        const std::string_view word = line.substr(start, i - start);
        const auto eq = word.find('=');
        if (eq != std::string_view::npos && eq > 0) {
            std::string key(word.substr(0, eq));
            std::string val;
            if (eq + 1 == word.size() && i < line.size() && line[i] == '"') {
                ++i;
                while (i < line.size() && line[i] != '"') {
                    if (line[i] == '\\' && i + 1 < line.size()) ++i;
                    val.push_back(line[i++]);
                }
                if (i < line.size() && line[i] == '"') ++i;
                tokens.push_back({std::move(key), std::move(val), true});
            } else {
                val.assign(word.substr(eq + 1));
                tokens.push_back({std::move(key), std::move(val), false});
            }
        } else {
            tokens.push_back({{}, std::string(word), false});
        }
    }
    return tokens;
}

template <typename Fn>
void ForEachCanvasStatement(std::string_view script, Fn&& fn) {
    size_t i = 0;
    size_t start = 0;
    bool in_quotes = false;
    for (; i < script.size(); ++i) {
        const char ch = script[i];
        if (ch == '\\' && in_quotes && i + 1 < script.size()) {
            ++i;
            continue;
        }
        if (ch == '"') {
            in_quotes = !in_quotes;
            continue;
        }
        if (!in_quotes && (ch == '\n' || ch == ';')) {
            fn(script.substr(start, i - start));
            start = i + 1;
        }
    }
    if (start < script.size()) {
        fn(script.substr(start));
    }
}

void DrawExtensionCanvas(const extensions::UiNode& node, const ExtensionControlVisual& visual,
                         Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    auto* dc = compositor.Dc();
    if (!dc) return;
    const auto bounds = visual.bounds;
    const float w_px = bounds.right - bounds.left;
    const float h_px = bounds.bottom - bounds.top;
    if (w_px <= 1.0f || h_px <= 1.0f) return;

    const float scale = (std::max)(0.25f, visual.scale);
    const float alpha_state = visual.state.enabled ? 1.0f : 0.45f;
    const float radius = 8.0f * scale;

    if (!node.color.empty() && node.color.size() == 7 && node.color[0] == '#') {
        unsigned bg_hex = 0;
        if (std::from_chars(node.color.data() + 1, node.color.data() + 7, bg_hex, 16).ec == std::errc{}) {
            painter.FillRoundedRect(bounds, node.bordered ? radius : 0.0f, HexColor(bg_hex, alpha_state));
        }
    } else if (node.bordered) {
        painter.FillRoundedRect(bounds, radius, theme.surface_card);
    }
    if (!node.command.empty() && visual.state.enabled) {
        if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
        else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
    }

    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    dc->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke;
    if (factory) {
        auto props = D2D1::StrokeStyleProperties();
        props.startCap = props.endCap = props.dashCap = D2D1_CAP_STYLE_ROUND;
        props.lineJoin = D2D1_LINE_JOIN_ROUND;
        factory->CreateStrokeStyle(props, nullptr, 0, round_stroke.GetAddressOf());
    }
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
    dc->CreateSolidColorBrush(theme.text, brush.GetAddressOf());
    if (!brush) return;

    dc->PushAxisAlignedClip(bounds, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    float vb_w = w_px / scale;
    float vb_h = h_px / scale;
    bool stretch = false;
    float sx = scale, sy = scale, ox = bounds.left, oy = bounds.top;
    const auto update_transform = [&]() noexcept {
        if (vb_w <= 0.1f || vb_h <= 0.1f) return;
        if (stretch) {
            sx = w_px / vb_w;
            sy = h_px / vb_h;
            ox = bounds.left;
            oy = bounds.top;
        } else {
            const float s = (std::min)(w_px / vb_w, h_px / vb_h);
            sx = sy = s;
            ox = bounds.left + (w_px - vb_w * s) * 0.5f;
            oy = bounds.top + (h_px - vb_h * s) * 0.5f;
        }
    };
    update_transform();

    size_t command_budget = 0;
    const auto exec_line = [&](std::string_view raw) {
        if (command_budget >= 256) return;
        const auto tokens = TokenizeCanvasCommand(raw);
        if (tokens.empty() || !tokens[0].key.empty()) return;
        const std::string_view op = tokens[0].value;
        if (op.empty() || op.front() == '#') return;
        ++command_budget;

        std::vector<const CanvasToken*> pos;
        pos.reserve(tokens.size());
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].key.empty()) pos.push_back(&tokens[i]);
        }
        const auto get_kw = [&](std::string_view k) -> std::string_view {
            for (size_t i = 1; i < tokens.size(); ++i) {
                if (tokens[i].key == k) return tokens[i].value;
            }
            return {};
        };

        float opacity = 1.0f;
        if (const auto op_str = get_kw("opacity"); !op_str.empty()) {
            float parsed_op = 1.0f;
            if (ParseCanvasFloat(op_str, parsed_op)) opacity = std::clamp(parsed_op, 0.0f, 1.0f);
        }
        const float eff_alpha = alpha_state * opacity;
        float stroke_w = 1.35f;
        if (const auto w_str = get_kw("width"); !w_str.empty()) {
            float parsed_w = 1.35f;
            if (ParseCanvasFloat(w_str, parsed_w)) stroke_w = std::clamp(parsed_w, 0.25f, 32.0f);
        }
        const float unit_scale = (sx + sy) * 0.5f;
        const float px_stroke = (std::max)(0.5f, stroke_w * unit_scale);

        if (op == "viewbox" && pos.size() >= 2) {
            float nw = 0, nh = 0;
            if (ParseCanvasFloat(pos[0]->value, nw) && ParseCanvasFloat(pos[1]->value, nh) &&
                nw >= 1.0f && nh >= 1.0f) {
                vb_w = nw;
                vb_h = nh;
                stretch = (pos.size() >= 3 && pos[2]->value == "stretch") || get_kw("mode") == "stretch";
                update_transform();
            }
            return;
        }
        if (op == "clear") {
            const auto col_tok = !get_kw("color").empty() ? get_kw("color")
                               : !get_kw("fill").empty() ? get_kw("fill")
                               : (!pos.empty() ? std::string_view(pos[0]->value) : std::string_view("card"));
            D2D1_COLOR_F col{};
            if (ResolveCanvasColor(col_tok, theme, visual.dark, eff_alpha, col)) {
                brush->SetColor(col);
                dc->FillRectangle(bounds, brush.Get());
            }
            return;
        }
        if (op == "rect" && pos.size() >= 4) {
            float x = 0, y = 0, w = 0, h = 0, r = 0;
            if (!ParseCanvasFloat(pos[0]->value, x) || !ParseCanvasFloat(pos[1]->value, y) ||
                !ParseCanvasFloat(pos[2]->value, w) || !ParseCanvasFloat(pos[3]->value, h)) return;
            if (pos.size() >= 5) ParseCanvasFloat(pos[4]->value, r);
            if (const auto r_kw = get_kw("r"); !r_kw.empty()) ParseCanvasFloat(r_kw, r);
            const D2D1_RECT_F rc = D2D1::RectF(ox + x * sx, oy + y * sy, ox + (x + w) * sx, oy + (y + h) * sy);
            D2D1_COLOR_F fill_c{}, stroke_c{};
            const bool has_fill = ResolveCanvasColor(get_kw("fill"), theme, visual.dark, eff_alpha, fill_c);
            const bool has_stroke = ResolveCanvasColor(
                !get_kw("stroke").empty() ? get_kw("stroke") : (has_fill ? std::string_view{} : std::string_view("text")),
                theme, visual.dark, eff_alpha, stroke_c);
            if (r > 0.0f) {
                const D2D1_ROUNDED_RECT rr{rc, r * sx, r * sy};
                if (has_fill) { brush->SetColor(fill_c); dc->FillRoundedRectangle(rr, brush.Get()); }
                if (has_stroke) { brush->SetColor(stroke_c); dc->DrawRoundedRectangle(rr, brush.Get(), px_stroke, round_stroke.Get()); }
            } else {
                if (has_fill) { brush->SetColor(fill_c); dc->FillRectangle(rc, brush.Get()); }
                if (has_stroke) { brush->SetColor(stroke_c); dc->DrawRectangle(rc, brush.Get(), px_stroke, round_stroke.Get()); }
            }
            return;
        }
        if ((op == "circle" && pos.size() >= 3) || (op == "ellipse" && pos.size() >= 4)) {
            float cx = 0, cy = 0, rx = 0, ry = 0;
            if (!ParseCanvasFloat(pos[0]->value, cx) || !ParseCanvasFloat(pos[1]->value, cy) ||
                !ParseCanvasFloat(pos[2]->value, rx)) return;
            ry = rx;
            if (op == "ellipse" && !ParseCanvasFloat(pos[3]->value, ry)) return;
            const D2D1_ELLIPSE el = D2D1::Ellipse(D2D1::Point2F(ox + cx * sx, oy + cy * sy), rx * sx, ry * sy);
            D2D1_COLOR_F fill_c{}, stroke_c{};
            const bool has_fill = ResolveCanvasColor(get_kw("fill"), theme, visual.dark, eff_alpha, fill_c);
            const bool has_stroke = ResolveCanvasColor(
                !get_kw("stroke").empty() ? get_kw("stroke") : (has_fill ? std::string_view{} : std::string_view("text")),
                theme, visual.dark, eff_alpha, stroke_c);
            if (has_fill) { brush->SetColor(fill_c); dc->FillEllipse(el, brush.Get()); }
            if (has_stroke) { brush->SetColor(stroke_c); dc->DrawEllipse(el, brush.Get(), px_stroke, round_stroke.Get()); }
            return;
        }
        if (op == "line" && pos.size() >= 4) {
            float x1 = 0, y1 = 0, x2 = 0, y2 = 0;
            if (!ParseCanvasFloat(pos[0]->value, x1) || !ParseCanvasFloat(pos[1]->value, y1) ||
                !ParseCanvasFloat(pos[2]->value, x2) || !ParseCanvasFloat(pos[3]->value, y2)) return;
            const auto stroke_tok = !get_kw("stroke").empty() ? get_kw("stroke")
                                  : !get_kw("color").empty() ? get_kw("color") : std::string_view("text");
            D2D1_COLOR_F stroke_c{};
            if (ResolveCanvasColor(stroke_tok, theme, visual.dark, eff_alpha, stroke_c)) {
                brush->SetColor(stroke_c);
                dc->DrawLine(D2D1::Point2F(ox + x1 * sx, oy + y1 * sy),
                             D2D1::Point2F(ox + x2 * sx, oy + y2 * sy),
                             brush.Get(), px_stroke, round_stroke.Get());
            }
            return;
        }
        if (op == "path" && factory) {
            std::string_view d = get_kw("d");
            if (d.empty() && !pos.empty()) d = pos[0]->value;
            if (d.empty() || !extensions::icons::ValidateSvgPathSyntax(d)) return;
            D2D1_COLOR_F fill_c{}, stroke_c{};
            const bool has_fill = ResolveCanvasColor(get_kw("fill"), theme, visual.dark, eff_alpha, fill_c);
            const bool has_stroke = ResolveCanvasColor(
                !get_kw("stroke").empty() ? get_kw("stroke") : (has_fill ? std::string_view{} : std::string_view("text")),
                theme, visual.dark, eff_alpha, stroke_c);
            if (!has_fill && !has_stroke) return;

            std::string path_str(d);
            if (get_kw("closed") == "true" && !path_str.empty() && path_str.back() != 'z' && path_str.back() != 'Z') {
                path_str.push_back('z');
            }
            Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom;
            Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(geom.GetAddressOf())) ||
                FAILED(geom->Open(sink.GetAddressOf()))) return;
            command_icons::AppendSvgPath(sink.Get(), path_str.c_str(), has_fill);
            if (FAILED(sink->Close())) return;

            D2D1_MATRIX_3X2_F old_tx;
            dc->GetTransform(&old_tx);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) *
                             D2D1::Matrix3x2F::Translation(ox, oy) * old_tx);
            if (has_fill) {
                brush->SetColor(fill_c);
                dc->FillGeometry(geom.Get(), brush.Get());
            }
            if (has_stroke) {
                brush->SetColor(stroke_c);
                dc->DrawGeometry(geom.Get(), brush.Get(),
                                 unit_scale > 0.0f ? (px_stroke / unit_scale) : stroke_w,
                                 round_stroke.Get());
            }
            dc->SetTransform(old_tx);
            return;
        }
        if (op == "icon" && pos.size() >= 4) {
            float ix = 0, iy = 0, isize = 0;
            if (!ParseCanvasFloat(pos[0]->value, ix) || !ParseCanvasFloat(pos[1]->value, iy) ||
                !ParseCanvasFloat(pos[2]->value, isize) || isize <= 0.0f) return;
            const auto resolved = extensions::icons::GlobalRegistry().ResolveName({}, pos[3]->value);
            if (!resolved) return;
            const auto col_tok = !get_kw("color").empty() ? get_kw("color") : std::string_view("text");
            D2D1_COLOR_F icon_c = theme.text;
            icon_c.a *= eff_alpha;
            ResolveCanvasColor(col_tok, theme, visual.dark, eff_alpha, icon_c);
            const auto icon_rc = D2D1::RectF(ox + ix * sx, oy + iy * sy,
                                             ox + (ix + isize) * sx, oy + (iy + isize) * sy);
            float icon_stroke = 1.35f * scale;
            if (const auto st_kw = get_kw("stroke"); !st_kw.empty()) {
                float parsed_st = 1.35f;
                if (ParseCanvasFloat(st_kw, parsed_st)) icon_stroke = std::clamp(parsed_st, 0.4f, 8.0f) * scale;
            }
            DrawExtensionIcon(dc, *resolved, icon_rc, icon_c, &theme, icon_stroke);
            return;
        }
        if (op == "text" && pos.size() >= 3) {
            float tx = 0, ty = 0;
            if (!ParseCanvasFloat(pos[0]->value, tx) || !ParseCanvasFloat(pos[1]->value, ty)) return;
            const std::wstring label = WideExtra(pos[2]->value);
            if (label.empty()) return;
            float max_w = vb_w - tx, max_h = 28.0f;
            if (const auto w_kw = get_kw("w"); !w_kw.empty()) ParseCanvasFloat(w_kw, max_w);
            if (const auto h_kw = get_kw("h"); !h_kw.empty()) ParseCanvasFloat(h_kw, max_h);
            const auto col_tok = !get_kw("color").empty() ? get_kw("color") : std::string_view("text");
            D2D1_COLOR_F text_c = theme.text;
            text_c.a *= eff_alpha;
            ResolveCanvasColor(col_tok, theme, visual.dark, eff_alpha, text_c);
            const auto role = get_kw("role");
            auto* fmt = role == "caption" ? compositor.SmallFormat()
                      : role == "section" ? compositor.HeaderFormat()
                      : compositor.TextFormat();
            const auto align_kw = get_kw("align");
            const auto align = align_kw == "center" ? fluent::HorizontalAlignment::Center
                             : align_kw == "right" ? fluent::HorizontalAlignment::Right
                             : fluent::HorizontalAlignment::Left;
            const float px_x = ox + tx * sx;
            const float px_y = oy + ty * sy;
            const float px_w = (std::max)(8.0f, max_w * sx);
            const float px_h = (std::max)(14.0f * scale, max_h * sy);
            painter.DrawText(label, D2D1::RectF(px_x, px_y, px_x + px_w, px_y + px_h), fmt, text_c, align);
            return;
        }
    };

    for (const auto& entry : node.options) {
        ForEachCanvasStatement(entry, exec_line);
    }
    if (!node.detail.empty()) {
        ForEachCanvasStatement(node.detail, exec_line);
    }
    if (command_budget == 0 && !node.text.empty()) {
        painter.DrawText(WideExtra(node.text), bounds, compositor.SmallFormat(),
                         theme.text_secondary, fluent::HorizontalAlignment::Center);
    }

    dc->PopAxisAlignedClip();

    if (node.bordered) {
        painter.StrokeRoundedRect(bounds, radius, theme.stroke_card);
    }
    if (!node.command.empty() && visual.state.keyboard_focus) {
        painter.StrokeRoundedRect(bounds, radius, theme.accent, 2.0f * scale);
    }
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
    case UiNodeType::Canvas: height = 160 * scale; break;
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
    const auto text = WideExtra(node.text), glyph = WideExtra(node.glyph), detail = WideExtra(node.detail);
    const auto summary = WideExtra(node.summary);
    const auto bounds = visual.bounds;
    auto state = visual.state;
    switch (node.type) {
    case UiNodeType::Canvas:
        DrawExtensionCanvas(node, visual, compositor, painter, theme);
        return true;
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
