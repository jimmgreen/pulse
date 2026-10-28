#include "ai_assistant_settings.h"
#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_conn.h"
#include "ai_assistant_voice.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_model_stream.h"
#include "ai_pack.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/settings_page_ids.h"
#include "../ui/ui_view_model.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse {
namespace {

namespace ai = app::ai;

// 上次连接: this run's last check, from any window.
std::mutex g_lock;
ui::AsstConn g_conn = ui::AsstConn::None;
std::wstring* g_conn_time = nullptr;   // HH:MM; never freed (probe threads may outlive windows)
std::atomic<bool> g_testing{false};
bool g_seen = false;                   // the page was opened in this run: no more 新

std::wstring NowText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u", static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute));
    return text;
}

// ai.json and the pack state, read at most every 2 s.
struct Cached {
    ULONGLONG at = 0;
    ui::AssistantPageView view;
};

void Refresh(ui::AssistantPageView& v) {
    bool malformed = false;
    const ai::Settings settings = ai::LoadSettings(&malformed);
    const std::wstring base = AiAgentBaseUrl();
    v.key_saved = ai::HasModelKey();
    v.official = base == AiAgentDefaultBaseUrl();
    v.host = ai::ModelHost(base);
    v.model = json::Wide(settings.assistant.model);
    v.model_count = ai::ListedModels(base).size();
    const ai::VoicePrefs& voice = settings.assistant.voice;
    v.voice_set = voice.Configured();
    v.voice_key = v.voice_set && ai::HasVoiceKey();
    if (voice.service == "siliconflow") v.voice_service = l10n::Get(l10n::StringId::AsstVoiceSiliconFlow);
    else if (voice.service == "openai") v.voice_service = L"OpenAI";
    else v.voice_service = l10n::Get(l10n::StringId::AsstVoiceCustom);
    v.voice_host = v.voice_set ? ai::ModelHost(json::Wide(voice.base_url)) : std::wstring{};
    v.mcp_installed = app::AiPackReady(app::QueryAiPackState());
}

void StartTest(AppState& s) {
    if (g_testing.exchange(true)) return;
    ai::ModelEndpoint endpoint;
    endpoint.base_url = AiAgentBaseUrl();
    if (!ai::LoadModelKey(endpoint.key) || endpoint.key.empty()) {
        g_testing.store(false);
        return;
    }
    const HWND hwnd = s.hwnd;
    try {
        std::thread([hwnd, endpoint = std::move(endpoint)]() mutable {
            const std::wstring url = endpoint.base_url;
            const ai::ModelProbe result = ai::ProbeModelService(std::move(endpoint));   // wipes the key
            const bool ok = result.http_status == 200;
            if (ok) ai::StoreListedModels(url, result.models);
            NoteAssistantConnection(ok);
            g_testing.store(false);
            if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        }).detach();
    } catch (...) {
        g_testing.store(false);
    }
}

Cached& Cache() {
    static auto* cache = new Cached;
    return *cache;
}

} // namespace

void NoteAssistantConnection(bool ok) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_conn = ok ? ui::AsstConn::Ok : ui::AsstConn::Failed;
    if (!g_conn_time) g_conn_time = new std::wstring;
    *g_conn_time = NowText();
}

void FillAssistantSettingsVm(AppState& s, ui::WindowViewModel& vm) {
    (void)s;
    if (vm.settings_page == ui::kSettingsPageAssistant) g_seen = true;
    vm.settings_assistant_new = !g_seen;
    if (vm.settings_page != ui::kSettingsPageAssistant) return;
    Cached& cache = Cache();
    const ULONGLONG now = GetTickCount64();
    if (!cache.at || now - cache.at >= 2000) {
        Refresh(cache.view);
        cache.at = now;
    }
    vm.settings_assistant = cache.view;
    std::lock_guard<std::mutex> lock(g_lock);
    vm.settings_assistant.conn = g_testing.load() ? ui::AsstConn::Testing : g_conn;
    vm.settings_assistant.conn_time = g_conn_time ? *g_conn_time : std::wstring{};
}

void AssistantSettingsClick(AppState& s, int target) {
    switch (target) {
    case ui::kAsstTest:
        StartTest(s);
        break;
    case ui::kAsstManage:
        // The assistant panel's 管理连接 card: address, key and default model.
        AiAssistantOpen(s);
        s.aiAssistant.menu_open = true;
        AiConnOpen(s);
        Cache().at = 0;   // read ai.json again when the page shows next
        break;
    case ui::kAsstVoice:
        AiAssistantOpen(s);
        AiVoiceOpenConnection(s);
        Cache().at = 0;
        break;
    case ui::kAsstMcp:
        OpenSettingsTab(s, ui::kSettingsPageAi);
        break;
    default: break;
    }
    InvalidateRect(s.hwnd, nullptr, FALSE);
}

} // namespace pulse
