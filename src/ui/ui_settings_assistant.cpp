#include "../common/windows_compat.h"
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"

namespace pulse::ui {

// Settings > AI 助手 (kSettingsPageAssistant). Geometry: LayoutSettingsAssistant;
// texts: asst_text (settings_assistant_layout.h); clicks: SettingsAssistant.
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
    auto button = [&](D2D1_RECT_F r, std::wstring_view label, int target,
                      fluent::ButtonKind kind = fluent::ButtonKind::Standard, bool enabled = true) {
        if (r.right <= r.left) return;
        fluent::ControlState state{};
        state.enabled = enabled;
        state.hovered = enabled && IsHovered(vm, H::SettingsAssistant, target);
        painter_.DrawButton({r, label, {}, kind, state});
    };
    auto badge = [&](D2D1_RECT_F r, std::wstring_view label, fluent::BadgeKind kind) {
        fluent::BadgeSpec spec;
        spec.bounds = r;
        spec.text = label;
        spec.kind = kind;
        painter_.DrawBadge(spec);
    };
    // Icon, title and caption of a card, left of `controls_left`.
    auto head = [&](D2D1_RECT_F c, const wchar_t* icon, std::wstring_view title, std::wstring_view caption,
                    float controls_left) {
        DrawIconText(c.left + 16*s, c.top + 14*s, 24*s, 24*s, icon, L"", theme.text_secondary, 0.85f);
        text(title, D2D1::RectF(c.left + 54*s, c.top + 12*s, controls_left - 14*s, c.top + 36*s));
        painter_.DrawWrappedCaption(caption, D2D1::Point2F(c.left + 54*s, c.top + 38*s),
                                    controls_left - 14*s - c.left - 54*s, theme.text_secondary);
    };
    // A quiet pill (optionally led by a status dot); returns its right edge.
    auto pill = [&](float x, float mid, std::wstring_view value, const D2D1_COLOR_F* dot) {
        const float w = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), std::wstring(value)) +
                        (dot ? 30*s : 18*s);
        const D2D1_RECT_F r = D2D1::RectF(x, mid - 11*s, x + w, mid + 11*s);
        painter_.FillRoundedRect(r, 6*s, WithAlpha(theme.text, dark ? 0.08f : 0.05f));
        float tx = r.left + 9*s;
        if (dot) {
            painter_.FillRoundedRect(D2D1::RectF(tx, mid - 3*s, tx + 6*s, mid + 3*s), 3*s, *dot);
            tx += 12*s;
        }
        painter_.DrawText(value, D2D1::RectF(tx, mid - 10*s, r.right - 6*s, mid + 10*s), compositor_->SmallFormat(),
                          theme.text);
        return r.right;
    };
    auto field_label = [&](D2D1_RECT_F r, int row) {
        text(asst_text::RowLabel(row), D2D1::RectF(r.left, r.top + 5*s, r.left + 104*s, r.bottom - 5*s), true);
    };

    // 模型服务.
    painter_.DrawText(asst_text::T(I::AsstModelSection), o.model_section, compositor_->TextFormat(), theme.text);
    card(o.model_card);
    head(o.model_card, L"\xF5C2", asst_text::T(I::AsstModelTitle), asst_text::T(I::AsstModelDesc), o.test.left);
    badge(o.model_badge, asst_text::ModelBadge(v), asst_text::ModelBadgeKind(v));
    button(o.test, asst_text::T(I::AsstTest), kAsstTest, fluent::ButtonKind::Standard,
           v.key_saved && v.conn != AsstConn::Testing);
    button(o.manage, asst_text::ManageButton(v), kAsstManage, fluent::ButtonKind::Primary);
    {
        const float value_x = o.model_rows[0].left + 110*s;
        const D2D1_COLOR_F idle = theme.text_secondary;
        // 服务: the service with a dot (green once a key is saved and works) and its host.
        const auto& r0 = o.model_rows[0];
        field_label(r0, 0);
        const float mid0 = (r0.top + r0.bottom) * 0.5f;
        const bool live = v.key_saved && v.conn != AsstConn::Failed;
        float x = pill(value_x, mid0, asst_text::ServiceName(v), live ? &success : &idle);
        if (!v.host.empty())
            text(v.host, D2D1::RectF(x + 8*s, mid0 - 10*s, r0.right, mid0 + 10*s), true);
        // 默认模型.
        const auto& r1 = o.model_rows[1];
        field_label(r1, 1);
        const float mid1 = (r1.top + r1.bottom) * 0.5f;
        x = v.model.empty() ? value_x - 8*s : pill(value_x, mid1, v.model, nullptr);
        text(asst_text::ModelCount(v), D2D1::RectF(x + 8*s, mid1 - 10*s, r1.right, mid1 + 10*s), true);
        // API Key: whether one is stored; the key itself is never read here.
        const auto& r2 = o.model_rows[2];
        field_label(r2, 2);
        painter_.DrawText(asst_text::KeyState(v), D2D1::RectF(value_x, r2.top + 5*s, r2.right, r2.bottom - 5*s),
                          compositor_->SmallFormat(), v.key_saved ? theme.text : theme.text_secondary);
        // 上次连接 (this run).
        const auto& r3 = o.model_rows[3];
        field_label(r3, 3);
        const D2D1_COLOR_F tone = v.conn == AsstConn::Ok ? success : v.conn == AsstConn::Failed ? danger
                                                                                                : theme.text_secondary;
        painter_.DrawText(asst_text::ConnState(v), D2D1::RectF(value_x, r3.top + 5*s, r3.right, r3.bottom - 5*s),
                          compositor_->SmallFormat(), tone);
    }
    {
        // Where the connection is managed: a note with an accent edge.
        const auto& n = o.model_foot;
        painter_.FillRoundedRect(n, 6*s, WithAlpha(theme.text, dark ? 0.06f : 0.035f));
        painter_.FillRoundedRect(D2D1::RectF(n.left, n.top, n.left + 3*s, n.bottom), 1.5f*s, theme.accent);
        painter_.DrawWrappedCaption(asst_text::T(I::AsstManageFoot), D2D1::Point2F(n.left + 16*s, n.top + 9*s),
                                    n.right - n.left - 28*s, theme.text);
    }

    // 语音输入.
    painter_.DrawText(asst_text::T(I::AsstVoiceSection), o.voice_section, compositor_->TextFormat(), theme.text);
    card(o.voice_card);
    head(o.voice_card, L"\xE720", asst_text::T(I::AsstVoiceTitle), asst_text::T(I::AsstVoiceDesc), o.voice_button.left);
    badge(o.voice_badge, asst_text::VoiceBadge(v), asst_text::VoiceBadgeKind(v));
    button(o.voice_button, asst_text::T(I::AsstVoiceSet), kAsstVoice);
    {
        const auto& r = o.voice_row;
        field_label(r, 0);
        const float mid = (r.top + r.bottom) * 0.5f;
        const D2D1_COLOR_F idle = theme.text_secondary;
        const bool ready = v.voice_set && v.voice_key;
        const float x = pill(r.left + 110*s, mid, v.voice_set ? v.voice_service : asst_text::T(I::NotSet),
                             ready ? &success : &idle);
        text(v.voice_set ? v.voice_host : asst_text::T(I::AsstVoiceHint),
             D2D1::RectF(x + 8*s, mid - 10*s, r.right, mid + 10*s), true);
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
