// ai_providers.cpp - The model services Pulse 助手 knows (ai_providers.h).
#include "ai_providers.h"

#include "ai_settings.h"

#include "../common/json_value.h"
#include "../common/localization.h"

#include <algorithm>
#include <cstdlib>
#include <initializer_list>

namespace pulse::app::ai {
namespace {

using I = l10n::StringId;

const std::vector<Provider> kProviders = {
    {"deepseek", L"https://api.deepseek.com", L"https://platform.deepseek.com/api_keys", true, false, false},
    {"qwen", L"https://dashscope.aliyuncs.com/compatible-mode/v1", L"https://bailian.console.aliyun.com/?apiKey=1#/api-key",
     true, false, false},
    {"kimi", L"https://api.moonshot.cn/v1", L"https://platform.moonshot.cn/console/api-keys", true, false, false},
    {"zhipu", L"https://open.bigmodel.cn/api/paas/v4", L"https://open.bigmodel.cn/usercenter/apikeys", true, true, false},
    {"doubao", L"https://ark.cn-beijing.volces.com/api/v3", L"https://ark.volcengine.com/region:cn-beijing/apikey",
     true, true, false},
    {"mimo", L"https://api.xiaomimimo.com/v1", L"https://platform.xiaomimimo.com/#/console/api-keys", true, false, false},
    {"silicon", L"https://api.siliconflow.cn/v1", L"https://cloud.siliconflow.cn/account/ak", true, false, false},
    {"openai", L"https://api.openai.com/v1", L"https://platform.openai.com/api-keys", true, false, false},
    {"ollama", L"http://localhost:11434/v1", L"", false, false, false},
    {"custom", L"", L"", true, true, false},
    {"openrouter", L"https://openrouter.ai/api/v1", L"https://openrouter.ai/keys", true, false, true},
    {"gemini", L"https://generativelanguage.googleapis.com/v1beta/openai", L"https://aistudio.google.com/apikey",
     true, false, true},
    {"claude", L"https://api.anthropic.com/v1", L"https://console.anthropic.com/settings/keys", true, true, true},
};

// 小米 MiMo's Token Plan (tp- keys) has its own host.
constexpr wchar_t kMimoTokenPlan[] = L"https://token-plan-cn.xiaomimimo.com/v1";

bool g_loaded = false;
const Provider* g_current = nullptr;

// ---- Thinking per model (plans/ai/49_各模型思考档位.md) ----------------------

using L = ThinkLevel;

ThinkSpec Make(ThinkKind kind, ThinkWire wire, std::initializer_list<L> levels, L fallback,
               const char* off_value = nullptr) {
    ThinkSpec t;
    t.kind = kind;
    t.wire = wire;
    for (L level : levels)
        if (t.count < kThinkLevels) t.levels[t.count++] = level;
    t.fallback = fallback;
    t.off_value = off_value;
    return t;
}
ThinkSpec Levels(ThinkWire wire, std::initializer_list<L> levels, L fallback, const char* off_value = nullptr) {
    return Make(ThinkKind::Levels, wire, levels, fallback, off_value);
}
ThinkSpec Toggle(ThinkWire wire, bool on_unasked, const char* off_value = nullptr) {
    return Make(ThinkKind::Toggle, wire, {L::Off, L::High}, on_unasked ? L::High : L::Off, off_value);
}
ThinkSpec Fixed() {
    ThinkSpec t;
    t.kind = ThinkKind::Fixed;
    return t;
}
ThinkSpec NoThinking() { return {}; }

bool Starts(std::string_view s, std::string_view prefix) { return s.substr(0, prefix.size()) == prefix; }
bool Has(std::string_view s, std::string_view part) { return s.find(part) != std::string_view::npos; }
template <size_t N>
bool StartsAny(std::string_view s, const char* const (&prefixes)[N]) {
    for (const char* prefix : prefixes)
        if (Starts(s, prefix)) return true;
    return false;
}

// The model's own name, lower case: hubs prefix the maker (deepseek-ai/…,
// openai/…, models/…).
std::string Tail(std::string_view model) {
    if (const size_t slash = model.rfind('/'); slash != std::string_view::npos) model.remove_prefix(slash + 1);
    std::string out(model);
    for (char& ch : out) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    return out;
}

// Hybrid models the hubs switch with enable_thinking (or Ollama's effort).
constexpr const char* kHybrids[] = {"qwen3", "deepseek-v3.1", "deepseek-v3.2", "deepseek-v4", "glm-4.5", "glm-4.6",
                                    "glm-4.7", "glm-5", "kimi-k2.5", "kimi-k2.6"};

// DeepSeek: 关闭 / 低 / 高 / 最高 (medium and xhigh are folded into high).
ThinkSpec DeepSeekModel(std::string_view m) {
    if (Starts(m, "deepseek-r1")) return Fixed();
    return Levels(ThinkWire::ThinkingType, {L::Off, L::Low, L::High, L::Max}, L::High);
}

ThinkSpec OpenAiModel(std::string_view m) {
    if (Has(m, "codex"))
        return Has(m, "max") ? Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High, L::XHigh}, L::Medium)
                             : Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High}, L::Medium);
    if (Starts(m, "gpt-5")) {
        if (Has(m, "chat")) return NoThinking();
        if (Has(m, "-pro"))
            return Starts(m, "gpt-5-pro") ? Fixed() : Levels(ThinkWire::Effort, {L::Medium, L::High, L::XHigh}, L::High);
        if (m.size() > 6 && m[5] == '.') {
            int minor = 0;
            for (size_t i = 6; i < m.size() && m[i] >= '0' && m[i] <= '9' && minor < 100; ++i) minor = minor * 10 + (m[i] - '0');
            if (minor <= 1) return Levels(ThinkWire::Effort, {L::Off, L::Low, L::Medium, L::High}, L::Off, "none");
            return Levels(ThinkWire::Effort, {L::Off, L::Low, L::Medium, L::High, L::XHigh}, minor >= 5 ? L::Medium : L::Off,
                          "none");
        }
        return Levels(ThinkWire::Effort, {L::Minimal, L::Low, L::Medium, L::High}, L::Medium);
    }
    if (m.size() >= 2 && m[0] == 'o' && m[1] >= '0' && m[1] <= '9') {
        if (Starts(m, "o1-mini") || Starts(m, "o1-preview")) return Fixed();
        return Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High}, L::Medium);
    }
    return NoThinking();
}

ThinkSpec KimiModel(std::string_view m) {
    if (Starts(m, "kimi-k3")) return Levels(ThinkWire::Effort, {L::Low, L::High, L::Max}, L::Max);
    if (Has(m, "k2.7-code") || Has(m, "thinking")) return Fixed();
    if (Starts(m, "kimi-k2.6") || Starts(m, "kimi-k2.5")) return Toggle(ThinkWire::ThinkingType, true);
    return NoThinking();
}

ThinkSpec ZhipuModel(std::string_view m) {
    if (Starts(m, "glm-5.3")) return Levels(ThinkWire::Effort, {L::Low, L::High, L::Max}, L::Max);
    if (Starts(m, "glm-5.2")) return Levels(ThinkWire::ThinkingType, {L::Off, L::High, L::Max}, L::High);
    if (Starts(m, "glm-5") || Starts(m, "glm-4.5") || Starts(m, "glm-4.6") || Starts(m, "glm-4.7"))
        return Toggle(ThinkWire::ThinkingType, true);
    return NoThinking();
}

ThinkSpec DoubaoModel(std::string_view m) {
    if (Has(m, "seed-evolving"))
        return Levels(ThinkWire::Effort, {L::Off, L::Minimal, L::Low, L::Medium, L::High, L::XHigh, L::Max}, L::High,
                      "none");
    if (Has(m, "thinking")) return Fixed();
    if (Starts(m, "doubao-seed-1-6-flash") || Starts(m, "doubao-seed-1-6-250615") || m == "doubao-seed-1-6")
        return Toggle(ThinkWire::ThinkingType, true);
    if (Starts(m, "doubao-seed-1-6") || Starts(m, "doubao-seed-1-8") || Starts(m, "doubao-seed-2"))
        return Levels(ThinkWire::Effort, {L::Off, L::Low, L::Medium, L::High}, L::Medium, "minimal");
    if (Starts(m, "deepseek-v3-1") || Starts(m, "deepseek-v3-2") || Starts(m, "deepseek-v4"))
        return Toggle(ThinkWire::ThinkingType, true);
    return NoThinking();   // older doubao models and ep- endpoints refuse the field
}

ThinkSpec MimoModel(std::string_view m) {
    return Starts(m, "mimo") ? Toggle(ThinkWire::ThinkingType, true) : NoThinking();
}

ThinkSpec GeminiModel(std::string_view m) {
    if (Starts(m, "gemini-2.5-flash"))
        return Levels(ThinkWire::Effort, {L::Off, L::Low, L::Medium, L::High}, L::Medium, "none");
    if (Starts(m, "gemini-2.5-pro")) return Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High}, L::High);
    if (Starts(m, "gemini-3")) {
        if (Has(m, "flash")) return Levels(ThinkWire::Effort, {L::Minimal, L::Low, L::Medium, L::High}, L::High);
        if (Starts(m, "gemini-3-pro")) return Levels(ThinkWire::Effort, {L::Low, L::High}, L::High);
        if (Has(m, "pro")) return Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High}, L::High);
    }
    return NoThinking();
}

// 阿里云百炼: enable_thinking on its hybrids and the ones it hosts.
ThinkSpec QwenModel(std::string_view m) {
    if (Has(m, "qwq") || Has(m, "thinking") || Starts(m, "deepseek-r1") || Starts(m, "qwen3.7-max-preview") ||
        Starts(m, "qwen3.8-2.4t") || Starts(m, "kimi-k3") || Starts(m, "kimi-k2.7") || Starts(m, "glm-5.3"))
        return Fixed();   // false would be refused
    if (Has(m, "instruct") || Has(m, "coder") || Starts(m, "qwen2") || Starts(m, "qwen-long") || Starts(m, "qwen-vl") ||
        m == "qwen-max" || Starts(m, "qwen-max-"))
        return NoThinking();
    if (Starts(m, "qwen3.8-max"))
        return Levels(ThinkWire::EnableThinking, {L::Off, L::Low, L::Medium, L::XHigh}, L::XHigh);
    if (Starts(m, "qwen-plus") || Starts(m, "qwen-flash") || Starts(m, "qwen-turbo"))
        return Toggle(ThinkWire::EnableThinking, false);
    if (StartsAny(m, kHybrids)) return Toggle(ThinkWire::EnableThinking, true);
    return NoThinking();
}

// 硅基流动: enable_thinking, and reasoning_effort high/max on DeepSeek-V4 and GLM-5.2.
ThinkSpec SiliconModel(std::string_view m) {
    if (Starts(m, "deepseek-v4") || Starts(m, "glm-5.2"))
        return Levels(ThinkWire::EnableThinking, {L::Off, L::High, L::Max}, L::High);
    if (Has(m, "thinking") || Starts(m, "deepseek-r1") || Starts(m, "qwq")) return Fixed();
    if (Has(m, "instruct") || Has(m, "coder")) return NoThinking();
    if (StartsAny(m, kHybrids)) return Toggle(ThinkWire::EnableThinking, true);
    return NoThinking();
}

// Ollama's /v1 takes reasoning_effort (none switches a hybrid off).
ThinkSpec OllamaModel(std::string_view m) {
    if (Starts(m, "gpt-oss")) return Levels(ThinkWire::Effort, {L::Low, L::Medium, L::High}, L::Medium);
    if (Starts(m, "deepseek-r1") || Starts(m, "qwq") || Has(m, "thinking")) return Fixed();
    if (Starts(m, "qwen3") || Starts(m, "deepseek-v3.1")) return Toggle(ThinkWire::Effort, true, "none");
    return NoThinking();
}

// A custom address (a relay) or OpenRouter: by the maker the name shows.
ThinkSpec ByName(std::string_view m) {
    if (Starts(m, "deepseek")) return DeepSeekModel(m);
    if (Starts(m, "gpt-") || (m.size() >= 2 && m[0] == 'o' && m[1] >= '0' && m[1] <= '9')) return OpenAiModel(m);
    if (Starts(m, "kimi")) return KimiModel(m);
    if (Starts(m, "glm")) return ZhipuModel(m);
    if (Starts(m, "qwen") || Starts(m, "qwq")) return QwenModel(m);
    if (Starts(m, "doubao")) return DoubaoModel(m);
    if (Starts(m, "mimo")) return MimoModel(m);
    if (Starts(m, "gemini")) return GeminiModel(m);
    return NoThinking();
}

} // namespace

const std::vector<Provider>& Providers() { return kProviders; }

const Provider& ProviderById(std::string_view id) {
    for (const Provider& p : kProviders)
        if (id == p.id) return p;
    return kProviders.front();
}

bool KnownProvider(std::string_view id) {
    return std::any_of(kProviders.begin(), kProviders.end(), [&](const Provider& p) { return id == p.id; });
}

bool IsDefaultProvider(const Provider& p) { return &p == &kProviders.front(); }
bool IsCustomProvider(const Provider& p) { return std::string_view(p.id) == "custom"; }

std::wstring ProviderName(const Provider& p) {
    const std::string_view id = p.id;
    if (id == "deepseek") return L"DeepSeek";
    if (id == "qwen") return l10n::Get(I::ProvQwen);
    if (id == "kimi") return L"Kimi";
    if (id == "zhipu") return l10n::Get(I::ProvZhipu);
    if (id == "doubao") return l10n::Get(I::ProvDoubao);
    if (id == "mimo") return l10n::Get(I::ProvMimo);
    if (id == "silicon") return l10n::Get(I::ProvSilicon);
    if (id == "openai") return L"OpenAI";
    if (id == "openrouter") return L"OpenRouter";
    if (id == "ollama") return L"Ollama";
    if (id == "gemini") return L"Gemini";
    if (id == "claude") return L"Claude";
    return l10n::Get(I::ProvCustom);
}

std::wstring ProviderSub(const Provider& p) {
    const std::string_view id = p.id;
    if (id == "deepseek") return l10n::Get(I::ProvSubDeepseek);
    if (id == "qwen") return l10n::Get(I::ProvSubQwen);
    if (id == "kimi") return l10n::Get(I::ProvSubKimi);
    if (id == "zhipu") return l10n::Get(I::ProvSubZhipu);
    if (id == "doubao") return l10n::Get(I::ProvSubDoubao);
    if (id == "mimo") return l10n::Get(I::ProvSubMimo);
    if (id == "silicon") return L"SiliconCloud";
    if (id == "openai" || id == "gemini" || id == "claude") return l10n::Get(I::ProvSubProxy);
    if (id == "openrouter") return l10n::Get(I::ProvSubOpenRouter);
    if (id == "ollama") return l10n::Get(I::ProvSubOllama);
    return l10n::Get(I::ProvSubCustom);
}

const Provider& CurrentProvider() {
    if (!g_loaded) {
        g_loaded = true;
        g_current = &ProviderById(LoadSettings().assistant.service);
    }
    return g_current ? *g_current : kProviders.front();
}

void ProvidersChanged() { g_loaded = false; }

int ThinkSpec::IndexOf(ThinkLevel level) const {
    for (int i = 0; i < count; ++i)
        if (levels[i] == level) return i;
    return -1;
}

ThinkSpec ModelThinking(const Provider& p, std::string_view model) {
    const std::string m = Tail(model);
    if (m.empty()) return NoThinking();
    const std::string_view id = p.id;
    if (id == "deepseek") return DeepSeekModel(m);
    if (id == "qwen") return QwenModel(m);
    if (id == "kimi") return KimiModel(m);
    if (id == "zhipu") return ZhipuModel(m);
    if (id == "doubao") return DoubaoModel(m);
    if (id == "mimo") return MimoModel(m);
    if (id == "silicon") return SiliconModel(m);
    if (id == "openai") return OpenAiModel(m);
    if (id == "ollama") return OllamaModel(m);
    if (id == "gemini") return GeminiModel(m);
    if (id == "openrouter") {
        ThinkSpec t = ByName(m);
        if (t.Adjustable()) {
            t.wire = ThinkWire::Reasoning;
            t.off_value = nullptr;
        }
        return t;
    }
    if (id == "custom") return ByName(m);
    return NoThinking();   // Claude's compatible API ignores it
}

ThinkLevel SnapThinking(const ThinkSpec& spec, ThinkLevel preferred) {
    if (spec.count == 0) return preferred;
    if (spec.IndexOf(preferred) >= 0) return preferred;
    const int want = static_cast<int>(preferred);
    int best = -1, best_d = 0;
    for (int i = 0; i < spec.count; ++i) {
        const ThinkLevel level = spec.levels[i];
        if (level == ThinkLevel::Off) continue;   // only Off itself snaps to Off
        const int d = std::abs(static_cast<int>(level) - want);
        if (best < 0 || d < best_d || (d == best_d && level > spec.levels[best])) {
            best = i;
            best_d = d;
        }
    }
    return best < 0 ? spec.levels[0] : spec.levels[best];
}

bool ThinksAt(const ThinkSpec& spec, ThinkLevel preferred) {
    if (spec.kind == ThinkKind::Fixed) return true;
    return spec.Adjustable() && SnapThinking(spec, preferred) != ThinkLevel::Off;
}

const char* ThinkLevelName(ThinkLevel level) {
    static constexpr const char* kNames[kThinkLevels] = {"off", "minimal", "low", "medium", "high", "xhigh", "max"};
    const int i = static_cast<int>(level);
    return i >= 0 && i < kThinkLevels ? kNames[i] : "high";
}

bool ParseThinkLevel(std::string_view name, ThinkLevel& out) {
    for (int i = 0; i < kThinkLevels; ++i)
        if (name == ThinkLevelName(static_cast<ThinkLevel>(i))) {
            out = static_cast<ThinkLevel>(i);
            return true;
        }
    return false;
}

std::wstring ThinkLevelLabel(ThinkLevel level) {
    static constexpr I kLabels[kThinkLevels] = {I::AiAsstEffortOff,  I::AiAsstEffortMinimal, I::AiAsstEffortLow,
                                                I::AiAsstEffortMid,  I::AiAsstEffortHigh,    I::AiAsstEffortXHigh,
                                                I::AiAsstEffortMax};
    return l10n::Get(kLabels[std::clamp(static_cast<int>(level), 0, kThinkLevels - 1)]);
}

void AddThinking(const Provider& p, ThinkLevel preferred, std::string_view model, json::Value& body) {
    const ThinkSpec spec = ModelThinking(p, model);
    if (!spec.Adjustable()) return;   // None and Fixed take nothing
    const ThinkLevel level = SnapThinking(spec, preferred);
    const bool off = level == ThinkLevel::Off;
    const bool effort = spec.kind == ThinkKind::Levels;
    const char* value = ThinkLevelName(level);
    switch (spec.wire) {
    case ThinkWire::ThinkingType: {
        json::Value thinking = json::Value::Object();
        thinking.set("type", off ? "disabled" : "enabled");
        body.set("thinking", std::move(thinking));
        if (!off && effort) body.set("reasoning_effort", value);
        break;
    }
    case ThinkWire::EnableThinking:
        body.set("enable_thinking", !off);
        if (!off && effort) body.set("reasoning_effort", value);
        break;
    case ThinkWire::Effort:
        body.set("reasoning_effort", off ? (spec.off_value ? spec.off_value : "none") : value);
        break;
    case ThinkWire::Reasoning: {
        json::Value reasoning = json::Value::Object();
        if (off) reasoning.set("enabled", false);
        else if (effort) reasoning.set("effort", value);
        else reasoning.set("enabled", true);
        body.set("reasoning", std::move(reasoning));
        break;
    }
    case ThinkWire::None:
        break;
    }
}

std::string WithoutThinking(const Provider& p, std::string body) {
    json::Value v;
    if (!json::Parse(body, v) || !v.is_object()) return body;
    if (!ModelThinking(p, v.get_string("model")).Adjustable()) return body;
    AddThinking(p, ThinkLevel::Off, v.get_string("model"), v);   // Off, or the model's lowest
    return json::Serialize(v);
}

std::wstring AddressForKey(const Provider& p, const std::wstring& url, std::wstring_view key) {
    if (std::string_view(p.id) != "mimo") return url;
    const bool token_plan = key.substr(0, 3) == L"tp-";
    if (token_plan && url == p.base_url) return kMimoTokenPlan;
    if (!token_plan && url == kMimoTokenPlan) return p.base_url;
    return url;
}

} // namespace pulse::app::ai
