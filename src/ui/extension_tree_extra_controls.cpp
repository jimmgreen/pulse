#include "extension_tree_extra_controls.h"
#include "command_icons.h"
#include "store_icons.h"
#include "../app/extensions/icon_registry.h"
#include "../common/localization.h"
#include "typography.h"
#include <wincodec.h>
#include <commctrl.h>
#include <wrl/client.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cwctype>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
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

// A canvas `text size=` format: the UI text family at an exact pixel size, so labels scale with the
// canvas and still go through Painter::DrawText (LumaText). Formats are kept for the process: text
// caches key on the format's address, which must never be reused by a later format.
IDWriteTextFormat* CanvasScaledFormat(Compositor& compositor, float px, DWRITE_FONT_WEIGHT weight) {
    IDWriteFactory2* factory = compositor.DwriteFactory();
    if (!factory || !(px > 0.0f)) return nullptr;
    const int quarter = std::clamp(static_cast<int>(std::lround(px * 4.0f)), 16, 512);  // 4..128 px
    using Key = std::tuple<IDWriteFactory2*, int, int>;
    static std::mutex lock;
    static auto* cache = new std::map<Key, IDWriteTextFormat*>();  // intentionally never released
    const Key key{factory, quarter, static_cast<int>(weight)};
    std::lock_guard guard(lock);
    if (const auto it = cache->find(key); it != cache->end()) return it->second;
    if (cache->size() >= 128) return nullptr;  // the caller falls back to a role format
    IDWriteTextFormat* format = nullptr;
    if (FAILED(typography::CreateTextFormat(factory, {typography::FontRole::Text, quarter / 4.0f, weight}, &format)) || !format)
        return nullptr;
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    cache->emplace(key, format);
    return format;
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
            std::string_view icon_tok = pos[3]->value;
            if (icon_tok.starts_with("icon:")) icon_tok.remove_prefix(5);
            const auto resolved = extensions::icons::GlobalRegistry().ResolveName({}, icon_tok);
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
            IDWriteTextFormat* fmt = nullptr;
            // size= (viewBox units) scales the text with the canvas, e.g. a format name on an icon.
            if (float size = 0; ParseCanvasFloat(get_kw("size"), size) && size > 0.0f) {
                const auto weight_kw = get_kw("weight");
                const DWRITE_FONT_WEIGHT weight = weight_kw == "bold" ? DWRITE_FONT_WEIGHT_BOLD
                                                : weight_kw == "semibold" ? DWRITE_FONT_WEIGHT_SEMI_BOLD
                                                : DWRITE_FONT_WEIGHT_NORMAL;
                fmt = CanvasScaledFormat(compositor, size * sy, weight);
            }
            if (!fmt)
                fmt = role == "caption" ? compositor.SmallFormat()
                    : role == "section" ? compositor.HeaderFormat()
                    : compositor.TextFormat();
            const auto align_kw = get_kw("align");
            const auto align = align_kw == "center" ? fluent::HorizontalAlignment::Center
                             : align_kw == "right" ? fluent::HorizontalAlignment::Right
                             : fluent::HorizontalAlignment::Left;
            const float px_x = ox + tx * sx;
            const float px_y = oy + ty * sy;
            const float px_w = (std::max)(8.0f, max_w * sx);
            const float px_h = get_kw("size").empty() ? (std::max)(14.0f * scale, max_h * sy) : (std::max)(4.0f, max_h * sy);
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

bool HasSafeAbsoluteLocalPath(std::wstring_view path) {
    if (path.size() < 4 || path.find(L'\0') != std::wstring_view::npos || path.find(L"..") != std::wstring_view::npos)
        return false;
    const wchar_t drive = path[0];
    return ((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z')) &&
           path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
}

bool EndsWithInsensitive(std::wstring_view text, std::wstring_view suffix) {
    if (text.size() < suffix.size()) return false;
    const auto tail = text.substr(text.size() - suffix.size());
    for (size_t i = 0; i < suffix.size(); ++i) {
        if (std::towlower(tail[i]) != std::towlower(suffix[i])) return false;
    }
    return true;
}

ID2D1Bitmap* LoadExtensionImageBitmap(ID2D1DeviceContext* dc, std::wstring_view source, int target_px,
                                      D2D1_SIZE_F* out_size) {
    struct CacheEntry {
        ID2D1DeviceContext* dc = nullptr;
        std::wstring source;
        int px = 0;
        D2D1_SIZE_F size{};
        Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    };
    static std::vector<CacheEntry> cache;
    if (!dc || source.empty()) return nullptr;
    const bool is_resource = source.starts_with(L"resource:");
    const bool is_file = source.starts_with(L"file:");
    if (!is_resource && !is_file) return nullptr;
    const int cache_px = is_resource ? (std::clamp)(target_px, 16, 512) : 0;
    for (const auto& e : cache) {
        if (e.dc == dc && e.px == cache_px && e.source == source) {
            if (out_size) *out_size = e.size;
            return e.bitmap.Get();
        }
    }
    if (cache.size() >= 64) cache.clear();

    CacheEntry entry;
    entry.dc = dc;
    entry.source.assign(source);
    entry.px = cache_px;

    Microsoft::WRL::ComPtr<IWICImagingFactory> wic;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
        wic) {
        Microsoft::WRL::ComPtr<IWICBitmapSource> wic_source;
        if (is_resource) {
            const auto spec = source.substr(9);
            const auto hash_pos = spec.rfind(L'#');
            if (hash_pos != std::wstring_view::npos && hash_pos > 3 && hash_pos + 1 < spec.size()) {
                const std::wstring mod_path(spec.substr(0, hash_pos));
                int res_id = 0;
                const auto id_sv = spec.substr(hash_pos + 1);
                bool all_digits = !id_sv.empty();
                for (const wchar_t ch : id_sv) {
                    if (ch < L'0' || ch > L'9') { all_digits = false; break; }
                    res_id = res_id * 10 + (ch - L'0');
                    if (res_id > 65535) { all_digits = false; break; }
                }
                if (all_digits && res_id > 0 && HasSafeAbsoluteLocalPath(mod_path) &&
                    (EndsWithInsensitive(mod_path, L".dll") || EndsWithInsensitive(mod_path, L".exe") ||
                     EndsWithInsensitive(mod_path, L".cpl") || EndsWithInsensitive(mod_path, L".ico"))) {
                    if (HMODULE mod = LoadLibraryExW(mod_path.c_str(), nullptr,
                                                     LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE)) {
                        HICON icon = nullptr;
                        if (SUCCEEDED(LoadIconWithScaleDown(mod, MAKEINTRESOURCEW(res_id), cache_px, cache_px, &icon)) &&
                            icon) {
                            Microsoft::WRL::ComPtr<IWICBitmap> bmp;
                            if (SUCCEEDED(wic->CreateBitmapFromHICON(icon, &bmp))) wic_source = bmp;
                            DestroyIcon(icon);
                        }
                        FreeLibrary(mod);
                    }
                }
            }
        } else {
            const std::wstring file_path(source.substr(5));
            if (HasSafeAbsoluteLocalPath(file_path) &&
                (EndsWithInsensitive(file_path, L".png") || EndsWithInsensitive(file_path, L".ico") ||
                 EndsWithInsensitive(file_path, L".bmp") || EndsWithInsensitive(file_path, L".jpg") ||
                 EndsWithInsensitive(file_path, L".jpeg") || EndsWithInsensitive(file_path, L".webp"))) {
                Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
                Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
                if (SUCCEEDED(wic->CreateDecoderFromFilename(file_path.c_str(), nullptr, GENERIC_READ,
                                                             WICDecodeMetadataCacheOnDemand, &decoder)) &&
                    decoder && SUCCEEDED(decoder->GetFrame(0, &frame))) {
                    wic_source = frame;
                }
            }
        }
        if (wic_source) {
            UINT w = 0, h = 0;
            wic_source->GetSize(&w, &h);
            Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
            if (w > 0 && h > 0 && SUCCEEDED(wic->CreateFormatConverter(&converter)) && converter &&
                SUCCEEDED(converter->Initialize(wic_source.Get(), GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeMedianCut))) {
                if (SUCCEEDED(dc->CreateBitmapFromWicBitmap(converter.Get(), nullptr, &entry.bitmap)) && entry.bitmap) {
                    entry.size = {static_cast<float>(w), static_cast<float>(h)};
                }
            }
        }
    }
    if (out_size) *out_size = entry.size;
    ID2D1Bitmap* result = entry.bitmap.Get();
    cache.push_back(std::move(entry));
    return result;
}

// Glyph for a preview menu row: "icon:<name>" resolves through the icon registry, anything else is a
// font glyph; a checked row without a glyph shows the check mark.
void ResolveMenuPreviewGlyph(const extensions::UiMenuItem& item, std::wstring_view glyph_w, fluent::MenuItemSpec& spec) {
    if (!item.glyph.empty()) {
        std::string_view g = item.glyph;
        if (g.starts_with("icon:")) {
            g.remove_prefix(5);
            if (const auto id = extensions::icons::GlobalRegistry().ResolveName({}, g)) {
                if (IsCustomExtensionIcon(*id)) spec.custom_icon = *id;
                else spec.vector_icon = StoreIconFromId(*id);
            }
        } else {
            spec.glyph = glyph_w;
        }
    } else if (item.checked) {
        spec.vector_icon = StoreIcon::Check;
    }
}

// Previews that declare a tinted group, a detail line, a primary row or the context frame use the host
// right-click menu metrics (fluent_menu and the store's menu preview): 28 DIP rows, 40 DIP with a detail
// line, a group opens with a 26 DIP caption and spaces its rows by 2 DIP. Plain previews keep the original
// 30/26 DIP preview metrics.
bool MenuPreviewHostMetrics(const extensions::UiNode& node) {
    if (node.menu_kind == "context") return true;
    const auto extended = [](const extensions::UiMenuItem& item) {
        return !item.group.empty() || !item.detail.empty() || item.primary;
    };
    return std::any_of(node.menu.begin(), node.menu.end(), [&](const extensions::UiMenuItem& item) {
        return extended(item) || std::any_of(item.children.begin(), item.children.end(), extended);
    });
}

float HostMenuRowDip(const extensions::UiMenuItem& item) {
    return item.separator ? 9.0f : item.detail.empty() ? 28.0f : 40.0f;
}

struct MenuPreviewDraw {
    Compositor& compositor;
    fluent::Painter& painter;
    const Theme& theme;
    bool enabled = true;
};

// Lays out (and, with `draw`, paints) a host-metric preview inside `card`; returns the card height.
float WalkHostMenuPreview(const extensions::UiNode& node, D2D1_RECT_F card, float scale, const MenuPreviewDraw* draw) {
    const float s = scale;
    float y = card.top + 8.0f * s;
    const auto separator = [&](float left) {
        if (draw) {
            draw->painter.FillRoundedRect({left + 8.0f * s, y + 4.0f * s, card.right - 8.0f * s, y + 4.0f * s + 1.0f},
                                          0.0f, draw->theme.stroke_divider);
        }
        y += 9.0f * s;
    };
    const auto host_row = [&](StoreIcon icon, std::wstring_view text, std::wstring_view shortcut, bool submenu) {
        if (draw) {
            fluent::MenuItemSpec spec;
            spec.bounds = {card.left, y, card.right, y + 28.0f * s};
            spec.text = text;
            spec.shortcut = shortcut;
            spec.vector_icon = icon;
            spec.has_submenu = submenu;
            spec.state.enabled = draw->enabled;
            draw->painter.DrawMenuItem(spec);
        }
        y += 28.0f * s;
    };
    const auto item_row = [&](const extensions::UiMenuItem& item, float left, bool grouped) {
        if (item.separator) {
            separator(left);
            return;
        }
        const float h = HostMenuRowDip(item) * s;
        if (draw) {
            const auto text = WideExtra(item.text), detail = WideExtra(item.detail);
            const auto shortcut = WideExtra(item.shortcut), glyph = WideExtra(item.glyph);
            fluent::MenuItemSpec spec;
            spec.bounds = {left, y, card.right, y + h};
            spec.text = text;
            spec.detail = detail;
            spec.shortcut = shortcut;
            spec.checked = item.checked;
            spec.has_submenu = !item.children.empty();
            spec.primary = item.primary;
            spec.group_tint = grouped;
            spec.state.enabled = draw->enabled && item.enabled;
            ResolveMenuPreviewGlyph(item, glyph, spec);
            draw->painter.DrawMenuItem(spec);
        }
        y += h + (grouped ? 2.0f * s : 0.0f);
    };
    const bool context = node.menu_kind == "context";
    if (context) {
        // The host right-click menu around the plugin's rows, kept short so the plugin's rows stay in
        // view (as the store showcase): the quick-action strip and Open above, Properties below.
        if (draw) {
            const StoreIcon actions[] = {StoreIcon::Cut, StoreIcon::Copy, StoreIcon::Rename, StoreIcon::Trash};
            const float width = (card.right - card.left - 16.0f * s) / 4.0f;
            if (auto* dc = draw->compositor.Dc()) {
                for (int i = 0; i < 4; ++i) {
                    const float x = card.left + 8.0f * s + (static_cast<float>(i) + 0.5f) * width;
                    DrawStoreIcon(dc, actions[i], {x - 8.0f * s, y + 5.0f * s, x + 8.0f * s, y + 21.0f * s},
                                  draw->theme.text_secondary);
                }
            }
        }
        y += 32.0f * s;
        if (draw) {
            draw->painter.FillRoundedRect({card.left + 8.0f * s, y, card.right - 8.0f * s, y + 1.0f}, 0.0f,
                                          draw->theme.stroke_divider);
        }
        host_row(StoreIcon::Open, l10n::Get(l10n::StringId::Open), L"Enter", false);
        if (!node.menu.empty()) separator(card.left);
    }
    for (size_t i = 0; i < node.menu.size();) {
        const auto& item = node.menu[i];
        if (item.group.empty()) {
            item_row(item, card.left, false);
            for (const auto& sub : item.children) item_row(sub, card.left + 18.0f * s, false);
            ++i;
            continue;
        }
        size_t end = i + 1;
        while (end < node.menu.size() && node.menu[end].group == item.group) ++end;
        // A group of one item with a submenu reads as the collapsed-submenu placement: the item, then its
        // rows on a 2 DIP accent rail.
        const bool submenu = end == i + 1 && !item.children.empty();
        float band = (submenu ? 30.0f : 26.0f) * s;
        const auto add = [&](const extensions::UiMenuItem& row) {
            band += row.separator ? 9.0f * s : (HostMenuRowDip(row) + 2.0f) * s;
        };
        for (size_t k = i; k < end; ++k) {
            if (!submenu) add(node.menu[k]);
            for (const auto& sub : node.menu[k].children) add(sub);
        }
        const float band_top = y;
        if (draw) {
            draw->painter.FillHorizontalGradientRect({card.left + 4.0f * s, y, card.right - 4.0f * s, y + band}, 7.0f * s,
                                                     draw->theme.list_selected_start, draw->theme.list_selected_end);
        }
        if (submenu) {
            item_row(item, card.left, true);
            if (draw) {
                draw->painter.FillRoundedRect({card.left + 12.0f * s, y, card.left + 14.0f * s, band_top + band - 4.0f * s},
                                              1.0f * s, WithAlpha(draw->theme.accent, 0.5f));
            }
            for (const auto& sub : item.children) item_row(sub, card.left + 14.0f * s, true);
        } else {
            if (draw) {
                const auto caption = WideExtra(item.group);
                const auto color = MixColor(draw->theme.accent, draw->theme.text_secondary, 0.35f);
                draw->painter.DrawText(caption, {card.left + 16.0f * s, y + 2.0f * s, card.right - 34.0f * s, y + 24.0f * s},
                                       draw->painter.RampFormat(fluent::Painter::TextRamp::Section), color);
                if (auto* dc = draw->compositor.Dc()) {
                    DrawStoreIcon(dc, StoreIcon::Gear, {card.right - 28.0f * s, y + 7.0f * s, card.right - 16.0f * s, y + 19.0f * s},
                                  draw->theme.text_secondary);
                }
            }
            y += 26.0f * s;
            for (size_t k = i; k < end; ++k) {
                item_row(node.menu[k], card.left, true);
                for (const auto& sub : node.menu[k].children) item_row(sub, card.left + 18.0f * s, true);
            }
        }
        y = band_top + band;
        i = end;
    }
    if (context) {
        if (!node.menu.empty()) separator(card.left);
        host_row(StoreIcon::Properties, l10n::Get(l10n::StringId::Properties), L"Alt+Enter", false);
    }
    y += 6.0f * s;
    return y - card.top;
}

float MenuPreviewContentHeight(const extensions::UiNode& node, float width, float scale, fluent::Painter& painter) {
    float h = 0.0f;
    const auto title_w = WideExtra(node.text);
    const auto detail_w = WideExtra(node.detail);
    if (!title_w.empty()) h += 22.0f * scale;
    if (!detail_w.empty()) h += painter.MeasureWrappedCaptionHeight(detail_w, (std::max)(40.0f * scale, width)) + 4.0f * scale;
    if (h > 0.0f) h += 6.0f * scale;
    if (MenuPreviewHostMetrics(node)) {
        return h + WalkHostMenuPreview(node, D2D1::RectF(0.0f, 0.0f, (std::max)(1.0f, width), 0.0f), scale, nullptr);
    }
    h += 12.0f * scale;  // flyout card padding
    if (node.menu.empty()) {
        h += 36.0f * scale;
        return h;
    }
    for (const auto& item : node.menu) {
        h += (item.separator ? 9.0f : 30.0f) * scale;
        for (const auto& sub : item.children) {
            h += (sub.separator ? 9.0f : 26.0f) * scale;
        }
    }
    return h;
}

// An option card with a canvas child is a tile: the illustration on top, then title and detail.
const extensions::UiNode* OptionCardArt(const extensions::UiNode& node) {
    return node.children.size() == 1 && node.children[0].type == UiNodeType::Canvas && node.children[0].visible
               ? &node.children[0]
               : nullptr;
}

float OptionTileArtHeight(const extensions::UiNode& art, float scale) {
    return static_cast<float>((std::max)(64, art.min_height)) * scale;
}

// A canvas of at most 48 DIP is an icon, not an illustration: a compact row tile with the icon on
// the left (a square of that size), title and detail beside it, the check mark on the right.
constexpr int kOptionInlineArtMax = 48;
bool OptionCardInlineArt(const extensions::UiNode& art) {
    return art.min_height > 0 && art.min_height <= kOptionInlineArtMax;
}
float OptionInlineTileHeight(const extensions::UiNode& node, const extensions::UiNode& art, float width, float scale,
                             const fluent::Painter& painter) {
    const float side = static_cast<float>(art.min_height) * scale;
    const float lead = 12.0f * scale + side + 12.0f * scale, trail = (node.state.empty() ? 12.0f : 42.0f) * scale;
    const float text_w = (std::max)(40.0f * scale, (width > 0.0f ? width : 220.0f * scale) - lead - trail);
    const auto detail_w = WideExtra(node.detail);
    const float text_h = 21.0f * scale + (detail_w.empty() ? 0.0f : 2.0f * scale + painter.MeasureWrappedCaptionHeight(detail_w, text_w));
    return (std::max)(side, text_h) + 24.0f * scale;
}
void DrawExtensionOptionInlineTile(const extensions::UiNode& node, const extensions::UiNode& art,
                                   const ExtensionControlVisual& visual, bool picked, Compositor& compositor,
                                   fluent::Painter& painter, const Theme& theme) {
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    const float side = static_cast<float>(art.min_height) * scale;
    const float pad = 12.0f * scale;
    const float mid = (bounds.top + bounds.bottom) * 0.5f;
    ExtensionControlVisual art_visual;
    art_visual.host = visual.host;
    art_visual.bounds = {bounds.left + pad, mid - side * 0.5f, bounds.left + pad + side, mid + side * 0.5f};
    art_visual.state.enabled = visual.state.enabled;
    art_visual.scale = visual.scale;
    art_visual.dark = visual.dark;
    DrawExtensionCanvas(art, art_visual, compositor, painter, theme);
    float right = bounds.right - pad;
    if (!node.state.empty() || node.selected) {
        fluent::ControlState ind_state = visual.state;
        ind_state.checked = picked;
        const D2D1_RECT_F ind_rc{right - 20.0f * scale, mid - 10.0f * scale, right, mid + 10.0f * scale};
        if (node.option_kind == "toggle") painter.DrawCheckBox(ind_rc, L"", ind_state);
        else painter.DrawRadioButton(ind_rc, L"", ind_state, 18.0f);
        right -= 30.0f * scale;
    }
    const float left = art_visual.bounds.right + pad;
    if (right <= left) return;
    const auto title_w = WideExtra(node.text);
    const auto detail_w = WideExtra(node.detail);
    const float detail_h = detail_w.empty() ? 0.0f : 2.0f * scale + painter.MeasureWrappedCaptionHeight(detail_w, right - left);
    const float top = mid - (21.0f * scale + detail_h) * 0.5f;
    const D2D1_COLOR_F ink = visual.state.enabled ? theme.text : theme.text_disabled;
    const D2D1_COLOR_F sub_ink = visual.state.enabled ? theme.text_secondary : theme.text_disabled;
    if (!title_w.empty()) {
        painter.DrawText(title_w, D2D1::RectF(left, top, right, top + 21.0f * scale),
                         painter.RampFormat(fluent::Painter::TextRamp::BodyStrong), ink);
    }
    if (!detail_w.empty()) {
        painter.DrawWrappedCaption(detail_w, D2D1::Point2F(left, top + 23.0f * scale), (std::max)(1.0f, right - left), sub_ink);
    }
    if (visual.state.keyboard_focus) {
        painter.DrawFocusRing(bounds, 8.0f * scale);
    }
}

// Tile body (the card surface is already painted): illustration well, selection mark in the top-right
// corner, semibold title with the badge on its right, wrapped detail.
void DrawExtensionOptionTile(const extensions::UiNode& node, const extensions::UiNode& art,
                             const ExtensionControlVisual& visual, bool picked, Compositor& compositor,
                             fluent::Painter& painter, const Theme& theme) {
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    const float pad = 10.0f * scale;
    const D2D1_RECT_F well{bounds.left + pad, bounds.top + pad, bounds.right - pad,
                           bounds.top + pad + OptionTileArtHeight(art, scale)};
    painter.FillRoundedRect(well, 6.0f * scale, picked ? theme.fill_selected : theme.bg);
    ExtensionControlVisual art_visual;
    art_visual.host = visual.host;
    art_visual.bounds = {well.left + 4.0f * scale, well.top + 4.0f * scale, well.right - 4.0f * scale,
                         well.bottom - 4.0f * scale};
    art_visual.state.enabled = visual.state.enabled;
    art_visual.scale = visual.scale;
    art_visual.dark = visual.dark;
    DrawExtensionCanvas(art, art_visual, compositor, painter, theme);
    if (!node.state.empty() || node.selected) {
        fluent::ControlState ind_state = visual.state;
        ind_state.checked = picked;
        const D2D1_RECT_F ind_rc{bounds.right - 27.0f * scale, bounds.top + 9.0f * scale, bounds.right - 9.0f * scale,
                                 bounds.top + 27.0f * scale};
        if (node.option_kind == "toggle") painter.DrawCheckBox(ind_rc, L"", ind_state);
        else painter.DrawRadioButton(ind_rc, L"", ind_state, 18.0f);
    }
    float left = bounds.left + pad;
    const float right = bounds.right - pad;
    const float top = well.bottom + 9.0f * scale;
    const D2D1_COLOR_F ink = visual.state.enabled ? theme.text : theme.text_disabled;
    const D2D1_COLOR_F sub_ink = visual.state.enabled ? theme.text_secondary : theme.text_disabled;
    const auto glyph_w = WideExtra(node.glyph);
    if (!glyph_w.empty()) {
        const D2D1_RECT_F icon_rc{left, top + 1.0f * scale, left + 16.0f * scale, top + 17.0f * scale};
        const D2D1_COLOR_F icon_color = !visual.state.enabled ? theme.text_disabled : picked ? theme.accent : theme.text_secondary;
        if (auto* dc = compositor.Dc(); !dc || !DrawExtensionIconGlyph(dc, glyph_w, icon_rc, icon_color, &theme, 1.35f * scale)) {
            painter.DrawGlyph(glyph_w, icon_rc, icon_color);
        }
        left += 22.0f * scale;
    }
    float title_right = right;
    const auto badge_w = WideExtra(node.badge_text);
    if (!badge_w.empty()) {
        const float bw = painter.MeasureBadgeWidth(badge_w);
        if (bw > 0.0f && right - left > bw + 24.0f * scale) {
            painter.DrawBadge({D2D1::RectF(right - bw, top, right, top + 20.0f * scale), badge_w,
                               picked ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral});
            title_right = right - bw - 8.0f * scale;
        }
    }
    const auto title_w = WideExtra(node.text);
    if (!title_w.empty() && title_right > left) {
        painter.DrawText(title_w, D2D1::RectF(left, top - 1.0f * scale, title_right, top + 21.0f * scale),
                         painter.RampFormat(fluent::Painter::TextRamp::Section), ink);
    }
    const auto detail_w = WideExtra(node.detail);
    if (!detail_w.empty() && right > bounds.left + pad) {
        painter.DrawWrappedCaption(detail_w, D2D1::Point2F(bounds.left + pad, top + 23.0f * scale),
                                   (std::max)(1.0f, right - bounds.left - pad), sub_ink);
    }
    if (visual.state.keyboard_focus) {
        painter.DrawFocusRing(bounds, 8.0f * scale);
    }
}

void DrawExtensionOptionCard(const extensions::UiNode& node, const ExtensionControlVisual& visual,
                             Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    const float radius = 8.0f * scale;
    const bool picked = visual.state.checked || node.selected;
    if (picked) {
        painter.FillRoundedRect(bounds, radius, WithAlpha(theme.accent, visual.dark ? 0.14f : 0.08f));
        if (visual.state.enabled) {
            if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
            else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
        }
        painter.StrokeRoundedRect(bounds, radius, theme.accent, 1.5f * scale);
    } else {
        if (node.bordered) painter.FillRoundedRect(bounds, radius, theme.surface_card);
        if (visual.state.enabled) {
            if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
            else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
        }
        if (node.bordered) painter.StrokeRoundedRect(bounds, radius, theme.stroke_card);
    }
    if (const auto* art = OptionCardArt(node)) {
        if (OptionCardInlineArt(*art)) DrawExtensionOptionInlineTile(node, *art, visual, picked, compositor, painter, theme);
        else DrawExtensionOptionTile(node, *art, visual, picked, compositor, painter, theme);
        return;
    }

    float left = bounds.left + 14.0f * scale;
    const float right = bounds.right - 14.0f * scale;
    const float top = bounds.top + 12.0f * scale;
    if (!node.state.empty() || node.selected) {
        fluent::ControlState ind_state = visual.state;
        ind_state.checked = picked;
        const D2D1_RECT_F ind_rc{left, top, left + 20.0f * scale, top + 20.0f * scale};
        if (node.option_kind == "toggle") {
            painter.DrawCheckBox(ind_rc, L"", ind_state);
        } else {
            painter.DrawRadioButton(ind_rc, L"", ind_state, 18.0f);
        }
        left += 28.0f * scale;
    }
    const auto glyph_w = WideExtra(node.glyph);
    if (!glyph_w.empty()) {
        const D2D1_RECT_F icon_rc{left, top, left + 20.0f * scale, top + 20.0f * scale};
        const D2D1_COLOR_F icon_color = !visual.state.enabled ? theme.text_disabled
                                      : picked                ? theme.accent
                                                              : theme.text_secondary;
        if (auto* dc = compositor.Dc(); !dc || !DrawExtensionIconGlyph(dc, glyph_w, icon_rc, icon_color, &theme, 1.35f * scale)) {
            painter.DrawGlyph(glyph_w, icon_rc, icon_color);
        }
        left += 28.0f * scale;
    }
    float title_right = right;
    const auto badge_w = WideExtra(node.badge_text);
    if (!badge_w.empty()) {
        const float bw = painter.MeasureBadgeWidth(badge_w);
        if (bw > 0.0f && right - left > bw + 24.0f * scale) {
            const float bh = 20.0f * scale;
            painter.DrawBadge({D2D1::RectF(right - bw, top, right, top + bh), badge_w,
                               picked ? fluent::BadgeKind::Accent : fluent::BadgeKind::Neutral});
            title_right = right - bw - 8.0f * scale;
        }
    }
    const auto title_w = WideExtra(node.text);
    const D2D1_COLOR_F ink = visual.state.enabled ? theme.text : theme.text_disabled;
    const D2D1_COLOR_F sub_ink = visual.state.enabled ? theme.text_secondary : theme.text_disabled;
    if (!title_w.empty() && title_right > left) {
        painter.DrawText(title_w, D2D1::RectF(left, top - 1.0f * scale, title_right, top + 21.0f * scale),
                         compositor.TextFormat(), ink);
    }
    const auto detail_w = WideExtra(node.detail);
    if (!detail_w.empty() && right > left) {
        painter.DrawWrappedCaption(detail_w, D2D1::Point2F(left, top + 24.0f * scale),
                                   (std::max)(1.0f, right - left), sub_ink);
    }
    if (visual.state.keyboard_focus) {
        painter.DrawFocusRing(bounds, radius);
    }
}

void DrawExtensionSettingsRow(const extensions::UiNode& node, const ExtensionControlVisual& visual,
                              Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    const float radius = 8.0f * scale;
    if (node.bordered) {
        painter.FillRoundedRect(bounds, radius, theme.surface_card);
        if (!node.command.empty() && node.children.empty() && visual.state.enabled) {
            if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
            else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
        }
        painter.StrokeRoundedRect(bounds, radius, theme.stroke_card);
    } else if (!node.command.empty() && node.children.empty() && visual.state.enabled) {
        if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
        else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
    }

    const float pad_x = node.bordered ? 16.0f * scale : 0.0f;
    const float pad_y = node.bordered ? 12.0f * scale : 4.0f * scale;
    float left = bounds.left + pad_x;
    float right = bounds.right - pad_x;
    const float inner_w = (std::max)(1.0f, right - left);

    std::vector<float> control_widths;
    const float controls_total_w = ExtensionSettingsRowInlineWidths(node, inner_w, static_cast<float>(node.gap) * scale, scale,
        [&](size_t i) { return node.children[i].visible; }, control_widths);
    if (controls_total_w > 0.0f) {
        right = (std::max)(left + 1.0f, right - controls_total_w - 16.0f * scale);
    }

    auto* dc = compositor.Dc();
    const float top = bounds.top + pad_y;
    const auto glyph_w = WideExtra(node.glyph);
    if (!glyph_w.empty()) {
        const D2D1_RECT_F icon_rc{left, top + 1.0f * scale, left + 18.0f * scale, top + 19.0f * scale};
        const D2D1_COLOR_F icon_c = visual.state.enabled ? theme.text_secondary : theme.text_disabled;
        if (!dc || !DrawExtensionIconGlyph(dc, glyph_w, icon_rc, icon_c, &theme, 1.35f * scale)) {
            painter.DrawGlyph(glyph_w, icon_rc, icon_c);
        }
        left += 26.0f * scale;
    }

    const float avail_w = (std::max)(1.0f, right - left);
    const auto title_w = WideExtra(node.text);
    const auto detail_w = WideExtra(node.detail);
    const auto badge_w = WideExtra(node.badge_text);
    const D2D1_COLOR_F ink = visual.state.enabled ? theme.text : theme.text_disabled;
    const D2D1_COLOR_F sub_ink = visual.state.enabled ? theme.text_secondary : theme.text_disabled;
    float title_h = 0.0f;
    float badge_reserve = 0.0f;
    if (!badge_w.empty()) {
        const float bw = painter.MeasureBadgeWidth(badge_w);
        if (bw > 0.0f && avail_w > bw + 48.0f * scale) {
            badge_reserve = bw + 8.0f * scale;
            const float bh = 20.0f * scale;
            painter.DrawBadge({D2D1::RectF(right - bw, top, right, top + bh), badge_w, fluent::BadgeKind::Neutral});
        }
    }
    if (!title_w.empty()) {
        const float tw = (std::max)(1.0f, avail_w - badge_reserve);
        painter.DrawWrappedBody(title_w, D2D1::Point2F(left, top), tw, ink);
        title_h = painter.MeasureWrappedBodyHeight(title_w, tw);
    }
    if (!detail_w.empty()) {
        painter.DrawWrappedCaption(detail_w, D2D1::Point2F(left, top + (title_h > 0.0f ? title_h + 3.0f * scale : 0.0f)),
                                   avail_w, sub_ink);
    }
    if (!node.command.empty() && node.children.empty() && visual.state.keyboard_focus) {
        painter.DrawFocusRing(bounds, radius);
    }
}

void DrawExtensionImage(const extensions::UiNode& node, const ExtensionControlVisual& visual,
                        Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    auto* dc = compositor.Dc();
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    const float radius = 8.0f * scale;
    if (node.bordered) {
        painter.FillRoundedRect(bounds, radius, theme.surface_card);
        if (!node.command.empty() && visual.state.enabled) {
            if (visual.state.pressed) painter.FillRoundedRect(bounds, radius, theme.fill_pressed);
            else if (visual.state.hovered) painter.FillRoundedRect(bounds, radius, theme.fill_hover);
        }
        painter.StrokeRoundedRect(bounds, radius, theme.stroke_card);
    }
    const std::string& src = !node.glyph.empty() ? node.glyph : node.detail;
    const auto inner = node.bordered
        ? D2D1::RectF(bounds.left + 6.0f * scale, bounds.top + 6.0f * scale,
                      bounds.right - 6.0f * scale, bounds.bottom - 6.0f * scale)
        : bounds;
    const float iw = inner.right - inner.left;
    const float ih = inner.bottom - inner.top;
    bool drawn = false;
    if (dc && iw > 2.0f && ih > 2.0f && src.starts_with("icon:")) {
        if (const auto id = extensions::icons::GlobalRegistry().ResolveName({}, std::string_view(src).substr(5))) {
            D2D1_COLOR_F tile_color = theme.accent;
            if (node.color.size() == 7 && node.color[0] == '#') {
                unsigned rgb = 0;
                if (std::from_chars(node.color.data() + 1, node.color.data() + 7, rgb, 16).ec == std::errc{})
                    tile_color = HexColor(rgb);
            }
            float side = (std::min)(iw, ih);
            if (node.image_fit == "center") side = (std::min)(side, 32.0f * scale);
            const D2D1_RECT_F sq{inner.left + (iw - side) * 0.5f, inner.top + (ih - side) * 0.5f,
                                 inner.left + (iw + side) * 0.5f, inner.top + (ih + side) * 0.5f};
            if (node.image_fit == "tile") {
                DrawProductTile(dc, sq, ProductArt::Generic, tile_color, StoreIcon::Puzzle, *id, &theme);
                drawn = true;
            } else {
                D2D1_COLOR_F fg = !visual.state.enabled ? theme.text_disabled
                                : !node.color.empty()   ? tile_color
                                                        : theme.text;
                drawn = DrawExtensionIcon(dc, *id, sq, fg, &theme, 1.35f * scale);
            }
        }
    } else if (dc && iw > 2.0f && ih > 2.0f && (src.starts_with("resource:") || src.starts_with("file:"))) {
        D2D1_SIZE_F nat{};
        const int req_px = (std::max)(16, static_cast<int>(std::lround((std::min)(iw, ih))));
        if (ID2D1Bitmap* bmp = LoadExtensionImageBitmap(dc, WideExtra(src), req_px, &nat);
            bmp && nat.width > 0.0f && nat.height > 0.0f) {
            D2D1_RECT_F dest = inner;
            if (node.image_fit == "center") {
                const float dw = nat.width * scale, dh = nat.height * scale;
                dest = D2D1::RectF(inner.left + (iw - dw) * 0.5f, inner.top + (ih - dh) * 0.5f,
                                   inner.left + (iw + dw) * 0.5f, inner.top + (ih + dh) * 0.5f);
            } else {
                const float rx = iw / nat.width, ry = ih / nat.height;
                const float factor = node.image_fit == "cover" ? (std::max)(rx, ry) : (std::min)(rx, ry);
                const float dw = nat.width * factor, dh = nat.height * factor;
                dest = D2D1::RectF(inner.left + (iw - dw) * 0.5f, inner.top + (ih - dh) * 0.5f,
                                   inner.left + (iw + dw) * 0.5f, inner.top + (ih + dh) * 0.5f);
            }
            dc->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
            dc->DrawBitmap(bmp, &dest, visual.state.enabled ? 1.0f : 0.45f,
                           D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
            dc->PopAxisAlignedClip();
            drawn = true;
        }
    }
    if (!drawn && !node.text.empty()) {
        painter.DrawText(WideExtra(node.text), inner, compositor.SmallFormat(),
                         theme.text_secondary, fluent::HorizontalAlignment::Center);
    }
    if (!node.command.empty() && visual.state.keyboard_focus) {
        painter.DrawFocusRing(bounds, radius);
    }
}

void DrawExtensionMenuPreview(const extensions::UiNode& node, const ExtensionControlVisual& visual,
                              Compositor& compositor, fluent::Painter& painter, const Theme& theme) {
    const auto bounds = visual.bounds;
    const float scale = (std::max)(0.25f, visual.scale);
    float y = bounds.top;
    const auto title_w = WideExtra(node.text);
    const auto detail_w = WideExtra(node.detail);
    if (!title_w.empty()) {
        painter.DrawText(title_w, D2D1::RectF(bounds.left, y, bounds.right, y + 22.0f * scale),
                         compositor.HeaderFormat(), theme.text);
        y += 22.0f * scale;
    }
    if (!detail_w.empty()) {
        const float w = (std::max)(1.0f, bounds.right - bounds.left);
        painter.DrawWrappedCaption(detail_w, D2D1::Point2F(bounds.left, y), w, theme.text_secondary);
        y += painter.MeasureWrappedCaptionHeight(detail_w, w) + 4.0f * scale;
    }
    if (y > bounds.top) y += 6.0f * scale;
    const D2D1_RECT_F card{bounds.left, y, bounds.right, (std::max)(y + 32.0f * scale, bounds.bottom)};
    painter.DrawMenuSurface(card, false);
    if (MenuPreviewHostMetrics(node)) {
        const MenuPreviewDraw draw{compositor, painter, theme, visual.state.enabled};
        auto* dc = compositor.Dc();
        if (dc) dc->PushAxisAlignedClip(card, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        WalkHostMenuPreview(node, card, scale, &draw);
        if (dc) dc->PopAxisAlignedClip();
        return;
    }
    if (node.menu.empty()) {
        painter.DrawText(L"-", card, compositor.SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
        return;
    }

    auto draw_row = [&](const extensions::UiMenuItem& item, float row_h, float indent_dip) {
        const D2D1_RECT_F row{card.left + indent_dip * scale, y, card.right, y + row_h};
        y += row_h;
        if (item.separator) {
            const float sy = (row.top + row.bottom) * 0.5f;
            painter.FillRoundedRect({row.left + 12.0f * scale, sy, card.right - 12.0f * scale, sy + 1.0f}, 0.0f,
                                    theme.stroke_divider);
            return;
        }
        const auto item_text = WideExtra(item.text);
        const auto item_shortcut = WideExtra(item.shortcut);
        const auto item_glyph = WideExtra(item.glyph);
        fluent::MenuItemSpec spec;
        spec.bounds = row;
        spec.text = item_text;
        spec.shortcut = item_shortcut;
        spec.checked = item.checked;
        spec.has_submenu = !item.children.empty();
        spec.state.enabled = visual.state.enabled && item.enabled;
        ResolveMenuPreviewGlyph(item, item_glyph, spec);
        painter.DrawMenuItem(spec);
    };

    y = card.top + 6.0f * scale;
    for (const auto& item : node.menu) {
        draw_row(item, (item.separator ? 9.0f : 30.0f) * scale, 0.0f);
        for (const auto& sub : item.children) {
            draw_row(sub, (sub.separator ? 9.0f : 26.0f) * scale, 18.0f);
        }
    }
}
}

bool ResolveExtensionColorToken(std::string_view token, const Theme& theme, bool dark, D2D1_COLOR_F& out) noexcept {
    return ResolveCanvasColor(token, theme, dark, 1.0f, out);
}

bool ExtensionCanvasPoint(const extensions::UiNode& node, D2D1_RECT_F bounds, float scale, float x, float y,
                          float& view_x, float& view_y) noexcept {
    const float w_px = bounds.right - bounds.left, h_px = bounds.bottom - bounds.top;
    scale = (std::max)(0.25f, scale);
    if (w_px <= 1.0f || h_px <= 1.0f) return false;
    // Same mapping as DrawExtensionCanvas, using the script's first viewbox statement.
    float vb_w = w_px / scale, vb_h = h_px / scale;
    bool stretch = false, found = false;
    const auto visit = [&](std::string_view raw) {
        if (found) return;
        const auto tokens = TokenizeCanvasCommand(raw);
        if (tokens.empty() || !tokens[0].key.empty() || tokens[0].value != "viewbox") return;
        std::vector<const CanvasToken*> pos;
        bool stretch_mode = false;
        for (size_t i = 1; i < tokens.size(); ++i) {
            if (tokens[i].key.empty()) pos.push_back(&tokens[i]);
            else if (tokens[i].key == "mode" && tokens[i].value == "stretch") stretch_mode = true;
        }
        float nw = 0, nh = 0;
        if (pos.size() >= 2 && ParseCanvasFloat(pos[0]->value, nw) && ParseCanvasFloat(pos[1]->value, nh) &&
            nw >= 1.0f && nh >= 1.0f) {
            vb_w = nw;
            vb_h = nh;
            stretch = stretch_mode || (pos.size() >= 3 && pos[2]->value == "stretch");
            found = true;
        }
    };
    for (const auto& entry : node.options) ForEachCanvasStatement(entry, visit);
    if (!node.detail.empty()) ForEachCanvasStatement(node.detail, visit);
    float sx = scale, sy = scale, ox = bounds.left, oy = bounds.top;
    if (stretch) {
        sx = w_px / vb_w;
        sy = h_px / vb_h;
    } else {
        const float s = (std::min)(w_px / vb_w, h_px / vb_h);
        sx = sy = s;
        ox = bounds.left + (w_px - vb_w * s) * 0.5f;
        oy = bounds.top + (h_px - vb_h * s) * 0.5f;
    }
    view_x = std::clamp((x - ox) / sx, 0.0f, vb_w);
    view_y = std::clamp((y - oy) / sy, 0.0f, vb_h);
    return true;
}

float MeasureExtensionExtraControl(const extensions::UiNode& node, float width, float scale,
    fluent::Painter& painter) {
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
    // 160 DIP unless the extension caps it (a swatch or an icon-sized drawing).
    case UiNodeType::Canvas: height = static_cast<float>(node.max_height > 0 ? (std::min)(160, node.max_height) : 160) * scale; break;
    case UiNodeType::OptionCard: {
        if (const auto* art = OptionCardArt(node); art && OptionCardInlineArt(*art)) {
            height = OptionInlineTileHeight(node, *art, width, scale, painter);
            break;
        }
        if (const auto* art = OptionCardArt(node)) {
            const float text_w = (std::max)(80.0f * scale, (width > 0.0f ? width : 220.0f * scale) - 20.0f * scale);
            const auto detail_w = WideExtra(node.detail);
            height = (10.0f + 9.0f + 22.0f + 11.0f) * scale + OptionTileArtHeight(*art, scale) +
                     (detail_w.empty() ? 0.0f : painter.MeasureWrappedCaptionHeight(detail_w, text_w) + 2.0f * scale);
            break;
        }
        const float lead = (!node.state.empty() || node.selected ? 28.0f : 0.0f) + (!node.glyph.empty() ? 28.0f : 0.0f);
        const float text_w = (std::max)(80.0f * scale, (width > 0.0f ? width : 320.0f * scale) - (28.0f + lead) * scale);
        const auto detail_w = WideExtra(node.detail);
        height = 44.0f * scale + (detail_w.empty() ? 0.0f : painter.MeasureWrappedCaptionHeight(detail_w, text_w) + 4.0f * scale);
        height = (std::max)(height, 52.0f * scale);
        break;
    }
    case UiNodeType::SettingsRow: {
        const auto title_w = WideExtra(node.text);
        const auto detail_w = WideExtra(node.detail);
        const float text_w = (std::max)(40.0f * scale, (width > 0.0f ? width : 300.0f * scale) - (!node.glyph.empty() ? 26.0f * scale : 0.0f));
        float text_h = !title_w.empty() ? painter.MeasureWrappedBodyHeight(title_w, text_w) : 20.0f * scale;
        if (!detail_w.empty()) text_h += 3.0f * scale + painter.MeasureWrappedCaptionHeight(detail_w, text_w);
        height = (std::max)(24.0f * scale, text_h);
        break;
    }
    case UiNodeType::Image:
        height = 64.0f * scale;
        break;
    case UiNodeType::MenuPreview:
        height = MenuPreviewContentHeight(node, width, scale, painter);
        break;
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
    case UiNodeType::OptionCard:
        DrawExtensionOptionCard(node, visual, compositor, painter, theme);
        return true;
    case UiNodeType::SettingsRow:
        DrawExtensionSettingsRow(node, visual, compositor, painter, theme);
        return true;
    case UiNodeType::Image:
        DrawExtensionImage(node, visual, compositor, painter, theme);
        return true;
    case UiNodeType::MenuPreview:
        DrawExtensionMenuPreview(node, visual, compositor, painter, theme);
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
        spec.adaptive = true;
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