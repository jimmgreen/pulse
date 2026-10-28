#include "ai_assistant_settings.h"
#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_voice.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_model_stream.h"
#include "ai_pack.h"
#include "ai_providers.h"
#include "ai_settings.h"
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
    AsstStage stage = AsstStage::Pick;
    std::string pick;                  // the service being set up
    bool url_editing = false, model_editing = false, revealed = false;
    // EDITs, by AsstField.
    HWND edits[3]{};
    bool shown[3]{};
    bool focus_pending[3]{};
    int lengths[3]{};
    HFONT font = nullptr;
    float font_scale = 0.0f;
    int line_px = 0;
    HBRUSH brush = nullptr;
    COLORREF brush_color = CLR_INVALID;
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
};

namespace {

// Bumped by every write to the settings or a key: the page view is re-read at once.
std::atomic<unsigned> g_epoch{1};
void Changed() { g_epoch.fetch_add(1, std::memory_order_relaxed); }

using I = l10n::StringId;
constexpr UINT_PTR kEditSubclass = 0x4153;   // 'AS'
constexpr UINT_PTR kConnectTimer = 0x4153;     // 'AS'
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
    if (f == AsstField::Key) return Text(I::AsstKeyPlaceholder);
    if (f == AsstField::Model) return Text(I::AsstModelPlaceholder);
    return L"https://";
}

// Enter: 连接 (setting up) or 保存; Esc: stop editing.
void SubmitFrom(AppState& s, AsstPageUi& u, HWND hwnd) {
    if (u.stage == AsstStage::Setup) Connect(s, u);
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
            CancelEditing(s, *u);
            Redraw(s);
            return 0;
        }
        if (wp == VK_TAB) {
            // Key → address (when it is a field) and back.
            HWND key = Edit(*u, AsstField::Key), url = Edit(*u, AsstField::Url);
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
        for (int i = 0; i < 3; ++i) {
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
    for (int i = 0; i < 3; ++i) {
        if (u.edits[i]) continue;
        u.edits[i] = MakeEdit(s, i == static_cast<int>(AsstField::Key));
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
    if (listed) ai::StoreListedModels(u.job_url, result.models);
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
            if (ok) ai::StoreListedModels(url, result.models);
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

void SetEffort(AppState& s, int effort) {
    effort = std::clamp(effort, 0, 3);
    if (!UpdateAssistant([&](ai::AssistantPrefs& a) { a.effort = effort; })) return;
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

void Paste(AppState& s, AsstPageUi& u) {
    HWND key = Edit(u, AsstField::Key);
    if (!key || !OpenClipboard(s.hwnd)) return;
    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text.assign(p, wcsnlen(p, 1024));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    std::wstring trimmed = Trim(text);
    Wipe(text);
    if (trimmed.empty()) return;
    SetWindowTextW(key, trimmed.c_str());
    Wipe(trimmed);
    Focus(u, AsstField::Key);
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
struct Cached {
    ULONGLONG at = 0;
    unsigned epoch = 0;
    ui::AssistantPageView view;
};

Cached& Cache() {
    static auto* cache = new Cached;
    return *cache;
}

void Refresh(ui::AssistantPageView& v) {
    bool malformed = false;
    const ai::Settings settings = ai::LoadSettings(&malformed);
    const ai::Provider& current = ai::CurrentProvider();
    v = {};
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
    v.effort = std::clamp(settings.assistant.effort, 0, 3);
    switch (current.think) {
        case ai::ThinkStyle::DeepSeek: v.think = ui::AsstThink::Levels4; break;
        case ai::ThinkStyle::Effort3: v.think = ui::AsstThink::Levels3; break;
        case ai::ThinkStyle::EnableThinking:
        case ai::ThinkStyle::ThinkingType: v.think = ui::AsstThink::Toggle; break;
        default: v.think = ui::AsstThink::None; break;
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
    v.voice_set = voice.Configured();
    v.voice_key = v.voice_set && ai::HasVoiceKey();
    if (voice.service == "siliconflow") v.voice_service = l10n::Get(I::AsstVoiceSiliconFlow);
    else if (voice.service == "openai") v.voice_service = L"OpenAI";
    else v.voice_service = l10n::Get(I::AsstVoiceCustom);
    v.voice_host = v.voice_set ? ai::ModelHost(json::Wide(voice.base_url)) : std::wstring{};
    v.mcp_installed = app::AiPackReady(app::QueryAiPackState());
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
    if (!cache.at || now - cache.at >= 2000 || cache.epoch != epoch) {
        Refresh(cache.view);
        cache.at = now;
        cache.epoch = epoch;
    }
    ui::AssistantPageView& v = vm.settings_assistant;
    v = cache.view;
    // A key removed elsewhere (or the file): back to the tiles.
    if (u.stage == AsstStage::Connected && !v.in_use) u.stage = AsstStage::Pick;
    v.stage = u.stage;
    const ai::Provider& p = u.stage == AsstStage::Setup ? ai::ProviderById(u.pick) : ai::CurrentProvider();
    v.id = p.id;
    v.name = ai::ProviderName(p);
    v.sub = ai::ProviderSub(p);
    v.needs_key = p.needs_key;
    v.custom = ai::IsCustomProvider(p);
    v.console = p.key_url && *p.key_url ? ai::ModelHost(p.key_url) : std::wstring();
    v.url = u.stage == AsstStage::Setup ? ServiceUrl(ai::LoadSettings(), p) : AiAgentBaseUrl();
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

void AssistantSettingsClick(AppState& s, int target) {
    AsstPageUi* up = Ui(s);
    if (!up) return;
    AsstPageUi& u = *up;
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
    if (target >= ui::kAsstThinkBase && target < ui::kAsstThinkBase + 4) {
        const int i = target - ui::kAsstThinkBase;
        SetEffort(s, ai::CurrentProvider().think == ai::ThinkStyle::Effort3 ? i + 1 : i);
        Changed();
        Redraw(s);
        return;
    }
    if (target >= ui::kAsstOtherBase) {
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
    case ui::kAsstPaste:
        Paste(s, u);
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
        const int effort = std::clamp(ai::LoadSettings().assistant.effort, 0, 3);
        SetEffort(s, effort > 0 ? 0 : 2);
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
    case ui::kAsstVoice:
        AiAssistantOpen(s);
        AiVoiceOpenConnection(s);
        break;
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
    for (AsstField f : {AsstField::Key, AsstField::Url, AsstField::Model}) Place(s, *u, f);
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
    ++u->gen;
    u->job.reset();
    std::wstring key = EditText(u->edits[static_cast<int>(AsstField::Key)]);
    Wipe(key);
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
