// ai_assistant.cpp - Pulse 助手: panel state, composer EDIT, pointer routing.
#include "ai_assistant.h"

#include "ai_assistant_chat.h"
#include "ai_assistant_conn.h"
#include "ai_assistant_voice.h"
#include "ai_model_list.h"
#include "ai_settings.h"
#include "app_internal.h"
#include "app_navigation.h"
#include "keymap.h"
#include "../common/json_value.h"
#include "../common/localization.h"
#include "../ui/edit_host.h"
#include "../ui/typography.h"
#include "../ui/ui_motion.h"
#include "../ui/ui_renderer.h"

#include <commctrl.h>
#include <uxtheme.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace pulse {
namespace {

namespace ai = app::ai;
using I = l10n::StringId;

constexpr UINT_PTR kComposerSubclass = 0x4149;   // 'AI'
constexpr UINT_PTR kTipTimer = 0x4154;           // 'AT'
constexpr UINT kTipDelayMs = 520;
constexpr uint64_t kMenuFadeMs = 240;

// Models come from the connected service (ai_model_list.h) and are shown
// and sent as it names them. The two official DeepSeek ones get a line.
struct KnownModel { const char* id; I detail; };
constexpr KnownModel kKnownModels[] = {
    {"deepseek-flash", I::AiAsstModelFlashDetail},
    {"deepseek-v4-pro", I::AiAsstModelProDetail},
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
constexpr I kEffortNames[] = {I::AiAsstEffortOff, I::AiAsstEffortLow, I::AiAsstEffortMid, I::AiAsstEffortHigh};
constexpr I kEffortDetails[] = {I::AiAsstEffortOffDetail, I::AiAsstEffortLowDetail, I::AiAsstEffortMidDetail,
                                I::AiAsstEffortHighDetail};

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

void LoadPrefs(AiAssistantState& a) {
    if (a.prefs_loaded) return;
    a.prefs_loaded = true;
    const ai::Settings settings = ai::LoadSettings();
    a.floating = settings.assistant.floating;
    a.effort = std::clamp(settings.assistant.effort, 0, 3);
    a.model = 0;
    if (!settings.assistant.model.empty()) g_model = settings.assistant.model;
}

void SavePrefs(const AiAssistantState& a) {
    bool malformed = false;
    ai::Settings settings = ai::LoadSettings(&malformed);
    if (malformed) return;   // never overwrite a file we could not read
    settings.assistant.floating = a.floating;
    settings.assistant.model = g_model;
    settings.assistant.effort = a.effort;
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

bool Submit(AppState& s, const std::wstring& text) {
    if (Chat(s).busy) return false;
    AiChatSubmit(s, text, ContextText(s));
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
    if (a.edit && a.font) {
        SendMessageW(a.edit, WM_SETFONT, reinterpret_cast<WPARAM>(a.font), TRUE);
        SendMessageW(a.edit, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
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
    if (a.edit && a.line_px > 0) {
        // LumaText lays the text out at the same line height the composer is
        // measured with (1/64 px), and draws the placeholder itself.
        SetPropW(a.edit, L"Pulse.EditMultiline", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(a.line_px) * 64u));
    }
}

void EnsureEdit(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.edit || !s.hwnd) return;
    // A redirected layered child like the other hosted editors (edit_host.h),
    // but multi-line and wrapping (no ES_AUTOHSCROLL).
    a.edit = CreateWindowExW(WS_EX_LAYERED, L"EDIT", L"",
                             WS_CHILD | WS_CLIPSIBLINGS | WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN,
                             0, 0, 0, 0, s.hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!a.edit) return;
    SetLayeredWindowAttributes(a.edit, 0, 255, LWA_ALPHA);
    SetWindowTheme(a.edit, L"", L"");
    SendMessageW(a.edit, EM_LIMITTEXT, 8000, 0);
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

void CloseMenu(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiVoiceCloseMenu(s);
    AiConnClose(s);
    if (!a.menu_open) return;
    a.menu_open = false;
    a.menu_closed_at = ui::motion::NowMs();
}

// Header buttons and the answer's acts show a tip after a dwell.
bool HasTip(ui::AiPanelPart part) {
    using P = ui::AiPanelPart;
    return part == P::NewChat || part == P::Mode || part == P::Close || part == P::ActCopy || part == P::ActRetry ||
           part == P::ActThumb || part == P::ConnBack || part == P::ConnClose || part == P::ConnEye || AiVoiceHasTip(part);
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

void Activate(AppState& s, const ui::AiPanelHit& hit) {
    AiAssistantState& a = s.aiAssistant;
    using P = ui::AiPanelPart;
    if (IsPeekPart(hit.part)) {
        if (hit.part == P::PeekCard) return;
        AiChatHidePeek(s);
        if (hit.part == P::PeekUndo && a.chat && a.chat->peek_failed) {
            // A failed turn: 重试 in place, or 连接模型 opens 管理连接.
            if (a.chat->peek_tone == ui::AiErrTone::Accent) {
                AiAssistantOpen(s);
                a.menu_open = true;
                AiConnOpen(s);
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
    if (bool close_menu = false; AiConnActivate(s, hit, close_menu)) {
        if (close_menu) {
            CloseMenu(s);
            a.focus_pending = true;
        }
        Redraw(s);
        return;
    }
    // The mic, its menu and the voice card. The model menu gives way to them.
    if ((hit.part == P::Mic || hit.part == P::MicMenu) && a.menu_open) {
        AiConnClose(s);
        a.menu_open = false;
        a.menu_closed_at = ui::motion::NowMs();
    }
    if (AiVoiceActivate(s, hit)) {
        Redraw(s);
        return;
    }
    switch (hit.part) {
    case P::NewChat:
        CloseMenu(s);
        AiChatReset(s);
        if (a.edit) SetWindowTextW(a.edit, L"");
        a.context_removed_for.clear();
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
    case P::ContextRemove: {
        std::wstring path, name;
        size_t count = 0;
        if (CurrentFolder(s, path, name, count)) a.context_removed_for = path;
        a.focus_pending = true;
        break;
    }
    case P::ModelButton:
        if (a.menu_open) CloseMenu(s);
        else {
            AiVoiceCloseMenu(s);
            a.menu_open = true;
            if (AiAgentConnected()) ai::RefreshListedModels(s.hwnd, AiAgentBaseUrl());
        }
        break;
    case P::MenuModel: {
        const std::vector<std::string>& list = Listed();
        if (hit.index >= 0 && hit.index < static_cast<int>(list.size()) && list[hit.index] != CurrentModel()) {
            g_model = list[hit.index];
            SavePrefs(a);
        }
        CloseMenu(s);
        a.focus_pending = true;
        break;
    }
    case P::MenuEffort:
        // The menu stays open to show the chosen strength's line.
        if (hit.index >= 0 && hit.index < 4 && hit.index != a.effort) {
            a.effort = hit.index;
            SavePrefs(a);
        }
        break;
    case P::MenuManage:
        // The menu unfolds into the 管理连接 card in place.
        AiConnOpen(s);
        break;
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
    case P::CutRetry:
        CloseMenu(s);
        AiChatActivate(s, hit.part, hit.index);
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
            a.menu_open = true;
            AiConnOpen(s);
        } else if (act == ui::AiErrAct::SwitchModel) {
            a.menu_open = true;
            if (AiAgentConnected()) ai::RefreshListedModels(s.hwnd, AiAgentBaseUrl());
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

void AiAssistantOpen(AppState& s, bool animate) {
    AiAssistantState& a = s.aiAssistant;
    LoadPrefs(a);
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
    AiConnClose(s);
    AiVoiceStop(s);
    a.menu_open = false;
    a.hover = {};
    a.pressed = {};
    if (a.captured && GetCapture() == s.hwnd) ReleaseCapture();
    a.captured = false;
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

void FillAiAssistantVm(AppState& s, ui::WindowViewModel& vm) {
    AiAssistantState& a = s.aiAssistant;
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
    p.add_files = Text(I::AiAsstAddFiles);
    const std::vector<std::string>& listed = Listed();
    p.model_label = json::Wide(CurrentModel());
    p.effort = std::clamp(a.effort, 0, 3);
    p.effort_label = p.effort == 0 ? std::wstring() : L" \u00B7 " + Text(kEffortNames[p.effort]);
    p.send = chat.busy ? ui::AiSendState::Busy : p.composer_empty ? ui::AiSendState::Dim : ui::AiSendState::Ready;

    p.menu_open = a.menu_open;
    p.model_heading = Text(I::AiAsstModelHeading);
    p.effort_heading = Text(I::AiAsstEffortHeading);
    // Before the service listed anything: the chosen one alone.
    const std::vector<std::string> alone{CurrentModel()};
    const std::vector<std::string>& rows = listed.empty() ? alone : listed;
    for (const std::string& id : rows) {
        std::wstring detail;
        for (const KnownModel& k : kKnownModels)
            if (id == k.id) detail = Text(k.detail);
        p.models.push_back({json::Wide(id), std::move(detail)});
    }
    const auto chosen = std::find(rows.begin(), rows.end(), CurrentModel());
    p.model = chosen == rows.end() ? -1 : static_cast<int>(chosen - rows.begin());
    for (size_t i = 0; i < 4; ++i) p.effort_names[i] = Text(kEffortNames[i]);
    p.effort_detail = Text(kEffortDetails[p.effort]);
    // Connected once a DeepSeek key is stored (ai_model_key.h).
    p.connected = AiAgentConnected();
    p.connection_text = p.connected ? Fill(Text(I::AiAsstConnected), L"{host}", AiAgentHost())
                                    : Text(I::AiAsstNotConnected);
    p.manage_text = Text(I::AiAsstManage);
    AiConnFill(s, p);
    AiVoiceFill(s, vm);

    p.hover = a.hover;
    p.pressed = a.pressed;
    // Header tips after a short dwell (the timer repaints).
    using P = ui::AiPanelPart;
    if (HasTip(a.hover.part) && a.pressed.part == P::None && GetTickCount64() - a.hover_since >= kTipDelayMs) {
        const P part = a.hover.part;
        std::wstring conn_tip;
        if (AiVoiceHasTip(part)) {
            if (AiVoiceTip(s, part, conn_tip)) vm.tooltip_text = std::move(conn_tip);
        } else if (AiConnTip(s, part, conn_tip)) {
            // A card part under a pointer that has not moved since ‹ / ⌄.
            if (AiConnIsOpen(s)) vm.tooltip_text = std::move(conn_tip);
        } else vm.tooltip_text = Text(part == P::NewChat ? I::AiAsstNewChat
                               : part == P::Close ? I::AiAsstCollapse
                               : part == P::ActCopy ? I::AiAsstCopy
                               : part == P::ActRetry ? I::AiAsstRetry
                               : part == P::ActThumb ? I::AiAsstHelpful
                               : a.floating ? I::AiAsstDock : I::AiAsstFloat);
        vm.tooltip_x = static_cast<float>(a.hover_point.x);
        vm.tooltip_y = static_cast<float>(a.hover_point.y);
    }
}

void AiAssistantLayout(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    AiConnLayout(s);
    AiVoiceLayout(s);
    if (!a.edit) return;
    const ui::AiPanelView& view = s.renderer.AiPanel();
    const bool menu_fading = !a.menu_open && ui::motion::NowMs() - a.menu_closed_at < kMenuFadeMs;
    const bool menu_covers = a.menu_open || menu_fading || AiVoiceCovers(s);
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
    if (!Reachable(s) && !a.captured) {
        if (a.hover.part != ui::AiPanelPart::None) {
            a.hover = {};
            Redraw(s);
        }
        return false;
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
    if (hit.part == ui::AiPanelPart::None && !a.captured) return false;
    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, s.hwnd, 0};
    TrackMouseEvent(&tme);
    return true;
}

bool AiAssistantMouseDown(AppState& s, int x, int y) {
    AiAssistantState& a = s.aiAssistant;
    if (!Reachable(s)) return false;
    const ui::AiPanelHit hit = HitAt(s, x, y);
    if (hit.part == ui::AiPanelPart::None) {
        // Outside: the menus fold away and the click goes on to Pulse.
        if (a.menu_open || AiVoiceMenuOpen(s)) {
            CloseMenu(s);
            Redraw(s);
        }
        return false;
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

bool AiAssistantWheel(AppState& s, int x, int y, int delta) {
    if (!AiAssistantMouseOther(s, x, y)) return false;
    if (s.renderer.AiPanel().PeekHitTest(static_cast<float>(x), static_cast<float>(y)).part != ui::AiPanelPart::None)
        return true;   // over the bubble: nothing to scroll
    if (delta != 0) {
        // Three 16 px lines a notch, like the page's list.
        s.renderer.AiPanel().ScrollBy(-static_cast<float>(delta) / WHEEL_DELTA * 48.0f * s.scale);
        Redraw(s);
    }
    return true;
}

void AiAssistantMouseLeave(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (a.hover.part == ui::AiPanelPart::None || a.captured) return;
    a.hover = {};
    Redraw(s);
}

bool AiAssistantSetCursor(AppState& s, int x, int y) {
    const AiAssistantState& a = s.aiAssistant;
    if (!a.open || !s.renderer.AiPanel().Contains(static_cast<float>(x), static_cast<float>(y))) return false;
    const ui::AiPanelHit hit = s.renderer.AiPanel().HitTest(static_cast<float>(x), static_cast<float>(y));
    const bool text = hit.part == ui::AiPanelPart::Input || AiConnIsField(hit.part) || AiVoiceIsField(hit.part);
    SetCursor(LoadCursorW(nullptr, text ? IDC_IBEAM : IDC_ARROW));
    return true;
}

bool AiAssistantEscape(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (!a.open) return false;
    if (a.menu_open) {
        CloseMenu(s);
        Redraw(s);
        return true;
    }
    if (AiVoiceEscape(s)) return true;   // the voice card, its menu, listening
    if (a.chat && a.chat->busy) {
        AiChatStop(s);
        return true;
    }
    return false;
}

HBRUSH AiAssistantCtlColor(AppState& s, HDC hdc, HWND edit) {
    AiAssistantState& a = s.aiAssistant;
    if (!edit) return nullptr;
    if (edit != a.edit) {
        if (HBRUSH voice = AiVoiceCtlColor(s, hdc, edit)) return voice;
        return AiConnCtlColor(s, hdc, edit);
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
    AiConnDpiChanged(s);
    AiVoiceDpiChanged(s);
    if (!a.edit) return;
    EnsureFont(s);
    ReadComposerMetrics(a);
    Redraw(s);
}

void AiAssistantDestroy(AppState& s) {
    AiAssistantState& a = s.aiAssistant;
    if (s.hwnd) KillTimer(s.hwnd, kTipTimer);
    AiConnDestroy(s);
    AiVoiceDestroy(s);
    if (a.edit) DestroyWindow(a.edit);
    a.edit = nullptr;
    if (a.font) DeleteObject(a.font);
    a.font = nullptr;
    if (a.brush) DeleteObject(a.brush);
    a.brush = nullptr;
}

} // namespace pulse
