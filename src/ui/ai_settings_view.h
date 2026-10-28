// ai_settings_view.h - what Settings > 增强包 > AI 与 MCP (kSettingsPageAi) draws.
// Built by app/ai_settings_ui.cpp on the UI thread from ai.json and
// ai_status.h; immutable once published, shared by the view models.
#pragma once

#include <d2d1.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulse::ui {

// HitTestResult::SettingsAi indices.
enum AiHit : int {
    kAiToggle = 0,            // 允许 AI 应用连接
    kAiCopyConfig = 1,        // 复制配置
    kAiOpenLog = 2,           // 打开活动记录
    kAiUninstall = 3,         // 卸载 AI 增强包
    kAiClientAllowBase = 100,   // + client row: 允许 (always)
    kAiClientDenyBase = 200,    // + client row: 拒绝 (and disconnect)
    kAiClientForgetBase = 300,  // + client row: 移除
    kAiRuleRemoveBase = 400,    // + rule row: 移除
    kAiGroupBase = 500,         // + group row: its switch (AI 应用可以做的事)
    kAiRowLimit = 99,           // rows per list that get buttons
};

// app::ai::ClientGrant values, plus "allowed until Pulse quits".
enum class AiGrantView : uint8_t { Ask, Allowed, Session, Denied };

struct AiClientRowView {
    std::wstring name;
    AiGrantView grant = AiGrantView::Ask;
    bool connected = false;
};

struct AiRuleRowView {
    std::wstring client;
    std::string tool;          // "pulse.create_zip" (what RemoveAiAllowRule needs)
    std::wstring tool_label;   // 新建压缩包
    std::wstring folder;
};

// Tone of a call's outcome text.
enum class AiCallTone : uint8_t { Done, Running, Declined, Failed };

struct AiCallRowView {
    std::wstring time;         // local HH:MM:SS
    std::wstring client;
    std::wstring tool_label;
    std::wstring outcome;
    std::wstring summary;      // the line under it (选中 6 项 → 移动到 D:\财务\2026); may be empty
    AiCallTone tone = AiCallTone::Done;
};

// AI 应用可以做的事 (P5): one switch per group of actions (action_policy.h
// McpGroup); the last row, 只能你自己做, has none.
enum class AiGroupTag : uint8_t { None, DefaultOn, Asks, AsksOutside };

struct AiGroupRowView {
    int group = 0;             // McpGroup
    std::wstring icon;         // Segoe Fluent glyph
    std::wstring title, count, desc;
    AiGroupTag tag = AiGroupTag::None;
    bool on = false;
    bool locked = false;       // no switch
};

struct AiPageView {
    bool enabled = false;      // ai.json mcp_enabled
    bool listening = false;    // the broker runs
    bool broken = false;       // ai.json does not parse: nothing is written
    bool can_uninstall = false;  // an installed (not bundled) pack
    std::wstring status;       // under the switch
    std::vector<AiClientRowView> clients;
    std::vector<AiRuleRowView> rules;
    std::vector<AiGroupRowView> groups;
    std::vector<AiCallRowView> calls;   // newest first, at most kAiPageCalls
    size_t more_calls = 0;              // older calls not listed
};

inline constexpr size_t kAiPageCalls = 12;

// Geometry of the AI page (LayoutSettingsAi in settings_ai_layout.h).
struct AiClientRowLayout {
    D2D1_RECT_F row{}, allow{}, deny{}, forget{};
};
struct AiPageLayout {
    D2D1_RECT_F main_card{}, enable_row{}, toggle{}, setup_row{}, copy{}, broken{};
    D2D1_RECT_F groups_section{}, groups_card{}, groups_foot{};
    std::vector<D2D1_RECT_F> groups, group_toggles;   // group_toggles empty for the locked row
    D2D1_RECT_F apps_section{}, apps_card{}, apps_empty{};
    std::vector<AiClientRowLayout> clients;
    D2D1_RECT_F rules_section{}, rules_card{}, rules_empty{};
    std::vector<D2D1_RECT_F> rules, rule_remove;
    D2D1_RECT_F recent_section{}, open_log{}, recent_card{}, recent_empty{}, more_calls{};
    std::vector<D2D1_RECT_F> calls;
    D2D1_RECT_F pack_section{}, uninstall_row{}, uninstall{};
};

// Settings > AI 助手 (kSettingsPageAssistant): Pulse's own assistant (Ctrl+J), its
// model service and voice input, and a way to AI 与 MCP. Built by
// app/ai_assistant_settings.cpp; never holds the key itself.
enum AssistantHit : int {
    kAsstTest = 0,     // 测试连接
    kAsstManage = 1,   // 管理连接… / 连接模型…: the assistant panel's connection card
    kAsstVoice = 2,    // 设置语音…: the panel's voice connection card
    kAsstMcp = 3,      // 在增强包中管理: 增强包 > AI 与 MCP
};
enum class AsstConn : uint8_t { None, Ok, Failed, Testing };
struct AssistantPageView {
    bool key_saved = false;
    bool official = true;          // the default DeepSeek address
    std::wstring host;             // api.deepseek.com
    std::wstring model;            // the default model id
    size_t model_count = 0;        // ids the service listed (0: not read yet)
    AsstConn conn = AsstConn::None;
    std::wstring conn_time;        // HH:MM of this run's last check
    bool voice_set = false, voice_key = false;
    std::wstring voice_service;    // 硅基流动 / OpenAI / 自定义
    std::wstring voice_host;
    bool mcp_installed = false;    // the AI pack (bundled or installed)
};
struct AssistantPageLayout {
    D2D1_RECT_F model_section{}, model_card{}, model_badge{}, test{}, manage{}, model_foot{};
    D2D1_RECT_F model_rows[4]{};   // 服务, 默认模型, API Key, 上次连接
    D2D1_RECT_F voice_section{}, voice_card{}, voice_badge{}, voice_button{}, voice_row{};
    D2D1_RECT_F mcp_section{}, mcp_card{}, mcp_badge{}, mcp_button{}, mcp_meta{};
};

} // namespace pulse::ui
