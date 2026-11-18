#include "ai_assistant_settings.h"
#include "ai_assistant_settings_cache.h"
#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_voice.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_model_stream.h"
#include "ai_pack.h"
#include "ai_providers.h"
#include "ai_settings.h"
#include "ai_service.h"
#include "ai_voice_capture.h"
#include "ai_voice_service.h"
#include "app_commands.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/fluent_menu.h"
#include "../ui/settings_page_ids.h"
#include "../ui/typography.h"
#include "../ui/ui_renderer.h"
#include "../ui/ui_view_model.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <thread>

namespace pulse {

namespace ai = app::ai;
using ui::AsstField;
using ui::AsstStage;

struct AsstPageUi {
    bool started = false;
    float think_drag = -1.0f;          // 思考强度 knob held: stops under the pointer
    AsstStage stage = AsstStage::Pick;
    std::string pick;                  // the service being set up
    bool url_editing = false, model_editing = false, revealed = false;
    // EDITs, by AsstField.
    HWND edits[ui::kAsstFields]{};
    bool shown[ui::kAsstFields]{};
    bool focus_pending[ui::kAsstFields]{};
    int lengths[ui::kAsstFields]{};
    HFONT font = nullptr;
    float font_scale = 0.0f;
    int line_px = 0;
    HBRUSH brush = nullptr;
    COLORREF brush_color = CLR_INVALID;
    COLORREF painted_back = CLR_INVALID;  // the EDITs' colours when last invalidated
    COLORREF painted_fore = CLR_INVALID;
    // 连接.
    struct Job {
        uint64_t gen = 0;
        std::atomic<bool> done{false};
        ai::ModelProbe result;
    };
    std::shared_ptr<Job> job;
    uint64_t gen = 0;
    bool connecting = false;
    std::string job_service;
    std::wstring job_url;
    std::wstring error_title, error_sub;
    // 连接正常 · … right after connecting.
    std::wstring fresh;
    bool fresh_manual = false;
    // 语音输入: setting one up (the preset picked), and the tries in flight.
    bool voice_setup = false;
    int voice_pick = 0;
    bool voice_revealed = false;
    struct VoiceJob {
        uint64_t gen = 0;
        bool setup = false;                // 连接并测试: save it when it works
        std::atomic<bool> done{false};
        app::ai::VoiceAnswer list, run;
        std::string model;
    };
    std::shared_ptr<VoiceJob> voice_job;
    uint64_t voice_gen = 0;
    std::wstring voice_job_url;
    std::wstring voice_error, voice_ok;
    std::vector<std::string> voice_models;   // the service's, once read
    bool voice_listing = false, voice_listed = false, voice_list_tried = false;
    std::vector<app::ai::VoiceMic> mics;
    std::wstring default_mic;
    bool mics_read = false;
};

namespace {

// Bumped by every write to the settings or a key: the page view is re-read at once.
std::atomic<unsigned> g_epoch{1};
void Changed() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

using I = l10n::StringId;
constexpr UINT_PTR kEditSubclass = 0x4153;   // 'AS'
constexpr UINT_PTR kConnectTimer = 0x4153;     // 'AS'
constexpr UINT_PTR kVoiceTryTimer = 0x415A;    // the voice card's 连接 / model try
constexpr wchar_t kMaskChar = L'\u2022';

// 上次连接: this run's last check, from any window.
std::mutex g_lock;
ui::AsstConn g_conn = ui::AsstConn::None;
std::wstring* g_conn_time = nullptr;   // HH:MM; never freed (probe threads may outlive windows)
uint32_t g_conn_ms = 0;
std::atomic<bool> g_testing{false};
bool g_seen = false;                   // the page was opened in this run: no more 新

std::wstring Text(I id) { return l10n::Get(id); }
std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

std::wstring NowText() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t text[16]{};
    swprintf_s(text, L"%02u:%02u", static_cast<unsigned>(now.wHour), static_cast<unsigned>(now.wMinute));
    return text;
}

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

AsstPageUi* Ui(AppState& s) { return s.aiAssistant.page.get(); }

std::wstring EditText(HWND edit) {
    if (!edit) return {};
    const int n = GetWindowTextLengthW(edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

void Wipe(std::wstring& text) {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size() * sizeof(wchar_t));
    text.clear();
}

std::wstring Trim(std::wstring text) {
    auto space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000; };
    size_t a = 0, b = text.size();
    while (a < b && space(text[a])) ++a;
    while (b > a && space(text[b - 1])) --b;
    return text.substr(a, b - a);
}

// A typed address, tidied: no trailing slash, https:// when it has no scheme.
std::wstring TidyUrl(std::wstring url) {
    url = Trim(std::move(url));
    while (!url.empty() && url.back() == L'/') url.pop_back();
    if (!url.empty() && url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    return url;
}

HWND Edit(AsstPageUi& u, AsstField f) { return u.edits[static_cast<int>(f)]; }

// The service's settings as ai.json keeps them.
std::string StoredId(const ai::Provider& p) { return ai::IsDefaultProvider(p) ? std::string() : std::string(p.id); }

const ai::ServicePrefs* SavedPrefs(const ai::Settings& settings, std::string_view id) {
    for (const auto& [name, prefs] : settings.assistant.services)
        if (name == id) return &prefs;
    return nullptr;
}

// The address a service would use: the one in use, the remembered one, or its own.
std::wstring ServiceUrl(const ai::Settings& settings, const ai::Provider& p) {
    const ai::Provider& current = ai::ProviderById(settings.assistant.service);
    std::string stored;
    if (current.id == std::string_view(p.id)) stored = settings.assistant.base_url;
    else if (const ai::ServicePrefs* saved = SavedPrefs(settings, p.id)) stored = saved->base_url;
    return stored.empty() ? std::wstring(p.base_url) : json::Wide(stored);
}

// Ids that are not chat models (embeddings, speech, images): never the default.
bool ChatModel(const std::string& id) {
    std::string lower(id);
    for (char& ch : lower) ch = static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
    for (const char* word : {"embed", "rerank", "whisper", "tts", "audio", "speech", "asr", "image", "dall-e",
                             "moderation", "sensevoice", "cosyvoice", "flux", "stable-diffusion", "kolors"})
        if (lower.find(word) != std::string::npos) return false;
    return true;
}

// The default model after connecting: the remembered one when the service
// still lists it (or lists nothing, or leaves ids out, as typed names are),
// else the first chat model it lists.
std::string ChooseModel(const std::string& remembered, const std::vector<std::string>& listed, bool partial) {
    if (!remembered.empty() &&
        (partial || listed.empty() || std::find(listed.begin(), listed.end(), remembered) != listed.end()))
        return remembered;
    for (const char* preferred : {"deepseek-flash", "deepseek-chat"})
        if (std::find(listed.begin(), listed.end(), preferred) != listed.end()) return preferred;
    for (const std::string& id : listed)
        if (ChatModel(id)) return id;
    return listed.empty() ? std::string() : listed.front();
}

// Makes `p` the service in use: the one before it goes to "services" with its
// model and address; p's remembered ones come back. Nothing when ai.json does
// not parse. `url` empty: p's remembered address (or its own).
bool SwitchService(const ai::Provider& p, const std::wstring* url, const std::vector<std::string>* listed,
                   std::string* model_out) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return false;
    ai::AssistantPrefs& a = settings.assistant;
    const ai::Provider& before = ai::ProviderById(a.service);
    std::string remembered_model = a.model;
    std::string address = a.base_url;
    if (before.id != std::string_view(p.id)) {
        ai::ServicePrefs old{a.model, a.base_url};
        remembered_model.clear();
        address.clear();
        auto& list = a.services;
        for (auto it = list.begin(); it != list.end();) {
            if (it->first == p.id) {
                remembered_model = it->second.model;
                address = it->second.base_url;
                it = list.erase(it);
            } else if (it->first == before.id) {
                it = list.erase(it);
            } else {
                ++it;
            }
        }
        list.emplace_back(before.id, std::move(old));
    }
    if (url) {
        const std::wstring own = p.base_url;
        address = *url == own ? std::string() : json::Utf8(*url);
    }
    a.service = StoredId(p);
    a.base_url = address;
    // Switching to a saved service: its models as last read.
    const std::wstring effective = address.empty() ? std::wstring(p.base_url) : json::Wide(address);
    a.model = ChooseModel(remembered_model, listed ? *listed : ai::ListedModels(effective), p.partial_list);
    if (model_out) *model_out = a.model;
    if (!ai::SaveSettings(settings)) return false;
    Changed();
    AiAgentKeyChanged();   // also forgets the cached service (ai_providers.h)
    return true;
}

template <typename F>
bool UpdateAssistant(F&& change) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return false;
    change(settings.assistant);
    if (!ai::SaveSettings(settings)) return false;
    Changed();
    return true;
}

// Why 连接 failed, in the words the panel's card used.
void DescribeFailure(const ai::ModelProbe& probe, const ai::Provider& p, const std::wstring& url,
                     std::wstring& title, std::wstring& sub) {
    const int code = probe.http_status;
    const std::wstring codes = std::to_wstring(code);
    if (code == 0) {
        if (!p.needs_key) {
            title = Text(I::AsstErrOllama);
            sub = Text(I::AsstErrOllamaSub);
            return;
        }
        title = Fill(Text(I::AiConnErrNetwork), L"{host}", ai::ModelHost(url));
        sub = Text(I::AsstErrNetworkSub);
    } else if (code == 401 || code == 403) {
        title = Text(I::AiConnErrAuth);
        sub = Text(I::AsstErrAuthSub);
    } else if (code == 402) {
        title = Text(I::AiConnErrBalance);
        sub = Text(I::AsstErrBalanceSub);
    } else if (code == 429) {
        title = Text(I::AiConnErrRate);
        sub = Text(I::AiConnErrRateSub);
    } else if (code >= 500) {
        title = Fill(Text(I::AiConnErrServer), L"{code}", codes);
        sub = Text(I::AiConnErrServerSub);
    } else {
        title = Fill(Text(I::AiConnErrOther), L"{code}", codes);
        sub = Text(I::AsstErrOtherSub);
    }
}

// ---- EDITs ----------------------------------------------------------------

void HideEdit(AppState& s, AsstPageUi& u, AsstField f) {
    const int i = static_cast<int>(f);
    HWND edit = u.edits[i];
    if (!edit || !u.shown[i]) return;
    const bool had_focus = GetFocus() == edit;
    ShowWindow(edit, SW_HIDE);
    u.shown[i] = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

void HideEdits(AppState& s, AsstPageUi& u) {
    for (AsstField f : {AsstField::Key, AsstField::Url, AsstField::Model}) HideEdit(s, u, f);
}

void ClearEdit(AsstPageUi& u, AsstField f) {
    const int i = static_cast<int>(f);
    if (u.edits[i]) SetWindowTextW(u.edits[i], L"");
    u.lengths[i] = 0;
}

void SetReveal(AsstPageUi& u, bool on) {
    u.revealed = on;
    if (HWND key = Edit(u, AsstField::Key)) {
        SendMessageW(key, EM_SETPASSWORDCHAR, on ? 0 : kMaskChar, 0);
        InvalidateRect(key, nullptr, TRUE);
    }
}

void Focus(AsstPageUi& u, AsstField f) { u.focus_pending[static_cast<int>(f)] = true; }

void Connect(AppState& s, AsstPageUi& u);
void SaveUrl(AppState& s, AsstPageUi& u);
void SaveModel(AppState& s, AsstPageUi& u);
void CancelEditing(AppState& s, AsstPageUi& u);

// Text, caret, selection and the hint are presented by LumaText like every
// other Pulse field (edit_host.h); native painting stays as the fallback.
struct EditLook {
    IDWriteTextFormat* format = nullptr;
    D2D1_COLOR_F fore{};
    D2D1_COLOR_F back{};
};

EditLook Look(AppState& s) {
    EditLook look;
    look.format = s.compositor.TextFormat();
    D2D1_COLOR_F hint{};
    s.renderer.SettingsAssistantEditColors(look.back, look.fore, hint);
    return look;
}

std::wstring Hint(AsstField f) {
    if (f == AsstField::Key || f == AsstField::VoiceKey) return Text(I::AsstKeyPlaceholder);
    if (f == AsstField::Model) return Text(I::AsstModelPlaceholder);
    return L"https://";
}

// Enter: 连接 (setting up) or 保存; Esc: stop editing.
void VoiceConnect(AppState& s, AsstPageUi& u);
void VoiceCancel(AppState& s, AsstPageUi& u);

bool IsVoiceEdit(AsstPageUi& u, HWND hwnd) {
    return hwnd && (hwnd == Edit(u, AsstField::VoiceKey) || hwnd == Edit(u, AsstField::VoiceUrl));
}

void SubmitFrom(AppState& s, AsstPageUi& u, HWND hwnd) {
    if (IsVoiceEdit(u, hwnd)) VoiceConnect(s, u);
    else if (u.stage == AsstStage::Setup) Connect(s, u);
    else if (hwnd == Edit(u, AsstField::Model)) SaveModel(s, u);
    else if (hwnd == Edit(u, AsstField::Url)) SaveUrl(s, u);
}

LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AsstPageUi* u = Ui(s);
    if (!u) return DefSubclassProc(hwnd, msg, wp, lp);
    const EditLook look = Look(s);
    const auto presented = [&](LRESULT& out) {
        return ui::HandleChildEditMessage(s.compositor, look.format, look.fore, look.back,
                                          AssistantSettingsCtlColor(s, nullptr, hwnd), hwnd, msg, wp, lp, out);
    };
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            SubmitFrom(s, *u, hwnd);
            Redraw(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (IsVoiceEdit(*u, hwnd)) VoiceCancel(s, *u);
            else CancelEditing(s, *u);
            Redraw(s);
            return 0;
        }
        if (wp == VK_TAB) {
            // Key → address (when it is a field) and back, in either card.
            const bool voice = hwnd == Edit(*u, AsstField::VoiceKey) || hwnd == Edit(*u, AsstField::VoiceUrl);
            HWND key = Edit(*u, voice ? AsstField::VoiceKey : AsstField::Key);
            HWND url = Edit(*u, voice ? AsstField::VoiceUrl : AsstField::Url);
            HWND next = hwnd == key ? url : key;
            if (next && IsWindowVisible(next)) SetFocus(next);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == L'\n' || wp == 27 || wp == L'\t') return 0;
        if (wp == 1) {
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        LRESULT r = 0;
        if (!presented(r)) r = DefSubclassProc(hwnd, msg, wp, lp);
        Redraw(s);   // the field's focus underline
        return r;
    }
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        presented(ignored);
        RemoveWindowSubclass(hwnd, EditProc, kEditSubclass);
        break;
    }
    default: {
        LRESULT r = 0;
        if (presented(r)) return r;
        break;
    }
    }
    const LRESULT r = ui::DefPresentedChildEditProc(s.compositor, look.format, look.fore, look.back, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL:
        for (int i = 0; i < ui::kAsstFields; ++i) {
            if (u->edits[i] != hwnd) continue;
            const int n = GetWindowTextLengthW(hwnd);
            if ((n == 0) != (u->lengths[i] == 0)) {
                InvalidateRect(hwnd, nullptr, TRUE);   // the placeholder
                Redraw(s);                             // 连接 / 保存 enable
            }
            if (i == static_cast<int>(AsstField::Key) && n != u->lengths[i] && !u->error_title.empty()) {
                u->error_title.clear();   // a new key: the old failure no longer applies
                u->error_sub.clear();
                Redraw(s);
            }
            if (i == static_cast<int>(AsstField::VoiceKey) && n != u->lengths[i] && !u->voice_error.empty()) {
                u->voice_error.clear();
                Redraw(s);
            }
            u->lengths[i] = n;
        }
        break;
    default:
        break;
    }
    return r;
}

void EnsureFont(AppState& s, AsstPageUi& u) {
    if (u.font && u.font_scale == s.scale) return;
    if (u.font) DeleteObject(u.font);
    u.font = ui::typography::CreateEditFont(s.scale);   // the TextFormat size, like the other fields
    u.font_scale = s.scale;
    u.line_px = 0;
    for (HWND e : u.edits) {
        if (!e || !u.font) continue;
        SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(u.font), TRUE);
        SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    }
    if (u.font && s.hwnd) {
        HDC hdc = GetDC(s.hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, u.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(s.hwnd, hdc);
        u.line_px = static_cast<int>(tm.tmHeight);
    }
}

HWND MakeEdit(AppState& s, bool password) {
    // A redirected layered child like the composer (edit_host.h), one line.
    HWND e = ui::CreateChildEdit(s.hwnd, L"", password ? ES_PASSWORD : 0);
    if (!e) return nullptr;
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 512, 0);
    if (password) SendMessageW(e, EM_SETPASSWORDCHAR, kMaskChar, 0);
    SetWindowSubclass(e, EditProc, kEditSubclass, reinterpret_cast<DWORD_PTR>(&s));
    return e;
}

void EnsureEdits(AppState& s, AsstPageUi& u) {
    if (!s.hwnd) return;
    bool made = false;
    for (int i = 0; i < ui::kAsstFields; ++i) {
        if (u.edits[i]) continue;
        u.edits[i] = MakeEdit(s, i == static_cast<int>(AsstField::Key) || i == static_cast<int>(AsstField::VoiceKey));
        made = true;
    }
    if (made) u.font_scale = 0.0f;
    EnsureFont(s, u);
}

void Place(AppState& s, AsstPageUi& u, AsstField f) {
    const int i = static_cast<int>(f);
    HWND edit = u.edits[i];
    D2D1_RECT_F r{}, clip{};
    if (!edit || !s.renderer.SettingsAssistantField(f, r, clip) || r.top < clip.top || r.bottom > clip.bottom) {
        HideEdit(s, u, f);
        return;
    }
    // The floating panel over the field hides it (the EDIT would draw on top).
    const ui::AiPanelView& panel = s.renderer.AiPanel();
    if (panel.FloatingVisible() &&
        (panel.Contains(r.left, r.top) || panel.Contains(r.right, r.top) || panel.Contains(r.left, r.bottom) ||
         panel.Contains(r.right, r.bottom))) {
        HideEdit(s, u, f);
        return;
    }
    const int line = (std::max)(u.line_px, 1);
    const int x = static_cast<int>(std::lround(r.left));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int y = static_cast<int>(std::lround((r.top + r.bottom) * 0.5f - static_cast<float>(line) * 0.5f));
    RECT cur{};
    GetWindowRect(edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == line;
    if (!placed || !u.shown[i]) {
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(Hint(f).c_str()));
        SetWindowPos(edit, HWND_TOP, x, y, w, line, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        u.shown[i] = true;
        const EditLook look = Look(s);
        ui::PresentChildEdit(s.compositor, look.format, look.fore, look.back, edit);
    }
    if (u.focus_pending[i]) {
        u.focus_pending[i] = false;
        const HWND fg = GetForegroundWindow();
        if (fg == s.hwnd || fg == GetAncestor(s.hwnd, GA_ROOT)) {
            SetFocus(edit);
            const int n = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, 0, n);
        }
    }
}

// ---- Stages ---------------------------------------------------------------

bool InUse() { return ai::HasServiceKey(ai::CurrentProvider().id); }

void Start(AppState& s, AsstPageUi& u) {
    if (u.started) return;
    u.started = true;
    u.stage = InUse() ? AsstStage::Connected : AsstStage::Pick;
    EnsureEdits(s, u);
}

void ResetEditing(AppState& s, AsstPageUi& u) {
    u.url_editing = false;
    u.model_editing = false;
    u.error_title.clear();
    u.error_sub.clear();
    ++u.gen;               // a 连接 in flight no longer applies
    u.connecting = false;
    u.job.reset();
    std::wstring key = EditText(Edit(u, AsstField::Key));
    Wipe(key);
    ClearEdit(u, AsstField::Key);
    SetReveal(u, false);
    HideEdits(s, u);
}

void ToPick(AppState& s, AsstPageUi& u) {
    ResetEditing(s, u);
    u.fresh.clear();
    u.stage = AsstStage::Pick;
}

void ToConnected(AppState& s, AsstPageUi& u) {
    ResetEditing(s, u);
    u.stage = AsstStage::Connected;
}

// Setting up `p`: its key (and address) fields.
void ToSetup(AppState& s, AsstPageUi& u, const ai::Provider& p) {
    ResetEditing(s, u);
    u.fresh.clear();
    u.stage = AsstStage::Setup;
    u.pick = p.id;
    const std::wstring url = ServiceUrl(ai::LoadSettings(), p);
    if (HWND e = Edit(u, AsstField::Url)) SetWindowTextW(e, ai::IsCustomProvider(p) ? url.c_str() : L"");
    u.lengths[static_cast<int>(AsstField::Url)] = ai::IsCustomProvider(p) ? static_cast<int>(url.size()) : 0;
    u.url_editing = ai::IsCustomProvider(p);
    if (p.needs_key) Focus(u, AsstField::Key);
    else if (u.url_editing) Focus(u, AsstField::Url);
}

// A service whose key is stored: straight to it.
void SwitchTo(AppState& s, AsstPageUi& u, const ai::Provider& p) {
    if (!SwitchService(p, nullptr, nullptr, nullptr)) return;
    ToConnected(s, u);
    u.fresh.clear();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_conn = ui::AsstConn::None;   // 上次连接 was another service's
    }
    AiAssistantPrefsChanged(s);
}

// ---- 连接 ------------------------------------------------------------------

void ConnectDone(AppState& s, AsstPageUi& u, ai::ModelProbe result) {
    u.connecting = false;
    Changed();
    const ai::Provider& p = ai::ProviderById(u.pick);
    const bool listed = result.http_status == 200;
    const bool unlisted = !listed && p.partial_list && (result.http_status == 404 || result.http_status == 405);
    if (!listed && !unlisted) {
        DescribeFailure(result, p, u.job_url, u.error_title, u.error_sub);
        NoteAssistantConnection(false, result.ms);
        Redraw(s);
        return;
    }
    std::wstring key = p.needs_key ? Trim(EditText(Edit(u, AsstField::Key))) : std::wstring(ai::kNoKeyPlaceholder);
    const bool saved = !key.empty() && ai::SaveServiceKey(p.id, key);
    Wipe(key);
    std::string model;
    const std::vector<std::string> none;
    if (!saved || !SwitchService(p, &u.job_url, listed ? &result.models : &none, &model)) {
        u.error_title = Text(I::AiConnErrSave);
        u.error_sub = Text(I::AiConnErrSaveSub);
        Redraw(s);
        return;
    }
    if (listed) ai::StoreListedModels(u.job_url, result.models, result.chat_only);
    NoteAssistantConnection(true, result.ms);
    ToConnected(s, u);
    // No list (or nothing to choose from it): a typed name, kept if there was one.
    u.fresh_manual = unlisted || model.empty();
    const std::wstring ok = Text(I::AsstFresh) + L" \u00b7 ";
    if (u.fresh_manual) {
        u.fresh = ok + Text(I::AsstFreshManual);
        if (model.empty()) {
            u.model_editing = true;
            ClearEdit(u, AsstField::Model);
            Focus(u, AsstField::Model);
        }
    } else {
        std::wstring detail = Fill(Text(I::AsstFreshDetail), L"{ms}", std::to_wstring(result.ms));
        detail = Fill(std::move(detail), L"{n}", std::to_wstring(result.models.size()));
        u.fresh = ok + Fill(std::move(detail), L"{model}", json::Wide(model));
    }
    AiAssistantPrefsChanged(s);
    Redraw(s);
}

void CALLBACK ConnectTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    AsstPageUi* u = s ? Ui(*s) : nullptr;
    if (!u || !u->job || u->job->gen != u->gen) {
        if (u) u->job.reset();
        KillTimer(hwnd, id);
        return;
    }
    if (!u->job->done.load(std::memory_order_acquire)) return;
    KillTimer(hwnd, id);
    auto job = std::move(u->job);
    ConnectDone(*s, *u, std::move(job->result));
}

void Connect(AppState& s, AsstPageUi& u) {
    if (u.connecting || u.stage != AsstStage::Setup) return;
    const ai::Provider& p = ai::ProviderById(u.pick);
    ai::ModelEndpoint endpoint;
    endpoint.base_url = u.url_editing ? TidyUrl(EditText(Edit(u, AsstField::Url)))
                                      : ServiceUrl(ai::LoadSettings(), p);
    if (endpoint.base_url.empty()) {
        Focus(u, AsstField::Url);
        return;
    }
    endpoint.key = p.needs_key ? Trim(EditText(Edit(u, AsstField::Key))) : std::wstring(ai::kNoKeyPlaceholder);
    if (endpoint.key.empty()) {
        Focus(u, AsstField::Key);
        return;
    }
    // 小米 MiMo's Token Plan keys (tp-) connect to their own host.
    if (!u.url_editing) endpoint.base_url = ai::AddressForKey(p, endpoint.base_url, endpoint.key);
    u.connecting = true;
    u.error_title.clear();
    u.error_sub.clear();
    u.job_url = endpoint.base_url;
    auto job = std::make_shared<AsstPageUi::Job>();
    job->gen = ++u.gen;
    u.job = job;
    try {
        std::thread([job, endpoint = std::move(endpoint)]() mutable {
            job->result = ai::ProbeModelService(std::move(endpoint));   // wipes the key
            job->done.store(true, std::memory_order_release);
        }).detach();
    } catch (...) {
        u.connecting = false;
        u.job.reset();
        return;
    }
    if (s.hwnd) SetTimer(s.hwnd, kConnectTimer, 50, ConnectTimer);
    Redraw(s);
}

// 测试连接: the stored key against the address in use.
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
            if (ok) ai::StoreListedModels(url, result.models, result.chat_only, true);
            NoteAssistantConnection(ok, result.ms);
            g_testing.store(false);
            if (hwnd && IsWindow(hwnd)) InvalidateRect(hwnd, nullptr, FALSE);
        }).detach();
    } catch (...) {
        g_testing.store(false);
    }
}

// ---- Connected-stage edits ------------------------------------------------

void SaveUrl(AppState& s, AsstPageUi& u) {
    const ai::Provider& p = ai::CurrentProvider();
    const std::wstring url = TidyUrl(EditText(Edit(u, AsstField::Url)));
    if (url.empty()) return;
    const std::wstring own = p.base_url;
    const std::string stored = url == own ? std::string() : json::Utf8(url);
    if (!UpdateAssistant([&](ai::AssistantPrefs& a) { a.base_url = stored; })) return;
    AiAgentKeyChanged();
    u.url_editing = false;
    HideEdit(s, u, AsstField::Url);
    StartTest(s);   // the badge says whether the new address answers
    Redraw(s);
}

void SaveModel(AppState& s, AsstPageUi& u) {
    std::string model = json::Utf8(Trim(EditText(Edit(u, AsstField::Model))));
    if (model.empty() || model.size() > 200) return;
    if (!UpdateAssistant([&](ai::AssistantPrefs& a) { a.model = model; })) return;
    u.model_editing = false;
    u.fresh.clear();
    HideEdit(s, u, AsstField::Model);
    AiAssistantPrefsChanged(s);
    Redraw(s);
}

void CancelEditing(AppState& s, AsstPageUi& u) {
    if (u.stage != AsstStage::Connected) return;
    u.url_editing = false;
    u.model_editing = false;
    HideEdit(s, u, AsstField::Url);
    HideEdit(s, u, AsstField::Model);
}

// 思考 as the default model takes it, and the level the user prefers.
ai::ThinkSpec DefaultModelThinking(const ai::Settings& settings) {
    return ai::ModelThinking(ai::CurrentProvider(), settings.assistant.model);
}
ai::ThinkLevel PreferredThinking(const ai::Settings& settings) {
    return static_cast<ai::ThinkLevel>(std::clamp(settings.assistant.thinking, 0, 6));
}

void SetThinking(AppState& s, ai::ThinkLevel level) {
    if (!UpdateAssistant([&](ai::AssistantPrefs& a) { a.thinking = static_cast<int>(level); })) return;
    AiAssistantPrefsChanged(s);
}

int Dropdown(AppState& s, int index, std::vector<ui::FluentMenuItem> items) {
    if (!EnsureMenu(s)) return 0;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const auto r = s.renderer.SettingsDropdownBounds(BuildVm(s, false), index,
        static_cast<float>(s.compositor.Width()), static_cast<float>(s.compositor.Height()));
    POINT corners[] = {{static_cast<LONG>(std::lround(r.left)), static_cast<LONG>(std::lround(r.top))},
                       {static_cast<LONG>(std::lround(r.right)), static_cast<LONG>(std::lround(r.bottom))}};
    MapWindowPoints(s.hwnd, nullptr, corners, 2);
    // A long model list scrolls under the shared scrollbar instead of
    // running down the screen (like the panel's model menu).
    s.menu->SetMaxVisibleRows(12);
    return s.menu->TrackDropdown({corners[0].x, corners[0].y, corners[1].x, corners[1].y}, std::move(items));
}

// 默认模型: the service's models, then 手动输入模型名….
void ModelMenu(AppState& s, AsstPageUi& u) {
    const std::string current = ai::LoadSettings().assistant.model;
    std::vector<std::string> ids = ai::ListedModels(AiAgentBaseUrl());
    if (!current.empty() && std::find(ids.begin(), ids.end(), current) == ids.end()) ids.insert(ids.begin(), current);
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < ids.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = json::Wide(ids[i]);
        if (ai::IsChatOnlyModel(AiAgentBaseUrl(), ids[i])) item.badge_text = Text(I::AsstChatOnly);
        item.radio_group = true;
        item.radio = ids[i] == current;
        items.push_back(std::move(item));
    }
    if (!items.empty()) items.back().separator_after = true;
    const int manual = static_cast<int>(ids.size()) + 1;
    ui::FluentMenuItem typed;
    typed.command = manual;
    typed.text = Text(I::AsstModelManual);
    typed.glyph = L"\xE70F";
    items.push_back(std::move(typed));
    const int command = Dropdown(s, 3, std::move(items));
    if (command == manual) {
        u.model_editing = true;
        if (HWND e = Edit(u, AsstField::Model)) SetWindowTextW(e, json::Wide(current).c_str());
        u.lengths[static_cast<int>(AsstField::Model)] = static_cast<int>(current.size());
        Focus(u, AsstField::Model);
    } else if (command >= 1 && command <= static_cast<int>(ids.size())) {
        const std::string chosen = ids[static_cast<size_t>(command - 1)];
        if (UpdateAssistant([&](ai::AssistantPrefs& a) { a.model = chosen; })) {
            u.fresh.clear();
            AiAssistantPrefsChanged(s);
        }
    }
}

// ---- 语音输入 ---------------------------------------------------------------
// The service behind the composer's microphone (ai_voice_service.h): pick a
// preset, paste its key (硅基流动 may reuse the model service's), 连接并测试
// reads the recognition models and tries a short clip; then the key, model,
// 停顿 and microphone in place. The panel reads ai.json again
// (AiVoicePrefsChanged) after every change here.

const ai::VoicePreset& VoicePresetAt(int index) {
    size_t n = 0;
    const ai::VoicePreset* presets = ai::VoicePresets(n);
    return presets[static_cast<size_t>(std::clamp(index, 0, static_cast<int>(n) - 1))];
}

int VoicePresetIndex(std::string_view id) {
    size_t n = 0;
    const ai::VoicePreset* presets = ai::VoicePresets(n);
    for (size_t i = 0; i < n; ++i)
        if (id == presets[i].id) return static_cast<int>(i);
    return 0;
}

std::wstring VoiceServiceName(std::string_view id) {
    if (id == "siliconflow") return Text(I::AiVoiceSiliconFlow);
    if (id == "openai") return L"OpenAI";
    return Text(I::AiVoiceCustom);
}

// Where to get a key: the model presets know the same two consoles.
std::wstring VoiceConsoleUrl(std::string_view id) {
    if (id == "siliconflow") return ai::ProviderById("silicon").key_url;
    if (id == "openai") return ai::ProviderById("openai").key_url;
    return {};
}

// 硅基流动's model key, when the model service has one: the voice card may use it.
bool SharedSiliconKey(std::string_view id) { return id == "siliconflow" && ai::HasServiceKey("silicon"); }

int PauseSegment(int pause) { return pause == 2 ? 1 : pause == 0 ? 2 : 0; }

ai::VoicePrefs LoadVoice() { return ai::LoadSettings().assistant.voice; }

bool SaveVoice(AppState& s, const ai::VoicePrefs& voice) {
    if (!UpdateAssistant([&](ai::AssistantPrefs& a) { a.voice = voice; })) return false;
    AiVoicePrefsChanged(s);
    return true;
}

void HideVoiceEdits(AppState& s, AsstPageUi& u) {
    HideEdit(s, u, AsstField::VoiceKey);
    HideEdit(s, u, AsstField::VoiceUrl);
}

void SetVoiceReveal(AsstPageUi& u, bool on) {
    u.voice_revealed = on;
    if (HWND key = Edit(u, AsstField::VoiceKey)) {
        SendMessageW(key, EM_SETPASSWORDCHAR, on ? 0 : kMaskChar, 0);
        InvalidateRect(key, nullptr, TRUE);
    }
}

void ResetVoiceTry(AppState& s, AsstPageUi& u) {
    ++u.voice_gen;
    u.voice_job.reset();
    u.voice_listing = false;
    u.voice_error.clear();
    u.voice_ok.clear();
    if (s.hwnd) KillTimer(s.hwnd, kVoiceTryTimer);
}

void VoiceToSetup(AppState& s, AsstPageUi& u, int index) {
    ResetVoiceTry(s, u);
    u.voice_setup = true;
    u.voice_pick = index;
    const ai::VoicePreset& p = VoicePresetAt(index);
    std::wstring url = p.base_url ? p.base_url : L"";
    const ai::VoicePrefs voice = LoadVoice();
    if (url.empty() && voice.service == p.id) url = json::Wide(voice.base_url);   // 更换 a custom one's key
    ClearEdit(u, AsstField::VoiceKey);
    SetVoiceReveal(u, false);
    if (HWND e = Edit(u, AsstField::VoiceUrl)) SetWindowTextW(e, url.c_str());
    u.lengths[static_cast<int>(AsstField::VoiceUrl)] = static_cast<int>(url.size());
    Focus(u, !p.base_url || !*p.base_url ? (url.empty() ? AsstField::VoiceUrl : AsstField::VoiceKey) : AsstField::VoiceKey);
}

void VoiceCancel(AppState& s, AsstPageUi& u) {
    if (!u.voice_setup) return;
    ResetVoiceTry(s, u);
    u.voice_setup = false;
    std::wstring key = EditText(Edit(u, AsstField::VoiceKey));
    Wipe(key);
    ClearEdit(u, AsstField::VoiceKey);
    SetVoiceReveal(u, false);
    HideVoiceEdits(s, u);
    if (s.hwnd) SetFocus(s.hwnd);
    Redraw(s);
}

// The panel's words for a failed try.
std::wstring VoiceFailure(const ai::VoiceAnswer& list, const ai::VoiceAnswer& run, const std::wstring& url) {
    if (list.Ok() && list.models.empty()) return Text(I::AiVoiceErrNoModel);
    const ai::VoiceAnswer& bad = !list.Ok() ? list : run;
    if (bad.http_status == 0) return Fill(Text(I::AiVoiceErrNet), L"{host}", ai::ModelHost(url));
    if (bad.http_status == 401) return Text(I::AiVoiceErrKey);
    // Cut the service's message in UTF-16, never inside a surrogate pair.
    std::wstring msg = json::Wide(bad.error_message);
    if (msg.size() > 120) {
        size_t n = 119;
        if (IS_HIGH_SURROGATE(msg[n - 1])) --n;
        msg = msg.substr(0, n) + L"\u2026";
    }
    return Fill(Fill(Text(I::AiVoiceErrHttp), L"{code}", std::to_wstring(bad.http_status)), L"{msg}", msg);
}

std::wstring VoiceOk(uint32_t ms) {
    wchar_t secs[16]{};
    swprintf_s(secs, L"%.1f", static_cast<double>(ms) / 1000.0);
    return Fill(Text(I::AiVoiceTestOk), L"{s}", secs);
}

void VoiceTryDone(AppState& s, AsstPageUi& u, AsstPageUi::VoiceJob& job);

void CALLBACK VoiceTryTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    AsstPageUi* u = s ? Ui(*s) : nullptr;
    if (!u || !u->voice_job || u->voice_job->gen != u->voice_gen) {
        if (u) u->voice_job.reset();
        KillTimer(hwnd, id);
        return;
    }
    if (!u->voice_job->done.load(std::memory_order_acquire)) return;
    KillTimer(hwnd, id);
    const std::shared_ptr<AsstPageUi::VoiceJob> job = std::move(u->voice_job);
    VoiceTryDone(*s, *u, *job);
    Changed();
    Redraw(*s);
}

// Lists the models; with `model` empty picks one; then tries the clip with it.
// `list_only`: just the list (the 识别模型 drop-down).
void StartVoiceTry(AppState& s, AsstPageUi& u, ai::VoiceEndpoint endpoint, std::string service, std::string model,
                   bool setup, bool list_only = false) {
    ResetVoiceTry(s, u);
    auto job = std::make_shared<AsstPageUi::VoiceJob>();
    job->gen = u.voice_gen;
    job->setup = setup;
    u.voice_job = job;
    u.voice_job_url = endpoint.base_url;
    if (list_only) u.voice_listing = true;
    try {
        std::thread([job, endpoint = std::move(endpoint), service, model, list_only]() mutable {
            ai::VoiceEndpoint copy = endpoint;   // the list call wipes its key
            job->list = ai::ListVoiceModels(std::move(copy));
            const auto& m = job->list.models;
            if (!list_only && job->list.Ok() && !m.empty()) {
                job->model = !model.empty() && std::find(m.begin(), m.end(), model) != m.end()
                                 ? model : model.empty() ? ai::PickVoiceModel(service, m) : model;
                endpoint.model = job->model;
                job->run = ai::TranscribeVoice(std::move(endpoint), ai::EncodeVoiceWav(ai::VoiceTestClip()));
            } else {
                Wipe(endpoint.key);
            }
            job->done.store(true, std::memory_order_release);
        }).detach();
    } catch (...) {
        u.voice_job.reset();
        u.voice_listing = false;
        return;
    }
    if (s.hwnd) SetTimer(s.hwnd, kVoiceTryTimer, 50, VoiceTryTimer);
}

bool VoiceTrying(const AsstPageUi& u) { return u.voice_job != nullptr && !u.voice_listing; }

void VoiceConnect(AppState& s, AsstPageUi& u) {
    if (!u.voice_setup || VoiceTrying(u)) return;
    const ai::VoicePreset& p = VoicePresetAt(u.voice_pick);
    ai::VoiceEndpoint endpoint;
    endpoint.base_url = p.base_url && *p.base_url ? std::wstring(p.base_url) : TidyUrl(EditText(Edit(u, AsstField::VoiceUrl)));
    if (endpoint.base_url.empty()) {
        Focus(u, AsstField::VoiceUrl);
        return;
    }
    endpoint.key = Trim(EditText(Edit(u, AsstField::VoiceKey)));
    if (endpoint.key.empty() && SharedSiliconKey(p.id)) (void)ai::LoadServiceKey("silicon", endpoint.key);
    if (endpoint.key.empty()) {
        Focus(u, AsstField::VoiceKey);
        return;
    }
    const ai::VoicePrefs voice = LoadVoice();
    StartVoiceTry(s, u, std::move(endpoint), p.id, voice.service == p.id ? voice.model : std::string(), true);
    Redraw(s);
}

void VoiceTryDone(AppState& s, AsstPageUi& u, AsstPageUi::VoiceJob& job) {
    const bool listed = job.list.Ok() && !job.list.models.empty();
    if (listed) {
        u.voice_models = job.list.models;
        u.voice_listed = true;
    }
    if (u.voice_listing) {   // only the list: the drop-down shows it
        u.voice_listing = false;
        return;
    }
    const bool ok = listed && job.run.Ok();
    if (!ok) {
        u.voice_error = VoiceFailure(job.list, job.run, u.voice_job_url);
        return;
    }
    if (job.setup) {
        if (!u.voice_setup) return;
        const ai::VoicePreset& p = VoicePresetAt(u.voice_pick);
        std::wstring key = Trim(EditText(Edit(u, AsstField::VoiceKey)));
        if (key.empty() && SharedSiliconKey(p.id)) (void)ai::LoadServiceKey("silicon", key);
        const bool saved = !key.empty() && ai::SaveVoiceKey(key);
        Wipe(key);
        if (!saved) {
            u.voice_error = Text(I::AiVoiceErrSave);
            return;
        }
        ai::VoicePrefs voice = LoadVoice();
        voice.service = p.id;
        voice.base_url = json::Utf8(u.voice_job_url);
        voice.model = job.model;
        if (!SaveVoice(s, voice)) {
            u.voice_error = Text(I::AiVoiceErrSave);
            return;
        }
        u.voice_setup = false;
        ClearEdit(u, AsstField::VoiceKey);
        SetVoiceReveal(u, false);
        HideVoiceEdits(s, u);
    }
    u.voice_ok = VoiceOk(job.run.ms);
}

// The saved service with its key: for the model list and a new model's try.
bool SavedVoiceEndpoint(ai::VoiceEndpoint& endpoint, const ai::VoicePrefs& voice) {
    if (!voice.Configured()) return false;
    endpoint.base_url = json::Wide(voice.base_url);
    if (!ai::LoadVoiceKey(endpoint.key) || endpoint.key.empty()) return false;
    return true;
}

// The model drop-down lists what the service offers: read it once per run.
void PrefetchVoiceModels(AppState& s, AsstPageUi& u) {
    if (u.voice_listed || u.voice_list_tried || u.voice_job || u.voice_setup) return;
    u.voice_list_tried = true;   // once; a failure waits for the next 连接
    const ai::VoicePrefs voice = LoadVoice();
    ai::VoiceEndpoint endpoint;
    if (!SavedVoiceEndpoint(endpoint, voice)) return;
    StartVoiceTry(s, u, std::move(endpoint), voice.service, voice.model, false, true);
}

void VoiceServiceMenu(AppState& s, AsstPageUi& u) {
    const ai::VoicePrefs voice = LoadVoice();
    const bool set = voice.Configured();
    size_t n = 0;
    const ai::VoicePreset* presets = ai::VoicePresets(n);
    static constexpr I kSubs[] = {I::AsstVoiceSubSilicon, I::AsstVoiceSubOpenAi, I::AsstVoiceSubCustom};
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < n; ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = VoiceServiceName(presets[i].id);
        item.shortcut = Text(kSubs[(std::min)(i, std::size(kSubs) - 1)]);
        item.radio_group = true;
        item.radio = u.voice_setup ? static_cast<int>(i) == u.voice_pick : set && voice.service == presets[i].id;
        items.push_back(std::move(item));
    }
    const int remove = static_cast<int>(n) + 1;
    if (set) {
        items.back().separator_after = true;
        ui::FluentMenuItem item;
        item.command = remove;
        item.text = Text(I::AiVoiceRemove);
        item.glyph = L"\xE74D";
        items.push_back(std::move(item));
    }
    const int command = Dropdown(s, 4, std::move(items));
    if (command == remove && set) {
        VoiceCancel(s, u);
        ResetVoiceTry(s, u);
        (void)ai::ClearVoiceKey();
        ai::VoicePrefs cleared = voice;
        cleared.service.clear();
        cleared.base_url.clear();
        cleared.model.clear();
        (void)SaveVoice(s, cleared);
        u.voice_models.clear();
        u.voice_listed = false;
        u.voice_list_tried = false;
        return;
    }
    if (command < 1 || command > static_cast<int>(n)) return;
    const int index = command - 1;
    if (!u.voice_setup && set && voice.service == presets[index].id) return;   // the one in use
    if (u.voice_setup && index == u.voice_pick) return;
    VoiceToSetup(s, u, index);
}

void VoiceModelMenu(AppState& s, AsstPageUi& u) {
    ai::VoicePrefs voice = LoadVoice();
    std::vector<std::string> ids = u.voice_models;
    if (!voice.model.empty() && std::find(ids.begin(), ids.end(), voice.model) == ids.end()) ids.insert(ids.begin(), voice.model);
    std::vector<ui::FluentMenuItem> items;
    for (size_t i = 0; i < ids.size() && i < 60; ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = json::Wide(ids[i]);
        item.radio_group = true;
        item.radio = ids[i] == voice.model;
        items.push_back(std::move(item));
    }
    if (u.voice_listing) {
        if (!items.empty()) items.back().separator_after = true;
        ui::FluentMenuItem item;
        item.text = Text(I::AiVoiceTesting);
        item.enabled = false;
        items.push_back(std::move(item));
    }
    const int command = Dropdown(s, 5, std::move(items));
    if (command < 1 || command > static_cast<int>(ids.size())) return;
    const std::string chosen = ids[static_cast<size_t>(command - 1)];
    if (chosen == voice.model) return;
    voice.model = chosen;
    if (!SaveVoice(s, voice)) return;
    // The new one has to recognize too: its try shows under the row.
    ai::VoiceEndpoint endpoint;
    if (SavedVoiceEndpoint(endpoint, voice)) StartVoiceTry(s, u, std::move(endpoint), voice.service, chosen, false);
}

void ReadMics(AsstPageUi& u) {
    u.mics = ai::ListVoiceMics(&u.default_mic);
    u.mics_read = true;
}

std::wstring MicName(const AsstPageUi& u, const ai::VoicePrefs& voice) {
    const std::wstring want = json::Wide(voice.mic);
    for (const ai::VoiceMic& m : u.mics)
        if (!want.empty() && m.id == want) return m.name;
    // 系统默认 alone: a device name rarely fits, and the menu lists them.
    return u.mics.empty() ? Text(I::AiVoiceMicNone) : Text(I::AiVoiceMicDefault);
}

void VoiceMicMenu(AppState& s, AsstPageUi& u) {
    ReadMics(u);
    ai::VoicePrefs voice = LoadVoice();
    const std::wstring want = json::Wide(voice.mic);
    std::vector<ui::FluentMenuItem> items;
    {
        ui::FluentMenuItem item;
        item.command = 1;
        item.text = Text(I::AiVoiceMicDefault);
        item.radio_group = true;
        item.radio = voice.mic.empty();
        item.separator_after = !u.mics.empty();
        items.push_back(std::move(item));
    }
    for (size_t i = 0; i < u.mics.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 2;
        item.text = u.mics[i].name;
        item.radio_group = true;
        item.radio = !want.empty() && u.mics[i].id == want;
        items.push_back(std::move(item));
    }
    const int command = Dropdown(s, 6, std::move(items));
    if (command <= 0) return;
    const std::string mic = command == 1 || static_cast<size_t>(command - 2) >= u.mics.size()
                                ? std::string() : json::Utf8(u.mics[static_cast<size_t>(command - 2)].id);
    if (mic == voice.mic) return;
    voice.mic = mic;
    (void)SaveVoice(s, voice);
}

void SetVoicePause(AppState& s, int segment) {
    static constexpr int kPause[] = {1, 2, 0};
    ai::VoicePrefs voice = LoadVoice();
    const int pause = kPause[std::clamp(segment, 0, 2)];
    if (voice.pause == pause) return;
    voice.pause = pause;
    (void)SaveVoice(s, voice);
}

// The page's tiles: the first screen, then 更多.
std::vector<const ai::Provider*> TileOrder() {
    std::vector<const ai::Provider*> order;
    for (const ai::Provider& p : ai::Providers()) if (!p.more) order.push_back(&p);
    for (const ai::Provider& p : ai::Providers()) if (p.more) order.push_back(&p);
    return order;
}

std::vector<std::string> OtherSaved() {
    std::vector<std::string> ids;
    const ai::Provider& current = ai::CurrentProvider();
    for (const ai::Provider* p : TileOrder())
        if (p->id != std::string_view(current.id) && ai::HasServiceKey(p->id)) ids.emplace_back(p->id);
    return ids;
}

// ---- The view -------------------------------------------------------------

// What does not change with typing, read at most every 2 s or after a change.
struct Cached : app::AssistantSettingsFields {
    ui::AssistantPageView view;
};

Cached& Cache() {
    static auto* cache = new Cached;
    return *cache;
}

void Refresh(Cached& cache) {
    auto& v = cache.view;
    bool malformed = false;
    const ai::Settings settings = ai::LoadSettings(&malformed);
    cache.Assign(settings, ai::Providers(), ServiceUrl);
    const ai::Provider& current = ai::CurrentProvider();
    v = {};
    v.enabled = settings.enabled;
    v.in_use = ai::HasServiceKey(current.id);
    for (const ai::Provider* p : TileOrder()) {
        ui::AsstTileView tile;
        tile.id = p->id;
        tile.name = ai::ProviderName(*p);
        tile.sub = ai::ProviderSub(*p);
        tile.current = v.in_use && p == &current;
        tile.saved = ai::HasServiceKey(p->id);
        v.tiles.push_back(std::move(tile));
    }
    v.model = json::Wide(settings.assistant.model);
    // 思考强度 follows the default model: its levels, a switch, 始终思考, or
    // nothing (the foot says why).
    const ai::ThinkSpec think = ai::ModelThinking(current, settings.assistant.model);
    const ai::ThinkLevel level = ai::SnapThinking(think, PreferredThinking(settings));
    switch (think.kind) {
        case ai::ThinkKind::Levels:
            v.think = ui::AsstThink::Levels;
            for (int i = 0; i < think.count; ++i) v.think_names.push_back(ai::ThinkLevelLabel(think.levels[i]));
            v.think_chosen = (std::max)(think.IndexOf(level), 0);
            v.think_always = !think.CanOff();
            break;
        case ai::ThinkKind::Toggle:
            v.think = ui::AsstThink::Toggle;
            v.think_chosen = level != ai::ThinkLevel::Off ? 1 : 0;
            break;
        case ai::ThinkKind::Fixed: v.think = ui::AsstThink::Fixed; break;
        case ai::ThinkKind::None: v.think = ui::AsstThink::None; v.think_none_model = v.model; break;
    }
    std::wstring key;
    if (current.needs_key && ai::LoadServiceKey(current.id, key) && !key.empty()) v.key_mask = ai::MaskModelKey(key);
    Wipe(key);
    for (const std::string& id : OtherSaved()) {
        const ai::Provider& p = ai::ProviderById(id);
        ui::AsstOtherView other;
        other.id = id;
        other.name = ai::ProviderName(p);
        if (const ai::ServicePrefs* prefs = SavedPrefs(settings, id)) other.model = json::Wide(prefs->model);
        v.others.push_back(std::move(other));
    }
    const ai::VoicePrefs& voice = settings.assistant.voice;
    if (voice.Configured()) {
        v.voice_stage = ui::AsstVoiceStage::Set;
        v.voice_saved = true;
        v.voice_id = voice.service;
        v.voice_name = VoiceServiceName(voice.service);
        v.voice_url = json::Wide(voice.base_url);
        v.voice_model = ai::ShortVoiceModel(voice.model);   // the menu shows the whole id
        v.voice_pause = PauseSegment(voice.pause);
        std::wstring voice_key;
        v.voice_has_key = ai::LoadVoiceKey(voice_key) && !voice_key.empty();
        if (v.voice_has_key) {
            v.voice_key_mask = ai::MaskModelKey(voice_key);
            if (voice.service == "siliconflow") {
                std::wstring model_key;
                v.voice_shared = ai::LoadServiceKey("silicon", model_key) && model_key == voice_key;
                Wipe(model_key);
            }
        }
        Wipe(voice_key);
    }
    v.mcp_installed = app::AiPackReady(app::QueryAiPackState());
    v.history = settings.assistant.history;
    const int days = settings.assistant.history_days;
    v.history_keep = days <= 0 ? 0 : days <= 30 ? 1 : days <= 90 ? 2 : 3;
}


} // namespace

void NoteAssistantConnection(bool ok, uint32_t ms) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_conn = ok ? ui::AsstConn::Ok : ui::AsstConn::Failed;
    g_conn_ms = ms;
    if (!g_conn_time) g_conn_time = new std::wstring;
    *g_conn_time = NowText();
    Changed();
}

void FillAssistantSettingsVm(AppState& s, ui::WindowViewModel& vm) {
    if (vm.settings_page == ui::kSettingsPageAssistant) g_seen = true;
    vm.settings_assistant_new = !g_seen;
    if (vm.settings_page != ui::kSettingsPageAssistant) return;
    auto& slot = s.aiAssistant.page;
    if (!slot) slot = std::make_shared<AsstPageUi>();
    AsstPageUi& u = *slot;
    Start(s, u);
    Cached& cache = Cache();
    const ULONGLONG now = GetTickCount64();
    const unsigned epoch = g_epoch.load(std::memory_order_relaxed);
    if (cache.NeedsRefresh(now, epoch)) {
        Refresh(cache);
        cache.at = now;
        cache.epoch = epoch;
    }
    ui::AssistantPageView& v = vm.settings_assistant;
    v = cache.view;
    // A key removed elsewhere (or the file): back to the tiles.
    if (u.stage == AsstStage::Connected && !v.in_use) u.stage = AsstStage::Pick;
    v.stage = u.stage;
    v.think_drag = u.think_drag;
    const ai::Provider& p = u.stage == AsstStage::Setup ? ai::ProviderById(u.pick) : ai::CurrentProvider();
    v.id = p.id;
    v.name = ai::ProviderName(p);
    v.sub = ai::ProviderSub(p);
    v.needs_key = p.needs_key;
    v.custom = ai::IsCustomProvider(p);
    v.console = p.key_url && *p.key_url ? ai::ModelHost(p.key_url) : std::wstring();
    if (u.stage == AsstStage::Setup) {
        v.url = cache.SetupUrl(p.id);
    } else v.url = AiAgentBaseUrl();
    v.url_default = !v.custom && v.url == std::wstring(p.base_url);
    v.url_editing = u.url_editing;
    v.key_typed = u.lengths[static_cast<int>(AsstField::Key)] > 0;
    v.url_typed = !u.url_editing || u.lengths[static_cast<int>(AsstField::Url)] > 0;
    v.revealed = u.revealed;
    v.connecting = u.connecting;
    v.error_title = u.error_title;
    v.error_sub = u.error_sub;
    v.fresh = u.fresh;
    v.fresh_manual = u.fresh_manual;
    v.model_editing = u.model_editing;
    if (u.stage != AsstStage::Connected) v.others.clear();
    // 语音输入: being set up, or the saved one with the live bits.
    if (u.voice_setup) {
        const ai::VoicePreset& vp = VoicePresetAt(u.voice_pick);
        v.voice_stage = ui::AsstVoiceStage::Setup;
        v.voice_id = vp.id;
        v.voice_name = VoiceServiceName(vp.id);
        v.voice_custom = !vp.base_url || !*vp.base_url;
        v.voice_url = v.voice_custom ? std::wstring() : std::wstring(vp.base_url);
        const std::wstring console = VoiceConsoleUrl(vp.id);
        v.voice_console = console.empty() ? std::wstring() : ai::ModelHost(console);
        v.voice_shared = SharedSiliconKey(vp.id);
        v.voice_key_typed = u.lengths[static_cast<int>(AsstField::VoiceKey)] > 0;
        v.voice_url_typed = u.lengths[static_cast<int>(AsstField::VoiceUrl)] > 0;
        v.voice_revealed = u.voice_revealed;
    } else if (v.voice_stage == ui::AsstVoiceStage::Set) {
        if (!u.mics_read) ReadMics(u);
        ai::VoicePrefs mic;
        mic.mic = cache.voice_mic;
        v.voice_mic = MicName(u, mic);
        PrefetchVoiceModels(s, u);
    }
    v.voice_testing = VoiceTrying(u);
    v.voice_error = u.voice_error;
    v.voice_ok = u.voice_ok;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        v.conn = g_testing.load() ? ui::AsstConn::Testing : g_conn;
        const std::wstring host = ai::ModelHost(v.url);
        if (g_conn_time && g_conn == ui::AsstConn::Ok) {   // a failure: the badge says it, no timing
            std::wstring line = Fill(Text(I::AsstHostLine), L"{host}", host);
            line = Fill(std::move(line), L"{time}", *g_conn_time);
            v.host_line = Fill(std::move(line), L"{ms}", std::to_wstring(g_conn_ms));
        } else {
            v.host_line = host;
        }
    }
}

namespace {
float ThinkPositionAt(AppState& s, int x) {
    const ui::WindowViewModel vm = BuildVm(s, false);
    const D2D1_RECT_F rect = D2D1::RectF(0, 0, static_cast<float>(s.compositor.Width()),
                                         static_cast<float>(s.compositor.Height()));
    return s.renderer.AssistantThinkPositionAt(vm, rect, static_cast<float>(x));
}
} // namespace

bool AssistantThinkDragging(AppState& s) {
    const AsstPageUi* u = Ui(s);
    return u && u->think_drag >= 0.0f;
}

void AssistantThinkDrag(AppState& s, int x) {
    AsstPageUi* u = Ui(s);
    if (!u || u->think_drag < 0.0f) return;
    const float at = ThinkPositionAt(s, x);
    if (at < 0.0f || at == u->think_drag) return;
    u->think_drag = at;
    Redraw(s);
}

bool AssistantThinkDragEnd(AppState& s) {
    AsstPageUi* u = Ui(s);
    if (!u || u->think_drag < 0.0f) return false;
    const int stop = static_cast<int>(std::lround(u->think_drag));
    u->think_drag = -1.0f;
    // The stop already shown keeps the preference (it may be a level this
    // model lacks: 超高 shows as 高 on 豆包 and comes back on GPT-5.5).
    const ai::Settings settings = ai::LoadSettings();
    const ai::ThinkSpec think = DefaultModelThinking(settings);
    if (think.kind == ai::ThinkKind::Levels && stop >= 0 && stop < think.count &&
        stop != think.IndexOf(ai::SnapThinking(think, PreferredThinking(settings))))
        SetThinking(s, think.levels[stop]);
    Changed();
    Redraw(s);
    return true;
}

void AssistantSettingsClick(AppState& s, int target) {
    AsstPageUi* up = Ui(s);
    if (!up) return;
    AsstPageUi& u = *up;
    if (target == ui::kAsstEnabled) {
        bool enabled = true;
        if (!ai::UpdateAiSettings([&](ai::Settings& settings) {
            settings.enabled = !settings.enabled;
            enabled = settings.enabled;
        })) return;
        s.aiAssistant.enabled = enabled;
        if (!enabled) {
            if (s.aiAssistant.chat) AiChatStop(s);
            AiAssistantClose(s, false);
        }
        Changed();
        Redraw(s);
        return;
    }
    const std::vector<const ai::Provider*> tiles = TileOrder();
    if (target >= ui::kAsstTileBase && target < ui::kAsstTileBase + static_cast<int>(tiles.size())) {
        const ai::Provider& p = *tiles[static_cast<size_t>(target - ui::kAsstTileBase)];
        if (&p == &ai::CurrentProvider() && InUse()) ToConnected(s, u);
        else if (ai::HasServiceKey(p.id)) SwitchTo(s, u, p);
        else ToSetup(s, u, p);
        Changed();
        Redraw(s);
        return;
    }
    if (target >= ui::kAsstThinkBase && target < ui::kAsstThinkBase + ai::kThinkLevels) {
        // 思考强度: the knob jumps to the stop pressed and then follows the
        // pointer; the level is kept on release (AssistantThinkDragEnd), like
        // the other settings sliders.
        u.think_drag = static_cast<float>(target - ui::kAsstThinkBase);
        SetCapture(s.hwnd);
        Redraw(s);
        return;
    }
    if (target >= ui::kAsstVoicePauseBase && target < ui::kAsstVoicePauseBase + 3) {
        SetVoicePause(s, target - ui::kAsstVoicePauseBase);
        Changed();
        Redraw(s);
        return;
    }
    if (target == ui::kAsstHistToggle ||
        (target >= ui::kAsstHistKeepBase && target < ui::kAsstHistKeepBase + 4)) {
        // 55 P3 历史对话: 保存对话历史 and 保留时间 (永久 / 30 天 / 90 天 / 1 年).
        static constexpr int kKeepDays[] = {0, 30, 90, 365};
        (void)UpdateAssistant([&](ai::AssistantPrefs& a) {
            if (target == ui::kAsstHistToggle) a.history = !a.history;
            else a.history_days = kKeepDays[target - ui::kAsstHistKeepBase];
        });
        Changed();
        Redraw(s);
        return;
    }
    if (target >= ui::kAsstOtherBase && target < ui::kAsstVoiceService) {
        const std::vector<std::string> others = OtherSaved();
        const size_t i = static_cast<size_t>(target - ui::kAsstOtherBase);
        if (i < others.size()) SwitchTo(s, u, ai::ProviderById(others[i]));
        Changed();
        Redraw(s);
        return;
    }
    switch (target) {
    case ui::kAsstTest:
        StartTest(s);
        break;
    case ui::kAsstChange:
        ToPick(s, u);
        break;
    case ui::kAsstCancel:
        ToConnected(s, u);
        break;
    case ui::kAsstRechoose:
        ToPick(s, u);
        break;
    case ui::kAsstReveal:
        SetReveal(u, !u.revealed);
        break;
    case ui::kAsstKeyLink: {
        const ai::Provider& p = ai::ProviderById(u.pick);
        if (p.key_url && *p.key_url) ShellExecuteW(s.hwnd, L"open", p.key_url, nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case ui::kAsstConnect:
        Connect(s, u);
        break;
    case ui::kAsstKeyField: Focus(u, AsstField::Key); break;
    case ui::kAsstUrlField: Focus(u, AsstField::Url); break;
    case ui::kAsstModelField: Focus(u, AsstField::Model); break;
    case ui::kAsstUrlEdit: {
        const std::wstring url = u.stage == AsstStage::Setup ? ServiceUrl(ai::LoadSettings(), ai::ProviderById(u.pick))
                                                             : AiAgentBaseUrl();
        u.url_editing = true;
        if (HWND e = Edit(u, AsstField::Url)) SetWindowTextW(e, url.c_str());
        u.lengths[static_cast<int>(AsstField::Url)] = static_cast<int>(url.size());
        Focus(u, AsstField::Url);
        break;
    }
    case ui::kAsstUrlSave: SaveUrl(s, u); break;
    case ui::kAsstUrlCancel:
    case ui::kAsstModelCancel: CancelEditing(s, u); break;
    case ui::kAsstModelSave: SaveModel(s, u); break;
    case ui::kAsstModel: ModelMenu(s, u); break;
    case ui::kAsstThinkToggle: {
        const ai::Settings settings = ai::LoadSettings();
        const bool on = ai::ThinksAt(DefaultModelThinking(settings), PreferredThinking(settings));
        SetThinking(s, on ? ai::ThinkLevel::Off : ai::ThinkLevel::High);
        break;
    }
    case ui::kAsstRekey:
        ToSetup(s, u, ai::CurrentProvider());
        break;
    case ui::kAsstRemoveKey:
        (void)ai::ClearServiceKey(ai::CurrentProvider().id);
        AiAgentKeyChanged();
        ToPick(s, u);
        break;
    case ui::kAsstVoiceService: VoiceServiceMenu(s, u); break;
    case ui::kAsstVoiceReveal: SetVoiceReveal(u, !u.voice_revealed); break;
    case ui::kAsstVoiceKeyLink: {
        const std::wstring url = VoiceConsoleUrl(VoicePresetAt(u.voice_pick).id);
        if (!url.empty()) ShellExecuteW(s.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    }
    case ui::kAsstVoiceConnect: VoiceConnect(s, u); break;
    case ui::kAsstVoiceCancel: VoiceCancel(s, u); break;
    case ui::kAsstVoiceRekey: VoiceToSetup(s, u, VoicePresetIndex(LoadVoice().service)); break;
    case ui::kAsstVoiceModel: VoiceModelMenu(s, u); break;
    case ui::kAsstVoiceMic: VoiceMicMenu(s, u); break;
    case ui::kAsstVoiceKeyField: Focus(u, AsstField::VoiceKey); break;
    case ui::kAsstVoiceUrlField: Focus(u, AsstField::VoiceUrl); break;
    case ui::kAsstMcp:
        OpenSettingsTab(s, ui::kSettingsPageAi);
        break;
    default: break;
    }
    Changed();
    Redraw(s);
}

void AssistantSettingsLayout(AppState& s) {
    AsstPageUi* u = Ui(s);
    if (!u) return;
    for (int i = 0; i < ui::kAsstFields; ++i) Place(s, *u, static_cast<AsstField>(i));
    // A theme switch changes the colours under the hosted EDITs; repaint them
    // so a field that stays open doesn't keep the old background.
    D2D1_COLOR_F back{}, fore{}, hint{};
    s.renderer.SettingsAssistantEditColors(back, fore, hint);
    const COLORREF back_ref = ToColorRef(back);
    const COLORREF fore_ref = ToColorRef(fore);
    if (back_ref != u->painted_back || fore_ref != u->painted_fore) {
        u->painted_back = back_ref;
        u->painted_fore = fore_ref;
        for (HWND edit : u->edits) {
            if (edit) InvalidateRect(edit, nullptr, TRUE);
        }
    }
}

HBRUSH AssistantSettingsCtlColor(AppState& s, HDC hdc, HWND edit) {
    AsstPageUi* u = Ui(s);
    if (!u || !edit || std::find(std::begin(u->edits), std::end(u->edits), edit) == std::end(u->edits)) return nullptr;
    D2D1_COLOR_F back{}, fore{}, hint{};
    s.renderer.SettingsAssistantEditColors(back, fore, hint);
    const COLORREF color = ToColorRef(back);
    if (!u->brush || u->brush_color != color) {
        if (u->brush) DeleteObject(u->brush);
        u->brush = CreateSolidBrush(color);
        u->brush_color = color;
    }
    if (hdc) {
        SetTextColor(hdc, ToColorRef(fore));
        SetBkColor(hdc, color);
        SetBkMode(hdc, OPAQUE);
    }
    return u->brush;
}

void ShowAssistantVoiceSettings(AppState& s) {
    OpenSettingsTab(s, ui::kSettingsPageAssistant);
    // Scroll so 语音输入's heading sits near the top (its service dropdown
    // is the card's first row; the heading and card padding are above it).
    const float w = static_cast<float>(s.compositor.Width());
    const float h = static_cast<float>(s.compositor.Height());
    const ui::WindowViewModel vm = BuildVm(s, false);
    const D2D1_RECT_F r = s.renderer.SettingsDropdownBounds(vm, 4, w, h);
    D2D1_RECT_F track{}, thumb{};
    float max_scroll = 0.0f;
    if (r.bottom <= r.top || !s.renderer.SettingsScrollbarGeometry(vm, w, h, track, thumb, max_scroll)) return;
    const float above = s.renderer.TitleBarHeight() + 96.0f * s.scale;
    s.settings.SetScroll(s.settings.scroll() + r.top - above, max_scroll);
    Redraw(s);
}

void AssistantSettingsDpiChanged(AppState& s) {
    AsstPageUi* u = Ui(s);
    if (!u) return;
    u->font_scale = 0.0f;
    EnsureFont(s, *u);
}

void AssistantSettingsDestroy(AppState& s) {
    AsstPageUi* u = Ui(s);
    if (!u) return;
    if (s.hwnd) KillTimer(s.hwnd, kConnectTimer);
    if (s.hwnd) KillTimer(s.hwnd, kVoiceTryTimer);
    ++u->gen;
    u->job.reset();
    ++u->voice_gen;
    u->voice_job.reset();
    for (AsstField f : {AsstField::Key, AsstField::VoiceKey}) {
        std::wstring key = EditText(u->edits[static_cast<int>(f)]);
        Wipe(key);
    }
    for (HWND& e : u->edits) {
        if (e) DestroyWindow(e);
        e = nullptr;
    }
    if (u->font) DeleteObject(u->font);
    u->font = nullptr;
    if (u->brush) DeleteObject(u->brush);
    u->brush = nullptr;
}

} // namespace pulse
