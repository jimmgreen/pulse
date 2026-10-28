// model_thinking_test - thinking per model (ai_providers.h): which models
// think and how many levels each has, how the preferred level snaps to the
// ones a model has, the fields a request carries, 批量重命名's no-thinking
// body, 小米 MiMo's Token Plan address, and ai.json assistant.thinking with
// the move from the older "effort".
#include "../app/ai_providers.h"
#include "../app/ai_settings.h"
#include "../common/json_value.h"

#include <cstdio>
#include <initializer_list>
#include <string>
#include <string_view>

using namespace pulse;
using namespace pulse::app::ai;
using L = ThinkLevel;

namespace {
int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
    ok ? ++g_passed : ++g_failed;
}

ThinkSpec Spec(const char* service, std::string_view model) { return ModelThinking(ProviderById(service), model); }

bool LevelsAre(const ThinkSpec& t, std::initializer_list<L> want) {
    if (static_cast<size_t>(t.count) != want.size()) return false;
    int i = 0;
    for (L level : want)
        if (t.levels[i++] != level) return false;
    return true;
}

// The request body for `model` on `service` at the preferred level.
std::string Body(const char* service, std::string_view model, L preferred) {
    json::Value body = json::Value::Object();
    body.set("model", std::string(model));
    AddThinking(ProviderById(service), preferred, model, body);
    return json::Serialize(body);
}
bool Has(const std::string& s, std::string_view part) { return s.find(part) != std::string::npos; }
}  // namespace

int main() {
    // Every service by id, MiMo among them and on the first screen.
    Check(KnownProvider("mimo") && !ProviderById("mimo").more, "mimo is a first-screen service");
    {
        int first = 0;
        for (const Provider& p : Providers()) first += p.more ? 0 : 1;
        Check(first == 10 && Providers().size() == 13, "10 tiles + 3 under 更多");
        Check(std::string_view(Providers()[5].id) == "mimo", "mimo after 豆包");
    }

    // ---- Kinds and levels per model -----------------------------------------
    Check(LevelsAre(Spec("deepseek", "deepseek-v4-pro"), {L::Off, L::Low, L::High, L::Max}), "deepseek: 关闭/低/高/最高");
    Check(LevelsAre(Spec("deepseek", "deepseek-flash"), {L::Off, L::Low, L::High, L::Max}), "deepseek-flash: same");
    Check(LevelsAre(Spec("openai", "gpt-5.5"), {L::Off, L::Low, L::Medium, L::High, L::XHigh}), "gpt-5.5: five");
    Check(LevelsAre(Spec("openai", "gpt-5.1"), {L::Off, L::Low, L::Medium, L::High}), "gpt-5.1: four");
    Check(LevelsAre(Spec("openai", "gpt-5-mini"), {L::Minimal, L::Low, L::Medium, L::High}), "gpt-5-mini: minimal..high");
    Check(Spec("openai", "gpt-5-pro").kind == ThinkKind::Fixed, "gpt-5-pro: fixed");
    Check(Spec("openai", "gpt-4o").kind == ThinkKind::None, "gpt-4o: none");
    Check(Spec("openai", "gpt-4.1-mini").kind == ThinkKind::None, "gpt-4.1: none");
    Check(LevelsAre(Spec("openai", "o3"), {L::Low, L::Medium, L::High}), "o3: low/medium/high");
    Check(LevelsAre(Spec("kimi", "kimi-k3"), {L::Low, L::High, L::Max}) && !Spec("kimi", "kimi-k3").CanOff(),
          "kimi-k3: always thinks, three levels");
    Check(Spec("kimi", "kimi-k2.6").kind == ThinkKind::Toggle, "kimi-k2.6: toggle");
    Check(Spec("kimi", "kimi-k2.7-code").kind == ThinkKind::Fixed, "kimi-k2.7-code: fixed");
    Check(Spec("kimi", "moonshot-v1-8k").kind == ThinkKind::None, "moonshot-v1: none");
    Check(LevelsAre(Spec("zhipu", "glm-5.3"), {L::Low, L::High, L::Max}), "glm-5.3: forced, three levels");
    Check(LevelsAre(Spec("zhipu", "glm-5.2"), {L::Off, L::High, L::Max}), "glm-5.2: off/high/max");
    Check(Spec("zhipu", "glm-4.7").kind == ThinkKind::Toggle, "glm-4.7: toggle");
    Check(Spec("zhipu", "glm-4-plus").kind == ThinkKind::None, "glm-4-plus: none");
    Check(LevelsAre(Spec("doubao", "doubao-seed-1-8-251228"), {L::Off, L::Low, L::Medium, L::High}), "seed-1.8: four");
    Check(Spec("doubao", "doubao-seed-evolving").count == 7, "seed-evolving: seven");
    Check(Spec("doubao", "ep-20260101-abcde").kind == ThinkKind::None, "ark endpoint id: none");
    Check(Spec("mimo", "mimo-v2.6-pro").kind == ThinkKind::Toggle && Spec("mimo", "mimo-v2.6-pro").fallback == L::High,
          "mimo-v2.6-pro: toggle, on unasked");
    Check(Spec("mimo", "mimo-v2.6-flash").kind == ThinkKind::Toggle, "mimo-v2.6-flash: toggle");
    Check(Spec("qwen", "qwen3.6-plus").kind == ThinkKind::Toggle && Spec("qwen", "qwen3.6-plus").fallback == L::High,
          "qwen3.6-plus: toggle, on");
    Check(Spec("qwen", "qwen-plus").fallback == L::Off, "qwen-plus: toggle, off unasked");
    Check(Spec("qwen", "qwq-plus").kind == ThinkKind::Fixed, "qwq: fixed");
    Check(Spec("qwen", "qwen-max").kind == ThinkKind::None, "old qwen-max: none");
    Check(Spec("qwen", "qwen3-coder-plus").kind == ThinkKind::None, "qwen3-coder: none");
    Check(LevelsAre(Spec("qwen", "qwen3.8-max"), {L::Off, L::Low, L::Medium, L::XHigh}), "qwen3.8-max: effort");
    Check(LevelsAre(Spec("silicon", "deepseek-ai/DeepSeek-V4"), {L::Off, L::High, L::Max}), "silicon DeepSeek-V4");
    Check(Spec("silicon", "Qwen/Qwen3-8B").kind == ThinkKind::Toggle, "silicon Qwen3: toggle");
    Check(Spec("silicon", "Qwen/Qwen3-235B-A22B-Instruct-2507").kind == ThinkKind::None, "silicon Instruct: none");
    Check(Spec("silicon", "Qwen/Qwen3-235B-A22B-Thinking-2507").kind == ThinkKind::Fixed, "silicon Thinking: fixed");
    Check(Spec("ollama", "qwen3:8b").kind == ThinkKind::Toggle, "ollama qwen3: toggle");
    Check(Spec("ollama", "llama3.2").kind == ThinkKind::None, "ollama llama: none");
    Check(LevelsAre(Spec("gemini", "gemini-3-flash"), {L::Minimal, L::Low, L::Medium, L::High}), "gemini-3-flash");
    Check(Spec("claude", "claude-sonnet-4-5").kind == ThinkKind::None, "claude: none");
    Check(LevelsAre(Spec("openrouter", "openai/gpt-5.5"), {L::Off, L::Low, L::Medium, L::High, L::XHigh}) &&
              Spec("openrouter", "openai/gpt-5.5").wire == ThinkWire::Reasoning,
          "openrouter: the model's levels, reasoning field");
    Check(Spec("openrouter", "anthropic/claude-sonnet-4.5").kind == ThinkKind::None, "openrouter unknown: none");
    Check(Spec("custom", "deepseek-v4-pro").count == 4, "custom: by name");
    Check(Spec("custom", "my-model").kind == ThinkKind::None, "custom unknown: none");
    Check(Spec("deepseek", "").kind == ThinkKind::None, "no model: none");

    // ---- Snapping -------------------------------------------------------------
    const ThinkSpec ds = Spec("deepseek", "deepseek-v4-pro");
    Check(SnapThinking(ds, L::High) == L::High, "snap: same level");
    Check(SnapThinking(ds, L::Medium) == L::High, "snap: medium -> high (tie goes up)");
    Check(SnapThinking(ds, L::Minimal) == L::Low, "snap: minimal -> low, never off");
    Check(SnapThinking(ds, L::XHigh) == L::Max, "snap: xhigh -> max (tie goes up)");
    Check(SnapThinking(ds, L::Off) == L::Off, "snap: off stays off");
    const ThinkSpec k3 = Spec("kimi", "kimi-k3");
    Check(SnapThinking(k3, L::Off) == L::Low, "snap: off on an always-thinking model -> its lowest");
    Check(SnapThinking(Spec("mimo", "mimo-v2.6-pro"), L::Low) == L::High, "snap: toggle on for any level");
    Check(ThinksAt(Spec("qwen", "qwq-plus"), L::Off), "fixed thinks even at off");
    Check(!ThinksAt(Spec("openai", "gpt-4o"), L::High), "none never thinks");
    Check(!ThinksAt(ds, L::Off) && ThinksAt(ds, L::Low), "deepseek thinks unless off");

    // ---- Request fields -----------------------------------------------------
    {
        const std::string off = Body("deepseek", "deepseek-v4-pro", L::Off);
        Check(Has(off, R"("thinking":{"type":"disabled"})") && !Has(off, "reasoning_effort"), "deepseek off: disabled");
        const std::string hi = Body("deepseek", "deepseek-v4-pro", L::Medium);
        Check(Has(hi, R"("type":"enabled")") && Has(hi, R"("reasoning_effort":"high")"), "deepseek medium: high");
        Check(Has(Body("deepseek", "deepseek-flash", L::Max), R"("reasoning_effort":"max")"), "deepseek max");
        Check(Body("openai", "gpt-4o", L::High) == R"({"model":"gpt-4o"})", "gpt-4o: nothing sent");
        Check(Has(Body("openai", "gpt-5.5", L::Off), R"("reasoning_effort":"none")"), "gpt-5.5 off: none");
        Check(Has(Body("openai", "gpt-5-mini", L::Off), R"("reasoning_effort":"minimal")"), "gpt-5-mini off: minimal");
        Check(Has(Body("openai", "gpt-5.5", L::XHigh), R"("reasoning_effort":"xhigh")"), "gpt-5.5 xhigh");
        Check(Has(Body("doubao", "doubao-seed-1-8-251228", L::Off), R"("reasoning_effort":"minimal")"),
              "seed off: minimal");
        Check(Has(Body("doubao", "doubao-seed-1-8-251228", L::Minimal), R"("reasoning_effort":"low")"),
              "seed minimal: low");
        const std::string mimo_off = Body("mimo", "mimo-v2.6-pro", L::Off);
        const std::string mimo_on = Body("mimo", "mimo-v2.6-pro", L::Medium);
        Check(Has(mimo_off, R"("thinking":{"type":"disabled"})"), "mimo off: thinking disabled");
        Check(Has(mimo_on, R"("thinking":{"type":"enabled"})") && !Has(mimo_on, "reasoning_effort"),
              "mimo on: enabled, no effort");
        Check(Has(Body("qwen", "qwen3.6-plus", L::Off), R"("enable_thinking":false)"), "qwen off: enable_thinking false");
        Check(Has(Body("qwen", "qwen3.6-plus", L::Low), R"("enable_thinking":true)"), "qwen on: enable_thinking true");
        Check(Body("qwen", "qwq-plus", L::Off) == R"({"model":"qwq-plus"})", "qwq: nothing sent");
        const std::string k3b = Body("kimi", "kimi-k3", L::Off);
        Check(Has(k3b, R"("reasoning_effort":"low")") && !Has(k3b, "thinking"), "kimi-k3 off: low, no thinking field");
        const std::string glm = Body("zhipu", "glm-4.7", L::Off);
        Check(Has(glm, R"("thinking":{"type":"disabled"})"), "glm-4.7 off: disabled");
        Check(Has(Body("silicon", "deepseek-ai/DeepSeek-V4", L::XHigh), R"("reasoning_effort":"max")") &&
                  Has(Body("silicon", "deepseek-ai/DeepSeek-V4", L::XHigh), R"("enable_thinking":true)"),
              "silicon V4 xhigh: max + enable_thinking");
        Check(Has(Body("ollama", "qwen3:8b", L::Off), R"("reasoning_effort":"none")"), "ollama off: none");
        Check(Has(Body("openrouter", "openai/gpt-5.5", L::High), R"("reasoning":{"effort":"high"})"),
              "openrouter: reasoning.effort");
        Check(Has(Body("openrouter", "deepseek/deepseek-v4-pro", L::Off), R"("reasoning":{"enabled":false})"),
              "openrouter off: reasoning.enabled false");
        Check(Body("claude", "claude-sonnet-4-5", L::High) == R"({"model":"claude-sonnet-4-5"})", "claude: nothing");
    }

    // ---- 批量重命名: thinking off, or as low as the model goes ---------------
    {
        const std::string ds_body = WithoutThinking(ProviderById("deepseek"), R"({"model":"deepseek-flash"})");
        Check(Has(ds_body, R"("type":"disabled")"), "without thinking: deepseek disabled");
        const std::string k3_body = WithoutThinking(ProviderById("kimi"), R"({"model":"kimi-k3"})");
        Check(Has(k3_body, R"("reasoning_effort":"low")"), "without thinking: kimi-k3 lowest");
        const std::string plain = R"({"model": "gpt-4o"})";
        Check(WithoutThinking(ProviderById("openai"), plain) == plain, "without thinking: none untouched");
        Check(WithoutThinking(ProviderById("deepseek"), "not json") == "not json", "without thinking: bad body kept");
    }

    // ---- 小米 MiMo Token Plan --------------------------------------------------
    {
        const Provider& mimo = ProviderById("mimo");
        const std::wstring base = mimo.base_url;
        const std::wstring plan = L"https://token-plan-cn.xiaomimimo.com/v1";
        Check(AddressForKey(mimo, base, L"tp-abc") == plan, "mimo: tp- key -> token plan host");
        Check(AddressForKey(mimo, plan, L"sk-abc") == base, "mimo: sk- key -> usual host");
        Check(AddressForKey(mimo, L"https://relay.example/v1", L"tp-abc") == L"https://relay.example/v1",
              "mimo: a typed address is kept");
        Check(AddressForKey(ProviderById("deepseek"), L"https://api.deepseek.com", L"tp-x") == L"https://api.deepseek.com",
              "other services: unchanged");
    }

    // ---- ai.json assistant.thinking --------------------------------------------
    {
        Check(ParseSettings(R"({"assistant":{"model":"deepseek-flash"}})").assistant.thinking == 4, "default: high");
        Check(ParseSettings(R"({"assistant":{"model":"deepseek-flash","effort":0}})").assistant.thinking == 0,
              "effort 0 -> off");
        Check(ParseSettings(R"({"assistant":{"effort":2}})").assistant.thinking == 4, "deepseek effort 2 -> high");
        Check(ParseSettings(R"({"assistant":{"effort":3}})").assistant.thinking == 6, "deepseek effort 3 -> max");
        Check(ParseSettings(R"({"assistant":{"service":"qwen","effort":2}})").assistant.thinking == 3,
              "other effort 2 -> medium");
        Check(ParseSettings(R"({"assistant":{"thinking":"xhigh","effort":0}})").assistant.thinking == 5,
              "thinking wins over effort");
        Check(ParseSettings(R"({"assistant":{"thinking":"bogus","effort":1}})").assistant.thinking == 2,
              "bad thinking falls back to effort");
        Settings s = ParseSettings(R"({"assistant":{"model":"deepseek-flash","effort":0}})");
        const std::string saved = SerializeSettings(s);
        Check(Has(saved, R"("thinking":"off")") && Has(saved, R"("effort":0)"), "saved: thinking + effort");
        s.assistant.thinking = 5;
        const std::string xhigh = SerializeSettings(s);
        Check(Has(xhigh, R"("thinking":"xhigh")") && Has(xhigh, R"("effort":3)"), "saved: xhigh -> deepseek effort 3");
        Check(ParseSettings(xhigh).assistant.thinking == 5, "round trip");
    }

    std::printf("%d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
