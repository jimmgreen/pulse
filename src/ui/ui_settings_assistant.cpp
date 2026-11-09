#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../app/resource.h"
#include "../common/localization.h"

namespace pulse::ui {

namespace {

// The vendor marks (assets/providers, lobe-icons MIT / simple-icons CC0).
int ProviderLogoResource(std::string_view id) {
    struct Entry { const char* id; int resource; };
    static constexpr Entry kLogos[] = {
        {"deepseek", IDR_PROVIDER_DEEPSEEK_SVG}, {"qwen", IDR_PROVIDER_QWEN_SVG},
        {"kimi", IDR_PROVIDER_KIMI_SVG},         {"zhipu", IDR_PROVIDER_ZHIPU_SVG},
        {"doubao", IDR_PROVIDER_DOUBAO_SVG},     {"silicon", IDR_PROVIDER_SILICON_SVG},
        {"openai", IDR_PROVIDER_OPENAI_SVG},     {"openrouter", IDR_PROVIDER_OPENROUTER_SVG},
        {"ollama", IDR_PROVIDER_OLLAMA_SVG},     {"gemini", IDR_PROVIDER_GEMINI_SVG},
        {"claude", IDR_PROVIDER_CLAUDE_SVG},     {"mimo", IDR_PROVIDER_MIMO_SVG},
    };
    for (const Entry& e : kLogos)
        if (id == e.id) return e.resource;
    return 0;
}

// `text` as it fits `width` in `format`: whole, else cut with an ellipsis
// (never sliced mid-glyph by the clip).
std::wstring FitLine(Compositor* compositor, IDWriteTextFormat* format, const std::wstring& text, float width) {
    if (typography::MeasureLine(compositor, format, text) <= width) return text;
    std::wstring cut = text;
    while (cut.size() > 1 && typography::MeasureLine(compositor, format, cut + L"\u2026") > width) cut.pop_back();
    return cut + L"\u2026";
}

} // namespace

// A service's mark; a letter in a rounded square when there is none (自定义)
// or Direct2D cannot draw it (Windows 8.1).
void MainRenderer::DrawProviderMark(std::string_view id, std::wstring_view name, const D2D1_RECT_F& r,
                                    const Theme& theme) {
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    // 小米 MiMo's mark is a two-line wordmark: a little wider than the square ones.
    D2D1_RECT_F logo = r;
    if (id == "mimo") {
        const float grow = (r.right - r.left) * 0.15f;
        logo.left -= grow;
        logo.right += grow;
    }
    if (const int resource = ProviderLogoResource(id); resource && DrawProviderLogo(resource, dark, logo)) return;
    const float side = (std::min)(r.right - r.left, r.bottom - r.top);
    const D2D1_RECT_F box = D2D1::RectF((r.left + r.right - side) * 0.5f, (r.top + r.bottom - side) * 0.5f,
                                        (r.left + r.right + side) * 0.5f, (r.top + r.bottom + side) * 0.5f);
    painter_.FillRoundedRect(box, side * 0.28f, WithAlpha(theme.accent, dark ? 0.30f : 0.16f));
    std::wstring letter = id == "custom" || name.empty() ? std::wstring(L"\xE71B") : std::wstring(name.substr(0, 1));
    if (id == "custom") {
        DrawIconText(box.left, box.top, side, side, letter.c_str(), L"", theme.text, 0.62f);
        return;
    }
    painter_.DrawText(letter, box, compositor_->TextFormat(), theme.text, fluent::HorizontalAlignment::Center);
}

// Settings > AI 助手 (kSettingsPageAssistant). Geometry: LayoutSettingsAssistant;
// texts: asst_text (settings_assistant_layout.h); clicks: SettingsAssistant.
// 思考强度's knob: under the pointer while held, then a short snap onto the
// chosen level with a slight overshoot, as in the composer's popover.
float MainRenderer::AssistantThinkPosition(int stops, int chosen, float drag) {
    constexpr uint64_t kSnapMs = 240;
    AsstThinkMotion& m = asst_think_;
    const uint64_t now = motion::NowMs();
    // Another service, or the page coming back: start on the level.
    if (stops != m.stops || (!m.dragging && now - m.drawn > 400)) {
        m.position = static_cast<float>(chosen);
        m.target = chosen;
        m.until = 0;
        m.dragging = false;
    }
    m.stops = stops;
    m.drawn = now;
    if (drag >= 0.0f) {
        m.position = std::clamp(drag, 0.0f, static_cast<float>(stops - 1));
        m.dragging = true;
        return m.position;
    }
    if (m.dragging || chosen != m.target) {
        m.from = m.position;
        m.target = chosen;
        m.at = now;
        m.until = motion::SystemAnimationsEnabled() ? now + kSnapMs : 0;
        m.dragging = false;
    }
    if (now >= m.until) return m.position = static_cast<float>(chosen);
    const float u = std::clamp(static_cast<float>(now - m.at) / static_cast<float>(kSnapMs), 0.0f, 1.0f);
    const float c1 = 1.1f, c3 = c1 + 1.0f, w = u - 1.0f;
    const float e = 1.0f + c3 * w * w * w + c1 * w * w;
    return m.position = m.from + (static_cast<float>(chosen) - m.from) * e;
}

void MainRenderer::DrawSettingsAssistant(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    using H = HitTestResult;
    using I = l10n::StringId;
    const AssistantPageView& v = vm.settings_assistant;
    auto* dc = compositor_->Dc();
    const auto lay = MakeSettingsLayout(vm, rect, scale_, title_bar_height_, status_height_, &painter_);
    const AssistantPageLayout& o = lay.assistant;
    const float s = scale_;
    const bool dark = theme.bg.r + theme.bg.g + theme.bg.b < 1.5f;
    const D2D1_COLOR_F success = dark ? D2D1::ColorF(0x45D7A4) : D2D1::ColorF(0x107C5A);
    const D2D1_COLOR_F danger = painter_.StatusToneColor(fluent::InfoBarKind::Error);
    auto hovered = [&](int target) { return IsHovered(vm, H::SettingsAssistant, target); };
    auto text = [&](std::wstring_view value, D2D1_RECT_F r, bool is_small = false) {
        painter_.DrawText(value, r, is_small ? compositor_->SmallFormat() : compositor_->TextFormat(),
                          is_small ? theme.text_secondary : theme.text);
    };
    auto card = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, WithAlpha(theme.surface_card, card_alpha_), brFillInput_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brFillInput_.get());
        dc->DrawRoundedRectangle(D2D1::RoundedRect(r, 8*s, 8*s), brStrokeCard_.get(), 1);
    };
    auto divider_below = [&](D2D1_RECT_F r) {
        if (r.bottom <= r.top) return;
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        FillRect(dc, brStrokeDivider_.get(), r.left + 16*s, r.bottom, r.right - r.left - 32*s, 1);
    };
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, int target,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && hovered(target);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto badge = [&](D2D1_RECT_F r, std::wstring_view label, fluent::BadgeKind kind) {
        if (r.right <= r.left) return;
        fluent::BadgeSpec spec;
        spec.bounds = r;
        spec.text = label;
        spec.kind = kind;
        painter_.DrawBadge(spec);
    };
    // A row's icon, title and caption, left of `controls_left`.
    auto head = [&](D2D1_RECT_F c, const wchar_t* icon, std::wstring_view title, std::wstring_view caption,
                    float controls_left, const D2D1_COLOR_F* caption_color = nullptr) {
        if (c.bottom <= c.top) return;
        if (caption.empty()) {
            // A title alone sits in the middle of its row, level with the control.
            const float mid = (c.top + c.bottom) * 0.5f;
            if (icon) DrawIconText(c.left + 16*s, mid - 12*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
            text(title, D2D1::RectF(c.left + 54*s, mid - 12*s, controls_left - 14*s, mid + 12*s));
            return;
        }
        if (icon) DrawIconText(c.left + 16*s, c.top + 14*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(c.left + 54*s, c.top + 12*s, controls_left - 14*s, c.top + 36*s));
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(c.left + 54*s, c.top + 38*s),
                                    controls_left - 14*s - c.left - 54*s,
                                    caption_color ? *caption_color : theme.text_secondary);
    };
    // A field frame for a hosted EDIT (the EDIT draws the text on top); the
    // app places the EDITs where the last frame drew them.
    asst_drawn_ = true;
    asst_clip_ = lay.content;
    for (auto& f : asst_fields_) f = {};
    asst_edit_back_ = theme.fill_input_focus;
    asst_edit_back_.a = 1.0f;
    asst_edit_fore_ = theme.text;
    asst_edit_hint_ = theme.text_secondary;
    auto field = [&](D2D1_RECT_F r, bool focused, AsstField which) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.focused = focused;
        painter_.DrawTextFieldFrame(r, state, true);
        // Text inside the frame: 10 DIP in, clear of the key field's eye.
        const bool eye = which == AsstField::Key || which == AsstField::VoiceKey;
        asst_fields_[static_cast<int>(which)] =
            D2D1::RectF(r.left + 10*s, r.top + 2*s, r.right - (eye ? 36*s : 10*s), r.bottom - 3*s);
    };
    const float controls_left_head = [&] {
        float x = o.model_card.right - 16*s;
        for (const auto& b : o.head_buttons) if (b.right > b.left) x = (std::min)(x, b.left);
        if (o.head_badge.right > o.head_badge.left) x = (std::min)(x, o.head_badge.left);
        return x;
    }();

    card(o.enabled_row);
    head(o.enabled_row, L"\xE713", asst_text::T(I::AsstEnabledTitle), asst_text::T(I::AsstEnabledDesc), o.enabled_toggle.left);
    {
        fluent::ControlState state{};
        state.checked = v.enabled;
        state.hovered = hovered(kAsstEnabled);
        painter_.DrawSwitch(o.enabled_toggle, L"", state);
    }
    // 模型服务.
    painter_.DrawText(asst_text::T(I::AsstModelSection), o.model_section, compositor_->TextFormat(), theme.text);
    card(o.model_card);
    {
        const D2D1_RECT_F& h = o.head;
        if (v.stage == AsstStage::Pick) {
            head(h, L"\xF5C2", asst_text::HeadTitle(v), asst_text::HeadCaption(v), controls_left_head);
            if (!v.in_use) badge(o.head_badge, asst_text::T(I::AsstNotConnectedBadge), fluent::BadgeKind::Neutral);
        } else {
            DrawProviderMark(v.id, v.name, D2D1::RectF(h.left + 14*s, h.top + 12*s, h.left + 42*s, h.top + 40*s), theme);
            head(h, nullptr, v.name, asst_text::HeadCaption(v), controls_left_head);
            if (v.stage == AsstStage::Connected) badge(o.head_badge, asst_text::ConnBadge(v), asst_text::ConnBadgeKind(v));
        }
        I labels[3]{};
        int targets[3]{};
        const int n = asst_text::HeadButtons(v, labels, targets);
        for (int i = 0; i < n; ++i) {
            const bool testing = targets[i] == kAsstTest && v.conn == AsstConn::Testing;
            button(o.head_buttons[i], asst_text::T(testing ? I::AsstConnecting : labels[i]), targets[i],
                   fluent::ButtonKind::Standard, !testing);
        }
    }
    if (v.stage == AsstStage::Pick) {
        divider_below(o.head);
        // Tiles: the mark, the name and a line; the one in use is marked like
        // the selected window effect card, with a check in the corner.
        for (int i = 0; i < kAsstTiles && i < static_cast<int>(v.tiles.size()); ++i) {
            const D2D1_RECT_F& t = o.tiles[i];
            if (t.right <= t.left) continue;
            const AsstTileView& tile = v.tiles[static_cast<size_t>(i)];
            const bool hot = hovered(kAsstTileBase + i);
            const D2D1_COLOR_F fill = tile.current ? WithAlpha(theme.accent, dark ? 0.16f : 0.08f)
                                    : hot ? theme.fill_hover : WithAlpha(theme.text, dark ? 0.04f : 0.025f);
            painter_.FillRoundedRect(t, 6*s, fill);
            MakeBrush(dc, tile.current ? theme.accent : theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(t.left + 0.5f, t.top + 0.5f, t.right - 0.5f, t.bottom - 0.5f),
                                                       6*s, 6*s), brStrokeCard_.get(), tile.current ? 1.5f : 1.0f);
            const float cx = (t.left + t.right) * 0.5f;
            DrawProviderMark(tile.id, tile.name, D2D1::RectF(cx - 14*s, t.top + 12*s, cx + 14*s, t.top + 40*s), theme);
            // A narrow page (the assistant docked): a long name steps down to
            // the small face, and what still does not fit ends in an ellipsis.
            const float text_w = t.right - t.left - 12*s;
            IDWriteTextFormat* name_format = compositor_->TextFormat();
            if (typography::MeasureLine(compositor_, name_format, tile.name) > text_w)
                name_format = compositor_->SmallFormat();
            painter_.DrawText(FitLine(compositor_, name_format, tile.name, text_w),
                              D2D1::RectF(t.left + 6*s, t.top + 44*s, t.right - 6*s, t.top + 64*s),
                              name_format, theme.text, fluent::HorizontalAlignment::Center);
            painter_.DrawText(FitLine(compositor_, compositor_->SmallFormat(), tile.sub, text_w),
                              D2D1::RectF(t.left + 6*s, t.top + 64*s, t.right - 6*s, t.top + 82*s),
                              compositor_->SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
            if (tile.current) {
                const auto center = D2D1::Point2F(t.right - 11*s, t.top + 11*s);
                MakeBrush(dc, theme.accent, brAccent_);
                dc->FillEllipse(D2D1::Ellipse(center, 7*s, 7*s), brAccent_.get());
                DrawIconText(center.x - 6*s, center.y - 6*s, 12*s, 12*s, L"\xE73E", L"", theme.accent_text, 0.6f);
            }
        }
        // 更多: Gemini, Claude.
        if (o.more_label.right > o.more_label.left) {
            text(asst_text::T(I::AsstMore), o.more_label, true);
            for (size_t i = 0; i < o.more.size(); ++i) {
                const size_t index = static_cast<size_t>(kAsstTiles) + i;
                if (index >= v.tiles.size()) break;
                const AsstTileView& tile = v.tiles[index];
                const D2D1_RECT_F& c = o.more[i];
                const bool hot = hovered(kAsstTileBase + static_cast<int>(index));
                painter_.FillRoundedRect(c, 15*s, tile.current ? WithAlpha(theme.accent, dark ? 0.16f : 0.08f)
                                                  : hot ? theme.fill_hover : WithAlpha(theme.text, dark ? 0.05f : 0.035f));
                DrawProviderMark(tile.id, tile.name, D2D1::RectF(c.left + 8*s, c.top + 7*s, c.left + 24*s, c.top + 23*s), theme);
                painter_.DrawText(tile.name, D2D1::RectF(c.left + 30*s, c.top + 5*s, c.right - 8*s, c.bottom - 5*s),
                                  compositor_->SmallFormat(), theme.text);
            }
        }
    } else if (v.stage == AsstStage::Setup) {
        divider_below(o.head);
        if (o.key_row.bottom > o.key_row.top) {
            // The link to the key page reads as a link.
            const bool link = !v.console.empty();
            const D2D1_COLOR_F& c = theme.accent;   // lifted toward white in dark mode to stay readable
            const D2D1_COLOR_F accent = dark ? D2D1::ColorF(c.r + (1 - c.r) * 0.45f, c.g + (1 - c.g) * 0.45f,
                                                            c.b + (1 - c.b) * 0.45f) : c;
            head(o.key_row, L"\xE8D7", asst_text::T(I::AsstKey), asst_text::KeyCaption(v), o.key_field.left,
                 link ? &accent : nullptr);
            field(o.key_field, v.key_typed, AsstField::Key);
            DrawIconText(o.key_eye.left, o.key_eye.top, o.key_eye.right - o.key_eye.left, o.key_eye.bottom - o.key_eye.top,
                         v.revealed ? L"\xED1A" : L"\xE7B3", L"", hovered(kAsstReveal) ? theme.text : theme.text_secondary, 0.6f);
            divider_below(o.key_row);
        }
        head(o.url_row, L"\xE774", asst_text::T(I::AsstUrlTitle), asst_text::UrlCaption(v),
             o.url_field.right > o.url_field.left ? o.url_field.left : o.url_buttons[0].left);
        field(o.url_field, v.url_typed, AsstField::Url);
        button(o.url_buttons[0], asst_text::T(I::AsstEdit), kAsstUrlEdit);
        divider_below(o.url_row);
        const bool failed = !v.error_title.empty();
        head(o.connect_row, L"\xE703", asst_text::T(I::AsstConnectTitle), asst_text::ConnectCaption(v), o.connect.left,
             failed ? &danger : nullptr);
        const bool ready = (!v.needs_key || v.key_typed) && (!v.custom || v.url_typed) && !v.connecting;
        button(o.connect, asst_text::T(v.connecting ? I::AsstConnecting : I::AsstConnectBtn), kAsstConnect,
               fluent::ButtonKind::Primary, ready);
    } else {
        divider_below(o.head);
        if (o.fresh_row.bottom > o.fresh_row.top) {
            // 连接正常 · 120 毫秒 · 读到 12 个模型…: right after connecting.
            const D2D1_RECT_F& r = o.fresh_row;
            const float mid = r.top + 24*s;
            const D2D1_COLOR_F dot = v.fresh_manual ? painter_.StatusToneColor(fluent::InfoBarKind::Warning) : success;
            painter_.FillRoundedRect(D2D1::RectF(r.left + 25*s, mid - 4*s, r.left + 33*s, mid + 4*s), 4*s, dot);
            painter_.DrawWrappedCaption(v.fresh, D2D1::Point2F(r.left + 54*s, r.top + 14*s), r.right - 16*s - r.left - 54*s,
                                        theme.text);
            divider_below(r);
        }
        // 默认模型.
        head(o.model_row, L"\xF5C2", asst_text::T(I::AsstDefaultModel), asst_text::T(I::AsstModelDesc2),
             v.model_editing ? o.model_field.left : o.model_choice.left);
        if (v.model_editing) {
            field(o.model_field, true, AsstField::Model);
            button(o.model_buttons[0], asst_text::T(I::Save), kAsstModelSave, fluent::ButtonKind::Primary);
            button(o.model_buttons[1], asst_text::T(I::Cancel), kAsstModelCancel);
        } else if (o.model_choice.right > o.model_choice.left) {
            fluent::ButtonSpec control{};
            control.bounds = o.model_choice;
            const std::wstring& placeholder = asst_text::T(I::AsstModelNone);
            control.text = v.model.empty() ? std::wstring_view(placeholder) : std::wstring_view(v.model);
            control.drop_down = true;
            control.state.hovered = hovered(kAsstModel);
            painter_.DrawButton(control);
        }
        divider_below(o.model_row);
        // 思考强度: the step slider the composer's popover uses, its level on
        // the right. 深度思考 is a switch.
        if (const int n = asst_text::ThinkSegments(v)) {
            head(o.think_row, L"\xEA80", asst_text::ThinkTitle(v), asst_text::ThinkCaption(v), o.think_slider.left);
            const int chosen = asst_text::ThinkChecked(v);
            const bool held = v.think_drag >= 0.0f;
            fluent::StepSliderSpec slider;
            slider.bounds = o.think_slider;
            slider.stops = n;
            slider.position = AssistantThinkPosition(n, chosen, v.think_drag);
            for (int i = 0; i < n; ++i) slider.state.hovered = slider.state.hovered || hovered(kAsstThinkBase + i);
            slider.state.pressed = held;
            painter_.DrawStepSlider(slider);
            const int shown = held ? std::clamp(static_cast<int>(std::lround(slider.position)), 0, n - 1) : chosen;
            text(asst_text::ThinkLabel(v, shown), o.think_value);
            divider_below(o.think_row);
        } else if (v.think == AsstThink::Toggle) {
            head(o.think_row, L"\xEA80", asst_text::ThinkTitle(v), asst_text::ThinkCaption(v), o.think_toggle.left);
            fluent::ControlState state{};
            state.checked = v.think_chosen > 0;
            state.hovered = hovered(kAsstThinkToggle);
            painter_.DrawSwitch(o.think_toggle, L"", state);
            divider_below(o.think_row);
        } else if (v.think == AsstThink::Fixed) {
            head(o.think_row, L"\xEA80", asst_text::ThinkTitle(v), asst_text::ThinkCaption(v), o.think_value.left);
            text(asst_text::T(I::AiAsstThinkAlways), o.think_value);
            divider_below(o.think_row);
        }
        // API Key.
        if (o.key_row.bottom > o.key_row.top) {
            head(o.key_row, L"\xE8D7", asst_text::T(I::AsstKey), asst_text::KeyCaption(v), o.key_buttons[0].left);
            button(o.key_buttons[0], asst_text::T(I::AsstReplace), kAsstRekey);
            button(o.key_buttons[1], asst_text::T(I::Remove), kAsstRemoveKey);
            divider_below(o.key_row);
        }
        // 接口地址.
        head(o.url_row, L"\xE774", asst_text::T(I::AsstUrlTitle), asst_text::UrlCaption(v),
             v.url_editing ? o.url_field.left : o.url_buttons[0].left);
        if (v.url_editing) {
            field(o.url_field, true, AsstField::Url);
            button(o.url_buttons[0], asst_text::T(I::Save), kAsstUrlSave, fluent::ButtonKind::Primary, v.url_typed);
            button(o.url_buttons[1], asst_text::T(I::Cancel), kAsstUrlCancel);
        } else {
            button(o.url_buttons[0], asst_text::T(I::AsstEdit), kAsstUrlEdit);
        }
        // 其他已保存的服务.
        if (o.others_row.bottom > o.others_row.top) {
            divider_below(o.url_row);
            head(o.others_row, L"\xE8F1", asst_text::T(I::AsstOthers), asst_text::T(I::AsstOthersDesc),
                 o.others_row.right - 16*s);
            for (size_t i = 0; i < o.others.size() && i < v.others.size(); ++i) {
                const D2D1_RECT_F& c = o.others[i];
                const AsstOtherView& other = v.others[i];
                const bool hot = hovered(kAsstOtherBase + static_cast<int>(i));
                painter_.FillRoundedRect(c, 6*s, hot ? theme.fill_hover : WithAlpha(theme.text, dark ? 0.05f : 0.035f));
                MakeBrush(dc, theme.stroke_card, brStrokeCard_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(c.left + 0.5f, c.top + 0.5f, c.right - 0.5f, c.bottom - 0.5f),
                                                           6*s, 6*s), brStrokeCard_.get(), 1.0f);
                DrawProviderMark(other.id, other.name, D2D1::RectF(c.left + 8*s, c.top + 8*s, c.left + 24*s, c.top + 24*s), theme);
                const float name_w = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->TextFormat(), other.name);
                painter_.DrawText(other.name, D2D1::RectF(c.left + 30*s, c.top + 6*s, c.right - 8*s, c.bottom - 6*s),
                                  compositor_->TextFormat(), theme.text);
                if (!other.model.empty())
                    painter_.DrawText(other.model, D2D1::RectF(c.left + 30*s + name_w + 8*s, c.top + 7*s, c.right - 8*s, c.bottom - 5*s),
                                      compositor_->SmallFormat(), theme.text_secondary);
            }
        }
    }
    // The note under the card, as on every other page.
    if (o.model_foot.bottom > o.model_foot.top)
        painter_.DrawWrappedCaption(asst_text::Foot(v), D2D1::Point2F(o.model_foot.left, o.model_foot.top),
                                    o.model_foot.right - o.model_foot.left, theme.text_secondary);

    // 语音输入.
    // A drop-down button with a service's mark (or none) before its text.
    auto choice = [&](D2D1_RECT_F r, std::wstring_view label, int target, std::string_view mark, bool muted = false) {
        if (r.right <= r.left) return;
        fluent::ButtonSpec control{};
        control.bounds = r;
        control.drop_down = true;
        control.state.hovered = hovered(target);
        painter_.DrawButton(control);
        float x = r.left + 12*s;
        if (!mark.empty()) {
            const float mid = (r.top + r.bottom) * 0.5f;
            DrawProviderMark(mark, label, D2D1::RectF(x, mid - 8*s, x + 16*s, mid + 8*s), theme);
            x += 24*s;
        }
        painter_.DrawText(label, D2D1::RectF(x, r.top, r.right - 30*s, r.bottom), compositor_->TextFormat(),
                          muted ? theme.text_secondary : theme.text);
    };
    // The voice services' marks: 硅基流动 and OpenAI share the model tiles'.
    auto voice_mark = [&](const std::string& id) -> std::string_view {
        if (id == "siliconflow") return "silicon";
        if (id == "openai") return "openai";
        if (id == "custom") return "custom";
        return {};
    };
    // A row under the header without an icon (the approved card's sub rows).
    auto sub_row = [&](D2D1_RECT_F r, std::wstring_view title, std::wstring_view caption, float controls_left,
                       const D2D1_COLOR_F* caption_color = nullptr) {
        head(r, nullptr, title, caption, controls_left, caption_color);
    };
    painter_.DrawText(asst_text::T(I::AsstVoiceSection), o.voice_section, compositor_->TextFormat(), theme.text);
    card(o.voice_card);
    head(o.voice_head, L"\xE720", asst_text::T(I::AsstVoiceTitle), asst_text::T(I::AsstVoiceDesc), o.voice_badge.left);
    badge(o.voice_badge, asst_text::VoiceBadge(v), asst_text::VoiceBadgeKind(v));
    choice(o.voice_service, asst_text::VoiceChoice(v), kAsstVoiceService,
           v.voice_stage == AsstVoiceStage::Off ? std::string_view{} : voice_mark(v.voice_id),
           v.voice_stage == AsstVoiceStage::Off);
    if (v.voice_stage == AsstVoiceStage::Setup) {
        divider_below(o.voice_head);
        const bool link = !v.voice_shared && !v.voice_console.empty();
        const D2D1_COLOR_F& c = theme.accent;
        const D2D1_COLOR_F accent = dark ? D2D1::ColorF(c.r + (1 - c.r) * 0.45f, c.g + (1 - c.g) * 0.45f,
                                                        c.b + (1 - c.b) * 0.45f) : c;
        head(o.voice_key_row, L"\xE8D7", asst_text::T(I::AsstKey), asst_text::VoiceKeyCaption(v), o.voice_key_field.left,
             link ? &accent : nullptr);
        field(o.voice_key_field, v.voice_key_typed, AsstField::VoiceKey);
        DrawIconText(o.voice_key_eye.left, o.voice_key_eye.top, o.voice_key_eye.right - o.voice_key_eye.left,
                     o.voice_key_eye.bottom - o.voice_key_eye.top, v.voice_revealed ? L"\xED1A" : L"\xE7B3", L"",
                     hovered(kAsstVoiceReveal) ? theme.text : theme.text_secondary, 0.6f);
        divider_below(o.voice_key_row);
        head(o.voice_url_row, L"\xE774", asst_text::T(I::AsstUrlTitle), asst_text::VoiceUrlCaption(v),
             o.voice_url_field.right > o.voice_url_field.left ? o.voice_url_field.left : o.voice_url_row.right - 16*s);
        field(o.voice_url_field, v.voice_url_typed, AsstField::VoiceUrl);
        divider_below(o.voice_url_row);
        const bool failed = !v.voice_error.empty() && !v.voice_testing;
        head(o.voice_connect_row, L"\xE703", asst_text::T(I::AsstConnectTitle), asst_text::VoiceConnectCaption(v),
             o.voice_cancel.left, failed ? &danger : nullptr);
        button(o.voice_cancel, asst_text::T(I::Cancel), kAsstVoiceCancel);
        const bool ready = (v.voice_key_typed || v.voice_shared) && (!v.voice_custom || v.voice_url_typed) && !v.voice_testing;
        button(o.voice_connect, asst_text::T(v.voice_testing ? I::AsstConnecting : I::AsstVoiceConnectBtn), kAsstVoiceConnect,
               fluent::ButtonKind::Primary, ready);
    } else if (v.voice_stage == AsstVoiceStage::Set) {
        divider_below(o.voice_head);
        sub_row(o.voice_key_row, asst_text::T(I::AsstKey), asst_text::VoiceKeyCaption(v), o.voice_rekey.left);
        button(o.voice_rekey, asst_text::T(I::AsstReplace), kAsstVoiceRekey);
        divider_below(o.voice_key_row);
        const bool failed = !v.voice_error.empty() && !v.voice_testing;
        sub_row(o.voice_model_row, asst_text::T(I::AsstVoiceModelTitle), asst_text::VoiceModelCaption(v), o.voice_model.left,
                failed ? &danger : nullptr);
        choice(o.voice_model, v.voice_model, kAsstVoiceModel, {});
        divider_below(o.voice_model_row);
        sub_row(o.voice_pause_row, asst_text::T(I::AiVoicePauseHeading), {}, o.voice_pause[0].left);
        painter_.DrawSegmentedTrack(D2D1::RectF(o.voice_pause[0].left, o.voice_pause[0].top,
                                                o.voice_pause[2].right, o.voice_pause[2].bottom));
        if (v.voice_pause >= 0 && v.voice_pause < 3) DrawSettingsSegmentPlate(100001, v.voice_pause, o.voice_pause[v.voice_pause]);
        for (int i = 0; i < 3; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = o.voice_pause[i];
            item.text = asst_text::PauseLabel(i);
            item.state.checked = i == v.voice_pause;
            item.suppress_plate = item.state.checked;
            item.state.hovered = hovered(kAsstVoicePauseBase + i);
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                : i == 2 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
        divider_below(o.voice_pause_row);
        sub_row(o.voice_mic_row, asst_text::T(I::AiVoiceMicLabel), {}, o.voice_mic.left);
        choice(o.voice_mic, v.voice_mic, kAsstVoiceMic, {});
    }
    if (o.voice_foot.bottom > o.voice_foot.top)
        painter_.DrawWrappedCaption(asst_text::T(I::AsstVoiceFoot), D2D1::Point2F(o.voice_foot.left, o.voice_foot.top),
                                    o.voice_foot.right - o.voice_foot.left, theme.text_secondary);

    // 55 P3 历史对话.
    painter_.DrawText(asst_text::T(I::AiHist), o.hist_section, compositor_->TextFormat(), theme.text);
    card(o.hist_card);
    head(o.hist_row, L"\xE81C", asst_text::T(I::AiHistSaveRow), asst_text::T(I::AiHistSaveDesc), o.hist_toggle.left);
    {
        fluent::ControlState state{};
        state.checked = v.history;
        state.hovered = hovered(kAsstHistToggle);
        painter_.DrawSwitch(o.hist_toggle, L"", state);
    }
    if (o.hist_keep_row.bottom > o.hist_keep_row.top) {
        divider_below(o.hist_row);
        sub_row(o.hist_keep_row, asst_text::T(I::AiHistKeepRow), asst_text::T(I::AiHistKeepDesc), o.hist_keep[0].left);
        painter_.DrawSegmentedTrack(D2D1::RectF(o.hist_keep[0].left, o.hist_keep[0].top, o.hist_keep[3].right,
                                                o.hist_keep[3].bottom));
        if (v.history_keep >= 0 && v.history_keep < 4) DrawSettingsSegmentPlate(100002, v.history_keep, o.hist_keep[v.history_keep]);
        for (int i = 0; i < 4; ++i) {
            fluent::SegmentedItemSpec item{};
            item.bounds = o.hist_keep[i];
            item.text = asst_text::KeepLabel(i);
            item.state.checked = i == v.history_keep;
            item.suppress_plate = item.state.checked;
            item.state.hovered = hovered(kAsstHistKeepBase + i);
            item.shared_track = true;
            item.position = i == 0 ? fluent::SegmentPosition::First
                : i == 3 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            painter_.DrawSegmentedItem(item);
        }
    }

    // 让其他 AI 应用使用 Pulse.
    painter_.DrawText(asst_text::T(I::AsstMcpSection), o.mcp_section, compositor_->TextFormat(), theme.text);
    card(o.mcp_card);
    head(o.mcp_card, L"\xE99A", asst_text::T(I::AiPackTitle), asst_text::T(I::AsstMcpDesc), o.mcp_button.left);
    badge(o.mcp_badge, asst_text::McpBadge(v), v.mcp_installed ? fluent::BadgeKind::Success : fluent::BadgeKind::Neutral);
    button(o.mcp_button, asst_text::T(I::AsstMcpManage), kAsstMcp);
    text(asst_text::T(I::AsstMcpMeta), o.mcp_meta, true);
}

} // namespace pulse::ui
