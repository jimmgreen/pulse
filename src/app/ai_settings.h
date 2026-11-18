// ai_settings.h - %LOCALAPPDATA%\Pulse\ai\ai.json: whether AI clients may
// connect over MCP, how each known client was approved, the folders where a
// client may create files without asking, and how long the activity log is
// kept. Read with the strict json_value parser; a file that does not parse
// is ignored (defaults: everything off) and never overwritten silently.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulse::app::ai {

// The broker's permission classes (ai_broker.h). Only Create may be covered
// by an always-allow rule; Change is never offered without confirmation.
enum class PermissionClass : uint8_t { Read, View, Create, Undo, Change };

enum class ClientGrant : uint8_t {
    Ask,       // not decided yet: the first connection asks
    Allowed,   // 允许
    Session,   // 只允许这次会话 (stored as Ask; kept in memory only)
    Denied,    // 拒绝
};

struct ClientRecord {
    std::wstring name;
    ClientGrant grant = ClientGrant::Ask;
};

struct AllowRule {
    std::wstring client;   // MCP client name from hello
    std::string tool;      // e.g. "pulse.create_zip"
    std::wstring folder;   // absolute; covers its subfolders
};

// 语音输入 (third batch, flow V): the speech recognition service the composer's
// microphone uploads to (OpenAI format, POST {base}/audio/transcriptions), the
// microphone and how long a pause ends a sentence. The key is in
// voice_key.bin (ai_model_key.h). Written only once it differs from the
// defaults, so an ai.json without voice input stays as it was.
struct VoicePrefs {
    std::string service;   // "siliconflow" | "openai" | "custom"; empty: no voice connection
    std::string base_url;  // https://api.siliconflow.cn/v1
    std::string model;     // FunAudioLLM/SenseVoiceSmall
    int pause = 1;         // seconds of quiet that end a sentence: 1, 2; 0 = manual
    std::string mic;       // capture endpoint id (UTF-8); empty: the system default

    bool Configured() const { return !service.empty() && !base_url.empty() && !model.empty(); }
    bool IsDefault() const { return service.empty() && base_url.empty() && model.empty() && pause == 1 && mic.empty(); }
};

// What Pulse 助手 remembers of a model service that is not the one in use
// (设置 › AI 助手 › 其他已保存的服务), so switching back keeps its model and
// address. Its key stays in its own key file (ai_model_key.h).
struct ServicePrefs {
    std::string model;
    std::string base_url;   // empty: the service's own address
};

// Pulse 助手 (ai_assistant.h): where its panel sits, the model service in use
// (ai_providers.h) and the model last used.
struct AssistantPrefs {
    bool floating = false;                    // "dock": "docked" | "floating"
    std::string service;                      // ai_providers.h id; empty: deepseek
    std::string model = "deepseek-flash";
    // "thinking": the level the user prefers, 0 off … 6 max (ai_providers.h
    // ThinkLevel); each model uses the nearest one it has. "effort" (0..3,
    // what older versions read) is still written alongside, and read when
    // "thinking" is missing.
    int thinking = 4;                         // high
    std::string base_url;                     // empty: the service's own address
    std::vector<std::pair<std::string, ServicePrefs>> services;   // "services": {id: {...}}, the others
    VoicePrefs voice;                         // "voice": {...}
    // 55 P3 历史对话: "history": false stops saving conversations;
    // "history_days" removes those not updated for that many days (pinned
    // ones stay). Written only when not the default, so older files stay.
    bool history = true;
    int history_days = 0;                     // 0: keep them all
};

// What AI apps (MCP) may do in Pulse (P5, 设置 › 增强包 › AI 与 MCP › AI
// 应用可以做的事): the switched-on groups of actions/action_policy.h McpGroup
// by key ("view", "organize", "create", "files", "external"). Without the
// key in ai.json: view and create, which is what the first batch's tools
// did (reveal; create_zip asking first). Written only once the user changed
// a switch, so older files stay as they are.
inline constexpr const char* kMcpDefaultAllow[] = {"view", "create"};

struct Settings {
    bool enabled = true; // Built-in assistant, independent of MCP access.
    bool mcp_enabled = false;
    int log_days = 30;     // 1..365
    std::vector<ClientRecord> clients;
    std::vector<AllowRule> allow;
    AssistantPrefs assistant;
    bool mcp_allow_set = false;          // "mcp_allow" is in the file
    std::vector<std::string> mcp_allow;  // only when mcp_allow_set
};

// True when the group (a McpGroup key) is switched on; "locked" never is.
bool McpAllows(const Settings& settings, std::string_view group);
// Switches a group on or off; the first change writes the defaults out too.
void SetMcpAllow(Settings& settings, std::string_view group, bool on);

std::wstring AiDirectory();          // %LOCALAPPDATA%\Pulse\ai, empty without LOCALAPPDATA
std::wstring SettingsPath();         // ...\ai.json
std::wstring ActivityDirectory();    // ...\activity

// Missing file: defaults. A file that exists but does not parse: defaults
// and *malformed = true.
Settings LoadSettings(bool* malformed = nullptr);
Settings ParseSettings(std::string_view utf8, bool* malformed = nullptr);
bool SaveSettings(const Settings& settings);
std::string SerializeSettings(const Settings& settings);

// True when a rule lets `client` run `tool` in `folder` without asking.
// Always false for any class but Create.
bool IsAlwaysAllowed(const Settings& settings, PermissionClass cls, std::wstring_view client,
                     std::string_view tool, std::wstring_view folder);

} // namespace pulse::app::ai
