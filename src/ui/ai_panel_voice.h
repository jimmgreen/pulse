// ai_panel_voice.h - 语音输入 (third batch, flow V): the 语音设置 menu over
// the mic's chevron (.vmenu). AiPanelView draws it (ai_panel_voice.cpp);
// app/ai_assistant_voice.cpp fills the view. The connection itself is set up
// in Settings > AI 助手.
#pragma once

#include "ai_panel_view.h"

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pulse::ui {

struct AiVoiceMenuView {
    std::wstring heading;            // 语音识别
    std::wstring service;            // 硅基流动 / 还没有语音连接
    std::wstring service_sub;        // SenseVoiceSmall · 已连接
    bool connected = false;          // the ok dot (else a grey one)
    std::wstring manage;             // 语音设置…
    std::wstring mic_label;          // 麦克风
    std::wstring mic_name;           // 耳机 (USB Audio) / 没有找到
    bool mic_missing = false;        // the name in text3
    std::wstring pause_heading;      // 停顿多久算说完
    std::array<std::wstring, 3> pauses;   // 1 秒 · 2 秒 · 手动
    int pause = 0;
    std::wstring note;               // 只在点麦克风时录音…
};

// The view's voice state (private to ai_panel_view.cpp / ai_panel_voice.cpp).
struct AiPanelView::VoiceGeom {
    // The menu's pop (shown keeps the last view while it fades out).
    bool menu_shown = false;
    uint64_t menu_at = 0;
    std::shared_ptr<const AiVoiceMenuView> menu_view;
    D2D1_RECT_F menu_rect{};
    // The menu's hit rects of the last frame.
    struct Hit {
        D2D1_RECT_F rect;
        AiPanelHit hit;
    };
    std::vector<Hit> menu_hits;
    D2D1_RECT_F mic_box{};
    // The composer while listening: the indicator did not fit after the text.
    bool extra_line = false;
    // The waveform as drawn: each bar eases toward its level (.12 s).
    std::array<float, 14> shown{};
    uint64_t shown_at = 0;
};

} // namespace pulse::ui
