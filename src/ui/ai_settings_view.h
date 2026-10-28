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
// app/ai_assistant_settings.cpp; never holds a key, only its mask.
enum AssistantHit : int {
    kAsstTest = 0,          // 测试连接
    kAsstVoice = 2,         // 设置语音…: the panel's voice connection card
    kAsstMcp = 3,           // 在增强包中管理: 增强包 > AI 与 MCP
    kAsstChange = 10,       // 更换服务: back to the tiles
    kAsstCancel = 11,       // 取消 (tiles, while a service is in use)
    kAsstRechoose = 12,     // 换一个服务 (setting one up)
    kAsstReveal = 13,       // the key field's eye
    kAsstPaste = 14,        // 粘贴
    kAsstKeyLink = 15,      // 到…创建: the service's key page
    kAsstConnect = 16,      // 连接并读取模型
    kAsstModel = 17,        // 默认模型 drop-down
    kAsstThinkToggle = 18,  // 深度思考
    kAsstRekey = 19,        // API Key 更换
    kAsstRemoveKey = 20,    // API Key 移除
    kAsstUrlEdit = 21,      // 接口地址 修改
    kAsstUrlSave = 22,
    kAsstUrlCancel = 23,
    kAsstModelSave = 24,    // a typed model name
    kAsstModelCancel = 25,
    kAsstKeyField = 26,     // a field whose EDIT is hidden: focus it
    kAsstUrlField = 27,
    kAsstModelField = 28,
    kAsstTileBase = 100,    // + tile (kAsstTiles first-screen tiles, then 更多's)
    kAsstThinkBase = 200,   // + 思考强度 segment
    kAsstOtherBase = 300,   // + 其他已保存的服务 chip
};
// Fields the page hosts an EDIT in (MainRenderer::SettingsAssistantField).
enum class AsstField : uint8_t { Key, Url, Model };
enum class AsstConn : uint8_t { None, Ok, Failed, Testing };
// Tiles to pick from, the service being set up, or the one in use.
enum class AsstStage : uint8_t { Pick, Setup, Connected };
// How the service takes 思考: four levels, three, on/off, or not at all.
enum class AsstThink : uint8_t { None, Levels4, Levels3, Toggle };
inline constexpr int kAsstTiles = 10;
struct AsstTileView {
    std::string id;            // ai_providers.h
    std::wstring name, sub;
    bool current = false;      // the service in use
    bool saved = false;        // its key is stored: a click switches to it
};
struct AsstOtherView {
    std::string id;
    std::wstring name, model;
};
struct AssistantPageView {
    AsstStage stage = AsstStage::Pick;
    bool in_use = false;               // a service is set up (the tiles offer 取消)
    std::vector<AsstTileView> tiles;   // first screen, then 更多 (Gemini, Claude)
    // The service being set up or in use.
    std::string id;
    std::wstring name, sub;            // sub: under the name
    bool needs_key = true, custom = false;
    std::wstring console;              // 到{console}创建; empty: no key page
    std::wstring url;                  // the address
    bool url_default = true;           // the service's own
    bool url_editing = false;          // the address row is a field
    bool key_typed = false, url_typed = false;
    bool revealed = false;
    bool connecting = false;
    std::wstring error_title, error_sub;   // the last 连接 that failed
    // In use.
    std::wstring host_line;            // api.deepseek.com · 上次连接 14:32 · 120 毫秒
    AsstConn conn = AsstConn::None;
    std::wstring fresh;                // 连接正常 · … right after connecting; empty otherwise
    bool fresh_manual = false;         // …but the service lists no models: type one
    std::wstring model;                // the default model id
    bool model_editing = false;        // 手动输入模型名
    AsstThink think = AsstThink::None;
    int effort = 0;                    // 0 off .. 3 high (Toggle: 0 off, else on)
    std::wstring key_mask;             // sk-…9832; empty when the service needs none
    std::vector<AsstOtherView> others;
    // 语音输入.
    bool voice_set = false, voice_key = false;
    std::wstring voice_service;        // 硅基流动 / OpenAI / 自定义
    std::wstring voice_host;
    bool mcp_installed = false;        // the AI pack (bundled or installed)
};
struct AssistantPageLayout {
    D2D1_RECT_F model_section{}, model_card{}, model_foot{};
    // Every stage: a header row (icon or logo, title, caption, controls).
    D2D1_RECT_F head{}, head_badge{}, head_buttons[3]{};
    // Tiles.
    D2D1_RECT_F tiles[kAsstTiles]{};
    D2D1_RECT_F more_label{};
    std::vector<D2D1_RECT_F> more;     // 更多's chips
    // Rows under the header; zero-height when not shown.
    D2D1_RECT_F fresh_row{};
    D2D1_RECT_F key_row{}, key_field{}, key_eye{}, key_paste{}, key_link{};
    D2D1_RECT_F key_buttons[2]{};      // 更换, 移除
    D2D1_RECT_F url_row{}, url_field{}, url_buttons[2]{};   // 修改 | 保存 取消
    D2D1_RECT_F connect_row{}, connect{};
    D2D1_RECT_F model_row{}, model_choice{}, model_field{}, model_buttons[2]{};
    D2D1_RECT_F think_row{}, think_toggle{};
    D2D1_RECT_F think_seg[4]{};
    D2D1_RECT_F others_row{};
    std::vector<D2D1_RECT_F> others;
    D2D1_RECT_F voice_section{}, voice_card{}, voice_badge{}, voice_button{}, voice_row{};
    D2D1_RECT_F mcp_section{}, mcp_card{}, mcp_badge{}, mcp_button{}, mcp_meta{};
};

} // namespace pulse::ui
