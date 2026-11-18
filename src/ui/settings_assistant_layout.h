// settings_assistant_layout.h - Settings > AI 助手 (kSettingsPageAssistant):
// texts, geometry and hit testing. Included by ui_renderer_internal.h inside
// its anonymous namespace after settings_ai_layout.h, so no includes here.
// Drawing: ui_settings_assistant.cpp; clicks: HitTestResult::SettingsAssistant
// (AssistantHit), handled in app/ai_assistant_settings.cpp; data: AssistantPageView.
//
// 模型服务 is one card in three stages: tiles to pick a service from, the
// picked service with its key field and 连接, and the service in use with its
// default model, 思考, key, address and the other services whose keys are kept.
// Then 语音输入, and a pointer to AI 与 MCP (outside apps using Pulse).

namespace asst_text {
inline const std::wstring& T(l10n::StringId id) { return l10n::Get(id); }
inline std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}
inline std::wstring Fill(l10n::StringId id, std::wstring_view key, const std::wstring& value) {
    return Fill(T(id), key, value);
}
inline const std::wstring& Title() { return T(l10n::StringId::AsstPageTitle); }
inline const std::wstring& Intro() { return T(l10n::StringId::AsstPageIntro); }
// The header row of the card.
inline const std::wstring& HeadTitle(const AssistantPageView& v) {
    using I = l10n::StringId;
    return T(v.in_use ? I::AsstPickChange : I::AsstPickTitle);
}
inline std::wstring HeadCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    switch (v.stage) {
        case AsstStage::Pick: return T(I::AsstPickDesc);
        case AsstStage::Setup: return v.custom ? T(I::AsstCustomSub) : Fill(I::AsstCompatSub, L"{sub}", v.sub);
        default: return v.host_line;
    }
}
inline const std::wstring& ConnBadge(const AssistantPageView& v) {
    using I = l10n::StringId;
    return T(v.conn == AsstConn::Failed ? I::AsstBadgeFailed : I::AiConnStatusOn);
}
inline fluent::BadgeKind ConnBadgeKind(const AssistantPageView& v) {
    return v.conn == AsstConn::Failed ? fluent::BadgeKind::Danger : fluent::BadgeKind::Success;
}
// The header's buttons, right to left.
inline int HeadButtons(const AssistantPageView& v, l10n::StringId (&out)[3], int (&targets)[3]) {
    using I = l10n::StringId;
    int n = 0;
    if (v.stage == AsstStage::Pick) {
        if (v.in_use) { out[n] = I::Cancel; targets[n++] = kAsstCancel; }
    } else if (v.stage == AsstStage::Setup) {
        out[n] = I::AsstRechoose; targets[n++] = kAsstRechoose;
    } else {
        out[n] = I::AsstChangeService; targets[n++] = kAsstChange;
        out[n] = I::AsstTest; targets[n++] = kAsstTest;
    }
    return n;
}
inline std::wstring KeyCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.stage == AsstStage::Connected) return v.key_mask;
    return v.console.empty() ? T(I::AsstKeyGetPlain) : Fill(I::AsstKeyGet, L"{console}", v.console) + L" \u2197";
}
inline std::wstring UrlCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.custom && v.stage == AsstStage::Setup) return T(I::AsstUrlCustomDesc);
    // 小米 MiMo: Token Plan keys (tp-) go to their own host (ai_providers.h AddressForKey).
    if (v.id == "mimo" && v.stage == AsstStage::Setup && v.url_default) return Fill(I::AsstUrlMimo, L"{url}", v.url);
    return v.url_default ? Fill(I::AsstUrlDefault, L"{url}", v.url) : v.url;
}
inline std::wstring ConnectCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (!v.error_title.empty()) return v.error_title + L" \u00b7 " + v.error_sub;
    return T(v.needs_key ? I::AsstConnectDesc : I::AsstConnectOllama);
}
inline const std::wstring& ThinkTitle(const AssistantPageView& v) {
    using I = l10n::StringId;
    return T(v.think == AsstThink::Toggle ? I::AsstThinkToggle : I::AiAsstEffortHeading);
}
inline const std::wstring& ThinkCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.think == AsstThink::Toggle) return T(I::AsstThinkToggleDesc);
    if (v.think == AsstThink::Fixed) return T(I::AsstThinkFixedDesc);
    return T(v.think_always ? I::AsstThinkAlwaysDesc : I::AsstThinkLevelsDesc);
}
inline int ThinkSegments(const AssistantPageView& v) {
    return v.think == AsstThink::Levels ? static_cast<int>(v.think_names.size()) : 0;
}
inline const std::wstring& ThinkLabel(const AssistantPageView& v, int i) {
    static const std::wstring kNone;
    return i >= 0 && i < static_cast<int>(v.think_names.size()) ? v.think_names[static_cast<size_t>(i)] : kNone;
}
inline int ThinkChecked(const AssistantPageView& v) { return v.think_chosen; }
// The note under the model card; a model without thinking says so first.
inline std::wstring Foot(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.stage == AsstStage::Connected && v.think == AsstThink::None && !v.think_none_model.empty())
    {
        std::wstring none = Fill(I::AsstThinkNone, L"{model}", v.think_none_model);
        if (!none.empty() && none.back() < 0x80) none += L' ';  // Latin sentences need a gap; CJK ones don't
        return none + T(I::AsstFoot);
    }
    return T(I::AsstFoot);
}
inline const std::wstring& VoiceBadge(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.voice_stage != AsstVoiceStage::Set && !v.voice_saved) return T(I::NotSet);
    return T(v.voice_has_key ? I::AsstVoiceSetBadge : I::AsstVoiceNoKey);
}
inline fluent::BadgeKind VoiceBadgeKind(const AssistantPageView& v) {
    if (v.voice_stage != AsstVoiceStage::Set && !v.voice_saved) return fluent::BadgeKind::Neutral;
    return v.voice_has_key ? fluent::BadgeKind::Success : fluent::BadgeKind::Warning;
}
// The service drop-down's text: 选择服务… until one is picked.
inline const std::wstring& VoiceChoice(const AssistantPageView& v) {
    return v.voice_stage == AsstVoiceStage::Off ? T(l10n::StringId::AsstVoicePick) : v.voice_name;
}
inline std::wstring VoiceKeyCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.voice_stage == AsstVoiceStage::Set) {
        if (!v.voice_has_key) return T(I::AsstVoiceNoKeyMask);
        return v.voice_shared ? Fill(I::AsstVoiceSharedMask, L"{mask}", v.voice_key_mask) : v.voice_key_mask;
    }
    if (v.voice_shared) return T(I::AsstVoiceKeyShared);
    return v.voice_console.empty() ? T(I::AsstKeyGetPlain) : Fill(I::AsstKeyGet, L"{console}", v.voice_console) + L" \u2197";
}
inline std::wstring VoiceUrlCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.voice_custom) return T(I::AsstUrlCustomDesc);
    return Fill(I::AsstUrlDefault, L"{url}", v.voice_url);
}
inline const std::wstring& VoiceConnectCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.voice_testing) return T(I::AiVoiceTesting);
    if (!v.voice_error.empty()) return v.voice_error;
    return T(I::AsstVoiceConnectDesc);
}
// Under 识别模型: how the last try went, while there is one to tell.
inline const std::wstring& VoiceModelCaption(const AssistantPageView& v) {
    using I = l10n::StringId;
    if (v.voice_testing) return T(I::AiVoiceTesting);
    if (!v.voice_error.empty()) return v.voice_error;
    return v.voice_ok;
}
inline const std::wstring& PauseLabel(int i) {
    using I = l10n::StringId;
    static constexpr I k[] = {I::AiVoicePause1, I::AiVoicePause2, I::AiVoicePauseManual};
    return T(k[(std::clamp)(i, 0, 2)]);
}
inline const std::wstring& KeepLabel(int i) {
    using I = l10n::StringId;
    static constexpr I k[] = {I::AiHistKeepForever, I::AiHistKeep30, I::AiHistKeep90, I::AiHistKeep365};
    return T(k[(std::clamp)(i, 0, 3)]);
}
inline const std::wstring& McpBadge(const AssistantPageView& v) {
    return T(v.mcp_installed ? l10n::StringId::Installed : l10n::StringId::NotInstalled);
}
} // namespace asst_text

inline float LayoutSettingsAssistant(SettingsLayout& l, const WindowViewModel& vm, float s, float y,
                                     const fluent::Painter* painter) {
    using I = l10n::StringId;
    AssistantPageLayout& o = l.assistant;
    o = {};
    o.scale = s;
    const AssistantPageView& v = vm.settings_assistant;
    const float left = l.content.left + 20.0f * s, right = l.content.right - 20.0f * s;
    const float text_left = left + 54.0f * s, inner_right = right - 16.0f * s;
    auto caption_h = [&](std::wstring_view text, float width) {
        if (text.empty()) return 0.0f;
        const float h = painter ? painter->MeasureWrappedCaptionHeight(text, (std::max)(40.0f * s, width)) : 0.0f;
        return h > 0.0f ? h : 20.0f * s;
    };
    auto button_w = [&](std::wstring_view text) { return painter ? painter->MeasureButtonWidth(text) : 88.0f * s; };
    auto section = [&](bool first) {
        y += first ? 4.0f * s : 22.0f * s;
        const auto r = D2D1::RectF(left, y, right, y + 30.0f * s);
        y = r.bottom;
        return r;
    };
    // A row: title and caption from text_left to controls_left; at least 64.
    auto row = [&](std::wstring_view caption, float controls_left) {
        const float h = (std::max)(64.0f * s, 38.0f * s + caption_h(caption, controls_left - 14.0f * s - text_left) + 12.0f * s);
        const auto r = D2D1::RectF(left, y, right, y + h);
        y = r.bottom;
        return r;
    };
    // Controls right to left, centred in the row: returns the leftmost edge.
    auto place = [&](const D2D1_RECT_F& r, float x_right, float w, float h = 32.0f) {
        const float mid = (r.top + r.bottom) * 0.5f;
        return D2D1::RectF(x_right - w, mid - h * 0.5f * s, x_right, mid + h * 0.5f * s);
    };
    // Fields are 280 wide when there is room (half the row at most).
    const float field_w = (std::min)(280.0f * s, (inner_right - text_left) * 0.42f);

    y += caption_h(asst_text::Intro(), right - left) + 18.0f * s;

    o.enabled_row = row(asst_text::T(I::AsstEnabledDesc), inner_right - 56.0f * s);
    o.enabled_toggle = place(o.enabled_row, inner_right, 42.0f * s);
    y += 18.0f * s;

    // 模型服务.
    o.model_section = section(true);
    const float card_top = y;
    {
        // Header: buttons (and the badge) at the right.
        I labels[3]{};
        int targets[3]{};
        const int n = asst_text::HeadButtons(v, labels, targets);
        float x = inner_right;
        float widths[3]{};
        for (int i = 0; i < n; ++i) { widths[i] = button_w(asst_text::T(labels[i])); x -= widths[i] + (i ? 8.0f * s : 0.0f); }
        float badge_w = 0.0f;
        const bool badge = v.stage == AsstStage::Connected || (v.stage == AsstStage::Pick && !v.in_use);
        if (badge) {
            const std::wstring& text = v.stage == AsstStage::Pick ? asst_text::T(I::AsstNotConnectedBadge)
                                                                   : asst_text::ConnBadge(v);
            badge_w = painter ? painter->MeasureBadgeWidth(text) : 56.0f * s;
            x -= badge_w + (n ? 10.0f * s : 0.0f);
        }
        o.head = row(asst_text::HeadCaption(v), x);
        float bx = inner_right;
        for (int i = 0; i < n; ++i) {
            o.head_buttons[i] = place(o.head, bx, widths[i]);
            bx = o.head_buttons[i].left - 8.0f * s;
        }
        if (badge) o.head_badge = place(o.head, bx + 8.0f * s - (n ? 10.0f * s : 0.0f), badge_w, 22.0f);
    }
    if (v.stage == AsstStage::Pick) {
        // Five columns of tiles (four when the page is narrow, e.g. beside the
        // docked assistant, so names like OpenRouter still fit), then 更多's chips.
        const float gap = 8.0f * s, pad = 16.0f * s, tile_h = 88.0f * s;
        const int cols = (right - left - pad * 2 - gap * 4) / 5.0f >= 100.0f * s ? 5 : 4;
        const float tile_w = (right - left - pad * 2 - gap * static_cast<float>(cols - 1)) / static_cast<float>(cols);
        const int first = (std::min)(static_cast<int>(v.tiles.size()), kAsstTiles);
        for (int i = 0; i < first; ++i) {
            const float tx = left + pad + static_cast<float>(i % cols) * (tile_w + gap);
            const float ty = y + 4.0f * s + static_cast<float>(i / cols) * (tile_h + gap);
            o.tiles[i] = D2D1::RectF(tx, ty, tx + tile_w, ty + tile_h);
        }
        y += 4.0f * s + static_cast<float>((first + cols - 1) / cols) * (tile_h + gap) + 4.0f * s;
        if (static_cast<int>(v.tiles.size()) > first) {
            const float lw = button_w(asst_text::T(I::AsstMore)) - 16.0f * s;
            o.more_label = D2D1::RectF(left + pad, y, left + pad + lw, y + 30.0f * s);
            float cx = o.more_label.right + 8.0f * s;
            for (size_t i = static_cast<size_t>(first); i < v.tiles.size(); ++i) {
                const float w = button_w(v.tiles[i].name) + 20.0f * s;
                o.more.push_back(D2D1::RectF(cx, y, cx + w, y + 30.0f * s));
                cx += w + 8.0f * s;
            }
            y += 30.0f * s + 14.0f * s;
        } else {
            y += 8.0f * s;
        }
    } else if (v.stage == AsstStage::Setup) {
        if (v.needs_key) {
            const float controls = inner_right - field_w;
            o.key_row = row(asst_text::KeyCaption(v), controls);
            o.key_field = place(o.key_row, inner_right, field_w);
            o.key_eye = D2D1::RectF(o.key_field.right - 32.0f * s, o.key_field.top + 2.0f * s,
                                    o.key_field.right - 2.0f * s, o.key_field.bottom - 2.0f * s);
            if (!v.console.empty())
                o.key_link = D2D1::RectF(text_left, o.key_row.top + 36.0f * s, controls - 14.0f * s, o.key_row.bottom - 8.0f * s);
        }
        if (v.custom || v.url_editing) {
            o.url_row = row(asst_text::UrlCaption(v), inner_right - field_w);
            o.url_field = place(o.url_row, inner_right, field_w);
        } else {
            const float w = button_w(asst_text::T(I::AsstEdit));
            o.url_row = row(asst_text::UrlCaption(v), inner_right - w);
            o.url_buttons[0] = place(o.url_row, inner_right, w);
        }
        const std::wstring& label = asst_text::T(v.connecting ? I::AsstConnecting : I::AsstConnectBtn);
        const float w = button_w(label) + (v.connecting ? 24.0f * s : 0.0f);
        o.connect_row = row(asst_text::ConnectCaption(v), inner_right - w);
        o.connect = place(o.connect_row, inner_right, w);
    } else {
        if (!v.fresh.empty()) {
            const float h = (std::max)(48.0f * s, caption_h(v.fresh, inner_right - text_left) + 24.0f * s);
            o.fresh_row = D2D1::RectF(left, y, right, y + h);
            y = o.fresh_row.bottom;
        }
        // 默认模型: the drop-down, or a field for a typed name.
        if (v.model_editing) {
            const float save_w = button_w(asst_text::T(I::Save)), cancel_w = button_w(asst_text::T(I::Cancel));
            const float controls = inner_right - cancel_w - save_w - field_w - 16.0f * s;
            o.model_row = row(asst_text::T(I::AsstModelDesc2), controls);
            o.model_buttons[1] = place(o.model_row, inner_right, cancel_w);
            o.model_buttons[0] = place(o.model_row, o.model_buttons[1].left - 8.0f * s, save_w);
            o.model_field = place(o.model_row, o.model_buttons[0].left - 8.0f * s, field_w);
        } else {
            o.model_row = row(asst_text::T(I::AsstModelDesc2), inner_right - field_w);
            o.model_choice = place(o.model_row, inner_right, field_w);
        }
        // 思考强度: the shared step slider with its level on the right, like
        // the other settings sliders. 深度思考 is a switch.
        if (const int n = asst_text::ThinkSegments(v)) {
            float value_w = 24.0f * s;
            for (int i = 0; i < n; ++i)
                value_w = (std::max)(value_w, button_w(asst_text::ThinkLabel(v, i)) - 16.0f * s);
            // Slider and level as wide as the model field above (200 at most),
            // so a narrow page leaves the caption room.
            const float gap = 12.0f * s;
            const float slider_w = (std::clamp)(field_w - value_w - gap, 128.0f * s, 200.0f * s);
            o.think_row = row(asst_text::ThinkCaption(v), inner_right - value_w - gap - slider_w);
            o.think_value = place(o.think_row, inner_right, value_w, 24.0f);
            o.think_slider = place(o.think_row, inner_right - value_w - gap, slider_w, fluent::StepSliderHeight(s) / s);
        } else if (v.think == AsstThink::Toggle) {
            o.think_row = row(asst_text::ThinkCaption(v), inner_right - 42.0f * s);
            o.think_toggle = place(o.think_row, inner_right, 42.0f * s);
        } else if (v.think == AsstThink::Fixed) {
            // 始终思考: no control, the words where the level would be.
            const float value_w = button_w(asst_text::T(I::AiAsstThinkAlways)) - 16.0f * s;
            o.think_row = row(asst_text::ThinkCaption(v), inner_right - value_w);
            o.think_value = place(o.think_row, inner_right, value_w, 24.0f);
        }
        // API Key: 更换, 移除 (Ollama has none).
        if (v.needs_key) {
            const float a = button_w(asst_text::T(I::AsstReplace)), b = button_w(asst_text::T(I::Remove));
            o.key_row = row(asst_text::KeyCaption(v), inner_right - a - b - 8.0f * s);
            o.key_buttons[1] = place(o.key_row, inner_right, b);
            o.key_buttons[0] = place(o.key_row, o.key_buttons[1].left - 8.0f * s, a);
        }
        // 接口地址: 修改, or a field with 保存 / 取消.
        if (v.url_editing) {
            const float save_w = button_w(asst_text::T(I::Save)), cancel_w = button_w(asst_text::T(I::Cancel));
            const float controls = inner_right - cancel_w - save_w - field_w - 16.0f * s;
            o.url_row = row(asst_text::UrlCaption(v), controls);
            o.url_buttons[1] = place(o.url_row, inner_right, cancel_w);
            o.url_buttons[0] = place(o.url_row, o.url_buttons[1].left - 8.0f * s, save_w);
            o.url_field = place(o.url_row, o.url_buttons[0].left - 8.0f * s, field_w);
        } else {
            const float w = button_w(asst_text::T(I::AsstEdit));
            o.url_row = row(asst_text::UrlCaption(v), inner_right - w);
            o.url_buttons[0] = place(o.url_row, inner_right, w);
        }
        // 其他已保存的服务: chips under the title, wrapping.
        if (!v.others.empty()) {
            const float top = y;
            float cx = text_left, cy = top + 38.0f * s + caption_h(asst_text::T(I::AsstOthersDesc), inner_right - text_left) + 6.0f * s;
            for (const AsstOtherView& other : v.others) {
                std::wstring label = other.name;
                if (!other.model.empty()) label += L"  " + other.model;
                const float w = (std::min)(button_w(label) + 22.0f * s, inner_right - text_left);
                if (cx > text_left && cx + w > inner_right) { cx = text_left; cy += 38.0f * s; }
                o.others.push_back(D2D1::RectF(cx, cy, cx + w, cy + 32.0f * s));
                cx += w + 8.0f * s;
            }
            o.others_row = D2D1::RectF(left, top, right, cy + 32.0f * s + 14.0f * s);
            y = o.others_row.bottom;
        }
    }
    y += 4.0f * s;
    o.model_card = D2D1::RectF(left, card_top, right, y);
    {
        // The note under the card, like every other page's.
        const std::wstring foot = asst_text::Foot(v);
        const float h = caption_h(foot, right - left - 12.0f * s);
        o.model_foot = D2D1::RectF(left + 6.0f * s, y + 8.0f * s, right - 6.0f * s, y + 8.0f * s + h);
        y = o.model_foot.bottom;
    }

    // 语音输入: the service drop-down in the header; setting one up: key,
    // address (custom), 连接; in use: key, model, 停顿, microphone.
    o.voice_section = section(false);
    {
        const float top = y;
        const float choice_w = (std::min)(200.0f * s, field_w);
        const float bw = painter ? painter->MeasureBadgeWidth(asst_text::VoiceBadge(v)) : 56.0f * s;
        o.voice_head = row(asst_text::T(I::AsstVoiceDesc), inner_right - choice_w - 10.0f * s - bw);
        o.voice_service = place(o.voice_head, inner_right, choice_w);
        o.voice_badge = place(o.voice_head, o.voice_service.left - 10.0f * s, bw, 22.0f);
        if (v.voice_stage == AsstVoiceStage::Setup) {
            const float controls = inner_right - field_w;
            o.voice_key_row = row(asst_text::VoiceKeyCaption(v), controls);
            o.voice_key_field = place(o.voice_key_row, inner_right, field_w);
            o.voice_key_eye = D2D1::RectF(o.voice_key_field.right - 32.0f * s, o.voice_key_field.top + 2.0f * s,
                                          o.voice_key_field.right - 2.0f * s, o.voice_key_field.bottom - 2.0f * s);
            if (!v.voice_shared && !v.voice_console.empty())
                o.voice_key_link = D2D1::RectF(text_left, o.voice_key_row.top + 36.0f * s, controls - 14.0f * s,
                                               o.voice_key_row.bottom - 8.0f * s);
            if (v.voice_custom) {
                o.voice_url_row = row(asst_text::VoiceUrlCaption(v), inner_right - field_w);
                o.voice_url_field = place(o.voice_url_row, inner_right, field_w);
            } else {
                o.voice_url_row = row(asst_text::VoiceUrlCaption(v), inner_right);
            }
            const std::wstring& label = asst_text::T(v.voice_testing ? I::AsstConnecting : I::AsstVoiceConnectBtn);
            const float w = button_w(label) + (v.voice_testing ? 24.0f * s : 0.0f);
            const float cw = button_w(asst_text::T(I::Cancel));
            o.voice_connect_row = row(asst_text::VoiceConnectCaption(v), inner_right - w - 8.0f * s - cw);
            o.voice_connect = place(o.voice_connect_row, inner_right, w);
            o.voice_cancel = place(o.voice_connect_row, o.voice_connect.left - 8.0f * s, cw);
        } else if (v.voice_stage == AsstVoiceStage::Set) {
            const float rw = button_w(asst_text::T(I::AsstReplace));
            o.voice_key_row = row(asst_text::VoiceKeyCaption(v), inner_right - rw);
            o.voice_rekey = place(o.voice_key_row, inner_right, rw);
            o.voice_model_row = row(asst_text::VoiceModelCaption(v), inner_right - field_w);
            o.voice_model = place(o.voice_model_row, inner_right, field_w);
            float widths[3]{}, total = 0.0f;
            for (int i = 0; i < 3; ++i) {
                widths[i] = (std::max)(52.0f * s, button_w(asst_text::PauseLabel(i)) - 8.0f * s);
                total += widths[i];
            }
            o.voice_pause_row = row({}, inner_right - total);
            float sx = inner_right - total;
            for (int i = 0; i < 3; ++i) {
                o.voice_pause[i] = place(o.voice_pause_row, sx + widths[i], widths[i]);
                sx += widths[i];
            }
            o.voice_mic_row = row({}, inner_right - field_w);
            o.voice_mic = place(o.voice_mic_row, inner_right, field_w);
        }
        y += 4.0f * s;
        o.voice_card = D2D1::RectF(left, top, right, y);
        const std::wstring& foot = asst_text::T(I::AsstVoiceFoot);
        const float h = caption_h(foot, right - left - 12.0f * s);
        o.voice_foot = D2D1::RectF(left + 6.0f * s, y + 8.0f * s, right - 6.0f * s, y + 8.0f * s + h);
        y = o.voice_foot.bottom;
    }

    // 55 P3 历史对话: 保存对话历史 (a switch) and, while it is on, 保留时间.
    o.hist_section = section(false);
    {
        const float top = y;
        o.hist_row = row(asst_text::T(I::AiHistSaveDesc), inner_right - 42.0f * s);
        o.hist_toggle = place(o.hist_row, inner_right, 42.0f * s);
        if (v.history) {
            float widths[4]{}, total = 0.0f;
            for (int i = 0; i < 4; ++i) {
                widths[i] = (std::max)(52.0f * s, button_w(asst_text::KeepLabel(i)) - 8.0f * s);
                total += widths[i];
            }
            o.hist_keep_row = row(asst_text::T(I::AiHistKeepDesc), inner_right - total);
            float sx = inner_right - total;
            for (int i = 0; i < 4; ++i) {
                o.hist_keep[i] = place(o.hist_keep_row, sx + widths[i], widths[i]);
                sx += widths[i];
            }
        }
        y += 4.0f * s;
        o.hist_card = D2D1::RectF(left, top, right, y);
    }

    // 让其他 AI 应用使用 Pulse: AI 与 MCP lives on its own page under 增强包.
    o.mcp_section = section(false);
    {
        const float top = y;
        const float w = button_w(asst_text::T(I::AsstMcpManage));
        o.mcp_button = D2D1::RectF(inner_right - w, top + 48.0f * s, inner_right, top + 80.0f * s);
        const float bw = painter ? painter->MeasureBadgeWidth(asst_text::McpBadge(v)) : 56.0f * s;
        o.mcp_badge = D2D1::RectF(inner_right - bw, top + 14.0f * s, inner_right, top + 36.0f * s);
        const float desc = caption_h(asst_text::T(I::AsstMcpDesc), o.mcp_button.left - 14.0f * s - text_left);
        const float ry = top + (std::max)(70.0f * s, 38.0f * s + desc + 8.0f * s);
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
    if (in(o.enabled_toggle)) return kAsstEnabled;
    l10n::StringId labels[3]{};
    int targets[3]{};
    const int n = asst_text::HeadButtons(v, labels, targets);
    for (int i = 0; i < n; ++i) {
        if (!in(o.head_buttons[i])) continue;
        if (targets[i] == kAsstTest && v.conn == AsstConn::Testing) return -1;
        return targets[i];
    }
    for (int i = 0; i < kAsstTiles; ++i)
        if (in(o.tiles[i])) return kAsstTileBase + i;
    for (size_t i = 0; i < o.more.size(); ++i)
        if (in(o.more[i])) return kAsstTileBase + kAsstTiles + static_cast<int>(i);
    if (v.stage == AsstStage::Setup) {
        if (in(o.key_eye)) return kAsstReveal;
        if (in(o.key_field)) return kAsstKeyField;
        if (in(o.key_link)) return kAsstKeyLink;
        if (in(o.url_field)) return kAsstUrlField;
        if (in(o.url_buttons[0])) return kAsstUrlEdit;
        if (in(o.connect) && !v.connecting) return kAsstConnect;
    }
    if (in(o.voice_service)) return kAsstVoiceService;
    if (v.voice_stage == AsstVoiceStage::Setup) {
        if (in(o.voice_key_eye)) return kAsstVoiceReveal;
        if (in(o.voice_key_field)) return kAsstVoiceKeyField;
        if (in(o.voice_key_link)) return kAsstVoiceKeyLink;
        if (in(o.voice_url_field)) return kAsstVoiceUrlField;
        if (in(o.voice_cancel)) return kAsstVoiceCancel;
        if (in(o.voice_connect) && !v.voice_testing) return kAsstVoiceConnect;
    } else if (v.voice_stage == AsstVoiceStage::Set) {
        if (in(o.voice_rekey)) return kAsstVoiceRekey;
        if (in(o.voice_model) && !v.voice_testing) return kAsstVoiceModel;
        for (int i = 0; i < 3; ++i)
            if (in(o.voice_pause[i])) return kAsstVoicePauseBase + i;
        if (in(o.voice_mic)) return kAsstVoiceMic;
    }
    if (in(o.hist_toggle)) return kAsstHistToggle;
    for (int i = 0; i < 4; ++i)
        if (in(o.hist_keep[i])) return kAsstHistKeepBase + i;
    if (in(o.mcp_button)) return kAsstMcp;
    if (v.stage != AsstStage::Connected) return -1;
    if (in(o.model_choice)) return kAsstModel;
    if (in(o.model_field)) return kAsstModelField;
    if (in(o.model_buttons[0])) return kAsstModelSave;
    if (in(o.model_buttons[1])) return kAsstModelCancel;
    if (in(o.think_slider)) {
        const int stops = asst_text::ThinkSegments(v);
        const float at = fluent::StepSliderPositionAt(o.think_slider, stops, x, o.scale);
        return kAsstThinkBase + std::clamp(static_cast<int>(std::lround(at)), 0, stops - 1);
    }
    if (in(o.think_toggle)) return kAsstThinkToggle;
    if (in(o.key_buttons[0])) return kAsstRekey;
    if (in(o.key_buttons[1])) return kAsstRemoveKey;
    if (in(o.url_field)) return kAsstUrlField;
    if (in(o.url_buttons[0])) return v.url_editing ? kAsstUrlSave : kAsstUrlEdit;
    if (in(o.url_buttons[1])) return kAsstUrlCancel;
    for (size_t i = 0; i < o.others.size(); ++i)
        if (in(o.others[i])) return kAsstOtherBase + static_cast<int>(i);
    return -1;
}
