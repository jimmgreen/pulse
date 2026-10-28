// ai_providers.h - The model services Pulse 助手 knows by name (设置 › AI
// 助手 › 模型服务): each one's address, the page where its keys are made and
// how it takes the thinking setting. They all speak the OpenAI chat format; a
// custom service is any other address that does.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::json { class Value; }

namespace pulse::app::ai {

// How a service takes 思考 (ai.json assistant.effort, 0..3).
enum class ThinkStyle : uint8_t {
    None,            // nothing is sent and the setting is hidden
    DeepSeek,        // thinking.type + reasoning_effort low/high/max: off, low, medium, high
    EnableThinking,  // enable_thinking: off (0) or on
    ThinkingType,    // thinking.type disabled/enabled: off (0) or on
    Effort3,         // reasoning_effort low/medium/high (1..3), for reasoning models only
};

struct Provider {
    const char* id;            // ai.json assistant.service ("deepseek" is the default)
    const wchar_t* base_url;   // empty: typed by the user (custom)
    const wchar_t* key_url;    // where its keys are made; empty: none
    ThinkStyle think;
    bool needs_key;            // false: a local service (Ollama)
    bool partial_list;         // GET /models can leave out ids the service takes
    bool more;                 // listed under 更多 rather than on the first screen
};

// The services in tile order: the first screen, then 更多.
const std::vector<Provider>& Providers();
// The service with this id; an unknown or empty id is DeepSeek.
const Provider& ProviderById(std::string_view id);
bool KnownProvider(std::string_view id);
bool IsDefaultProvider(const Provider& p);   // DeepSeek
bool IsCustomProvider(const Provider& p);
std::wstring ProviderName(const Provider& p);   // DeepSeek, 通义千问, 自定义 …
std::wstring ProviderSub(const Provider& p);    // 深度求索, 阿里云百炼 …

// The service in use (ai.json assistant.service), read once and cached;
// ProvidersChanged() drops the cache after the settings change.
const Provider& CurrentProvider();
void ProvidersChanged();

// The key stored for a service that takes none (Ollama, or a custom address
// left without one): sent as the bearer token, which such services ignore.
inline constexpr wchar_t kNoKeyPlaceholder[] = L"none";

// Adds the thinking setting in the form p takes to a chat request body.
void AddThinking(const Provider& p, int effort, std::string_view model, json::Value& body);
// A serialized chat request with thinking switched off (or as low as p goes),
// for the quick structured asks (批量重命名). Unchanged when it does not parse.
std::string WithoutThinking(const Provider& p, std::string body);

} // namespace pulse::app::ai
