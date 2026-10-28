// settings_assistant_layout.h - Settings > AI 助手 (kSettingsPageAssistant):
// texts, geometry and hit testing. Included by ui_renderer_internal.h inside
// its anonymous namespace after settings_ai_layout.h, so no includes here.
// Drawing: ui_settings_assistant.cpp; clicks: HitTestResult::SettingsAssistant
// (AssistantHit), handled in app/ai_assistant_settings.cpp; data: AssistantPageView.
//
// Three cards: 模型服务 (where the assistant's model comes from), 语音输入, and
// a pointer to AI 与 MCP, which is something else (outside apps using Pulse).

namespace asst_text {
inline const std::wstring& T(l10n::StringId id) { return l10n::Get(id); }
inline std::wstring Fill(l10n::StringId id, std::wstring_view key, const std::wstring& value) {
    std::wstring text = T(id);
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}
inline const std::wstring& Title() { return T(l10n::StringId::AsstPageTitle); }
inline const std::wstring& Intro() { return T(l10n::StringId::AsstPageIntro); }
inline const std::wstring& ManageButton(const AssistantPageView& v) {
    return T(v.key_saved ? l10n::StringId::AsstManage : l10n::StringId::AsstConnect);
}
// The badge at a card's top right, and its tone.
inline const std::wstring& ModelBadge(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (!v.key_saved) return T(I::NotSet);
    return T(v.conn == AsstConn::Failed ? I::AsstBadgeFailed : I::AiConnStatusOn);
}
inline fluent::BadgeKind ModelBadgeKind(const AssistantPageView& v) {
    if (!v.key_saved) return fluent::BadgeKind::Neutral;
    return v.conn == AsstConn::Failed ? fluent::BadgeKind::Danger : fluent::BadgeKind::Success;
}
inline const std::wstring& VoiceBadge(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (!v.voice_set) return T(I::NotSet);
    return T(v.voice_key ? I::AsstVoiceSetBadge : I::AsstVoiceNoKey);
}
inline fluent::BadgeKind VoiceBadgeKind(const AssistantPageView& v) {
    if (!v.voice_set) return fluent::BadgeKind::Neutral;
    return v.voice_key ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
}
inline const std::wstring& McpBadge(const AssistantPageView& v) {
    return T(v.mcp_installed ? l10n::StringId::Installed : l10n::StringId::NotInstalled);
}
inline std::wstring ServiceName(const AssistantPageView& v) {
    return T(v.official ? l10n::StringId::AsstServiceOfficial : l10n::StringId::AsstServiceCustom);
}
inline std::wstring ModelCount(const AssistantPageView& v) {
    if (v.model_count == 0) return T(l10n::StringId::AsstModelUnlisted);
    return Fill(l10n::StringId::AsstModelCount, L"{n}", std::to_wstring(v.model_count));
}
inline std::wstring KeyState(const AssistantPageView& v) {
    return T(v.key_saved ? l10n::StringId::AsstKeySaved : l10n::StringId::NotSet);
}
inline std::wstring ConnState(const AssistantPageView& v) {
    using I = l10n::StringId;
    const std::wstring when = Fill(I::AsstToday, L"{time}", v.conn_time);
    switch (v.conn) {
        case AsstConn::Ok: return Fill(I::AsstConnOk, L"{time}", when);
        case AsstConn::Failed: return Fill(I::AsstConnBad, L"{time}", when);
        case AsstConn::Testing: return T(I::AsstConnTesting);
        default: return T(I::AsstConnNever);
    }
}
inline const std::wstring& RowLabel(int row) {
    using I = l10n::StringId;
    const I ids[] = {I::AsstService, I::AsstDefaultModel, I::AsstKey, I::AsstLastConn};
    return T(ids[row < 0 || row > 3 ? 0 : row]);
}
} // namespace asst_text

inline float LayoutSettingsAssistant(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                     const fluent::Painter* painter) {
    AssistantPageLayout& o = l.assistant;
    o = {};
    const AssistantPageView& v = vm.settings_assistant;
    const float left = l.content.left + 20.0f * s, right = l.content.right - 20.0f * s;
    const float text_left = left + 54.0f * s, inner_right = right - 16.0f * s;
    auto caption_h = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * s, width)) : 0.0f;
        return h > 0.0f ? h : 20.0f * s;
    };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 88.0f * s; };
    auto badge = [&](float top, std::wstring_view text) {
        const float w = painter ? painter->MeasureBadgeWidth(text) : 56.0f * s;
        return D2D1::RectF(inner_right - w, top + 14.0f * s, inner_right, top + 36.0f * s);
    };
    auto section = [&](bool first) {
        y += first ? 4.0f * s : 22.0f * s;
        const auto r = D2D1::RectF(left, y, right, y + 30.0f * s);
        y = r.bottom;
        return r;
    };
    // A card's header: title and caption left of the buttons (which sit under
    // the badge); returns the header height, at least `minimum` (rows that run
    // under the buttons start below them).
    auto header = [&](std::wstring_view caption, float buttons_left, float minimum) {
        return (std::max)(minimum * s, 38.0f * s + caption_h(caption, buttons_left - 14.0f * s - text_left) + 8.0f * s);
    };

    y += caption_h(asst_text::Intro(), right - left) + 18.0f * s;

    // 模型服务: the service, model, key and last check; 测试连接 / 管理连接….
    o.model_section = section(true);
    {
        const float top = y;
        const float manage_w = button_w(asst_text::ManageButton(v));
        const float test_w = button_w(asst_text::T(l10n::StringId::AsstTest));
        o.manage = D2D1::RectF(inner_right - manage_w, top + 48.0f * s, inner_right, top + 80.0f * s);
        o.test = D2D1::RectF(o.manage.left - 8.0f * s - test_w, o.manage.top, o.manage.left - 8.0f * s, o.manage.bottom);
        o.model_badge = badge(top, asst_text::ModelBadge(v));
        float ry = top + header(asst_text::T(l10n::StringId::AsstModelDesc), o.test.left, 92.0f);
        for (auto& r : o.model_rows) {
            r = D2D1::RectF(text_left, ry, inner_right, ry + 30.0f * s);
            ry = r.bottom;
        }
        const std::wstring& foot = asst_text::T(l10n::StringId::AsstManageFoot);
        ry += 12.0f * s;
        o.model_foot = D2D1::RectF(text_left, ry, inner_right, ry + caption_h(foot, inner_right - text_left - 28.0f * s) + 18.0f * s);
        y = o.model_foot.bottom + 16.0f * s;
        o.model_card = D2D1::RectF(left, top, right, y);
    }

    // 语音输入: the speech service; 设置语音….
    o.voice_section = section(false);
    {
        const float top = y;
        const float w = button_w(asst_text::T(l10n::StringId::AsstVoiceSet));
        o.voice_button = D2D1::RectF(inner_right - w, top + 48.0f * s, inner_right, top + 80.0f * s);
        o.voice_badge = badge(top, asst_text::VoiceBadge(v));
        const float ry = top + header(asst_text::T(l10n::StringId::AsstVoiceDesc), o.voice_button.left, 66.0f);
        o.voice_row = D2D1::RectF(text_left, ry, o.voice_button.left - 12.0f * s, ry + 30.0f * s);
        y = o.voice_row.bottom + 14.0f * s;
        o.voice_card = D2D1::RectF(left, top, right, y);
    }

    // 让其他 AI 应用使用 Pulse: AI 与 MCP lives on its own page under 增强包.
    o.mcp_section = section(false);
    {
        const float top = y;
        const float w = button_w(asst_text::T(l10n::StringId::AsstMcpManage));
        o.mcp_button = D2D1::RectF(inner_right - w, top + 48.0f * s, inner_right, top + 80.0f * s);
        o.mcp_badge = badge(top, asst_text::McpBadge(v));
        const float ry = top + header(asst_text::T(l10n::StringId::AsstMcpDesc), o.mcp_button.left, 70.0f);
        o.mcp_meta = D2D1::RectF(text_left, ry - 4.0f * s, o.mcp_button.left - 12.0f * s, ry + 22.0f * s);
        y = o.mcp_meta.bottom + 14.0f * s;
        o.mcp_card = D2D1::RectF(left, top, right, y);
    }
    return y + 24.0f * s;
}

inline int HitTestSettingsAssistant(const SettingsLayout& l, const WindowViewModel& vm, float x, float y) {
    const AssistantPageLayout& o = l.assistant;
    auto in = [&](const D2D1_RECT_F& r) { return r.right > r.left && x >= r.left && x < r.right && y >= r.top && y < r.bottom; };
    const AssistantPageView& v = vm.settings_assistant;
    if (in(o.test) && v.key_saved && v.conn != AsstConn::Testing) return kAsstTest;
    if (in(o.manage)) return kAsstManage;
    if (in(o.voice_button)) return kAsstVoice;
    if (in(o.mcp_button)) return kAsstMcp;
    return -1;
}
