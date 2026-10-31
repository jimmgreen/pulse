// ai_assistant.cpp - Pulse 助手: panel state, composer EDIT, pointer routing.
#include "ai_assistant.h"

#include "ai_assistant_chat.h"
#include "ai_assistant_settings.h"
#include "ai_assistant_voice.h"
#include "ai_model_list.h"
#include "ai_providers.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "app_commands.h"
#include "app_navigation.h"
#include "places.h"
#include "keymap.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/fluent_menu.h"
#include "../ui/settings_page_ids.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <vector>

namespace pulse {
namespace {

namespace ai = app::ai;
using I = l10n::StringId;

constexpr UINT_PTR kComposerSubclass = 0x4149;   // 'AI'
constexpr UINT_PTR kTurnEditSubclass = 0x4145;   // 'AE': a message edited in place
constexpr UINT_PTR kHistEditSubclass = 0x4148;   // 'AH': 历史对话's search box and a title renamed
constexpr UINT_PTR kTipTimer = 0x4154;           // 'AT'
constexpr UINT kTipDelayMs = 520;
constexpr uint64_t kMenuFadeMs = 240;

// Models come from the connected service (ai_model_list.h) and are shown
// and sent as it names them. The two official DeepSeek ones get a short
// caption in the list and their line as its tip.
struct KnownModel { const char* id; I caption; I detail; };
constexpr KnownModel kKnownModels[] = {
    {"deepseek-flash", I::AiAsstModelFlashShort, I::AiAsstModelFlashDetail},
    {"deepseek-v4-pro", I::AiAsstModelProShort, I::AiAsstModelProDetail},
};
std::string g_model = "deepseek-flash";   // the chosen id (ai.json assistant.model)

const std::vector<std::string>& Listed() { return ai::ListedModels(AiAgentBaseUrl()); }

// The chosen model's place in the list: -1 when the service does not list it.
int ChosenIndex(const std::vector<std::string>& list) {
    const auto it = std::find(list.begin(), list.end(), g_model);
    return it == list.end() ? -1 : static_cast<int>(it - list.begin());
}

// The id a request uses: the chosen one if listed, else the service's first.
const std::string& CurrentModel() {
    const std::vector<std::string>& list = Listed();
    if (list.empty() || ChosenIndex(list) >= 0) return g_model;
    return list.front();
}
// The seven levels' lines (ai_providers.h ThinkLevel; ThinkLevelLabel names them).
constexpr I kLevelDetails[] = {I::AiAsstEffortOffDetail,  I::AiAsstEffortMinimalDetail, I::AiAsstEffortLowDetail,
                               I::AiAsstEffortMidDetail,  I::AiAsstEffortHighDetail,    I::AiAsstEffortXHighDetail,
                               I::AiAsstEffortMaxDetail};

std::wstring Text(I id) { return l10n::Get(id).c_str(); }

std::wstring LevelName(ai::ThinkLevel level) { return ai::ThinkLevelLabel(level); }

// 思考强度 is the model's (ai_providers.h ModelThinking): its own levels, a
// switch, always on, or none. ai.json keeps the level the user prefers and
// the slider shows the model's nearest one.
ai::ThinkLevel Preferred(const AiAssistantState& a) { return static_cast<ai::ThinkLevel>(std::clamp(a.think, 0, 6)); }
ai::ThinkSpec CurrentThinking() { return ai::ModelThinking(ai::CurrentProvider(), CurrentModel()); }
int StopOf(const ai::ThinkSpec& t, ai::ThinkLevel preferred) {
    return (std::max)(t.IndexOf(ai::SnapThinking(t, preferred)), 0);
}

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

void LoadPrefs(AiAssistantState& a) {
    if (a.prefs_loaded) return;
    a.prefs_loaded = true;
    const ai::Settings settings = ai::LoadSettings();
    a.enabled = settings.enabled;
    a.floating = settings.assistant.floating;
    a.think = std::clamp(settings.assistant.thinking, 0, 6);
    a.model = 0;
    if (!settings.assistant.model.empty()) g_model = settings.assistant.model;
}

void SavePrefs(const AiAssistantState& a) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    settings.assistant.floating = a.floating;
    settings.assistant.model = g_model;
    settings.assistant.thinking = a.think;
    (void)ai::SaveSettings(settings);
}

// The folder the assistant sees: the active tab's, if it is a real folder.
bool CurrentFolder(AppState& s, std::wstring& path, std::wstring& name, size_t& count) {
    app::Tab* tab = ActiveTab(s);
    if (!tab || tab->current_path.empty()) return false;
    if (tab->current_path.rfind(L"pulse:", 0) == 0) return false;
    path = tab->current_path;
    name = app::TabTitle(path);
    count = tab->EntryCount();
    return !name.empty();
}

std::vector<ui::AiSuggestionView> Suggestions(AppState& s) {
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    struct Sug { ui::AiIcon icon; ui::AiTint tint; I title; I detail; bool folder; };
    static constexpr Sug kSugs[] = {
        {ui::AiIcon::Search, ui::AiTint::Document, I::AiQSugTitle, I::AiQSugDetail, false},
        {ui::AiIcon::Rename, ui::AiTint::Image, I::AiRnSugTitle, I::AiRnSugDetail, false},
        {ui::AiIcon::Move, ui::AiTint::App, I::AiAsstSugSortTitle, I::AiAsstSugSortDetail, true},
        {ui::AiIcon::Size, ui::AiTint::Archive, I::AiAsstSugSizeTitle, I::AiAsstSugSizeDetail, true},
    };
    std::vector<ui::AiSuggestionView> out;
    for (const Sug& sug : kSugs) {
        // Folder suggestions only make sense with a folder open.
        if (sug.folder && !has_folder) continue;
        out.push_back({sug.icon, sug.tint, Fill(Text(sug.title), L"{folder}", folder), Text(sug.detail)});
    }
    return out;
}

void ApplyPanel(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    s.renderer.SetAiPanel(a.open, a.floating ? ui::AiPanelMode::Floating : ui::AiPanelMode::Docked, animate);
    Redraw(s);
}

void ReadComposerMetrics(AiAssistantState& a) {
    if (!a.edit) return;
    a.text_len = GetWindowTextLengthW(a.edit);
    a.lines = (std::max)(1, static_cast<int>(SendMessageW(a.edit, EM_GETLINECOUNT, 0, 0)));
}

std::wstring ComposerText(const AiAssistantState& a) {
    if (!a.edit) return {};
    const int n = GetWindowTextLengthW(a.edit);
    std::wstring text(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(a.edit, text.data(), n + 1);
    text.resize(static_cast<size_t>(n));
    return text;
}

// The composer's folder chip as the user bubble shows it ("" when removed).
std::wstring ContextText(AppState& s) {
    std::wstring path, folder;
    size_t count = 0;
    if (!CurrentFolder(s, path, folder, count) || s.aiAssistant.context_removed_for == path) return {};
    return Fill(Fill(Text(I::AiAsstContext), L"{folder}", folder), L"{n}", std::to_wstring(count));
}

std::wstring QuoteMarkdown(const std::wstring& quote);

bool Submit(AppState& s, const std::wstring& text) {
    if (Chat(s).busy) return false;
    // 引用: the quoted words lead the message as a Markdown block quote.
    AiAssistantState& a = s.aiAssistant;
    AiChatSubmit(s, a.quote.empty() ? text : QuoteMarkdown(a.quote) + L"\n\n" + text, ContextText(s));
    a.quote.clear();
    return true;
}

void SubmitComposer(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    std::wstring text = ComposerText(a);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    if (text.empty()) return;
    if (Submit(s, text) && a.edit) SetWindowTextW(a.edit, L"");
}

void SyncComposer(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    const int old_len = a.text_len, old_lines = a.lines;
    ReadComposerMetrics(a);
    if ((old_len == 0) != (a.text_len == 0) && a.edit) InvalidateRect(a.edit, nullptr, TRUE);   // placeholder
    if (old_len != a.text_len || old_lines != a.lines) Redraw(s);
}

// The placeholder over the empty EDIT, in the panel's text3.
void PaintPlaceholder(AppState& s, HWND hwnd) {
    AiAssistantState& a = s.aiAssistant;
    HDC hdc = GetDC(hwnd);
    if (!hdc) return;
    HideCaret(hwnd);
    RECT r{};
    SendMessageW(hwnd, EM_GETRECT, 0, reinterpret_cast<LPARAM>(&r));
    HFONT old = static_cast<HFONT>(SelectObject(hdc, a.font));
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, ToColorRef(s.renderer.AiPanel().PlaceholderColor()));
    const std::wstring text = Text(I::AiAsstPlaceholder);
    DrawTextW(hdc, text.c_str(), static_cast<int>(text.size()), &r, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
    SelectObject(hdc, old);
    ShowCaret(hwnd);
    ReleaseDC(hwnd, hdc);
}

void CloseMenu(AppState& s);

LRESULT CALLBACK ComposerProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiAssistantState& a = s.aiAssistant;
    // Text, caret, selection and the placeholder are presented by LumaText
    // like every other Pulse field (edit_host.h), in the panel's own body face
    // and colours; native painting stays as the fallback.
    const ui::AiPanelView& view = s.renderer.AiPanel();
    IDWriteTextFormat* format = view.ComposerFormat() ? view.ComposerFormat() : s.compositor.TextFormat();
    const D2D1_COLOR_F fore = view.TextColor();
    const D2D1_COLOR_F back = view.CardColor();
    const auto presented = [&](LRESULT& out) {
        const bool handled = ui::HandleChildEditMessage(s.compositor, format, fore, back, a.brush, hwnd, msg, wp, lp, out);
        if (handled || msg == WM_PAINT) {
            a.edit_back = ToColorRef(back);
            a.edit_fore = ToColorRef(fore);
        }
        return handled;
    };
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && (GetKeyState(VK_SHIFT) & 0x8000) == 0) {
            SubmitComposer(s);
            return 0;
        }
        if (wp == VK_TAB && (GetKeyState(VK_CONTROL) & 0x8000) == 0) {
            // Tab / Shift+Tab leave the panel for the file list (the panel's
            // other controls are drawn, not focusable).
            if (a.menu_open) CloseMenu(s);
            if (s.hwnd) SetFocus(s.hwnd);
            Redraw(s);
            return 0;
        }
        if (wp != VK_CONTROL && wp != VK_SHIFT && wp != VK_MENU) {
            // Pulse 助手's own key (Ctrl+J unless rebound) collapses from here.
            const app::KeyChord chord = app::CurrentChord(static_cast<UINT>(wp));
            for (const app::KeyChord bound : app::ActiveKeymap().EffectiveChords(app::KeyAction::AiAssistant))
                if (bound == chord) {
                    AiAssistantHotkey(s);
                    return 0;
                }
            for (const app::KeyChord bound : app::ActiveKeymap().EffectiveChords(app::KeyAction::AiHistory))
                if (bound == chord) {
                    AiAssistantHistoryKey(s);   // 55 P3 历史对话
                    return 0;
                }
        }
        if (wp == VK_ESCAPE) {
            if (a.menu_open) {
                CloseMenu(s);
                Redraw(s);
            } else if (a.chat && a.chat->busy) {
                AiChatStop(s);
            } else {
                AiAssistantClose(s);
            }
            return 0;
        }
        break;
    case WM_CHAR:
        // Enter sends (WM_KEYDOWN); Shift+Enter is a new line. No beeps.
        if ((wp == L'\r' || wp == L'\n') && (GetKeyState(VK_SHIFT) & 0x8000) == 0) return 0;
        if (wp == 27 || wp == L'\t') return 0;
        if (wp == 8 && GetKeyState(VK_CONTROL) < 0) return 0;   // Ctrl+H's backspace (历史对话)
        if (wp == 1) {   // Ctrl+A: select all (a multiline EDIT does not)
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        LRESULT r = 0;
        if (!presented(r)) r = DefSubclassProc(hwnd, msg, wp, lp);
        a.focused = msg == WM_SETFOCUS;
        Redraw(s);
        return r;
    }
    case WM_PAINT: {
        LRESULT r = 0;
        if (presented(r)) return r;
        r = DefSubclassProc(hwnd, msg, wp, lp);
        if (GetWindowTextLengthW(hwnd) == 0) PaintPlaceholder(s, hwnd);
        return r;
    }
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        presented(ignored);
        RemovePropW(hwnd, L"Pulse.EditMultiline");
        RemovePropW(hwnd, L"Pulse.EditCue");
        RemovePropW(hwnd, L"Pulse.EditCueColor");
        RemoveWindowSubclass(hwnd, ComposerProc, kComposerSubclass);
        break;
    }
    default: {
        LRESULT r = 0;
        if (presented(r)) return r;
        break;
    }
    }
    const LRESULT r = ui::DefPresentedChildEditProc(s.compositor, format, fore, back, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL: case WM_SIZE:
        SyncComposer(s);
        break;
    default:
        break;
    }
    return r;
}

void EnsureFont(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.font && a.font_scale == s.scale) return;
    if (a.font) DeleteObject(a.font);
    a.font = ui::typography::CreateEditFont(s.scale, 13.5f);
    a.font_scale = s.scale;
    a.line_px = 0;
    for (HWND e : {a.edit, a.turn_edit, a.hist_search, a.hist_rename})
        if (e && a.font) {
            SendMessageW(e, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
            SendMessageW(e, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
        }
    if (a.font) {
        HDC hdc = GetDC(s.hwnd);
        HFONT old = static_cast<HFONT>(SelectObject(hdc, a.font));
        TEXTMETRICW tm{};
        GetTextMetricsW(hdc, &tm);
        SelectObject(hdc, old);
        ReleaseDC(s.hwnd, hdc);
        a.line_px = static_cast<int>(tm.tmHeight);
    }
    // LumaText lays the text out at the same line height the composer is
    // measured with (1/64 px), and draws the placeholder itself.
    for (HWND e : {a.edit, a.turn_edit})
        if (e && a.line_px > 0)
            SetPropW(e, L"Pulse.EditMultiline", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(a.line_px) * 64u));
}

// The panel's text fields (the composer, a message edited in place): a
// redirected layered child like the other hosted editors (edit_host.h), but
// multi-line and wrapping (no ES_AUTOHSCROLL).
HWND CreatePanelEdit(AppState& s) {
    HWND e = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"",
                             WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!e) return nullptr;
    SetLayeredWindowAttributes(e, 0, 255, LWA_ALPHA);
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 8000, 0);
    return e;
}

void EnsureEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.edit || !s.hwnd) return;
    a.edit = CreatePanelEdit(s);
    if (!a.edit) return;
    a.font_scale = 0.0f;
    EnsureFont(s);
    SetWindowSubclass(a.edit, ComposerProc, kComposerSubclass, reinterpret_cast<DWORD_PTR>(&s));
    ReadComposerMetrics(a);
}

void HideEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit || !a.edit_visible) return;
    const bool had_focus = GetFocus() == a.edit;
    ShowWindow(a.edit, SW_HIDE);
    a.edit_visible = false;
    if (had_focus && s.hwnd) SetFocus(s.hwnd);
}

// 55 P2: a message edited in place. Enter sends it as a new version, Esc
// gives up, Shift+Enter is a new line.
void EndTurnEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    a.edit_id = 0;
    a.turn_edit_focus = false;
    if (a.turn_edit && a.turn_edit_visible) {
        const bool had_focus = GetFocus() == a.turn_edit;
        ShowWindow(a.turn_edit, SW_HIDE);
        a.turn_edit_visible = false;
        if (had_focus) a.focus_pending = true;   // back to the composer
    }
    Redraw(s);
}

void SendTurnEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.edit_id || !a.turn_edit || !a.chat) return;
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(a.turn_edit)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(a.turn_edit, text.data(), static_cast<int>(text.size()))));
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    while (!text.empty() && (text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    int block = -1;
    for (size_t i = 0; i < a.chat->blocks.size(); ++i)
        if (a.chat->blocks[i].id == a.edit_id) block = static_cast<int>(i);
    EndTurnEdit(s);
    if (!text.empty() && block >= 0) AiChatEditTurn(s, block, text);
}

void SyncTurnEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.turn_edit) return;
    const int lines = static_cast<int>(SendMessageW(a.turn_edit, EM_GETLINECOUNT, 0, 0));
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(a.turn_edit)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(a.turn_edit, text.data(), static_cast<int>(text.size()))));
    text.erase(std::remove(text.begin(), text.end(), L'\r'), text.end());
    if (lines != a.turn_edit_lines || text != a.turn_edit_text) {
        a.turn_edit_lines = (std::max)(1, lines);
        a.turn_edit_text = std::move(text);
        Redraw(s);
    }
}

LRESULT CALLBACK TurnEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiAssistantState& a = s.aiAssistant;
    // Presented by LumaText in the composer's face and colours (edit_host.h).
    const ui::AiPanelView& view = s.renderer.AiPanel();
    IDWriteTextFormat* format = view.ComposerFormat() ? view.ComposerFormat() : s.compositor.TextFormat();
    const D2D1_COLOR_F fore = view.TextColor();
    const D2D1_COLOR_F back = view.CardColor();
    const auto presented = [&](LRESULT& out) {
        return ui::HandleChildEditMessage(s.compositor, format, fore, back, a.brush, hwnd, msg, wp, lp, out);
    };
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN && !shift) {
            SendTurnEdit(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            EndTurnEdit(s);
            return 0;
        }
        break;
    case WM_CHAR:
        if ((wp == L'\r' || wp == L'\n') && !shift) return 0;
        if (wp == 27) return 0;
        if (wp == 1) {   // Ctrl+A: select all (a multiline EDIT does not)
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        presented(ignored);
        RemovePropW(hwnd, L"Pulse.EditMultiline");
        RemovePropW(hwnd, L"Pulse.TurnBack");
        RemovePropW(hwnd, L"Pulse.TurnFore");
        RemoveWindowSubclass(hwnd, TurnEditProc, kTurnEditSubclass);
        break;
    }
    default: {
        LRESULT r = 0;
        if (presented(r)) return r;
        break;
    }
    }
    const LRESULT r = ui::DefPresentedChildEditProc(s.compositor, format, fore, back, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL: case WM_SIZE:
        SyncTurnEdit(s);
        break;
    default:
        break;
    }
    return r;
}

void BeginTurnEdit(AppState& s, int block) {
    AiAssistantState& a = s.aiAssistant;
    AiChat& c = Chat(s);
    if (c.busy || block < 0 || block >= static_cast<int>(c.blocks.size()) ||
        c.blocks[static_cast<size_t>(block)].kind != ui::AiBlockKind::User)
        return;
    if (!a.turn_edit) {
        a.turn_edit = CreatePanelEdit(s);
        if (!a.turn_edit) return;
        a.font_scale = 0.0f;
        EnsureFont(s);
        SetWindowSubclass(a.turn_edit, TurnEditProc, kTurnEditSubclass, reinterpret_cast<DWORD_PTR>(&s));
    }
    const ui::AiBlockView& u = c.blocks[static_cast<size_t>(block)];
    a.edit_id = u.id;
    std::wstring text;
    for (wchar_t ch : u.text) {
        if (ch == L'\n') text += L'\r';
        text += ch;
    }
    SetWindowTextW(a.turn_edit, text.c_str());
    SendMessageW(a.turn_edit, EM_SETSEL, text.size(), static_cast<LPARAM>(text.size()));
    a.turn_edit_text = u.text;
    a.turn_edit_lines = 1 + static_cast<int>(std::count(u.text.begin(), u.text.end(), L'\n'));
    a.turn_edit_focus = true;
    s.renderer.AiPanel().ClearSelection();
    Redraw(s);
}

// After every frame: the EDIT sits on the drawn box while all of it is in
// view; the edit ends when its message went or a turn started.
void LayoutTurnEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.turn_edit) return;
    if (a.edit_id && (!a.chat || a.chat->busy || !a.chat->Find(a.edit_id))) EndTurnEdit(s);
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const bool covered = a.menu_open || AiVoiceCovers(s);
    if (!a.edit_id || !a.open || !view.TurnEditPlaced() || covered) {
        if (a.turn_edit_visible) {
            if (GetFocus() == a.turn_edit) {
                if (a.edit_id) a.turn_edit_focus = true;   // it comes back focused
                SetFocus(s.hwnd);
            }
            ShowWindow(a.turn_edit, SW_HIDE);
            a.turn_edit_visible = false;
            Redraw(s);
        }
        return;
    }
    const D2D1_RECT_F r = view.TurnEditRect();
    const int x = static_cast<int>(std::lround(r.left));
    const int y = static_cast<int>(std::lround(r.top));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int h = (std::max)(1, static_cast<int>(std::lround(r.bottom - r.top)));
    RECT cur{};
    GetWindowRect(a.turn_edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == h;
    if (!placed || !a.turn_edit_visible) {
        SetWindowPos(a.turn_edit, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        a.turn_edit_visible = true;
        Redraw(s);
    }
    // A theme switch: the EDIT keeps its old card and ink until repainted.
    const UINT_PTR back = 0x01000000u | ToColorRef(view.CardColor());
    const UINT_PTR fore = 0x01000000u | ToColorRef(view.TextColor());
    if (reinterpret_cast<UINT_PTR>(GetPropW(a.turn_edit, L"Pulse.TurnBack")) != back ||
        reinterpret_cast<UINT_PTR>(GetPropW(a.turn_edit, L"Pulse.TurnFore")) != fore) {
        SetPropW(a.turn_edit, L"Pulse.TurnBack", reinterpret_cast<HANDLE>(back));
        SetPropW(a.turn_edit, L"Pulse.TurnFore", reinterpret_cast<HANDLE>(fore));
        InvalidateRect(a.turn_edit, nullptr, TRUE);
    }
    if (a.turn_edit_focus) {
        a.turn_edit_focus = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) SetFocus(a.turn_edit);
    }
}

// 55 P3 历史对话: the search box and a title being renamed are one-line
// EDITs presented like the others, over the boxes the panel draws once the
// layer stands still. Enter opens the first match / saves the name, Esc
// gives up the name or closes the layer.
std::wstring EditText(HWND e) {
    std::wstring text(static_cast<size_t>(GetWindowTextLengthW(e)) + 1, L'\0');
    text.resize(static_cast<size_t>(GetWindowTextW(e, text.data(), static_cast<int>(text.size()))));
    return text;
}

void EndHistRename(AppState& s, bool save) {
    AiAssistantState& a = s.aiAssistant;
    if (a.hist_rename && AiHistoryRenaming(s)) AiHistoryRenameText(s, EditText(a.hist_rename));
    AiHistoryRenameEnd(s, save);
    a.hist_rename_focus = false;
    if (a.hist_rename && a.hist_rename_visible) {
        const bool had_focus = GetFocus() == a.hist_rename;
        ShowWindow(a.hist_rename, SW_HIDE);
        a.hist_rename_visible = false;
        if (had_focus) {
            if (s.hwnd) SetFocus(s.hwnd);
            a.hist_search_focus = true;   // the keyboard stays in the layer
        }
    }
    Redraw(s);
}

LRESULT CALLBACK HistEditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    AppState& s = *reinterpret_cast<AppState*>(ref);
    AiAssistantState& a = s.aiAssistant;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    IDWriteTextFormat* format = view.ComposerFormat() ? view.ComposerFormat() : s.compositor.TextFormat();
    const D2D1_COLOR_F fore = view.TextColor();
    const D2D1_COLOR_F back = view.CardColor();
    const auto presented = [&](LRESULT& out) {
        return ui::HandleChildEditMessage(s.compositor, format, fore, back, a.brush, hwnd, msg, wp, lp, out);
    };
    const bool rename = hwnd == a.hist_rename;
    switch (msg) {
    case WM_GETDLGCODE:
        return DefSubclassProc(hwnd, msg, wp, lp) | DLGC_WANTALLKEYS;
    case WM_KEYDOWN:
        if (wp == VK_RETURN) {
            if (rename) EndHistRename(s, true);
            else if (AiHistoryOpenFirst(s)) a.focus_pending = true;
            Redraw(s);
            return 0;
        }
        if (wp == VK_ESCAPE) {
            if (rename) {
                EndHistRename(s, false);
            } else if (AiHistoryEscape(s) && !a.hist_open) {
                a.focus_pending = true;   // the composer, once the layer is gone
            }
            Redraw(s);
            return 0;
        }
        if (wp != VK_CONTROL && wp != VK_SHIFT && wp != VK_MENU) {
            const app::KeyChord chord = app::CurrentChord(static_cast<UINT>(wp));
            for (const app::KeyChord bound : app::ActiveKeymap().EffectiveChords(app::KeyAction::AiHistory))
                if (bound == chord) {
                    AiAssistantHistoryKey(s);
                    return 0;
                }
        }
        break;
    case WM_CHAR:
        if (wp == L'\r' || wp == L'\n' || wp == 27 || wp == L'\t') return 0;
        if (wp == 8 && GetKeyState(VK_CONTROL) < 0) return 0;   // Ctrl+H's backspace
        if (wp == 1) {   // Ctrl+A
            SendMessageW(hwnd, EM_SETSEL, 0, -1);
            return 0;
        }
        break;
    case WM_NCDESTROY: {
        LRESULT ignored = 0;
        presented(ignored);
        RemovePropW(hwnd, L"Pulse.HistBack");
        RemovePropW(hwnd, L"Pulse.HistFore");
        RemoveWindowSubclass(hwnd, HistEditProc, kHistEditSubclass);
        break;
    }
    default: {
        LRESULT r = 0;
        if (presented(r)) return r;
        break;
    }
    }
    const LRESULT r = ui::DefPresentedChildEditProc(s.compositor, format, fore, back, hwnd, msg, wp, lp);
    switch (msg) {
    case WM_CHAR: case WM_KEYDOWN: case WM_PASTE: case WM_CUT: case WM_CLEAR: case WM_UNDO:
    case EM_UNDO: case WM_SETTEXT: case EM_REPLACESEL:
        if (hwnd == a.hist_search) AiHistorySetQuery(s, EditText(hwnd));
        else if (hwnd == a.hist_rename && AiHistoryRenaming(s)) {
            AiHistoryRenameText(s, EditText(hwnd));
        }
        break;
    default:
        break;
    }
    return r;
}

HWND CreateLineEdit(AppState& s) {
    HWND e = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_AUTOHSCROLL, 0, 0,
                             0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!e) return nullptr;
    SetLayeredWindowAttributes(e, 0, 255, LWA_ALPHA);
    SetWindowTheme(e, L"", L"");
    SendMessageW(e, EM_LIMITTEXT, 200, 0);
    SetWindowSubclass(e, HistEditProc, kHistEditSubclass, reinterpret_cast<DWORD_PTR>(&s));
    s.aiAssistant.font_scale = 0.0f;
    return e;
}

void BeginHistRename(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.hist_rename) {
        a.hist_rename = CreateLineEdit(s);
        if (!a.hist_rename) return;
        EnsureFont(s);
    }
    const std::wstring title = AiHistoryRenameTitle(s);   // a copy: SETTEXT syncs it
    SetWindowTextW(a.hist_rename, title.c_str());
    SendMessageW(a.hist_rename, EM_SETSEL, 0, -1);
    a.hist_rename_focus = true;
}

void PlaceHistEdit(AppState& s, HWND e, bool& visible, bool& focus, const D2D1_RECT_F& r, bool want, bool keep_focus) {
    if (!e) return;
    if (!want || r.right - r.left < 1.0f || r.bottom - r.top < 1.0f) {
        if (visible) {
            if (GetFocus() == e) {
                if (s.hwnd) SetFocus(s.hwnd);
                if (keep_focus) focus = true;   // it comes back focused
            }
            ShowWindow(e, SW_HIDE);
            visible = false;
            Redraw(s);
        }
        return;
    }
    const int x = static_cast<int>(std::lround(r.left));
    const int y = static_cast<int>(std::lround(r.top));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int h = (std::max)(1, static_cast<int>(std::lround(r.bottom - r.top)));
    RECT cur{};
    GetWindowRect(e, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == h;
    if (!placed || !visible) {
        SetWindowPos(e, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        visible = true;
        Redraw(s);
    }
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const UINT_PTR back = 0x01000000u | ToColorRef(view.CardColor());
    const UINT_PTR fore = 0x01000000u | ToColorRef(view.TextColor());
    if (reinterpret_cast<UINT_PTR>(GetPropW(e, L"Pulse.HistBack")) != back ||
        reinterpret_cast<UINT_PTR>(GetPropW(e, L"Pulse.HistFore")) != fore) {
        SetPropW(e, L"Pulse.HistBack", reinterpret_cast<HANDLE>(back));
        SetPropW(e, L"Pulse.HistFore", reinterpret_cast<HANDLE>(fore));
        InvalidateRect(e, nullptr, TRUE);
    }
    if (focus) {
        focus = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) SetFocus(e);
    }
}

void LayoutHistEdits(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const bool shown = a.open && a.hist_open && view.HistorySettled() && !a.menu_open && !AiVoiceCovers(s);
    if (shown && !a.hist_search) {
        a.hist_search = CreateLineEdit(s);
        if (a.hist_search) EnsureFont(s);
    }
    if (a.hist_search) {
        // A single-line EDIT: the presenter draws its cue banner, also while focused.
        if (const std::wstring cue = Text(I::AiHistSearch); cue != a.hist_cue) {
            a.hist_cue = cue;
            SendMessageW(a.hist_search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(a.hist_cue.c_str()));
            InvalidateRect(a.hist_search, nullptr, TRUE);
        }
    }
    PlaceHistEdit(s, a.hist_search, a.hist_search_visible, a.hist_search_focus, view.HistorySearchRect(), shown,
                  a.hist_open);
    const bool renaming = AiHistoryRenaming(s);
    PlaceHistEdit(s, a.hist_rename, a.hist_rename_visible, a.hist_rename_focus, view.HistoryRenameRect(),
                  shown && renaming, renaming);
}

void CloseMenu(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiVoiceCloseMenu(s);
    if (!a.menu_open) return;
    s.renderer.AiPanel().EndSliderDrag();   // a drag that Esc cut short is dropped
    a.menu_open = false;
    a.menu_closed_at = ui::motion::NowMs();
}

// The model service is set up in one place: Settings > AI 助手.
void OpenServiceSettings(AppState& s) {
    CloseMenu(s);
    OpenSettingsTab(s, ui::kSettingsPageAssistant);
}

// Header buttons and the answer's acts show a tip after a dwell.
bool HasTip(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    return part == P::History || part == P::HistPin || part == P::HistRename || part == P::HistDelete ||
           part == P::NewChat || part == P::Mode || part == P::Close || part == P::ActCopy || part == P::ActRetry ||
           part == P::ActThumb || part == P::ActThumbDown || part == P::VerPrev || part == P::VerNext ||
           part == P::UserCopy || part == P::UserEdit || part == P::MenuReset || part == P::MenuSlider ||
           AiVoiceHasTip(part);
}

// The model list: the shared FluentMenu (the dropdown of Pulse's other menus),
// under the model button or the 思考强度 popover's 「model ›」. The popover
// stays open behind it.
void PickModel(AppState& s, const D2D1_RECT_F& box) {
    AiAssistantState& a = s.aiAssistant;
    if (box.right <= box.left || !EnsureMenu(s)) return;
    const std::vector<std::string>& listed = Listed();
    // Before the service listed anything: the chosen one alone.
    const std::vector<std::string> alone{CurrentModel()};
    const std::vector<std::string> rows = listed.empty() ? alone : listed;
    std::vector<ui::FluentMenuItem> items;
    {
        ui::FluentMenuItem head;
        head.heading = true;
        head.enabled = false;   // a caption: never hovered or picked
        head.text = Text(I::AiAsstModelHeading);
        items.push_back(std::move(head));
    }
    const std::string current = CurrentModel();
    for (size_t i = 0; i < rows.size(); ++i) {
        ui::FluentMenuItem item;
        item.command = static_cast<int>(i) + 1;
        item.text = json::Wide(rows[i]);
        item.checked = rows[i] == current;
        for (const KnownModel& k : kKnownModels)
            if (rows[i] == k.id) {
                item.shortcut = Text(k.caption);
                item.tooltip = Text(k.detail);
            }
        if (ai::IsChatOnlyModel(AiAgentBaseUrl(), rows[i])) item.badge_text = Text(I::AsstChatOnly);
        items.push_back(std::move(item));
    }
    items.back().separator_after = true;
    constexpr int kManage = 100000;
    {
        ui::FluentMenuItem manage;
        manage.command = kManage;
        manage.text = Text(I::AiAsstServiceSettings);
        manage.glyph = L"\xE713";
        manage.shortcut = Text(AiAgentConnected() ? I::AiConnStatusOn : I::AsstNotConnectedBadge);
        items.push_back(std::move(manage));
    }
    POINT c[] = {{static_cast<LONG>(std::lround(box.left)), static_cast<LONG>(std::lround(box.top))},
                 {static_cast<LONG>(std::lround(box.right)), static_cast<LONG>(std::lround(box.bottom))}};
    MapWindowPoints(s.hwnd, nullptr, c, 2);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackDropdown({c[0].x, c[0].y, c[1].x, c[1].y}, std::move(items));
    if (cmd == kManage) {
        OpenServiceSettings(s);
        return;
    }
    if (cmd >= 1 && static_cast<size_t>(cmd) <= rows.size() && rows[static_cast<size_t>(cmd) - 1] != current) {
        g_model = rows[static_cast<size_t>(cmd) - 1];
        SavePrefs(a);
    }
    Redraw(s);
}

// 重新生成's caret (55 P2): 再试一次 / 更详细 / 更简洁, then 换个模型 — two
// other models the service listed and 更多模型… (the model list); a model
// picked here becomes the assistant's model and answers again.
void RegenMenu(AppState& s, int block) {
    AiAssistantState& a = s.aiAssistant;
    const D2D1_RECT_F box = s.renderer.AiPanel().ConvHitRect({ui::AiPanelPart::ActRegenMenu, block});
    if (box.right <= box.left || !EnsureMenu(s) || (a.chat && a.chat->busy)) return;
    enum : int { kAgain = 1, kLonger, kShorter, kMore, kModel0 = 10 };
    const std::string current = CurrentModel();
    std::vector<std::string> others;
    for (const std::string& m : Listed())
        if (m != current && others.size() < 2) others.push_back(m);
    std::vector<ui::FluentMenuItem> items;
    const auto heading = [&](I id) {
        ui::FluentMenuItem h;
        h.heading = true;
        h.enabled = false;
        h.text = Text(id);
        items.push_back(std::move(h));
    };
    const auto row = [&](int command, I id, const wchar_t* glyph) {
        ui::FluentMenuItem it;
        it.command = command;
        it.text = Text(id);
        it.glyph = glyph;
        items.push_back(std::move(it));
    };
    heading(I::AiRegen);
    row(kAgain, I::AiRegenAgain, L"\xE72C");
    row(kLonger, I::AiRegenLonger, L"\xE8FD");
    row(kShorter, I::AiRegenShorter, L"\xE8E4");
    items.back().separator_after = true;
    heading(I::AiRegenModel);
    for (size_t i = 0; i < others.size(); ++i) {
        ui::FluentMenuItem it;
        it.command = kModel0 + static_cast<int>(i);
        it.text = json::Wide(others[i]);
        it.glyph = L"\xE950";
        items.push_back(std::move(it));
    }
    row(kMore, I::AiRegenMoreModels, L"\xE712");
    POINT c[] = {{static_cast<LONG>(std::lround(box.left)), static_cast<LONG>(std::lround(box.top))},
                 {static_cast<LONG>(std::lround(box.right)), static_cast<LONG>(std::lround(box.bottom))}};
    MapWindowPoints(s.hwnd, nullptr, c, 2);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int cmd = s.menu->TrackDropdown({c[0].x, c[0].y, c[1].x, c[1].y}, std::move(items));
    if (cmd == kAgain || cmd == kLonger || cmd == kShorter) {
        AiChatRegenerate(s, block, cmd == kLonger ? AiRegen::Longer : cmd == kShorter ? AiRegen::Shorter : AiRegen::Again);
    } else if (cmd >= kModel0 && static_cast<size_t>(cmd - kModel0) < others.size()) {
        g_model = others[static_cast<size_t>(cmd - kModel0)];
        SavePrefs(a);
        AiChatRegenerate(s, block, AiRegen::Again);
    } else if (cmd == kMore) {
        PickModel(s, box);
        if (CurrentModel() != current) AiChatRegenerate(s, block, AiRegen::Again);
    }
    Redraw(s);
}

// Sets 思考强度 to a stop of the model's slider (or switch). The stop already
// shown keeps the preference, which may be a level this model lacks.
void SetEffortStop(AppState& s, int stop) {
    AiAssistantState& a = s.aiAssistant;
    const ai::ThinkSpec t = CurrentThinking();
    if (!t.Adjustable() || stop < 0 || stop >= t.count || stop == StopOf(t, Preferred(a))) return;
    a.think = static_cast<int>(t.levels[stop]);
    SavePrefs(a);
}

void ShowTipLater(AppState& s) {
    if (s.hwnd) SetTimer(s.hwnd, kTipTimer, kTipDelayMs, [](HWND hwnd, UINT, UINT_PTR id, DWORD) {
        KillTimer(hwnd, id);
        InvalidateRect(hwnd, nullptr, FALSE);
    });
}

bool IsPeekPart(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    return part == P::PeekCard || part == P::PeekClose || part == P::PeekUndo || part == P::PeekOpen;
}

// The peek bubble first (it floats over the list), then the panel.
ui::AiPanelHit HitAt(AppState& s, int x, int y) {
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const ui::AiPanelHit peek = view.PeekHitTest(static_cast<float>(x), static_cast<float>(y));
    if (peek.part != ui::AiPanelPart::None) return peek;
    return view.HitTest(static_cast<float>(x), static_cast<float>(y));
}

bool Reachable(AppState& s) { return s.aiAssistant.open || s.renderer.AiPanel().PeekVisible(); }

// ---- Text selection, links and path chips in the conversation (55 P1).

constexpr UINT_PTR kSelectTimer = 0x4153;   // 'AS': a selection dragged past the conversation's edge

bool IsSelectionPart(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    return part == P::SelCopy || part == P::SelQuote || part == P::SelSearch;
}

std::wstring Trimmed(std::wstring t) {
    const auto space = [](wchar_t ch) { return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n' || ch == 0x3000; };
    while (!t.empty() && space(t.back())) t.pop_back();
    size_t i = 0;
    while (i < t.size() && space(t[i])) ++i;
    return t.substr(i);
}

// 在 Pulse 中搜索 takes a short single line (≤ 80 characters); "" otherwise.
std::wstring SearchableText(const std::wstring& text) {
    std::wstring q = Trimmed(text);
    if (q.empty() || q.size() > 80 || q.find_first_of(L"\r\n") != std::wstring::npos) return {};
    return q;
}

std::wstring Printf1(I id, const std::wstring& arg) {
    const std::wstring fmt = Text(id);
    std::vector<wchar_t> buf(fmt.size() + arg.size() + 16);
    swprintf_s(buf.data(), buf.size(), fmt.c_str(), arg.c_str());
    return buf.data();
}

std::wstring Shortened(const std::wstring& text, size_t max) {
    return text.size() <= max ? text : text.substr(0, max - 1) + L"\u2026";
}

void Snack(AppState& s, std::wstring text) {
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Suggest, std::move(text), {}, {}, {}, 2500);
}

void CopyAndSay(AppState& s, const std::wstring& text) {
    if (text.empty()) return;
    AiChatCopyText(s.hwnd, text);
    Snack(s, Text(I::CopiedShort));
}

// Links open in the browser: http, https and mailto only.
void OpenLink(AppState& s, const std::wstring& url) {
    const auto starts = [&](const wchar_t* p) { return _wcsnicmp(url.c_str(), p, wcslen(p)) == 0; };
    if (!starts(L"http://") && !starts(L"https://") && !starts(L"mailto:")) return;
    ShellExecuteW(s.hwnd, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// A path chip: a folder opens, a file opens its folder with it selected.
void OpenPath(AppState& s, std::wstring path, bool new_tab) {
    for (wchar_t& ch : path)
        if (ch == L'/') ch = L'\\';
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        const size_t slash = path.find_last_of(L'\\');
        Snack(s, Printf1(I::AiPathMissing, Shortened(slash == std::wstring::npos ? path : path.substr(slash + 1), 40)));
        return;
    }
    std::wstring folder = path, leaf;
    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) == 0) {
        const size_t slash = path.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        folder = path.substr(0, slash);
        leaf = path.substr(slash + 1);
        if (folder.size() == 2 && folder[1] == L':') folder += L'\\';
    }
    if (new_tab) OpenFolderTab(s, folder);
    else NavigateTo(s, folder);
    if (!leaf.empty())
        if (app::Tab* tab = ActiveTab(s)) SelectNameInTab(s, *tab, leaf);
}

// 引用: the words go above the composer and lead the next message as "> …".
void QuoteText(AppState& s, const std::wstring& text) {
    AiAssistantState& a = s.aiAssistant;
    std::wstring q = Trimmed(text);
    if (q.empty()) return;
    std::wstring clean;
    clean.reserve(q.size());
    for (wchar_t ch : q)
        if (ch != L'\r') clean += ch;
    a.quote = Shortened(clean, 1200);
    s.renderer.AiPanel().ClearSelection();
    a.focus_pending = true;
}

std::wstring QuoteMarkdown(const std::wstring& quote) {
    std::wstring out = L"> ";
    for (wchar_t ch : quote) {
        out += ch;
        if (ch == L'\n') out += L"> ";
    }
    return out;
}

void FinishSelect(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.selecting) return;
    a.selecting = false;
    a.captured = false;
    if (s.hwnd) KillTimer(s.hwnd, kSelectTimer);
    if (GetCapture() == s.hwnd) ReleaseCapture();
    s.renderer.AiPanel().EndSelect();
}

void CALLBACK SelectTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    auto* s = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || !s->aiAssistant.selecting) {
        KillTimer(hwnd, id);
        return;
    }
    ui::AiPanelView& view = s->renderer.AiPanel();
    const POINT p = s->aiAssistant.sel_drag;
    const float speed = view.SelectScrollSpeed(static_cast<float>(p.y));
    if (speed == 0.0f) {
        KillTimer(hwnd, id);
        return;
    }
    view.ScrollBy(speed, false);
    view.DragSelect(static_cast<float>(p.x), static_cast<float>(p.y));
    Redraw(*s);
}

// The whole answer under the pointer when nothing is selected.
std::wstring AnswerText(ui::AiPanelView& view, int block, bool has, bool markdown) {
    if (has) return view.SelectionText(markdown);
    if (!view.SelectAnswer(block)) return {};
    std::wstring t = view.SelectionText(markdown);
    view.ClearSelection();
    return t;
}

// 55 P3: 历史对话 in and out. Opening ends a message edit and the selection
// and puts the keyboard in the search box; closing gives it back to the
// composer once the layer has slid away.
void OpenHistory(AppState& s, bool open) {
    AiAssistantState& a = s.aiAssistant;
    if (open == a.hist_open) return;
    CloseMenu(s);
    if (open) {
        if (a.edit_id) EndTurnEdit(s);
        s.renderer.AiPanel().ClearSelection();
        if (a.hist_search) SetWindowTextW(a.hist_search, L"");
        a.hist_search_focus = true;
    } else {
        if (AiHistoryRenaming(s)) EndHistRename(s, false);
        a.hist_search_focus = false;
        a.focus_pending = true;
    }
    AiHistoryShow(s, open);
    a.hover = {};
    Redraw(s);
}

bool IsHistoryPart(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    switch (part) {
    case P::HistArea: case P::HistSearch: case P::HistSettings: case P::HistRow: case P::HistPin:
    case P::HistRename: case P::HistDelete: case P::HistDelCancel: case P::HistDelOk: case P::HistRenameBox:
        return true;
    default:
        return false;
    }
}

void HistoryClick(AppState& s, const ui::AiPanelHit& hit) {
    AiAssistantState& a = s.aiAssistant;
    using P = ui::AiPanelPart;
    if (hit.part == P::HistRenameBox) return;   // its EDIT takes the clicks once placed
    if (hit.part == P::HistSearch) {
        a.hist_search_focus = true;
        Redraw(s);
        return;
    }
    // A click anywhere else gives up a name being typed (Enter saves it).
    if (AiHistoryRenaming(s)) {
        EndHistRename(s, false);
        if (hit.part == P::HistRow || hit.part == P::HistArea) return;
    }
    const AiChat* before = a.chat.get();
    if (AiHistoryActivate(s, hit.part, hit.index)) BeginHistRename(s);
    if (a.chat.get() != before) {
        // Another conversation came in: nothing of the old one stays around.
        if (a.edit) SetWindowTextW(a.edit, L"");
        a.context_removed_for.clear();
        a.quote.clear();
        s.renderer.AiPanel().ClearSelection();
    }
    if (!a.hist_open) {
        a.hist_search_focus = false;
        a.focus_pending = true;
    }
    Redraw(s);
}

void Activate(AppState& s, const ui::AiPanelHit& hit) {
    AiAssistantState& a = s.aiAssistant;
    using P = ui::AiPanelPart;
    if (IsHistoryPart(hit.part)) {
        HistoryClick(s, hit);
        return;
    }
    if (IsPeekPart(hit.part)) {
        if (hit.part == P::PeekCard) return;
        AiChatHidePeek(s);
        if (hit.part == P::PeekUndo && a.chat && a.chat->peek_failed) {
            // A failed turn: 重试 in place, or 连接模型 opens Settings.
            if (a.chat->peek_tone == ui::AiErrTone::Accent) {
                OpenServiceSettings(s);
            } else {
                AiChatRetryTurn(s);
            }
        } else if (hit.part == P::PeekUndo) {
            AiChatUndoLast(s);
        }
        if (hit.part == P::PeekOpen) {
            AiAssistantOpen(s);
            s.renderer.AiPanel().ScrollToEnd();
        }
        Redraw(s);
        return;
    }
    // The mic, its menu and the voice card. The model menu gives way to them.
    if ((hit.part == P::Mic || hit.part == P::MicMenu) && a.menu_open) {
        a.menu_open = false;
        a.menu_closed_at = ui::motion::NowMs();
    }
    if (AiVoiceActivate(s, hit)) {
        Redraw(s);
        return;
    }
    switch (hit.part) {
    case P::History:
        OpenHistory(s, !a.hist_open);
        break;
    case P::JumpLatest:
        s.renderer.AiPanel().ScrollToEnd();
        break;
    case P::NewChat:
        CloseMenu(s);
        OpenHistory(s, false);
        AiChatReset(s);
        if (a.edit) SetWindowTextW(a.edit, L"");
        a.context_removed_for.clear();
        a.quote.clear();
        s.renderer.AiPanel().ClearSelection();
        s.renderer.AiPanel().Rewake(ui::motion::NowMs());
        a.focus_pending = true;
        break;
    case P::Mode:
        CloseMenu(s);
        a.floating = !a.floating;
        a.focus_pending = GetFocus() == a.edit;
        ApplyPanel(s, true);
        SavePrefs(a);
        break;
    case P::Close:
        AiAssistantClose(s);
        return;
    case P::Suggestion: {
        CloseMenu(s);
        const std::vector<ui::AiSuggestionView> suggestions = Suggestions(s);
        if (hit.index >= 0 && hit.index < static_cast<int>(suggestions.size())) {
            const std::wstring& title = suggestions[static_cast<size_t>(hit.index)].title;
            // 找上个月的发票 only fills the composer: "last month" is easy to change.
            if (title == Text(I::AiQSugTitle)) AiAssistantFillComposer(s, Text(I::AiQSugPrompt));
            else Submit(s, title);
        }
        break;
    }
    case P::RichLink:
    case P::RichPath:
    case P::CodeCopy:
    case P::TableCopy: {
        CloseMenu(s);
        ui::AiPanelView& view = s.renderer.AiPanel();
        const ui::AiPanelView::RichTargetInfo t = view.TargetOf(hit);
        if (t.text.empty()) break;
        if (hit.part == P::RichLink) {
            OpenLink(s, t.text);
        } else if (hit.part == P::RichPath) {
            OpenPath(s, t.text, true);
        } else {
            // The button says 已复制 for 2 s; no snack.
            AiChatCopyText(s.hwnd, t.text);
            view.MarkCopied(hit, ui::motion::NowMs());
        }
        break;
    }
    case P::SelCopy:
        CopyAndSay(s, s.renderer.AiPanel().SelectionText(false));
        s.renderer.AiPanel().HideSelectionBar();
        break;
    case P::SelQuote:
        QuoteText(s, s.renderer.AiPanel().SelectionText(false));
        break;
    case P::SelSearch: {
        const std::wstring q = SearchableText(s.renderer.AiPanel().SelectionText(false));
        s.renderer.AiPanel().HideSelectionBar();
        if (!q.empty()) NavigateTo(s, app::MakeSearchPath(q));
        break;
    }
    case P::QuoteRemove:
        a.quote.clear();
        a.focus_pending = true;
        break;
    case P::ContextRemove: {
        std::wstring path, name;
        size_t count = 0;
        if (CurrentFolder(s, path, name, count)) a.context_removed_for = path;
        a.focus_pending = true;
        break;
    }
    case P::ModelButton:
        // 思考强度 when the model has it (the slider, switch or 始终思考, and
        // 「model ›」), else the model list right away.
        if (a.menu_open) CloseMenu(s);
        else {
            AiVoiceCloseMenu(s);
            if (AiAgentConnected()) ai::RefreshListedModels(s.hwnd, AiAgentBaseUrl());
            if (CurrentThinking().kind != ai::ThinkKind::None) {
                a.menu_open = true;
            } else {
                PickModel(s, s.renderer.AiPanel().ModelButtonRect());
                a.focus_pending = true;
            }
        }
        break;
    case P::MenuModel:
        PickModel(s, s.renderer.AiPanel().ModelLinkRect());
        break;
    case P::MenuReset:
        a.think = static_cast<int>(CurrentThinking().fallback);
        SavePrefs(a);
        break;
    case P::MenuSlider:   // the switch of a model that is only on or off
        SetEffortStop(s, hit.index);
        break;
    case P::GoSettings:
        OpenServiceSettings(s);
        return;
    case P::Send:
        CloseMenu(s);
        // While the mic listens 发送 only stops it: the last words are still
        // coming, and nothing is sent without a look.
        if (AiVoiceBusy(s)) AiVoiceStop(s);
        else if (a.chat && a.chat->busy) AiChatStop(s);
        else SubmitComposer(s);
        a.focus_pending = true;
        break;
    case P::ThinkToggle:
    case P::StepsToggle:
    case P::PlanToggle:
    case P::ConfirmDeny:
    case P::ConfirmAccept:
    case P::ResultUndo:
    case P::ResultShow:
    case P::RenameToggle:
    case P::TaskToggle:
    case P::DiskToggle:
    case P::CleanToggle:
    case P::FunnelToggle:
    case P::DupToggle:
    case P::ResultPurge:
    case P::RenameMore:
    case P::ConfirmExtra:
    case P::ActCopy:
    case P::ActRetry:
    case P::ActThumb:
    case P::ActThumbDown:
    case P::VerPrev:
    case P::VerNext:
    case P::UserCopy:
    case P::StopContinue:
    case P::CutRetry:
        CloseMenu(s);
        AiChatActivate(s, hit.part, hit.index);
        break;
    case P::ActRegenMenu:
        CloseMenu(s);
        RegenMenu(s, hit.index);
        break;
    // These rebuild the turn under the pointer: its old tip must not linger
    // (the next move finds what is there now).
    case P::UserEdit:
        CloseMenu(s);
        a.hover = {};
        BeginTurnEdit(s, hit.index);
        break;
    case P::EditCancel:
        a.hover = {};
        EndTurnEdit(s);
        break;
    case P::EditSend:
        a.hover = {};
        SendTurnEdit(s);
        break;
    case P::EditBox:
        s.renderer.AiPanel().RevealTurnEdit();
        a.turn_edit_focus = true;
        Redraw(s);
        break;
    case P::CleanCheck:
    case P::CleanLink:
    case P::CleanFile:
        CloseMenu(s);
        AiChatClean(s, hit.part, hit.index);
        break;
    case P::DupFile:
    case P::DupRule:
    case P::DupMore:
        CloseMenu(s);
        AiChatDup(s, hit.part, hit.index);
        break;
    case P::QueryChipRemove:
        CloseMenu(s);
        AiAgentQueryChip(s, hit.index / 16, hit.index % 16);
        break;
    case P::QueryAdd:
        CloseMenu(s);
        AiAgentQueryAdd(s, hit.index);
        break;
    case P::FollowItem:
        CloseMenu(s);
        AiAgentFollow(s, hit.index / 8, hit.index % 8);
        break;
    case P::ErrAction: {
        // The error card's buttons: connection and model open the menu in
        // place; retry, top up and copy are the chat's.
        ui::AiErrAct act{};
        if (!AiChatErrAction(s, hit.index, act)) break;
        if (AiVoiceErrAct(s, act, hit.index)) break;
        if (act == ui::AiErrAct::ManageConn || act == ui::AiErrAct::Connect) {
            OpenServiceSettings(s);
            return;
        } else if (act == ui::AiErrAct::SwitchModel) {
            CloseMenu(s);
            PickModel(s, s.renderer.AiPanel().ModelButtonRect());
        } else {
            CloseMenu(s);
            AiChatErrRun(s, hit.index);
            a.focus_pending = true;
        }
        break;
    }
    case P::AddFiles:
    case P::ContextChip:
    case P::Input:
    case P::Panel:
    case P::Conversation:
        CloseMenu(s);
        a.focus_pending = true;
        break;
    default:
        break;
    }
    Redraw(s);
}

} // namespace

void AiAssistantAppendComposer(AppState& s, const std::wstring& words) {
    AiAssistantState& a = s.aiAssistant;
    EnsureEdit(s);
    if (!a.edit || words.empty()) return;
    auto word = [](wchar_t ch) { return (ch >= L'0' && ch <= L'9') || (ch >= L'A' && ch <= L'Z') || (ch >= L'a' && ch <= L'z'); };
    std::wstring text = ComposerText(a);
    if (!text.empty() && word(text.back()) && word(words.front())) text += L' ';
    text += words;
    SetWindowTextW(a.edit, text.c_str());
    const int end = static_cast<int>(text.size());
    SendMessageW(a.edit, EM_SETSEL, static_cast<WPARAM>(end), static_cast<LPARAM>(end));
    SendMessageW(a.edit, EM_SCROLLCARET, 0, 0);
    ReadComposerMetrics(a);
    a.focus_pending = true;
    Redraw(s);
}

void AiAssistantFillComposer(AppState& s, const std::wstring& text, size_t select_from) {
    AiAssistantState& a = s.aiAssistant;
    EnsureEdit(s);
    if (!a.edit) return;
    SetWindowTextW(a.edit, text.c_str());
    const int end = static_cast<int>(text.size());
    const int from = select_from < text.size() ? static_cast<int>(select_from) : end;
    SendMessageW(a.edit, EM_SETSEL, static_cast<WPARAM>(from), static_cast<LPARAM>(end));
    ReadComposerMetrics(a);
    a.focus_pending = true;
    Redraw(s);
}

bool AiAssistantIsOpen(const AppState& s) { return s.aiAssistant.open; }

void AiChipContextMenu(AppState& s, POINT screen_pt) {
    if (!EnsureMenu(s)) return;
    s.menu->SetTheme(s.darkMode, s.accentColor);
    std::vector<ui::FluentMenuItem> items(2);
    items[0].command = 1;
    items[0].text = Text(s.aiAssistant.open ? I::AiChipCollapse : I::AiChipOpen);
    items[0].glyph = L"\xF5C2";
    items[0].shortcut = L"Ctrl+J";
    items[0].separator_after = true;
    items[1].command = 2;
    items[1].text = Text(I::AiChipSettings);
    items[1].glyph = L"\xE713";
    const int command = s.menu->TrackPopup(screen_pt, std::move(items));
    if (command == 1) {
        if (s.aiAssistant.open) AiAssistantClose(s);
        else AiAssistantOpen(s);
    } else if (command == 2) {
        OpenSettingsTab(s, ui::kSettingsPageAssistant);
    }
    Redraw(s);
}

void AiAssistantOpen(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    LoadPrefs(a);
    if (!a.enabled) return;
    if (a.open) return;
    a.open = true;
    a.focus_pending = true;
    if (a.chat) a.chat->unread = false;
    // Seen: the card in the panel says it now.
    if (a.chat && (a.chat->chip == ui::StatusAiState::Error || a.chat->chip == ui::StatusAiState::Warn))
        AiChatSetChip(*a.chat, ui::StatusAiState::Idle, L"");
    if (a.chat) AiChatHidePeek(s);
    EnsureEdit(s);
    ApplyPanel(s, animate);
}

void AiAssistantClose(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return;
    a.open = false;
    AiVoiceStop(s);
    a.menu_open = false;
    a.hover = {};
    a.pressed = {};
    if (a.captured && GetCapture() == s.hwnd) ReleaseCapture();
    a.captured = false;
    s.renderer.AiPanel().EndScrollDrag();
    FinishSelect(s);
    s.renderer.AiPanel().ClearSelection();
    HideEdit(s);
    ApplyPanel(s, animate);
    // Closed on a finished turn: the bubble over the chip says what happened.
    if (a.chat) AiChatPeek(s, 650);
}

void AiAssistantToggle(AppState& s) {
    if (s.aiAssistant.open) AiAssistantClose(s);
    else AiAssistantOpen(s);
}

void AiAssistantHotkey(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    // Closed: open and type. Open with the focus elsewhere: back to the
    // composer. In the composer: collapse (ComposerProc), focus to the list.
    if (!a.open) {
        AiAssistantOpen(s);
    } else if (a.edit && GetFocus() == a.edit) {
        AiAssistantClose(s);
        if (s.hwnd) SetFocus(s.hwnd);
    } else {
        a.focus_pending = true;
        Redraw(s);
    }
}

void AiAssistantHistoryKey(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) {
        AiAssistantOpen(s);
        OpenHistory(s, true);
        return;
    }
    OpenHistory(s, !a.hist_open);
}

void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm) {
    AiAssistantState& a = s.aiAssistant;
    LoadPrefs(a);
    if (!a.enabled) {
        vm.status.ai_state = ui::StatusAiState::Hidden;
        vm.status.ai_open = false;
        return;
    }
    // The chip is always there: "✦ AI" opens the panel.
    if (vm.status.ai_state == ui::StatusAiState::Hidden) vm.status.ai_state = ui::StatusAiState::Idle;
    if (vm.status.ai_state == ui::StatusAiState::Idle) vm.status.ai_text.clear();
    vm.status.ai_label = Text(I::AiAsstChipLabel);
    vm.status.ai_open = a.open;
    // While Pulse 助手 works its words take the chip.
    if (a.chat && a.chat->chip != ui::StatusAiState::Idle) {
        vm.status.ai_state = a.chat->chip;
        vm.status.ai_text = a.chat->chip_text;
        vm.status.ai_progress = a.chat->chip_progress;
        vm.status.ai_unread = a.chat->unread;
    }
    if (a.chat) {
        AiChatFillPeek(s, a.open, vm.ai_panel.peek);
        vm.ai_panel.dark = vm.dark;
        vm.ai_panel.hover = a.hover;
        vm.ai_panel.pressed = a.pressed;
    }
    if (!a.open && !s.renderer.AiPanel().Visible()) return;
    LoadPrefs(a);

    ui::AiPanelViewModel& p = vm.ai_panel;
    p.dark = vm.dark;
    p.title = Text(I::AiAsstTitle);
    AiChat& chat = Chat(s);
    p.empty = chat.blocks.empty();
    p.blocks = chat.blocks;
    p.hover_turn = a.hover_turn;
    AiHistoryFillVm(s, p.history);   // 55 P3
    p.edited_label = Text(I::AiUserEdited);
    p.edit_block = -1;
    if (a.edit_id)
        for (size_t i = 0; i < chat.blocks.size(); ++i)
            if (chat.blocks[i].id == a.edit_id) p.edit_block = static_cast<int>(i);
    if (p.edit_block >= 0) {
        p.edit_lines = a.turn_edit_lines;
        p.edit_text = a.turn_edit_text;
        p.edit_warn = Text(I::AiEditWarn);
        p.edit_cancel = Text(I::Cancel);
        p.edit_send = Text(I::AiEditSend);
    }
    if (chat.think_id) {
        // The running thinking header counts seconds.
        for (ui::AiBlockView& b : p.blocks)
            if (b.id == chat.think_id && b.running) {
                const uint64_t secs = (ui::motion::NowMs() - chat.think_start) / 1000;
                if (secs > 0) b.seconds = Fill(Text(I::AiAsstSeconds), L"{n}", std::to_wstring(secs));
            }
    }
    SYSTEMTIME now{};
    GetLocalTime(&now);
    p.greeting = Text(now.wHour < 11 ? I::AiAsstGreetMorning : now.wHour < 13 ? I::AiAsstGreetNoon
                      : now.wHour < 18 ? I::AiAsstGreetAfternoon : I::AiAsstGreetEvening);
    p.intro = Text(I::AiAsstIntro);
    std::wstring path, folder;
    size_t count = 0;
    const bool has_folder = CurrentFolder(s, path, folder, count);
    p.suggestions = Suggestions(s);
    p.safe_note = Text(I::AiAsstSafe);
    p.jump_latest = Text(I::AiJumpLatest);

    p.context_visible = has_folder && a.context_removed_for != path;
    if (p.context_visible)
        p.context_text = Fill(Fill(Text(I::AiAsstContext), L"{folder}", folder), L"{n}", std::to_wstring(count));
    p.composer_focused = a.focused && a.edit_visible;
    p.composer_empty = a.text_len == 0;
    p.composer_lines = a.lines;
    p.composer_line_px = static_cast<float>(a.line_px);
    const bool menu_covers = a.menu_open || ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    if (!p.composer_empty && (!a.edit_visible || menu_covers || AiVoiceCovers(s))) {
        p.draw_composer_text = true;
        p.composer_text = ComposerText(a);
    }
    p.placeholder = Text(I::AiAsstPlaceholder);
    p.quote = a.quote;
    p.add_files = Text(I::AiAsstAddFiles);
    p.model_label = json::Wide(CurrentModel());
    const ai::ThinkSpec think = CurrentThinking();
    const ai::ThinkLevel level = ai::SnapThinking(think, Preferred(a));
    p.effort_mode = think.kind == ai::ThinkKind::Fixed    ? ui::AiEffortMode::Fixed
                    : think.kind == ai::ThinkKind::Toggle ? ui::AiEffortMode::Toggle
                                                          : ui::AiEffortMode::Levels;
    p.effort_count = think.Adjustable() ? think.count : 0;
    p.effort = StopOf(think, Preferred(a));
    p.effort_default = (std::max)(think.IndexOf(think.fallback), 0);
    p.effort_label.clear();
    if (think.kind == ai::ThinkKind::Fixed) p.effort_label = L" \u00B7 " + Text(I::AiAsstThinkAlways);
    else if (think.kind == ai::ThinkKind::Toggle && level != ai::ThinkLevel::Off)
        p.effort_label = L" \u00B7 " + Text(I::AiAsstThinkShort);
    else if (think.kind == ai::ThinkKind::Levels && level != ai::ThinkLevel::Off)
        p.effort_label = L" \u00B7 " + LevelName(level);
    p.send = chat.busy ? ui::AiSendState::Busy : p.composer_empty ? ui::AiSendState::Dim : ui::AiSendState::Ready;

    p.menu_open = a.menu_open && think.kind != ai::ThinkKind::None;
    p.effort_names = {};
    p.effort_details = {};
    p.effort_note.clear();
    if (think.kind == ai::ThinkKind::Fixed) {
        p.effort_names[0] = Text(I::AiAsstThinkAlways);
        p.effort_details[0] = Text(I::AiAsstThinkFixedDetail);
    } else if (think.Adjustable()) {
        for (int i = 0; i < think.count; ++i) {
            const auto at = static_cast<size_t>(i);
            const int lv = static_cast<int>(think.levels[i]);
            const bool toggle = think.kind == ai::ThinkKind::Toggle;
            p.effort_names[at] = toggle ? Text(i ? I::AiAsstEffortOn : I::AiAsstEffortOff) : LevelName(think.levels[i]);
            p.effort_details[at] = Text(toggle ? (i ? I::AiAsstThinkOnDetail : I::AiAsstEffortOffDetail) : kLevelDetails[lv]);
            if (i == p.effort_default) p.effort_details[at] = Fill(Text(I::AiAsstEffortDefault), L"{detail}", p.effort_details[at]);
        }
        if (think.kind == ai::ThinkKind::Toggle) {
            p.effort_toggle_label = Text(I::AsstThinkToggle);
            p.effort_note = Text(I::AiAsstThinkToggleNote);
        } else if (!think.CanOff()) {
            p.effort_note = Text(I::AiAsstThinkAlwaysNote);
        }
    }
    // Connected once a service key is stored (ai_model_key.h).
    p.connected = AiAgentConnected();
    // No service yet: the welcome leads to Settings instead of suggestions.
    p.setup = !p.connected;
    if (p.setup) {
        p.setup_title = Text(I::AiAsstSetupTitle);
        p.setup_intro = Text(I::AiAsstSetupIntro);
        p.setup_more = Text(I::AiAsstSetupMore);
        p.setup_go = Text(I::AiAsstSetupGo);
        p.setup_marks = {"deepseek", "qwen", "kimi", "zhipu", "doubao", "mimo", "silicon", "openai", "ollama"};
        p.suggestions.clear();
    }
    AiVoiceFill(s, vm);

    p.hover = a.hover;
    p.pressed = a.pressed;
    // Header tips after a short dwell (the timer repaints).
    using P = ui::AiPanelPart;
    if (HasTip(a.hover.part) && a.pressed.part == P::None && GetTickCount64() - a.hover_since >= kTipDelayMs) {
        const P part = a.hover.part;
        std::wstring voice_tip;
        if (AiVoiceHasTip(part)) {
            if (AiVoiceTip(s, part, voice_tip)) vm.tooltip_text = std::move(voice_tip);
        } else if (part == P::MenuSlider) {
            if (p.effort_mode == ui::AiEffortMode::Levels && a.hover.index >= 0 && a.hover.index < p.effort_count)
                vm.tooltip_text = p.effort_names[static_cast<size_t>(a.hover.index)];
        } else if (part == P::HistPin) {
            vm.tooltip_text = Text(AiHistoryRowPinned(s, a.hover.index) ? I::AiHistUnpin : I::AiHistPin);
        } else vm.tooltip_text = Text(part == P::MenuReset ? I::RestoreDefaults
                               : part == P::History ? I::AiHistTip
                               : part == P::HistRename ? I::Rename
                               : part == P::HistDelete ? I::Delete
                               : part == P::NewChat ? I::AiAsstNewChat
                               : part == P::Close ? I::AiAsstCollapse
                               : part == P::ActCopy || part == P::UserCopy ? I::AiAsstCopy
                               : part == P::ActRetry ? I::AiRegen
                               : part == P::ActThumb ? I::AiAsstHelpful
                               : part == P::ActThumbDown ? I::AiNotHelpful
                               : part == P::VerPrev ? I::AiVerPrev
                               : part == P::VerNext ? I::AiVerNext
                               : part == P::UserEdit ? I::AiUserEdit
                               : a.floating ? I::AiAsstDock : I::AiAsstFloat);
        vm.tooltip_x = static_cast<float>(a.hover_point.x);
        vm.tooltip_y = static_cast<float>(a.hover_point.y);
    }
}

void AiAssistantPrefsChanged(AppState& s) {
    s.aiAssistant.prefs_loaded = false;
    LoadPrefs(s.aiAssistant);
    Redraw(s);
}

void AiAssistantLayout(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AssistantSettingsLayout(s);
    AiVoiceLayout(s);
    LayoutTurnEdit(s);
    LayoutHistEdits(s);
    if (!a.edit) return;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const bool menu_fading = !a.menu_open && ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    // 55 P3: 历史对话 covers the composer while it is there or sliding.
    const bool menu_covers = a.menu_open || menu_fading || AiVoiceCovers(s) || a.hist_open ||
                             view.HistoryAmount() > 0.001f;
    // Glass panel: the composer rests translucent with the text drawn by the
    // panel; the opaque EDIT shows only while it has (or is about to take)
    // focus.
    const bool resting = view.Glass() && !a.focus_pending && GetFocus() != a.edit;
    if (!a.open || !view.InputPlaced() || menu_covers || resting) {
        const bool was_visible = a.edit_visible;
        HideEdit(s);
        // The panel draws the text from the next frame on.
        if (was_visible && a.open) Redraw(s);
        // Show it again after the model menu's fade; AiVoiceLayout times the
        // voice menu's and card's own fades.
        if (a.open && menu_fading) Redraw(s);
        return;
    }
    EnsureFont(s);
    const D2D1_RECT_F r = view.InputRect();
    const int x = static_cast<int>(std::lround(r.left));
    const int y = static_cast<int>(std::lround(r.top));
    const int w = (std::max)(1, static_cast<int>(std::lround(r.right - r.left)));
    const int h = (std::max)(1, static_cast<int>(std::lround(r.bottom - r.top)));
    RECT cur{};
    GetWindowRect(a.edit, &cur);
    MapWindowPoints(nullptr, s.hwnd, reinterpret_cast<POINT*>(&cur), 2);
    const bool placed = cur.left == x && cur.top == y && cur.right - cur.left == w && cur.bottom - cur.top == h;
    if (!placed || !a.edit_visible) {
        if (!placed) {
            // The send button's busy arc (.send.busy::after, inset -4, 2 px)
            // reaches 4 px above the controls row, into the bottom-right corner
            // of the input box. The page's textarea is transparent; the EDIT
            // is not, so cut a circle out of it around the arc (+1 px).
            // Send: 32 round, right-aligned 2 px right of / 2 + 16 px below
            // the input rect.
            const float sc = s.scale;
            const float cx = r.right + 2.0f * sc - 16.0f * sc - r.left;
            const float cy = r.bottom + 2.0f * sc + 16.0f * sc - r.top;
            const float cr = 21.0f * sc;
            HRGN rgn = CreateRectRgn(0, 0, w, h);
            HRGN hole = CreateEllipticRgn(static_cast<int>(std::floor(cx - cr)), static_cast<int>(std::floor(cy - cr)),
                                          static_cast<int>(std::ceil(cx + cr)) + 1, static_cast<int>(std::ceil(cy + cr)) + 1);
            if (rgn && hole) CombineRgn(rgn, rgn, hole, RGN_DIFF);
            if (hole) DeleteObject(hole);
            if (rgn && !SetWindowRgn(a.edit, rgn, TRUE)) DeleteObject(rgn);
        }
        SetWindowPos(a.edit, HWND_TOP, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        a.edit_visible = true;
        Redraw(s);   // the panel stops drawing the text itself
    }
    // A theme switch changes the card colour on the next frame, but the EDIT
    // keeps its old pixels until something repaints it.
    if (const std::wstring cue = Text(I::AiAsstPlaceholder); cue != a.edit_cue || !GetPropW(a.edit, L"Pulse.EditCue")) {
        a.edit_cue = cue;
        SetPropW(a.edit, L"Pulse.EditCue", const_cast<wchar_t*>(a.edit_cue.c_str()));
        InvalidateRect(a.edit, nullptr, TRUE);
    }
    // The placeholder in the panel's text3, as the panel draws it itself.
    const UINT_PTR cue_rgb = 0x01000000u | ToColorRef(view.PlaceholderColor());
    if (reinterpret_cast<UINT_PTR>(GetPropW(a.edit, L"Pulse.EditCueColor")) != cue_rgb) {
        SetPropW(a.edit, L"Pulse.EditCueColor", reinterpret_cast<HANDLE>(cue_rgb));
        InvalidateRect(a.edit, nullptr, TRUE);
    }
    if (a.edit_back != CLR_INVALID &&
        (a.edit_back != ToColorRef(view.CardColor()) || a.edit_fore != ToColorRef(view.TextColor())))
        InvalidateRect(a.edit, nullptr, TRUE);
    if (a.focus_pending) {
        a.focus_pending = false;
        if (GetForegroundWindow() == s.hwnd || GetForegroundWindow() == GetAncestor(s.hwnd, GA_ROOT)) {
            SetFocus(a.edit);
            if (a.focus_click_set) {
                POINT pt = a.focus_click;
                MapWindowPoints(s.hwnd, a.edit, &pt, 1);
                const LRESULT hit = SendMessageW(a.edit, EM_CHARFROMPOS, 0, MAKELPARAM(pt.x, pt.y));
                const WPARAM ch = LOWORD(hit);
                SendMessageW(a.edit, EM_SETSEL, ch, static_cast<LPARAM>(ch));
            }
        }
        a.focus_click_set = false;
    }
}

bool AiAssistantMouseMove(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    ui::AiPanelView& view = s.renderer.AiPanel();
    if (view.ScrollDragging() != 0) {
        if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {   // the release went elsewhere
            view.EndScrollDrag();
            a.captured = false;
            if (GetCapture() == s.hwnd) ReleaseCapture();
        } else if (view.DragScroll(static_cast<float>(y))) {
            Redraw(s);
        }
        return true;
    }
    if (view.SliderDragging()) {
        if ((GetKeyState(VK_LBUTTON) & 0x8000) == 0) {   // the release went elsewhere
            SetEffortStop(s, view.EndSliderDrag());
            a.captured = false;
            a.pressed = {};
            if (GetCapture() == s.hwnd) ReleaseCapture();
        } else {
            view.DragSlider(static_cast<float>(x));
        }
        Redraw(s);
        return true;
    }
    if (a.selecting) {
        if (GetCapture() != s.hwnd) {   // the capture went elsewhere
            FinishSelect(s);
        } else {
            a.sel_drag = POINT{x, y};
            view.DragSelect(static_cast<float>(x), static_cast<float>(y));
            // Past the conversation's top or bottom: it scrolls while held.
            if (view.SelectScrollSpeed(static_cast<float>(y)) != 0.0f) SetTimer(s.hwnd, kSelectTimer, 16, SelectTimer);
        }
        Redraw(s);
        return true;
    }
    if (!Reachable(s) && !a.captured) {
        if (a.hover.part != ui::AiPanelPart::None) {
            a.hover = {};
            Redraw(s);
        }
        return false;
    }
    // 55 P3: under 历史对话 the conversation takes no pointer.
    const bool under_hist = view.HistoryCovers(static_cast<float>(x), static_cast<float>(y));
    if (view.SetScrollHot(under_hist ? -1.0e6f : static_cast<float>(x), under_hist ? -1.0e6f : static_cast<float>(y)))
        Redraw(s);
    if (const int turn = under_hist ? -1 : view.UserTurnAt(static_cast<float>(x), static_cast<float>(y));
        turn != a.hover_turn) {
        a.hover_turn = turn;   // the bubble's 复制 · 编辑 fade in
        Redraw(s);
    }
    const ui::AiPanelHit hit = HitAt(s, x, y);
    if (!(hit == a.hover)) {
        a.hover = hit;
        a.hover_since = GetTickCount64();
        using P = ui::AiPanelPart;
        if (HasTip(hit.part)) ShowTipLater(s);
        Redraw(s);
    }
    a.hover_point = POINT{x, y};
    if (hit.part == ui::AiPanelPart::None && !a.captured && !view.ScrollHot()) return false;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, s.hwnd, 0};
    TrackMouseEvent(&tme);
    return true;
}

bool AiAssistantMouseDown(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!Reachable(s)) return false;
    ui::AiPanelView& view = s.renderer.AiPanel();
    if (a.open && !view.HistoryCovers(static_cast<float>(x), static_cast<float>(y)) &&
        view.BeginScrollDrag(static_cast<float>(x), static_cast<float>(y))) {
        // The conversation's bar is outside the menus: they fold like for any click there.
        if (view.ScrollDragging() == 1 && (a.menu_open || AiVoiceMenuOpen(s))) CloseMenu(s);
        a.pressed = {};
        SetCapture(s.hwnd);
        a.captured = true;
        Redraw(s);
        return true;
    }
    const ui::AiPanelHit hit = HitAt(s, x, y);
    if (hit.part == ui::AiPanelPart::MenuSlider && view.BeginSliderDrag(static_cast<float>(x), static_cast<float>(y))) {
        // The knob jumps under the press and follows the drag.
        a.pressed = hit;
        a.hover = hit;
        SetCapture(s.hwnd);
        a.captured = true;
        Redraw(s);
        return true;
    }
    if (hit.part == ui::AiPanelPart::None) {
        // Outside: the menus fold away and the click goes on to Pulse (and
        // its Ctrl+C is the files' again).
        const bool had = view.HasSelection();
        view.ClearSelection();   // a bare caret too: Ctrl+A is the files' again
        if (had) Redraw(s);
        if (a.menu_open || AiVoiceMenuOpen(s)) {
            CloseMenu(s);
            Redraw(s);
        }
        return false;
    }
    if (a.open && hit.part == ui::AiPanelPart::Conversation) {
        // Text: a press selects (2 clicks a word, 3 a paragraph; Shift extends).
        const uint64_t now = GetTickCount64();
        const bool near_last = std::abs(x - a.sel_click_pt.x) <= GetSystemMetrics(SM_CXDOUBLECLK) / 2 + 1 &&
                               std::abs(y - a.sel_click_pt.y) <= GetSystemMetrics(SM_CYDOUBLECLK) / 2 + 1;
        a.sel_clicks = (near_last && now - a.sel_click_at <= GetDoubleClickTime()) ? a.sel_clicks % 3 + 1 : 1;
        a.sel_click_at = now;
        a.sel_click_pt = POINT{x, y};
        if (a.menu_open || AiVoiceMenuOpen(s)) CloseMenu(s);
        const bool extend = GetKeyState(VK_SHIFT) < 0;
        if (view.BeginSelect(static_cast<float>(x), static_cast<float>(y), a.sel_clicks, extend)) {
            // Ctrl+C / Ctrl+A come to the window, not the composer.
            if (a.edit && GetFocus() == a.edit) SetFocus(s.hwnd);
            a.selecting = true;
            a.sel_drag = POINT{x, y};
            a.pressed = {};
            SetCapture(s.hwnd);
            a.captured = true;
            Redraw(s);
            return true;
        }
        view.ClearSelection();
    } else if (!IsSelectionPart(hit.part)) {
        view.HideSelectionBar();
    }
    a.pressed = hit;
    a.hover = hit;
    SetCapture(s.hwnd);
    a.captured = true;
    Redraw(s);
    return true;
}

bool AiAssistantMouseUp(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    ui::AiPanelView& view = s.renderer.AiPanel();
    if (view.ScrollDragging() != 0) {
        view.EndScrollDrag();
        a.captured = false;
        if (GetCapture() == s.hwnd) ReleaseCapture();
        view.SetScrollHot(static_cast<float>(x), static_cast<float>(y));
        Redraw(s);
        return true;
    }
    if (view.SliderDragging()) {
        // Snaps to the nearest stop; the level applies right away.
        SetEffortStop(s, view.EndSliderDrag());
        a.captured = false;
        a.pressed = {};
        if (GetCapture() == s.hwnd) ReleaseCapture();
        a.hover = HitAt(s, x, y);
        Redraw(s);
        return true;
    }
    if (a.selecting) {
        FinishSelect(s);
        a.hover = HitAt(s, x, y);
        Redraw(s);
        return true;
    }
    if (!a.captured) return false;
    a.captured = false;
    if (GetCapture() == s.hwnd) ReleaseCapture();
    const ui::AiPanelHit pressed = a.pressed;
    a.pressed = {};
    const ui::AiPanelHit hit = HitAt(s, x, y);
    a.hover = hit;
    if (hit.part == ui::AiPanelPart::Input && hit == pressed && !a.edit_visible && a.text_len > 0) {
        a.focus_click = {x, y};
        a.focus_click_set = true;
    }
    if ((a.open || IsPeekPart(hit.part)) && hit == pressed) Activate(s, hit);
    Redraw(s);
    return true;
}

bool AiAssistantMouseOther(AppState& s, int x, int y) {
    const AiAssistantState& a = s.aiAssistant;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    return (a.open && view.Contains(static_cast<float>(x), static_cast<float>(y))) ||
           view.PeekHitTest(static_cast<float>(x), static_cast<float>(y)).part != ui::AiPanelPart::None;
}

std::wstring AiAssistantModelName(const AppState&) { return json::Wide(CurrentModel()); }

bool AiAssistantThinks(const AppState& s) { return ai::ThinksAt(CurrentThinking(), Preferred(s.aiAssistant)); }

std::wstring AiAssistantThinkName(const AppState& s) {
    const ai::ThinkSpec think = CurrentThinking();
    if (think.kind == ai::ThinkKind::Fixed) return Text(I::AiAsstThinkAuto);
    if (think.kind == ai::ThinkKind::Toggle) return Text(I::AiAsstEffortOn);
    return LevelName(ai::SnapThinking(think, Preferred(s.aiAssistant)));
}

bool AiAssistantWheel(AppState& s, int x, int y, int delta, bool horizontal) {
    if (!AiAssistantMouseOther(s, x, y)) return false;
    if (s.renderer.AiPanel().PeekHitTest(static_cast<float>(x), static_cast<float>(y)).part != ui::AiPanelPart::None)
        return true;   // over the bubble: nothing to scroll
    if (delta != 0 && (horizontal || GetKeyState(VK_SHIFT) < 0)) {
        // Sideways (a tilt or Shift + wheel): the code block or table under it.
        const float dx = static_cast<float>(horizontal ? delta : -delta) / WHEEL_DELTA * 48.0f * s.scale;
        if (s.renderer.AiPanel().ScrollBoxAt(static_cast<float>(x), static_cast<float>(y), dx)) {
            Redraw(s);
            return true;
        }
        if (horizontal) return true;
    }
    if (delta != 0 && !horizontal && IsHistoryPart(s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y)).part)) {
        // 55 P3: the history list scrolls on its own.
        s.renderer.AiPanel().HistoryScrollBy(-static_cast<float>(delta) / WHEEL_DELTA * 48.0f * s.scale);
        if (!s.aiAssistant.captured) s.aiAssistant.hover = {};
        Redraw(s);
        return true;
    }
    if (delta != 0 && !horizontal) {
        // Three 16 px lines a notch, like the page's list. What was under the
        // pointer moved away: no tip for it (the next move finds the new part).
        s.renderer.AiPanel().ScrollBy(-static_cast<float>(delta) / WHEEL_DELTA * 48.0f * s.scale);
        if (!s.aiAssistant.captured) s.aiAssistant.hover = {};
        Redraw(s);
    }
    return true;
}

void AiAssistantMouseLeave(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (s.renderer.AiPanel().SetScrollHot(-1.0e6f, -1.0e6f)) Redraw(s);
    if (a.hover_turn >= 0 && !a.captured) {
        a.hover_turn = -1;
        Redraw(s);
    }
    if (a.hover.part == ui::AiPanelPart::None || a.captured) return;
    a.hover = {};
    Redraw(s);
}

bool AiAssistantSetCursor(AppState& s, int x, int y) {
    const AiAssistantState& a = s.aiAssistant;
    if (!a.open || !s.renderer.AiPanel().Contains(static_cast<float>(x), static_cast<float>(y))) return false;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const ui::AiPanelHit hit = view.HitTest(static_cast<float>(x), static_cast<float>(y));
    using P = ui::AiPanelPart;
    LPCWSTR cursor = IDC_ARROW;
    if (hit.part == P::Input) cursor = IDC_IBEAM;
    else if (hit.part == P::RichLink || hit.part == P::RichPath) cursor = IDC_HAND;
    else if (a.selecting || (hit.part == P::Conversation && view.OverText(static_cast<float>(x), static_cast<float>(y))))
        cursor = IDC_IBEAM;
    SetCursor(LoadCursorW(nullptr, cursor));
    return true;
}

bool AiAssistantMenuKey(AppState& s, WPARAM vk) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open || !a.menu_open) return false;
    const ai::ThinkSpec think = CurrentThinking();
    if (!think.Adjustable()) return false;
    const int now = StopOf(think, Preferred(a));
    int stop = -1;
    if (vk == VK_LEFT || vk == VK_DOWN) stop = now - 1;
    else if (vk == VK_RIGHT || vk == VK_UP) stop = now + 1;
    else if (vk == VK_HOME) stop = 0;
    else if (vk == VK_END) stop = think.count - 1;
    else if (vk >= '1' && vk <= '9') stop = static_cast<int>(vk - '1');
    else if (vk >= VK_NUMPAD1 && vk <= VK_NUMPAD9) stop = static_cast<int>(vk - VK_NUMPAD1);
    else return false;
    if (stop >= 0 && stop < think.count) SetEffortStop(s, stop);
    Redraw(s);
    return true;
}

bool AiAssistantEscape(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    if (a.edit_id) {   // a message edited in place gives up first
        EndTurnEdit(s);
        return true;
    }
    if (a.menu_open) {
        CloseMenu(s);
        Redraw(s);
        return true;
    }
    if (a.hist_open) {   // 55 P3: the rename, the confirmation, then the layer
        if (AiHistoryRenaming(s)) EndHistRename(s, false);
        else if (AiHistoryEscape(s) && !a.hist_open) a.focus_pending = true;
        Redraw(s);
        return true;
    }
    if (AiVoiceEscape(s)) return true;   // the voice card, its menu, listening
    if (s.renderer.AiPanel().HasSelection()) {
        s.renderer.AiPanel().ClearSelection();
        Redraw(s);
        return true;
    }
    if (a.chat && a.chat->busy) {
        AiChatStop(s);
        return true;
    }
    return false;
}

bool AiAssistantSelectionKey(AppState& s, WPARAM vk) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open || GetKeyState(VK_CONTROL) >= 0) return false;
    ui::AiPanelView& view = s.renderer.AiPanel();
    const bool has = view.HasSelection();
    if ((vk == 'C' || vk == VK_INSERT) && has) {
        AiChatCopyText(s.hwnd, view.SelectionText(false));
        return true;
    }
    if (vk == 'A') {
        // The answer holding the selection or the caret of a plain click
        // (the message, for a message).
        const int block = has ? view.SelectionBlock() : view.CaretBlock();
        if (block < 0) return false;
        view.SelectAnswer(block);
        Redraw(s);
        return true;
    }
    return false;
}

bool AiAssistantContextMenu(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open || a.selecting) return false;
    ui::AiPanelView& view = s.renderer.AiPanel();
    if (view.HistoryCovers(static_cast<float>(x), static_cast<float>(y))) return false;   // 55 P3
    const ui::AiPanelHit hit = HitAt(s, x, y);
    using P = ui::AiPanelPart;
    const bool target = hit.part == P::RichLink || hit.part == P::RichPath;
    if (!target && hit.part != P::Conversation && !IsSelectionPart(hit.part)) return false;
    const float fx = static_cast<float>(x), fy = static_cast<float>(y);
    const bool has = view.HasSelection();
    const int block = view.RichBlockAt(fx, fy);
    if (!target && !has && (block < 0 || !view.OverText(fx, fy))) return false;
    if (!EnsureMenu(s)) return false;
    if (a.menu_open || AiVoiceMenuOpen(s)) CloseMenu(s);
    std::vector<ui::FluentMenuItem> items;
    items.reserve(8);   // `add` hands out pointers into it
    const auto add = [&](int command, std::wstring text, const wchar_t* glyph, const wchar_t* key = L"") {
        ui::FluentMenuItem item;
        item.command = command;
        item.text = std::move(text);
        item.glyph = glyph;
        item.shortcut = key;
        items.push_back(std::move(item));
        return &items.back();
    };
    const ui::AiPanelView::RichTargetInfo info = target ? view.TargetOf(hit) : ui::AiPanelView::RichTargetInfo{};
    if (target && info.text.empty()) return false;
    std::wstring query;
    if (hit.part == P::RichLink) {
        add(1, Text(I::AiMenuOpenLink), L"\xE774");
        add(2, Text(I::AiMenuCopyLink), L"\xE71B");
    } else if (hit.part == P::RichPath) {
        add(3, Text(I::OpenNewTab), L"\xE8A7");
        add(4, Text(I::AiMenuLocate), L"\xE838")->separator_after = true;   // the tray's 在所在位置显示 glyph
        add(5, Text(I::CopyPath), L"\xE8C8");
    } else {
        query = has ? SearchableText(view.SelectionText(false)) : std::wstring();
        add(10, Text(I::Copy), L"\xE8C8", L"Ctrl+C");
        add(11, Text(I::AiMenuCopyMarkdown), L"\xE943")->separator_after = true;
        add(12, Text(I::AiMenuSelectAnswer), L"\xE8B3", L"Ctrl+A");
        ui::FluentMenuItem* quote = add(13, Text(I::AiMenuQuote), L"\xE8BD");
        quote->enabled = has;
        if (!query.empty()) {
            quote->separator_after = true;
            add(14, Printf1(I::AiMenuSearchFor, Shortened(query, 24)), L"\xE721");
        }
    }
    view.HideSelectionBar();
    POINT pt{x, y};
    ClientToScreen(s.hwnd, &pt);
    s.menu->SetTheme(s.darkMode, s.accentColor);
    const int command = s.menu->TrackPopup(pt, std::move(items));
    switch (command) {
    case 1: OpenLink(s, info.text); break;
    case 2: CopyAndSay(s, info.text); break;
    case 3: OpenPath(s, info.text, true); break;
    case 4: OpenPath(s, info.text, false); break;
    case 5: CopyAndSay(s, info.text); break;
    case 10: CopyAndSay(s, AnswerText(view, block, has, false)); break;
    case 11: CopyAndSay(s, AnswerText(view, block, has, true)); break;
    case 12: view.SelectAnswer(has ? view.SelectionBlock() : block); break;
    case 13: QuoteText(s, view.SelectionText(false)); break;
    case 14: if (!query.empty()) NavigateTo(s, app::MakeSearchPath(query)); break;
    default: break;
    }
    Redraw(s);
    return true;
}

HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiAssistantState& a = s.aiAssistant;
    if (!edit) return nullptr;
    if (edit != a.edit && edit != a.turn_edit && edit != a.hist_search && edit != a.hist_rename) {
        return AssistantSettingsCtlColor(s, hdc, edit);   // Settings > AI 助手
    }
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const COLORREF back = ToColorRef(view.CardColor());
    if (!a.brush || a.brush_color != back) {
        if (a.brush) DeleteObject(a.brush);
        a.brush = CreateSolidBrush(back);
        a.brush_color = back;
    }
    SetTextColor(hdc, ToColorRef(view.TextColor()));
    SetBkColor(hdc, back);
    SetBkMode(hdc, OPAQUE);
    return a.brush;
}

void AiAssistantDpiChanged(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AssistantSettingsDpiChanged(s);
    if (!a.edit) return;
    EnsureFont(s);
    ReadComposerMetrics(a);
    Redraw(s);
}

void AiAssistantDestroy(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (s.hwnd) KillTimer(s.hwnd, kTipTimer);
    AiHistorySave(s);   // 55 P3: the conversation as Pulse closes
    AssistantSettingsDestroy(s);
    AiVoiceDestroy(s);
    if (a.edit) DestroyWindow(a.edit);
    a.edit = nullptr;
    if (a.turn_edit) DestroyWindow(a.turn_edit);
    a.turn_edit = nullptr;
    a.edit_id = 0;
    for (HWND* e : {&a.hist_search, &a.hist_rename}) {
        if (*e) DestroyWindow(*e);
        *e = nullptr;
    }
    if (a.font) DeleteObject(a.font);
    a.font = nullptr;
    if (a.brush) DeleteObject(a.brush);
    a.brush = nullptr;
}

} // namespace pulse
