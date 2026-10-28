// ai_assistant_conn.cpp - 管理连接 card: state, EDITs, probe, saving.
#include "ai_assistant_conn.h"

#include "ai_assistant.h"
#include "ai_assistant_chat.h"
#include "ai_assistant_settings.h"
#include "ai_model_key.h"
#include "ai_model_list.h"
#include "ai_model_stream.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/ai_panel_conn.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <thread>

namespace pulse {

struct AiConnUi {
    bool open = false;
    bool saved = false;        // a key is stored
    bool changing = false;     // 更换: typing a new key over a stored one
    bool revealed = false;
    bool advanced = false;
    std::wstring saved_mask;   // sk-…9832
    // EDITs.
    HWND key_edit = nullptr, url_edit = nullptr;
    HFONT font = nullptr;
    float font_scale = 0.0f;
    int line_px = 0;
    HBRUSH brush = nullptr;
    COLORREF brush_color = 0;
    bool key_shown = false, url_shown = false;
    bool key_focus_pending = false, url_focus_pending = false;
    int key_len = 0;
    std::wstring key_tail;     // the typed key's last four
    // 测试连接.
    struct Job {
        uint64_t gen = 0;
        std::atomic<bool> done{false};
        app::ai::ModelProbe result;
    };
    std::shared_ptr<Job> job;
    uint64_t gen = 0;
    ui::AiConnTest test = ui::AiConnTest::None;
    bool save_after = false;
    bool save_failed = false;
    bool tested_saved = false;    // the result is the stored key's
    app::ai::ModelProbe probe;
    uint64_t tested_at = 0;
    std::wstring probe_host;
    std::wstring probe_url;   // the address that was probed
};

namespace {

using I = l10n::StringId;
constexpr UINT_PTR kConnSubclass = 0x4143;   // 'AC'
constexpr UINT_PTR kProbeTimer = 0x4150;     // 'AP'
constexpr wchar_t kKeysUrl[] = L"https://platform.deepseek.com/api_keys";
constexpr wchar_t kMaskChar = L'\u2022';

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring Fill(std::wstring text, std::wstring_view key, const std::wstring& value) {
    if (const size_t at = text.find(key); at != std::wstring::npos) text.replace(at, key.size(), value);
    return text;
}

COLORREF ToColorRef(D2D1_COLOR_F c) {
    auto b = [](float v) { return static_cast<BYTE>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return RGB(b(c.r), b(c.g), b(c.b));
}

void Redraw(AppState& s) {
    if (s.hwnd) InvalidateRect(s.hwnd, nullptr, FALSE);
}

AiConnUi* Ui(AppState& s) { return s.aiAssistant.conn.get(); }

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

bool Editing(const AiConnUi& u) { return !u.saved || u.changing; }

// The address the card would use: the field's, tidied; empty: the default.
std::wstring FieldUrl(const AiConnUi& u) {
    std::wstring url = Trim(EditText(u.url_edit));
    while (!url.empty() && url.back() == L'/') url.pop_back();
    if (url.empty()) return AiAgentDefaultBaseUrl();
    if (url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    return url;
}

void ReadKey(AiConnUi& u) {
    std::wstring key = Trim(EditText(u.key_edit));
    u.key_len = static_cast<int>(key.size());
    u.key_tail = key.size() > 4 ? key.substr(key.size() - 4) : std::wstring();
    Wipe(key);
}

void LoadSaved(AiConnUi& u) {
    std::wstring key;
    u.saved = app::ai::LoadModelKey(key) && !key.empty();
    u.saved_mask = u.saved ? app::ai::MaskModelKey(key) : std::wstring();
    Wipe(key);
}

void SaveUrl(AiConnUi& u) {
    const std::wstring url = FieldUrl(u);
    const std::string stored = url == AiAgentDefaultBaseUrl() ? std::string() : json::Utf8(url);
    bool malformed = false;
    app::ai::Settings settings = app::ai::LoadSettings(&malformed);
    if (malformed || settings.assistant.base_url == stored) return;
    settings.assistant.base_url = stored;
    (void)app::ai::SaveSettings(settings);
    AiAgentKeyChanged();
}

void SetReveal(AiConnUi& u, bool on) {
    u.revealed = on;
    if (u.key_edit) {
        SendMessageW(u.key_edit, EM_SETPASSWORDCHAR, on ? 0 : kMaskChar, 0);
        InvalidateRect(u.key_edit, nullptr, TRUE);
    }
}

void ClearKeyField(AiConnUi& u) {
    if (u.key_edit) SetWindowTextW(u.key_edit, L"");
    u.key_len = 0;
    u.key_tail.clear();
    SetReveal(u, false);
}

// A finished probe: show it, and on 保存 store the key once it works.
void ProbeDone(AppState& s, AiConnUi& u, app::ai::ModelProbe result) {
    u.probe = std::move(result);
    u.tested_at = GetTickCount64();
    u.save_failed = false;
    const bool ok = u.probe.http_status == 200;
    u.test = ok ? ui::AiConnTest::Ok : ui::AiConnTest::Bad;
    NoteAssistantConnection(ok);   // Settings > AI 助手 > 上次连接
    // The service's models, for the model menu (they do not depend on the key).
    if (ok) app::ai::StoreListedModels(u.probe_url, u.probe.models);
    if (ok && u.save_after && Editing(u)) {
        std::wstring key = Trim(EditText(u.key_edit));
        if (!key.empty() && app::ai::SaveModelKey(key)) {
            u.saved = true;
            u.changing = false;
            u.saved_mask = app::ai::MaskModelKey(key);
            u.tested_saved = true;
            ClearKeyField(u);
            SaveUrl(u);
            AiAgentKeyChanged();
        } else {
            u.test = ui::AiConnTest::Bad;
            u.save_failed = true;
        }
        Wipe(key);
    }
    u.save_after = false;
    Redraw(s);
}

void CALLBACK ProbeTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    AiConnUi* u = s ? Ui(*s) : nullptr;
    if (!u || !u->job) {
        KillTimer(hwnd, id);
        return;
    }
    if (u->job->gen != u->gen) {   // the key or the card changed meanwhile
        u->job.reset();
        KillTimer(hwnd, id);
        return;
    }
    if (!u->job->done.load(std::memory_order_acquire)) return;
    KillTimer(hwnd, id);
    auto job = std::move(u->job);
    ProbeDone(*s, *u, std::move(job->result));
}

void StartProbe(AppState& s, AiConnUi& u, bool save_after) {
    if (u.test == ui::AiConnTest::Running) return;
    app::ai::ModelEndpoint endpoint;
    endpoint.base_url = FieldUrl(u);
    const bool editing = Editing(u);
    if (editing) endpoint.key = Trim(EditText(u.key_edit));
    else (void)app::ai::LoadModelKey(endpoint.key);
    if (endpoint.key.empty()) return;
    u.test = ui::AiConnTest::Running;
    u.save_after = save_after;
    u.tested_saved = !editing;
    u.probe_host = app::ai::ModelHost(endpoint.base_url);
    u.probe_url = endpoint.base_url;
    auto job = std::make_shared<AiConnUi::Job>();
    job->gen = ++u.gen;
    u.job = job;
    std::thread([job, endpoint = std::move(endpoint)]() mutable {
        job->result = app::ai::ProbeModelService(std::move(endpoint));   // wipes the key
        job->done.store(true, std::memory_order_release);
    }).detach();
    if (s.hwnd) SetTimer(s.hwnd, kProbeTimer, 50, ProbeTimer);
    Redraw(s);
}

// The typed key changed: an earlier result no longer applies.
void KeyEdited(AppState& s, AiConnUi& u) {
    const int old_len = u.key_len;
    const std::wstring old_tail = u.key_tail;
    ReadKey(u);
    if (old_len == u.key_len && old_tail == u.key_tail) return;
    if ((old_len == 0) != (u.key_len == 0) && u.key_edit) InvalidateRect(u.key_edit, nullptr, TRUE);
    if (!u.tested_saved && u.test != ui::AiConnTest::None) {
        ++u.gen;
        u.test = ui::AiConnTest::None;
        u.save_after = false;
    }
    Redraw(s);
}

void Paste(AppState& s, AiConnUi& u) {
    if (!u.key_edit || !OpenClipboard(s.hwnd)) return;
    std::wstring text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text.assign(p, wcsnlen(p, 1024));
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    std::wstring key = Trim(text);
    Wipe(text);
    if (key.empty()) return;
    SetWindowTextW(u.key_edit, key.c_str());
    Wipe(key);
    KeyEdited(s, u);
}

void PaintPlaceholder(AppState& s, AiConnUi& u, HWND hwnd) {
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    HideCaret(hwnd);
    RECT r{};
    SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    HFONT old = static_cast<HFONT>(SelectObject(hdc, u.font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ToColorRef(s.renderer.AiPanel().PlaceholderColor()));
    const std::wstring text = hwnd == u.key_edit ? Text(I::AiConnKeyPlaceholder) : AiAgentDefaultBaseUrl();
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, old);
    ShowCaret(hwnd);
    ReleaseDC(hwnd, hdc);
}

void Save(AppState& s, AiConnUi& u, bool& close_menu);

LRESULT CALLBACK ConnEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiConnUi* u = Ui(s);
    if (!u) return DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            bool close_menu = false;
            Save(s, *u, close_menu);
            if (close_menu) AiAssistantEscape(s);
            Redraw(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            AiAssistantEscape(s);   // folds the card and the menu away
            return 0;
        }
        if (wp == VK_TAB) {
            HWND next = hwnd == u->key_edit ? u->url_edit : u->key_edit;
            if (next && IsWindowVisible(next)) SetFocus(next);
            else if (hwnd == u->key_edit && u->advanced) u->url_focus_pending = true;
            Redraw(s);
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
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        Redraw(s);
        return r;
    }
    case WM_PAINT: {
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        if (GetWindowTextLengthW(hwnd) == 0) PaintPlaceholder(s, *u, hwnd);
        return r;
    }
    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, ConnEditProc, kConnSubclass);
        break;
    default:
        break;
    }
    const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL:
        if (hwnd == u->key_edit) KeyEdited(s, *u);
        else if (GetWindowTextLengthW(hwnd) <= 1) InvalidateRect(hwnd, nullptr, TRUE);   // placeholder
        if (hwnd == u->url_edit) Redraw(s);
        break;
    default:
        break;
    }
    return r;
}

void EnsureFont(AppState& s, AiConnUi& u) {
    if (u.font && u.font_scale == s.scale) return;
    if (u.font) DeleteObject(u.font);
    u.font = ui::typography::CreateEditFont(s.scale, 13.0f);
    u.font_scale = s.scale;
    u.line_px = 0;
    for (HWND e : {u.key_edit, u.url_edit}) {
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
    const DWORD style = WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0);
    HWND e = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", style, 0, 0, 0, 0, s.hwnd, nullptr,
                             GetModuleHandleW(nullptr), nullptr);
    if (!e) return nullptr;
    SetLayeredWindowAttributes(e, 0, 255, LWA_ALPHA);
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 512, 0);
    if (password) SendMessageW(e, EM_SETPASSWORDCHAR, kMaskChar, 0);
    SetWindowSubclass(e, ConnEditProc, kConnSubclass, reinterpret_cast<DWORD_PTR>(&s));
    return e;
}

void EnsureEdits(AppState& s, AiConnUi& u) {
    if (!s.hwnd) return;
    if (!u.key_edit) u.key_edit = MakeEdit(s, true);
    if (!u.url_edit) {
        u.url_edit = MakeEdit(s, false);
        if (u.url_edit) SetWindowTextW(u.url_edit, AiAgentBaseUrl().c_str());
    }
    u.font_scale = 0.0f;
    EnsureFont(s, u);
}

void HideEdit(AppState& s, HWND edit, bool& shown) {
    if (!edit || !shown) return;
    const bool had_focus = GetFocus() == edit;
    ShowWindow(edit, SW_HIDE);
    shown = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

void Place(AppState& s, AiConnUi& u, HWND edit, bool& shown, int field, bool& focus_pending) {
    D2D1_RECT_F r{};
    if (!edit || !s.renderer.AiPanel().ConnFieldRect(field, r)) {
        HideEdit(s, edit, shown);
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
    if (!placed || !shown) {
        SetWindowPos(edit, HWND_TOP, x, y, w, line, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        shown = true;
        Redraw(s);   // the card stops drawing the text itself
    }
    if (focus_pending) {
        focus_pending = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) {
            SetFocus(edit);
            const int n = GetWindowTextLengthW(edit);
            SendMessageW(edit, EM_SETSEL, n, n);
        }
    }
}

void Save(AppState& s, AiConnUi& u, bool& close_menu) {
    if (u.test == ui::AiConnTest::Running) return;
    if (!Editing(u)) {
        // 完成: keep a changed address, fold away.
        SaveUrl(u);
        close_menu = true;
        return;
    }
    if (u.key_len == 0) return;
    StartProbe(s, u, true);
}

bool KeyFocused(const AiConnUi& u) { return u.key_edit && GetFocus() == u.key_edit; }

} // namespace

void AiConnOpen(AppState& s) {
    auto& slot = s.aiAssistant.conn;
    if (!slot) slot = std::make_shared<AiConnUi>();
    AiConnUi& u = *slot;
    if (u.open) return;
    u.open = true;
    LoadSaved(u);
    u.changing = false;
    EnsureEdits(s, u);
    ClearKeyField(u);
    if (u.url_edit) SetWindowTextW(u.url_edit, AiAgentBaseUrl().c_str());
    u.advanced = AiAgentBaseUrl() != AiAgentDefaultBaseUrl();
    // A stored key's last result stays (上次检查); anything else starts over.
    if (!(u.tested_saved && u.saved && u.test != ui::AiConnTest::Running)) {
        u.test = ui::AiConnTest::None;
        u.tested_saved = false;
    }
    u.key_focus_pending = !u.saved;
    Redraw(s);
}

bool AiConnIsOpen(const AppState& s) { return s.aiAssistant.conn && s.aiAssistant.conn->open; }

void AiConnClose(AppState& s) {
    AiConnUi* u = Ui(s);
    if (!u || !u->open) return;
    u->open = false;
    ++u->gen;   // a running probe's answer is dropped
    u->job.reset();
    if (u->test == ui::AiConnTest::Running || !u->tested_saved) u->test = ui::AiConnTest::None;
    u->save_after = false;
    u->changing = false;
    u->key_focus_pending = u->url_focus_pending = false;
    HideEdit(s, u->key_edit, u->key_shown);
    HideEdit(s, u->url_edit, u->url_shown);
    ClearKeyField(*u);
    if (s.hwnd) KillTimer(s.hwnd, kProbeTimer);
    Redraw(s);
}

void AiConnFill(AppState& s, ui::AiPanelViewModel& p) {
    AiConnUi* up = Ui(s);
    if (!up || !up->open) return;
    AiConnUi& u = *up;
    auto v = std::make_shared<ui::AiConnView>();
    const bool editing = Editing(u);
    const std::wstring url = FieldUrl(u);
    const std::wstring host = app::ai::ModelHost(url);
    const int http = u.probe.http_status;
    const bool bad = u.test == ui::AiConnTest::Bad;
    v->title = Text(I::AiAsstManage);
    v->service = L"DeepSeek";
    v->service_sub = Fill(Text(I::AiConnFormat), L"{host}", host);
    if (bad && !u.save_failed) {
        v->status = ui::AiConnStatus::Bad;
        v->status_text = Text(http == 401 ? I::AiConnStatusBadKey : I::AiConnStatusFailed);
    } else {
        v->status = u.saved ? ui::AiConnStatus::Ok : ui::AiConnStatus::Off;
        v->status_text = Text(u.saved ? I::AiConnStatusOn : I::AiConnStatusOff);
    }
    v->key_label = L"API Key";
    v->key_link = Text(I::AiConnKeyLink);
    v->key = !editing ? ui::AiConnKey::Saved : u.key_len > 0 ? ui::AiConnKey::Typing : ui::AiConnKey::Empty;
    v->key_focused = KeyFocused(u) || u.key_focus_pending;
    v->key_error = bad && editing && http == 401;
    v->key_revealed = u.revealed;
    v->key_edit = editing && (v->key_focused || u.revealed || u.key_len == 0);
    if (!editing) {
        v->key_mask = u.saved_mask;
    } else if (u.revealed) {
        std::wstring key = Trim(EditText(u.key_edit));
        v->key_mask = key;   // only drawn for a frame or two while the card moves
        Wipe(key);
    } else if (u.key_len > 0) {
        const int dots = u.key_len > 4 ? (std::min)(u.key_len - 4, 28) : u.key_len;
        v->key_mask.assign(static_cast<size_t>(dots), kMaskChar);
        v->key_mask += u.key_tail;
    }
    v->key_placeholder = Text(I::AiConnKeyPlaceholder);
    v->paste = Text(I::AiConnPaste);
    v->change = Text(I::AiConnChange);
    v->key_help = Text(I::AiConnKeyHelp);

    v->test = u.test;
    if (u.test == ui::AiConnTest::Running) {
        v->test_title = Fill(Text(I::AiConnTesting), L"{host}", u.probe_host);
    } else if (u.test == ui::AiConnTest::Ok) {
        v->test_title = Text(I::AiConnOkTitle);
        v->test_detail = Fill(Text(I::AiConnOkMs), L"{n}", std::to_wstring(u.probe.ms));
        const uint64_t mins = (GetTickCount64() - u.tested_at) / 60000;
        v->test_sub = mins == 0 ? Text(I::AiConnCheckedNow) : Fill(Text(I::AiConnCheckedMin), L"{n}", std::to_wstring(mins));
    } else if (bad) {
        const std::wstring code = std::to_wstring(http);
        if (u.save_failed) {
            v->test_title = Text(I::AiConnErrSave);
            v->test_sub = Text(I::AiConnErrSaveSub);
        } else if (http == 401) {
            v->test_title = Text(I::AiConnErrAuth);
            v->test_sub = Text(I::AiConnErrAuthSub);
        } else if (http == 402) {
            v->test_title = Text(I::AiConnErrBalance);
            v->test_sub = Text(I::AiConnErrBalanceSub);
        } else if (http == 429) {
            v->test_title = Text(I::AiConnErrRate);
            v->test_sub = Text(I::AiConnErrRateSub);
        } else if (http >= 500) {
            v->test_title = Fill(Text(I::AiConnErrServer), L"{code}", code);
            v->test_sub = Text(I::AiConnErrServerSub);
        } else if (http == 0) {
            v->test_title = Fill(Text(I::AiConnErrNetwork), L"{host}", u.probe_host);
            v->test_sub = Text(I::AiConnErrNetworkSub);
        } else {
            v->test_title = Fill(Text(I::AiConnErrOther), L"{code}", code);
            v->test_sub = Text(I::AiConnErrOtherSub);
        }
    }
    if (u.test == ui::AiConnTest::Ok) {
        v->models_heading = Text(I::AiConnModels);
        // The ids the service lists, in its order and as it names them; the
        // official DeepSeek two get a line. 默认 marks the one requests use.
        // A long list shows its first rows (the model menu has them all).
        struct Known { const char* id; I desc; };
        constexpr Known kKnown[] = {{"deepseek-flash", I::AiConnModelFlash}, {"deepseek-v4-pro", I::AiConnModelPro}};
        constexpr size_t kRows = 5;
        const auto& listed = u.probe.models;
        const std::wstring current = AiAssistantModelName(s);
        std::vector<size_t> rows;
        for (size_t i = 0; i < listed.size() && rows.size() < kRows; ++i) rows.push_back(i);
        if (listed.size() > kRows) {
            for (size_t i = kRows; i < listed.size(); ++i)
                if (json::Wide(listed[i]) == current) rows.back() = i;
            v->models_heading += L" \u00B7 " + std::to_wstring(listed.size());
        }
        for (const size_t i : rows) {
            std::wstring desc;
            for (const Known& k : kKnown)
                if (listed[i] == k.id) desc = Text(k.desc);
            std::wstring name = json::Wide(listed[i]);
            std::wstring tag = name == current ? Text(I::AiConnDefault) : std::wstring();
            v->models.push_back({std::move(name), std::move(desc), std::move(tag)});
        }
        if (v->models.empty()) v->models_heading.clear();
    }

    v->advanced_open = u.advanced;
    v->advanced = Text(I::AiConnAdvanced);
    v->url_label = Text(I::AiConnUrl);
    v->url_help = Text(I::AiConnUrlHelp);
    v->url_focused = u.url_edit && (GetFocus() == u.url_edit || u.url_focus_pending);
    v->url_edit = u.advanced;
    v->url_text = EditText(u.url_edit);
    v->url_placeholder = AiAgentDefaultBaseUrl();
    v->safe = Text(I::AiConnSafe);

    if (u.saved) v->remove = Text(I::AiConnRemove);
    v->test_button = Text(I::AiConnTest);
    v->save_button = Text(editing ? I::AiConnSave : I::AiConnDone);
    const bool running = u.test == ui::AiConnTest::Running;
    v->can_test = !running && (editing ? u.key_len > 0 : u.saved);
    v->can_save = !running && (editing ? u.key_len > 0 : true);
    p.conn = std::move(v);
}

bool AiConnActivate(AppState& s, const ui::AiPanelHit& hit, bool& close_menu) {
    AiConnUi* up = Ui(s);
    if (!up || !up->open) return false;
    AiConnUi& u = *up;
    using P = ui::AiPanelPart;
    switch (hit.part) {
    case P::ConnBack:
        AiConnClose(s);
        return true;
    case P::ConnClose:
        close_menu = true;
        return true;
    case P::ConnCard:
        return true;
    case P::ConnLink:
        ShellExecuteW(s.hwnd, L"open", kKeysUrl, nullptr, nullptr, SW_SHOWNORMAL);
        return true;
    case P::ConnPaste:
        Paste(s, u);
        return true;
    case P::ConnEye:
        SetReveal(u, !u.revealed);
        u.key_focus_pending = true;
        return true;
    case P::ConnChange:
        u.changing = true;
        u.test = ui::AiConnTest::None;
        u.tested_saved = false;
        ClearKeyField(u);
        u.key_focus_pending = true;
        return true;
    case P::ConnAdvanced:
        u.advanced = !u.advanced;
        return true;
    case P::ConnTest:
        StartProbe(s, u, false);
        return true;
    case P::ConnSave:
        Save(s, u, close_menu);
        return true;
    case P::ConnRemove:
        (void)app::ai::ClearModelKey();
        AiAgentKeyChanged();
        LoadSaved(u);
        u.changing = false;
        u.test = ui::AiConnTest::None;
        u.tested_saved = false;
        ClearKeyField(u);
        u.key_focus_pending = true;
        return true;
    case P::ConnKeyField:
        u.key_focus_pending = true;
        return true;
    case P::ConnUrlField:
        u.url_focus_pending = true;
        return true;
    default:
        return false;
    }
}

void AiConnLayout(AppState& s) {
    AiConnUi* up = Ui(s);
    if (!up) return;
    AiConnUi& u = *up;
    if (!u.open) {
        HideEdit(s, u.key_edit, u.key_shown);
        HideEdit(s, u.url_edit, u.url_shown);
        return;
    }
    EnsureFont(s, u);
    Place(s, u, u.key_edit, u.key_shown, 0, u.key_focus_pending);
    Place(s, u, u.url_edit, u.url_shown, 1, u.url_focus_pending);
}

bool AiConnTip(const AppState& s, ui::AiPanelPart part, std::wstring& text) {
    using P = ui::AiPanelPart;
    switch (part) {
    case P::ConnBack: text = Text(I::AiConnBack); return true;
    case P::ConnClose: text = Text(I::AiAsstCollapse); return true;
    case P::ConnEye:
        text = Text(s.aiAssistant.conn && s.aiAssistant.conn->revealed ? I::AiConnHideKey : I::AiConnShowKey);
        return true;
    default: return false;
    }
}

bool AiConnIsField(ui::AiPanelPart part) {
    return part == ui::AiPanelPart::ConnKeyField || part == ui::AiPanelPart::ConnUrlField;
}

HBRUSH AiConnCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiConnUi* u = Ui(s);
    if (!u || !edit || (edit != u->key_edit && edit != u->url_edit)) return nullptr;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const COLORREF back = ToColorRef(view.CardColor());
    if (!u->brush || u->brush_color != back) {
        if (u->brush) DeleteObject(u->brush);
        u->brush = CreateSolidBrush(back);
        u->brush_color = back;
    }
    SetTextColor(hdc, ToColorRef(view.TextColor()));
    SetBkColor(hdc, back);
    SetBkMode(hdc, OPAQUE);
    return u->brush;
}

void AiConnDpiChanged(AppState& s) {
    AiConnUi* u = Ui(s);
    if (!u) return;
    u->font_scale = 0.0f;
    EnsureFont(s, *u);
    Redraw(s);
}

void AiConnDestroy(AppState& s) {
    AiConnUi* u = Ui(s);
    if (!u) return;
    ++u->gen;
    u->job.reset();
    if (s.hwnd) KillTimer(s.hwnd, kProbeTimer);
    if (u->key_edit) {
        SetWindowTextW(u->key_edit, L"");
        DestroyWindow(u->key_edit);
    }
    if (u->url_edit) DestroyWindow(u->url_edit);
    u->key_edit = u->url_edit = nullptr;
    if (u->font) DeleteObject(u->font);
    u->font = nullptr;
    if (u->brush) DeleteObject(u->brush);
    u->brush = nullptr;
}

} // namespace pulse
