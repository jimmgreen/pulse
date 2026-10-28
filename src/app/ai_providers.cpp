// ai_providers.cpp - The model services Pulse 助手 knows (ai_providers.h).
#include "ai_providers.h"

#include "ai_settings.h"

#include "../common/json_value.h"
#include "../common/localization.h"

#include <algorithm>

namespace pulse::app::ai {
namespace {

using I = l10n::StringId;

const std::vector<Provider> kProviders = {
    {"deepseek", L"https://api.deepseek.com", L"https://platform.deepseek.com/api_keys",
     ThinkStyle::DeepSeek, true, false, false},
    {"qwen", L"https://dashscope.aliyuncs.com/compatible-mode/v1", L"https://bailian.console.aliyun.com/?apiKey=1#/api-key",
     ThinkStyle::EnableThinking, true, false, false},
    {"kimi", L"https://api.moonshot.cn/v1", L"https://platform.moonshot.cn/console/api-keys",
     ThinkStyle::None, true, false, false},
    {"zhipu", L"https://open.bigmodel.cn/api/paas/v4", L"https://open.bigmodel.cn/usercenter/apikeys",
     ThinkStyle::ThinkingType, true, true, false},
    {"doubao", L"https://ark.cn-beijing.volces.com/api/v3", L"https://ark.volcengine.com/region:cn-beijing/apikey",
     ThinkStyle::ThinkingType, true, true, false},
    {"silicon", L"https://api.siliconflow.cn/v1", L"https://cloud.siliconflow.cn/account/ak",
     ThinkStyle::None, true, false, false},
    {"openai", L"https://api.openai.com/v1", L"https://platform.openai.com/api-keys",
     ThinkStyle::Effort3, true, false, false},
    {"openrouter", L"https://openrouter.ai/api/v1", L"https://openrouter.ai/keys",
     ThinkStyle::None, true, false, false},
    {"ollama", L"http://localhost:11434/v1", L"",
     ThinkStyle::None, false, false, false},
    {"custom", L"", L"",
     ThinkStyle::None, true, true, false},
    {"gemini", L"https://generativelanguage.googleapis.com/v1beta/openai", L"https://aistudio.google.com/apikey",
     ThinkStyle::None, true, false, true},
    {"claude", L"https://api.anthropic.com/v1", L"https://console.anthropic.com/settings/keys",
     ThinkStyle::None, true, true, true},
};

bool g_loaded = false;
const Provider* g_current = nullptr;

// OpenAI takes reasoning_effort only on its reasoning models (o1, o3, o4-mini,
// gpt-5…); the others refuse the request with it.
bool ReasoningModel(std::string_view model) {
    if (model.size() >= 2 && model[0] == 'o' && model[1] >= '0' && model[1] <= '9') return true;
    return model.starts_with("gpt-5");
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

void AddThinking(const Provider& p, int effort, std::string_view model, json::Value& body) {
    effort = std::clamp(effort, 0, 3);
    switch (p.think) {
    case ThinkStyle::DeepSeek: {
        json::Value thinking = json::Value::Object();
        thinking.set("type", effort == 0 ? "disabled" : "enabled");
        body.set("thinking", std::move(thinking));
        static constexpr const char* kEffort[] = {"", "low", "high", "max"};
        if (effort > 0) body.set("reasoning_effort", kEffort[effort]);
        break;
    }
    case ThinkStyle::EnableThinking:
        body.set("enable_thinking", effort > 0);
        break;
    case ThinkStyle::ThinkingType: {
        json::Value thinking = json::Value::Object();
        thinking.set("type", effort == 0 ? "disabled" : "enabled");
        body.set("thinking", std::move(thinking));
        break;
    }
    case ThinkStyle::Effort3: {
        // Reasoning models cannot switch it off: 0 asks for the least.
        static constexpr const char* kEffort[] = {"low", "low", "medium", "high"};
        if (ReasoningModel(model)) body.set("reasoning_effort", kEffort[effort]);
        break;
    }
    case ThinkStyle::None:
        break;
    }
}

std::string WithoutThinking(const Provider& p, std::string body) {
    json::Value v;
    if (!json::Parse(body, v) || !v.is_object()) return body;
    AddThinking(p, 0, v.get_string("model"), v);
    return json::Serialize(v);
}

} // namespace pulse::app::ai
