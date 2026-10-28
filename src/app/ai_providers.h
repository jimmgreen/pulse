// ai_providers.h - The model services Pulse 助手 knows by name (设置 › AI
// 助手 › 模型服务): each one's address and the page where its keys are made,
// and the thinking each model takes. They all speak the OpenAI chat format; a
// custom service is any other address that does.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::json { class Value; }

namespace pulse::app::ai {

// 思考 is per model, not per service: how many levels a model has, whether
// it can stop thinking and which field takes it all differ (plans/ai/
// 49_各模型思考档位.md). Pulse keeps one level the user prefers (ai.json
// assistant.thinking) and each model uses the nearest one it has.
enum class ThinkLevel : uint8_t { Off, Minimal, Low, Medium, High, XHigh, Max };
inline constexpr int kThinkLevels = 7;

enum class ThinkKind : uint8_t {
    None,     // no thinking: nothing is sent, the setting is hidden
    Fixed,    // always thinks and takes no setting: nothing is sent
    Toggle,   // on or off (levels Off, High)
    Levels,   // 2..7 levels, with Off when it can stop thinking
};

// The field a request carries the level in.
enum class ThinkWire : uint8_t {
    None,
    ThinkingType,     // thinking.type disabled/enabled, + reasoning_effort for Levels
    EnableThinking,   // enable_thinking false/true, + reasoning_effort for Levels
    Effort,           // reasoning_effort only; Off sends off_value (none if null)
    Reasoning,        // OpenRouter: reasoning.effort, Off reasoning.enabled false
};

struct ThinkSpec {
    ThinkKind kind = ThinkKind::None;
    ThinkWire wire = ThinkWire::None;
    uint8_t count = 0;                    // levels, ascending
    ThinkLevel levels[kThinkLevels]{};
    ThinkLevel fallback = ThinkLevel::High;   // what the service does unasked (↺)
    const char* off_value = nullptr;      // Effort: the value that means Off
    bool CanOff() const { return count > 0 && levels[0] == ThinkLevel::Off; }
    bool Adjustable() const { return kind == ThinkKind::Toggle || kind == ThinkKind::Levels; }
    int IndexOf(ThinkLevel level) const;  // -1 when not a level of this model
};

struct Provider {
    const char* id;            // ai.json assistant.service ("deepseek" is the default)
    const wchar_t* base_url;   // empty: typed by the user (custom)
    const wchar_t* key_url;    // where its keys are made; empty: none
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

// The thinking `model` has on service p. A custom service, OpenRouter and the
// hubs (百炼, 硅基流动, Ollama) go by the model's name; an id nothing matches
// gets None, so no field is sent that the service could refuse.
ThinkSpec ModelThinking(const Provider& p, std::string_view model);
// The level of `spec` nearest the preference: the same one, else the closer
// neighbour (the higher on a tie), never Off unless Off is preferred, and the
// lowest when Off is preferred but the model cannot stop thinking.
ThinkLevel SnapThinking(const ThinkSpec& spec, ThinkLevel preferred);
// Whether a request with this preference thinks (Fixed always does).
bool ThinksAt(const ThinkSpec& spec, ThinkLevel preferred);

// ai.json assistant.thinking: off, minimal, low, medium, high, xhigh, max.
const char* ThinkLevelName(ThinkLevel level);
bool ParseThinkLevel(std::string_view name, ThinkLevel& out);
// 关闭, 最低, 低, 中, 高, 超高, 最高.
std::wstring ThinkLevelLabel(ThinkLevel level);

// Adds the preferred level, snapped to the model, to a chat request body in
// the field the model takes; nothing for None and Fixed.
void AddThinking(const Provider& p, ThinkLevel preferred, std::string_view model, json::Value& body);
// A serialized chat request with thinking switched off (or as low as the
// model goes), for the quick structured asks (批量重命名). Unchanged when it
// does not parse.
std::string WithoutThinking(const Provider& p, std::string body);

// The address to connect with for this key: 小米 MiMo's Token Plan keys
// (tp-) take their own host; an sk- key goes back to the usual one. Any other
// service, or an address the user typed, is kept.
std::wstring AddressForKey(const Provider& p, const std::wstring& url, std::wstring_view key);

} // namespace pulse::app::ai
