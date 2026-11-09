#include "edit_host.h"
#include "dialog_recovery_test.h"
#include "../common/windows_compat.h"
#include "advanced_search_dialog.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "fluent_menu.h"
#include "folder_picker_dialog.h"
#include "svg_path_icon.h"
#include "typography.h"
#include "ui_compositor.h"
#include "window_helpers.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <algorithm>
#include <cmath>
#include <cwctype>
#include <string>
#include <vector>

#pragma comment(lib, "uxtheme.lib")

// Advanced search, in the dialog family of batch rename / confirm / progress:
//   52-dip title bar (tile, title, scope subtitle, names | contents switch)
//   -> one keyword box -> "try" recipe chips -> a card of type/date/size/
//   location rows -> collapsible expert options -> a footer band with a
//   plain-language summary and the live result count.
namespace pulse::ui {
namespace {

using I = l10n::StringId;

constexpr wchar_t kClass[] = L"PulseAdvancedSearchWindow";
constexpr float kDlgW = 640.0f;
constexpr float kPad = 18.0f;       // body inset, as batch rename
constexpr float kTitleH = 52.0f;
constexpr float kLabelW = 66.0f;    // condition labels inside the card
constexpr float kPillH = 28.0f;
constexpr float kChipH = 26.0f;
constexpr float kRowH = 46.0f;
constexpr float kFootH = 68.0f;
constexpr UINT_PTR kCountTimer = 73;
constexpr UINT_PTR kRecoveryTimer = 74;

enum HitId : int {
    kHitNone = 0, kHitSearch = 1, kHitCancel = 2, kHitClose = 3,
    kHitWholeWord = 11, kHitMatchCase = 12, kHitClear = 13, kHitMore = 20,
    kHitRecipe = 30, kHitMode = 40,
    kHitType = 100, kHitDate = 200, kHitSize = 300, kHitLocation = 400,
    kHitNameHow = 500, kHitContentMode = 600,
};

std::wstring Format(I id, const std::wstring& a) {
    std::wstring out(a.size() + 256, L'\0');
    const int n = swprintf_s(out.data(), out.size(), l10n::Get(id).c_str(), a.c_str());
    out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    return out;
}

// English labels are title-case ("This week"); mid-sentence they read better lower-case.
std::wstring LowerFirst(std::wstring text) {
    if (!text.empty() && text[0] < 0x80) text[0] = static_cast<wchar_t>(towlower(text[0]));
    return text;
}

std::wstring FolderLeaf(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos || slash + 1 >= p.size() ? p : p.substr(slash + 1);
}

std::wstring GroupDigits(size_t value) {
    std::wstring digits = std::to_wstring(value);
    for (int i = static_cast<int>(digits.size()) - 3; i > 0; i -= 3)
        digits.insert(static_cast<size_t>(i), 1, L',');
    return digits;
}

std::wstring KindLabel(index::SearchKind kind) {
    switch (kind) {
    case index::SearchKind::Folder: return l10n::Get(I::KindFolder);
    case index::SearchKind::Document: return l10n::Get(I::KindDocument);
    case index::SearchKind::Image: return l10n::Get(I::KindImage);
    case index::SearchKind::Video: return l10n::Get(I::KindVideo);
    case index::SearchKind::Audio: return l10n::Get(I::KindAudio);
    case index::SearchKind::Archive: return l10n::Get(I::KindArchive);
    case index::SearchKind::Code: return l10n::Get(I::KindCode);
    case index::SearchKind::Custom: return l10n::Get(I::KindCustom);
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring ExtLabel(const std::wstring& exts) {
    std::wstring out = L".";
    for (wchar_t c : exts) {
        if (c == L';') out += L" .";
        else out.push_back(static_cast<wchar_t>(towupper(c)));
    }
    return out;
}

std::wstring DateLabel(app::DatePreset preset) {
    switch (preset) {
    case app::DatePreset::Today: return l10n::Get(I::DateToday);
    case app::DatePreset::Yesterday: return l10n::Get(I::DateYesterday);
    case app::DatePreset::ThisWeek: return l10n::Get(I::DateThisWeek);
    case app::DatePreset::ThisMonth: return l10n::Get(I::DateThisMonth);
    case app::DatePreset::ThisYear: return l10n::Get(I::DateThisYear);
    case app::DatePreset::Custom: return l10n::Get(I::AdvCustom);
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring SizeLabel(app::SizePreset preset, const std::wstring& custom) {
    switch (preset) {
    case app::SizePreset::Empty: return l10n::Get(I::SizeEmpty);
    case app::SizePreset::Lt1MB: return l10n::Get(I::SizeLt1MB);
    case app::SizePreset::From1To10MB: return l10n::Get(I::Size1To10MB);
    case app::SizePreset::Gt10MB: return l10n::Get(I::SizeGt10MB);
    case app::SizePreset::Gt100MB: return l10n::Get(I::SizeGt100MB);
    case app::SizePreset::Gt1GB: return l10n::Get(I::SizeGt1GB);
    case app::SizePreset::Custom: {
        std::wstring text = custom;
        for (size_t at; (at = text.find(L"size:")) != std::wstring::npos;) text.erase(at, 5);
        return text.empty() ? l10n::Get(I::AdvCustom) : text;
    }
    default: return l10n::Get(I::KindAny);
    }
}

std::wstring NameHowLabel(app::NameMatchHow how) {
    switch (how) {
    case app::NameMatchHow::StartsWith: return l10n::Get(I::AdvSearchStarts);
    case app::NameMatchHow::Exact: return l10n::Get(I::AdvSearchExact);
    default: return l10n::Get(I::AdvSearchContains);
    }
}

std::wstring ContentModeLabel(index::ContentMatchMode mode) {
    switch (mode) {
    case index::ContentMatchMode::Phrase: return l10n::Get(I::ContentPhrase);
    case index::ContentMatchMode::AnyWord: return l10n::Get(I::ContentAnyWord);
    default: return l10n::Get(I::ContentAllWords);
    }
}

enum class Ico { None, Globe, Folder, FolderAdd };

struct Pill {
    D2D1_RECT_F bounds{};
    std::wstring text;
    Ico icon = Ico::None;
    int id = 0;
    bool selected = false;
    bool enabled = true;
    bool chip = false;   // a "try" recipe chip rather than a condition pill
};

// ---- look (same palette as batch rename) ---------------------------------------

D2D1_COLOR_F Hex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(((rgb >> 16) & 0xFF) / 255.0f, ((rgb >> 8) & 0xFF) / 255.0f, (rgb & 0xFF) / 255.0f, a);
}
D2D1_COLOR_F Mix(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}
D2D1_COLOR_F Alpha(D2D1_COLOR_F c, float a) { c.a *= a; return c; }

struct Pal {
    D2D1_COLOR_F accent, accent_soft, accent_line, card, solid, sheet, line, line2, text, text2, text3, hover, danger;
};

Pal MakePal(bool dark, bool high_contrast, const Theme& theme, bool glass) {
    Pal p{};
    if (high_contrast) {
        p.card = p.solid = p.sheet = theme.bg;
        p.line = p.line2 = theme.text;
        p.text = p.text2 = p.text3 = theme.text;
        p.hover = Alpha(theme.text, 0.15f);
        p.accent = theme.accent;
        p.accent_soft = theme.bg;
        p.accent_line = theme.accent;
        p.danger = theme.danger;
        return p;
    }
    if (!dark) {
        p.card = Hex(0xffffff); p.sheet = Hex(0xeef0ec); p.line = Hex(0xe3e4dd); p.line2 = Hex(0xecede7);
        p.text = Hex(0x1f2328); p.text2 = Hex(0x5b626c); p.text3 = Hex(0x8d939b); p.hover = Hex(0xe9ebe4);
    } else {
        p.card = Hex(0x2a2d2b); p.sheet = Hex(0x202221); p.line = Hex(0x353936); p.line2 = Hex(0x2d302e);
        p.text = Hex(0xe6e8e5); p.text2 = Hex(0xb1b6b0); p.text3 = Hex(0x858b85); p.hover = Hex(0x333734);
    }
    p.accent = theme.accent;
    p.accent.a = 1.0f;
    p.accent_soft = Mix(p.card, p.accent, dark ? 0.20f : 0.13f);
    p.accent_line = Alpha(p.accent, 0.35f);
    p.danger = theme.danger;
    p.solid = p.card;
    if (glass) {
        // Over acrylic the cards and footer let the frost through (效果图 ④).
        p.card = GlassCard(dark);
        p.sheet = GlassSheet(dark);
    }
    return p;
}

constexpr SvgPart kIcoTile[] = {
    {"M6.6 2.3a4.3 4.3 0 1 0 0 8.6a4.3 4.3 0 1 0 0-8.6z", false, 1.4f, 1.0f},
    {"M9.8 9.8l3.6 3.6", false, 1.4f, 1.0f}, {"M4.6 5.6h4M4.6 7.6h2.6", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoModeName[] = {{"M3 3.5h10M8 3.5v9.5", false, 1.4f, 1.0f}};
constexpr SvgPart kIcoModeContent[] = {
    {"M4 1.8h5.2L12.5 5v9.2H4z", false, 1.3f, 1.0f}, {"M6 8h4.5M6 10.5h4.5", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoGlobe[] = {
    {"M8 2a6 6 0 1 0 0 12a6 6 0 1 0 0-12z", false, 1.3f, 1.0f},
    {"M2 8h12M8 2c2 2 2.6 4 2.6 6c0 2-.6 4-2.6 6M8 2C6 4 5.4 6 5.4 8c0 2 .6 4 2.6 6", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoFolder[] = {
    {"M1.8 4.5a1 1 0 0 1 1-1h3.4l1.4 1.5h5.6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H2.8a1 1 0 0 1-1-1z", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoFolderAdd[] = {
    {"M1.8 4.5a1 1 0 0 1 1-1h3.4l1.4 1.5h5.6a1 1 0 0 1 1 1v6a1 1 0 0 1-1 1H2.8a1 1 0 0 1-1-1z", false, 1.3f, 1.0f},
    {"M8 7.5v3.5M6.3 9.2H9.7", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoX[] = {{"M3 3l8 8M11 3l-8 8", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoChevRight[] = {{"M4.5 2.5L8 6l-3.5 3.5", false, 1.3f, 1.0f}};
constexpr SvgPart kIcoChevDown[] = {{"M2.5 4.5L6 8l3.5-3.5", false, 1.3f, 1.0f}};

enum Fmt { kF15b, kF13, kF13b, kF125, kF12, kF12b, kFmtCount };

struct RowLabel {
    D2D1_RECT_F bounds{};
    std::wstring text;
};

class AdvancedSearchWindow {
public:
    AdvancedSearchWindow() = default;
    AdvancedSearchWindow(const AdvancedSearchWindow&) = delete;
    AdvancedSearchWindow& operator=(const AdvancedSearchWindow&) = delete;
    ~AdvancedSearchWindow() { ReleaseFormats(); }
#ifdef PULSE_WITH_SELFTEST
    bool TestDeviceRecovery(HWND host) {
        hwnd_ = host;
        if (!compositor_.Init(host)) return false;
        compositor_.RecreateTextFormats(scale_);
        Layout();
        compositor_.NotifyDeviceLost(D2DERR_RECREATE_TARGET);
        Render();
        return !compositor_.NeedsRecovery() && compositor_.Dc() && compositor_.TextFormat();
    }
#endif
#ifdef PULSE_ADVANCED_QUERY_TEST
    AdvancedSearchDialogResult TestQuery(app::AdvancedSearchSpec spec, int field,
                                         const std::wstring& value) {
        spec_ = std::move(spec);
        content_mode_ = !spec_.content.empty();
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kClass,
            L"Private query fixture", WS_POPUP, 0, 0, 760, 680, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd_) return {};
        Recompute(false);
        if (field == 1) SetWindowTextW(edit_kw_, value.c_str());
        if (field == 2) SetWindowTextW(edit_exts_, value.c_str());
        if (field == 3) SetWindowTextW(edit_exclude_, value.c_str());
        SendMessageW(hwnd_, WM_COMMAND, IDOK, 0);
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        return result_;
    }
#endif
    AdvancedSearchDialogResult Show(HWND owner, app::AdvancedSearchSpec spec, bool dark,
                                    D2D1_COLOR_F accent, AdvancedSearchCountRequest count, bool selection_outline) {
        owner_ = owner;
        selection_outline_ = selection_outline;
        spec_ = std::move(spec);
        dark_ = dark;
        accent_ = accent;
        count_request_ = std::move(count);
        content_mode_ = !spec_.content.empty();
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner ? owner : GetDesktopWindow())) / 96.0f;
        result_ = {};

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(kDlgW * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass,
            l10n::Get(I::AdvancedSearch).c_str(),
            WS_POPUP | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width,
            static_cast<int>(560.0f * scale_), owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        Recompute(false);
        const int height = static_cast<int>(std::ceil(height_px_));
        SetWindowPos(hwnd_, nullptr, 0, 0, width, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        Render();
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdits();
        SetForegroundWindow(hwnd_);
        if (edit_kw_) {
            SetFocus(edit_kw_);
            SendMessageW(edit_kw_, EM_SETSEL, 0, -1);
        }
        RequestCountSoon();

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (IsDialogMessageW(hwnd_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        done_ = true;
        if (IsWindow(hwnd_)) HideComposedDialog(hwnd_, owner_);
        DestroyEdits();
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) EnableWindow(owner_, TRUE);
        return result_;
    }


#ifdef PULSE_WITH_SELFTEST
    void TestRecovery(HWND owner, float scale, bool dark, const std::wstring& png,
                      const DialogRecoveryCheck& check) {
        owner_ = owner; scale_ = scale; dark_ = dark; more_ = true;
        accent_ = D2D1::ColorF(0.1f, 0.4f, 0.8f);
        spec_.name = L"draft 搜索 report";
        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kClass; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kClass, L"Recovery fixture",
            WS_POPUP, 0, 0, static_cast<int>(kDlgW * scale_), static_cast<int>(560 * scale_),
            owner, nullptr, wc.hInstance, this);
        check(hwnd_ && edit_kw_, "advanced private production window and EDIT created");
        if (!hwnd_) return;
        Recompute(false);
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE); LayoutEdits(); Render();
        SetFocus(edit_kw_); SendMessageW(edit_kw_, EM_SETSEL, 2, 8);
        const HWND edit = edit_kw_;
        const auto draft = EditText(edit);
        check(GetFocus() == edit, "advanced real EDIT owns focus before loss");
        auto* old_dc = compositor_.Dc();
        if (old_dc) old_dc->AddRef();
        compositor_.NotifyDeviceLost(DXGI_ERROR_DEVICE_REMOVED); Render();
        check(!compositor_.NeedsRecovery() && compositor_.Dc() && compositor_.Dc() != old_dc,
              "advanced replaces lost device resources");
        if (old_dc) old_dc->Release();
        auto state_kept = [&] {
            DWORD begin = 0, end = 0; SendMessageW(edit, EM_GETSEL,
                reinterpret_cast<WPARAM>(&begin), reinterpret_cast<LPARAM>(&end));
            return edit_kw_ == edit && IsWindow(edit) && EditText(edit) == draft &&
                GetFocus() == edit && begin == 2 && end == 8;
        };
        check(state_kept(), "advanced recovery preserves draft HWND focus and selection");
        snapshot_path_ = png;
        Render();
        check(snapshot_saved_, "advanced recovered frame captured before flip presentation");
        snapshot_path_.clear();
        SetPropW(hwnd_, L"Pulse.Test.RecoveryFailure", reinterpret_cast<HANDLE>(1));
        compositor_.NotifyDeviceLost(DXGI_ERROR_DEVICE_REMOVED); Render();
        const auto attempts = recovery_attempts_;
        for (int i = 0; i < 20; ++i) Render();
        check(compositor_.NeedsRecovery() && recovery_attempts_ == attempts,
              "advanced failed recovery backs off instead of repaint spin");
        RemovePropW(hwnd_, L"Pulse.Test.RecoveryFailure");
        Sleep(275); Render();
        check(!compositor_.NeedsRecovery() && state_kept(), "advanced retries transient failure with state retained");
        SendMessageW(edit, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(L"changed"));
        check(EditText(edit) != draft, "advanced recovered editor accepts replacement");
        SendMessageW(edit, EM_UNDO, 0, 0);
        check(EditText(edit) == draft, "advanced recovered editor preserves native undo");
        DestroyEdits(); DestroyWindow(hwnd_); hwnd_ = nullptr;
    }
#endif

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<AdvancedSearchWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<AdvancedSearchWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam) : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F R(float x, float y, float w, float h) const { return DipRect(scale_, x, y, w, h); }
    D2D1_RECT_F CloseRect() const { return R(kDlgW - 8 - 36, 8, 36, 36); }

    // ---- state helpers -------------------------------------------------------------

    bool RecipeActive(int recipe) const {
        const bool plain_size = spec_.size == app::SizePreset::Any;
        switch (recipe) {
        case 0: return !content_mode_ && spec_.kind == index::SearchKind::Document &&
                       spec_.date == app::DatePreset::ThisMonth && plain_size;
        case 1: return !content_mode_ && spec_.kind == index::SearchKind::Any &&
                       spec_.date == app::DatePreset::Any && spec_.size == app::SizePreset::Gt100MB;
        case 2: return !content_mode_ && spec_.kind == index::SearchKind::Image &&
                       spec_.date == app::DatePreset::ThisWeek && plain_size;
        case 3: return content_mode_ && spec_.kind == index::SearchKind::Document &&
                       spec_.date == app::DatePreset::Any && plain_size;
        default: return false;
        }
    }

    void SyncFromEdits() {
        if (!edits_ready_) return;
        const std::wstring keyword = EditText(edit_kw_);
        const std::wstring exclude = EditText(edit_exclude_);
        if (content_mode_) {
            spec_.content = keyword;
            spec_.name = EditText(edit_name_also_);
            spec_.content_exclude = exclude;
        } else {
            spec_.name = keyword;
            spec_.exclude_name = exclude;
        }
        spec_.custom_exts = app::NormalizeExtensionList(EditText(edit_exts_));
        if (!spec_.custom_exts.empty()) spec_.kind = index::SearchKind::Custom;
        else if (spec_.kind == index::SearchKind::Custom) spec_.kind = index::SearchKind::Any;
    }

    std::wstring ScopeName() const {
        if (spec_.location == app::LocationScope::CurrentFolder && !spec_.current_folder.empty())
            return FolderLeaf(spec_.current_folder);
        if (spec_.location == app::LocationScope::CustomFolder && !spec_.custom_folder.empty())
            return FolderLeaf(spec_.custom_folder);
        return l10n::Get(I::LocationIndexed);
    }

    // What the collapsed expert options hold, in faint text after the toggle.
    std::wstring MoreSummary() const {
        std::vector<std::wstring> parts;
        auto add = [&](I label, const std::wstring& value) {
            wchar_t text[512]{};
            swprintf_s(text, l10n::Get(I::AdvLabelValueFormat).c_str(), l10n::Get(label).c_str(), value.c_str());
            parts.push_back(text);
        };
        add(I::AdvMatch, content_mode_ ? ContentModeLabel(spec_.content_mode) : NameHowLabel(spec_.name_how));
        if (!spec_.custom_exts.empty()) add(I::AdvSearchExtensions, ExtLabel(spec_.custom_exts));
        const std::wstring& exclude = content_mode_ ? spec_.content_exclude : spec_.exclude_name;
        if (!exclude.empty()) add(I::AdvSearchExclude, exclude);
        if (content_mode_ && !spec_.name.empty()) add(I::AdvSearchName, spec_.name);
        std::wstring out;
        for (const auto& part : parts) {
            if (!out.empty()) out += L" \u00b7 ";
            out += part;
        }
        return out;
    }

    std::vector<std::wstring> SummaryParts() const {
        std::vector<std::wstring> parts;
        std::wstring what = spec_.kind == index::SearchKind::Any ? l10n::Get(I::AdvSumFiles)
            : spec_.kind == index::SearchKind::Custom ? ExtLabel(spec_.custom_exts)
            : LowerFirst(KindLabel(spec_.kind));
        parts.push_back(Format(I::AdvSumFindFormat, what));
        std::wstring keyword = content_mode_ ? spec_.content : spec_.name;
        if (content_mode_) {
            parts.push_back(keyword.empty() ? l10n::Get(I::AdvSumKeywordNeeded)
                                            : Format(I::AdvSumContentFormat, keyword));
            if (!spec_.name.empty()) parts.push_back(Format(I::AdvSumNameFormat, spec_.name));
        } else if (!keyword.empty()) {
            parts.push_back(Format(I::AdvSumNameFormat, keyword));
        }
        if (spec_.date != app::DatePreset::Any)
            parts.push_back(Format(I::AdvSumDateFormat, LowerFirst(DateLabel(spec_.date))));
        if (spec_.size != app::SizePreset::Any)
            parts.push_back(LowerFirst(SizeLabel(spec_.size, spec_.size_custom)));
        const std::wstring& exclude = content_mode_ ? spec_.content_exclude : spec_.exclude_name;
        if (!exclude.empty()) parts.push_back(Format(I::AdvSumExcludeFormat, exclude));
        parts.push_back(Format(I::AdvSumInFormat, ScopeName()));
        return parts;
    }

    std::wstring Summary() const {
        std::wstring out;
        for (const auto& part : SummaryParts()) {
            if (!out.empty()) out += L" \u00b7 ";
            out += part;
        }
        return out;
    }

    std::wstring CountLine(bool& danger) const {
        danger = false;
        if (scope_error_) {
            danger = true;
            return l10n::Get(I::AdvancedSearchNeedScopeMessage);
        }
        if (content_mode_) return l10n::Get(I::AdvCountContent);
        if (!count_request_) return {};
        if (count_pending_ || !count_known_) return l10n::Get(I::AdvCountBusy);
        if (count_ == 0) return l10n::Get(I::AdvCountNone);
        return Format(I::AdvCountFormat, GroupDigits(count_));
    }

    // ---- layout --------------------------------------------------------------------

    float X(float dip) const { return dip * scale_; }

    float PillWidth(const Pill& pill) const {
        if (pill.chip) return std::ceil(Measure(pill.text, kF125) + X(22 + 2));
        float w = Measure(pill.text, kF13b) + X(24 + 2);  // semibold when chosen: no width jump
        if (pill.icon != Ico::None) w += X(14 + 6);
        return std::ceil(w);
    }

    // Lays pills out left to right, wrapping inside [left, right]. Returns the bottom.
    float FlowPills(std::vector<Pill> row, float left, float right, float top, float h) {
        const float gap = X(6);
        float x = left, y = top;
        for (auto& pill : row) {
            const float w = std::min(right - left, PillWidth(pill));
            if (x > left && x + w > right) {
                x = left;
                y += h + gap;
            }
            pill.bounds = D2D1::RectF(x, y, x + w, y + h);
            x += w + gap;
            pills_.push_back(std::move(pill));
        }
        return y + h;
    }

    // One condition row inside a card: label on the left, pills flowing on the
    // right; at least 46 dip high, taller when the pills wrap.
    float PillRow(I label, std::vector<Pill> row, const D2D1_RECT_F& card, float top) {
        const float pills_bottom = FlowPills(std::move(row), card.left + X(14 + kLabelW + 6), card.right - X(12),
                                             top + X(9), X(kPillH));
        const float bottom = std::max(top + X(kRowH), pills_bottom + X(9));
        rows_.push_back({D2D1::RectF(card.left + X(14), top + X(9), card.left + X(14 + kLabelW), top + X(9 + kPillH)),
                         l10n::Get(label)});
        return bottom;
    }

    // A row holding a single-line field (expert options).
    float FieldRow(I label, D2D1_RECT_F& field, const D2D1_RECT_F& card, float top) {
        field = D2D1::RectF(card.left + X(14 + kLabelW + 6), top + X(7), card.right - X(12), top + X(7 + 32));
        rows_.push_back({D2D1::RectF(card.left + X(14), top + X(9), card.left + X(14 + kLabelW), top + X(9 + kPillH)),
                         l10n::Get(label)});
        return top + X(kRowH);
    }

    void Layout() {
        EnsureFormats();
        pills_.clear();
        rows_.clear();
        separators_.clear();
        const float s = scale_;
        const float left = kPad * s;
        const float right = (kDlgW - kPad) * s;

        // Title bar: names | contents switch left of the close button.
        {
            const std::wstring a = l10n::Get(I::AdvModeName), b = l10n::Get(I::AdvModeContent);
            const float w0 = X(12 + 14 + 6 + 12) + Measure(a, kF13);
            const float w1 = X(12 + 14 + 6 + 12) + Measure(b, kF13);
            const float mw = w0 + w1 + X(6);
            const D2D1_RECT_F close = CloseRect();
            seg_rect_ = D2D1::RectF(close.left - X(6) - mw, X(9), close.left - X(6), X(9 + 34));
            mode_rect_[0] = D2D1::RectF(seg_rect_.left + X(3), seg_rect_.top + X(3), seg_rect_.left + X(3) + w0,
                                        seg_rect_.bottom - X(3));
            mode_rect_[1] = D2D1::RectF(mode_rect_[0].right, mode_rect_[0].top, mode_rect_[0].right + w1,
                                        mode_rect_[0].bottom);
        }

        // Keyword box; in content mode it carries the whole-word / match-case toggles.
        float y = X(kTitleH + 4);
        kw_rect_ = D2D1::RectF(left, y, right, y + X(40));
        ww_rect_ = mc_rect_ = D2D1::RectF(0, 0, 0, 0);
        float edit_right = kw_rect_.right;
        if (content_mode_) {
            const float wa = Measure(L"Aa", kF12b) + X(12);
            const float ww = Measure(l10n::Get(I::AdvWholeWordShort), kF12b) + X(12);
            mc_rect_ = D2D1::RectF(kw_rect_.right - X(8) - wa, kw_rect_.top + X(8), kw_rect_.right - X(8),
                                   kw_rect_.bottom - X(8));
            ww_rect_ = D2D1::RectF(mc_rect_.left - X(2) - ww, mc_rect_.top, mc_rect_.left - X(2), mc_rect_.bottom);
            edit_right = ww_rect_.left + X(10) - X(6);
        }
        // PlaceEdit insets 10 dip; the search glyph takes 18 + 6.
        kw_edit_rect_ = D2D1::RectF(kw_rect_.left + X(24), kw_rect_.top, edit_right, kw_rect_.bottom);
        y = kw_rect_.bottom + X(10);

        // "Try": a whole search in one click.
        {
            const std::wstring label = l10n::Get(I::AdvTry);
            const float lw = Measure(label, kF125);
            try_label_rect_ = D2D1::RectF(left, y, left + lw, y + X(kChipH));
            std::vector<Pill> row;
            const I labels[] = {I::AdvRecipeDocs, I::AdvRecipeBig, I::AdvRecipeImages, I::AdvRecipeMentions};
            for (int i = 0; i < 4; ++i) {
                Pill chip{{}, l10n::Get(labels[i]), Ico::None, kHitRecipe + i, RecipeActive(i), true, true};
                row.push_back(std::move(chip));
            }
            y = FlowPills(std::move(row), left + lw + X(8), right, y, X(kChipH));
        }

        // Conditions card.
        card_[0] = D2D1::RectF(left, y + X(14), right, y + X(14));
        {
            float row_top = card_[0].top;
            std::vector<Pill> row;
            const index::SearchKind kinds[] = {
                index::SearchKind::Any, index::SearchKind::Folder, index::SearchKind::Document,
                index::SearchKind::Image, index::SearchKind::Video, index::SearchKind::Audio,
                index::SearchKind::Archive, index::SearchKind::Code};
            for (auto kind : kinds)
                row.push_back({{}, KindLabel(kind), Ico::None, kHitType + static_cast<int>(kind), spec_.kind == kind, true});
            if (spec_.kind == index::SearchKind::Custom && !spec_.custom_exts.empty())
                row.push_back({{}, ExtLabel(spec_.custom_exts), Ico::None,
                               kHitType + static_cast<int>(index::SearchKind::Custom), true, true});
            row_top = PillRow(I::AdvSearchKind, std::move(row), card_[0], row_top);
            separators_.push_back(row_top);

            row.clear();
            for (int v = 0; v <= static_cast<int>(app::DatePreset::ThisYear); ++v) {
                const auto preset = static_cast<app::DatePreset>(v);
                row.push_back({{}, DateLabel(preset), Ico::None, kHitDate + v, spec_.date == preset, true});
            }
            if (spec_.date == app::DatePreset::Custom)
                row.push_back({{}, DateLabel(spec_.date), Ico::None,
                               kHitDate + static_cast<int>(app::DatePreset::Custom), true, true});
            row_top = PillRow(I::AdvSearchDate, std::move(row), card_[0], row_top);
            separators_.push_back(row_top);

            row.clear();
            const app::SizePreset sizes[] = {
                app::SizePreset::Any, app::SizePreset::Lt1MB, app::SizePreset::From1To10MB,
                app::SizePreset::Gt10MB, app::SizePreset::Gt100MB, app::SizePreset::Gt1GB};
            // Compact, language-neutral labels keep the whole scale on one line.
            const wchar_t* compact[] = {nullptr, L"< 1 MB", L"1\u201310 MB", L"> 10 MB", L"> 100 MB", L"> 1 GB"};
            for (size_t i = 0; i < std::size(sizes); ++i)
                row.push_back({{}, compact[i] ? std::wstring(compact[i]) : SizeLabel(sizes[i], {}), Ico::None,
                               kHitSize + static_cast<int>(sizes[i]), spec_.size == sizes[i], true});
            if (spec_.size == app::SizePreset::Empty || spec_.size == app::SizePreset::Custom)
                row.push_back({{}, SizeLabel(spec_.size, spec_.size_custom), Ico::None,
                               kHitSize + static_cast<int>(spec_.size), true, true});
            row_top = PillRow(I::AdvSearchSize, std::move(row), card_[0], row_top);
            separators_.push_back(row_top);

            row.clear();
            row.push_back({{}, l10n::Get(I::LocationIndexed), Ico::Globe, kHitLocation + 0,
                           spec_.location == app::LocationScope::Indexed, true});
            std::wstring current = l10n::Get(I::LocationCurrent);
            if (!spec_.current_folder.empty()) current += L" \u00b7 " + FolderLeaf(spec_.current_folder);
            row.push_back({{}, current, Ico::Folder, kHitLocation + 1,
                           spec_.location == app::LocationScope::CurrentFolder, !spec_.current_folder.empty()});
            const bool custom = spec_.location == app::LocationScope::CustomFolder && !spec_.custom_folder.empty();
            row.push_back({{}, custom ? FolderLeaf(spec_.custom_folder) : l10n::Get(I::AdvPickFolder),
                           Ico::FolderAdd, kHitLocation + 2, custom, true});
            row_top = PillRow(I::AdvSearchLocation, std::move(row), card_[0], row_top);
            card_[0].bottom = row_top;
        }

        // Expert options, folded away until asked for.
        more_rect_ = D2D1::RectF(left, card_[0].bottom + X(12), right, card_[0].bottom + X(12 + 22));
        more_sum_ = more_ ? std::wstring() : MoreSummary();
        y = more_rect_.bottom;
        ext_rect_ = exclude_rect_ = name_also_rect_ = D2D1::RectF(0, 0, 0, 0);
        card_[1] = D2D1::RectF(0, 0, 0, 0);
        if (more_) {
            card_[1] = D2D1::RectF(left, y + X(8), right, y + X(8));
            float row_top = card_[1].top;
            row_top = FieldRow(I::AdvSearchExtensions, ext_rect_, card_[1], row_top);
            separators_.push_back(row_top);
            row_top = FieldRow(I::AdvSearchExclude, exclude_rect_, card_[1], row_top);
            separators_.push_back(row_top);
            if (content_mode_) {
                row_top = FieldRow(I::AdvSearchName, name_also_rect_, card_[1], row_top);
                separators_.push_back(row_top);
            }
            std::vector<Pill> row;
            for (int v = 0; v < 3; ++v) {
                if (content_mode_) {
                    const auto mode = static_cast<index::ContentMatchMode>(v);
                    row.push_back({{}, ContentModeLabel(mode), Ico::None, kHitContentMode + v, spec_.content_mode == mode, true});
                } else {
                    const auto how = static_cast<app::NameMatchHow>(v);
                    row.push_back({{}, NameHowLabel(how), Ico::None, kHitNameHow + v, spec_.name_how == how, true});
                }
            }
            row_top = PillRow(I::AdvMatch, std::move(row), card_[1], row_top);
            card_[1].bottom = row_top;
            y = card_[1].bottom;
        }

        // Footer band: summary + count on the left, the buttons on the right.
        footer_top_ = y + X(16);
        summary_parts_ = SummaryParts();
        summary_ = Summary();
        const float cy = footer_top_ + X(kFootH) * 0.5f;
        const float bh = X(32);
        const float sw = std::max(X(96), Measure(l10n::Get(I::AdvSearchRun), kF13b) + X(32));
        const float cw = std::max(X(96), Measure(l10n::Get(I::Cancel), kF13) + X(32));
        const float gw = Measure(l10n::Get(I::ClearAll), kF13) + X(28);
        search_rect_ = D2D1::RectF(right - sw, cy - bh * 0.5f, right, cy + bh * 0.5f);
        cancel_rect_ = D2D1::RectF(search_rect_.left - X(8) - cw, search_rect_.top, search_rect_.left - X(8),
                                   search_rect_.bottom);
        clear_rect_ = D2D1::RectF(cancel_rect_.left - X(8) - gw, search_rect_.top, cancel_rect_.left - X(8),
                                  search_rect_.bottom);
        count_rect_ = D2D1::RectF(left, cy - X(17.5f), clear_rect_.left - X(16), cy + X(17.5f));
        height_px_ = footer_top_ + X(kFootH);
    }

    // ---- edits ---------------------------------------------------------------------

    D2D1_COLOR_F EditForeground() const { return ColorFromRef(EditTextColor(dark_)); }
    D2D1_COLOR_F EditBackground() const { return ColorFromRef(EditBackColor(dark_)); }

    I KeywordHint() const { return content_mode_ ? I::AdvKeywordContentHint : I::AdvKeywordNameHint; }
    I ExcludeHint() const { return content_mode_ ? I::AdvExcludeContentHint : I::AdvExcludeNameHint; }

    HWND CreateField(int id, const std::wstring& text, I cue_id) {
        HWND edit = CreateChildEdit(hwnd_, text.c_str());
        if (!edit) return nullptr;
        SetWindowTheme(edit, L"", L"");
        const auto& cue = l10n::Get(cue_id);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit, EditProc, static_cast<UINT_PTR>(id), reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }

    void SetCue(HWND edit, I cue_id) {
        if (!edit) return;
        const auto& cue = l10n::Get(cue_id);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
    }

    static bool Visible(const D2D1_RECT_F& r) { return r.right > r.left; }

    void PlaceEdit(HWND hwnd, const D2D1_RECT_F& cell) {
        if (!hwnd || !hwnd_) return;
        if (!Visible(cell)) {
            if (GetFocus() == hwnd && edit_kw_) SetFocus(edit_kw_);
            ShowWindow(hwnd, SW_HIDE);
            return;
        }
        POINT pt{ static_cast<int>(std::lround(cell.left + 10.0f * scale_)),
                  static_cast<int>(std::lround(cell.top)) };
        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left - 20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        const int line_h = EditLineHeight(hwnd, font_, cell_h);
        pt.y += std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Layered children need an initial bitmap before they can receive clicks.
        PresentChildEdit(compositor_, compositor_.TextFormat(), EditForeground(), EditBackground(), hwnd);
    }

    void LayoutEdits() {
        if (done_) return;
        PlaceEdit(edit_kw_, kw_edit_rect_);
        PlaceEdit(edit_exts_, ext_rect_);
        PlaceEdit(edit_exclude_, exclude_rect_);
        PlaceEdit(edit_name_also_, name_also_rect_);
    }

    std::vector<HWND> VisibleEdits() const {
        std::vector<HWND> edits;
        if (edit_kw_) edits.push_back(edit_kw_);
        if (edit_exts_ && Visible(ext_rect_)) edits.push_back(edit_exts_);
        if (edit_exclude_ && Visible(exclude_rect_)) edits.push_back(edit_exclude_);
        if (edit_name_also_ && Visible(name_also_rect_)) edits.push_back(edit_name_also_);
        return edits;
    }

    static std::wstring EditText(HWND hwnd) {
        if (!hwnd) return {};
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
        return text;
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<AdvancedSearchWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                self->Complete(false);
                return 0;
            }
            if (wparam == VK_RETURN) {
                self->Complete(true);
                return 0;
            }
            if (wparam == VK_TAB) {
                const auto edits = self->VisibleEdits();
                const auto it = std::find(edits.begin(), edits.end(), hwnd);
                if (!edits.empty()) {
                    const int n = static_cast<int>(edits.size());
                    int i = it == edits.end() ? 0 : static_cast<int>(it - edits.begin());
                    i = (i + ((GetKeyState(VK_SHIFT) & 0x8000) ? n - 1 : 1)) % n;
                    SetFocus(edits[static_cast<size_t>(i)]);
                }
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        }
        // Shared LumaText hosting presents typed text at once and falls back
        // to the native EDIT when a layered present fails (#41), so a field
        // never turns invisible or click-through.
        const bool focus_ring = msg == WM_SETFOCUS || msg == WM_KILLFOCUS ||
                                msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK;
        LRESULT result = 0;
        if (!HandleChildEditMessage(self->compositor_, self->compositor_.TextFormat(),
                                    self->EditForeground(), self->EditBackground(),
                                    EditBackBrush(self->edit_brush_), hwnd, msg, wparam, lparam, result)) {
            result = DefPresentedChildEditProc(self->compositor_, self->compositor_.TextFormat(),
                                               self->EditForeground(), self->EditBackground(),
                                               hwnd, msg, wparam, lparam);
        }
        if (focus_ring) InvalidateRect(self->hwnd_, nullptr, FALSE);
        return result;
    }

    void DestroyEdits() {
        for (HWND* edit : {&edit_kw_, &edit_exts_, &edit_exclude_, &edit_name_also_}) {
            if (*edit) { DestroyWindow(*edit); *edit = nullptr; }
        }
    }

    // ---- behaviour -----------------------------------------------------------------

    // Re-reads the edits, recompiles the query, re-lays out and (if needed) resizes.
    void Recompute(bool resize = true) {
        if (!edits_ready_) return;
        SyncFromEdits();
        preview_ = app::CompileSearchQuery(spec_);
        const auto split = app::SplitSearchQueryText(preview_);
        scope_error_ = split.content.present() && app::ContentSearchNeedsScope(split) &&
                       spec_.location == app::LocationScope::Indexed;
        const float old_height = height_px_;
        Layout();
        if (resize && hwnd_ && std::fabs(old_height - height_px_) > 0.5f) {
            RECT rc{};
            GetWindowRect(hwnd_, &rc);
            SetWindowPos(hwnd_, nullptr, 0, 0, rc.right - rc.left,
                         static_cast<int>(std::ceil(height_px_)),
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        LayoutEdits();
        if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
        RequestCountSoon();
    }

    void RequestCountSoon() {
        if (!hwnd_ || !count_request_ || content_mode_ || scope_error_) {
            count_pending_ = false;
            return;
        }
        if (count_known_ && preview_ == counted_query_ && !count_timer_) return;
        count_pending_ = true;
        count_timer_ = true;
        SetTimer(hwnd_, kCountTimer, 250, nullptr);
    }

    void SetContentMode(bool content) {
        if (content == content_mode_) return;
        SyncFromEdits();
        content_mode_ = content;
        // Clear inactive criteria only on an explicit mode switch, not when an
        // existing mixed query is read back from the controls.
        if (content) spec_.exclude_name.clear();
        else {
            spec_.content.clear();
            spec_.content_exclude.clear();
            spec_.whole_word = spec_.case_sensitive = false;
        }
        if (!content && edit_name_also_) SetWindowTextW(edit_name_also_, L"");
        SetCue(edit_kw_, KeywordHint());
        SetCue(edit_exclude_, ExcludeHint());
    }

    void ApplyRecipe(int recipe) {
        SyncFromEdits();
        const bool active = RecipeActive(recipe);
        spec_.kind = index::SearchKind::Any;
        spec_.custom_exts.clear();
        if (edit_exts_) SetWindowTextW(edit_exts_, L"");
        spec_.date = app::DatePreset::Any;
        spec_.size = app::SizePreset::Any;
        if (active) {
            if (recipe == 3) SetContentMode(false);
        } else if (recipe == 0) {
            SetContentMode(false);
            spec_.kind = index::SearchKind::Document;
            spec_.date = app::DatePreset::ThisMonth;
        } else if (recipe == 1) {
            SetContentMode(false);
            spec_.size = app::SizePreset::Gt100MB;
        } else if (recipe == 2) {
            SetContentMode(false);
            spec_.kind = index::SearchKind::Image;
            spec_.date = app::DatePreset::ThisWeek;
        } else {
            SetContentMode(true);
            spec_.kind = index::SearchKind::Document;
            if (edit_kw_) SetFocus(edit_kw_);
        }
    }

    void Click(int id) {
        SyncFromEdits();
        if (id == kHitMore) {
            more_ = !more_;
        } else if (id == kHitWholeWord) {
            spec_.whole_word = !spec_.whole_word;
        } else if (id == kHitMatchCase) {
            spec_.case_sensitive = !spec_.case_sensitive;
        } else if (id == kHitClear) {
            const auto current = spec_.current_folder;
            spec_ = {};
            spec_.current_folder = current;
            if (!current.empty()) spec_.location = app::LocationScope::CurrentFolder;
            for (HWND edit : {edit_kw_, edit_exts_, edit_exclude_, edit_name_also_})
                if (edit) SetWindowTextW(edit, L"");
            SetContentMode(false);
            if (edit_kw_) SetFocus(edit_kw_);
        } else if (id >= kHitRecipe && id < kHitRecipe + 4) {
            ApplyRecipe(id - kHitRecipe);
        } else if (id == kHitMode || id == kHitMode + 1) {
            SetContentMode(id == kHitMode + 1);
            if (edit_kw_) SetFocus(edit_kw_);
        } else if (id >= kHitType && id < kHitType + 100) {
            const auto kind = static_cast<index::SearchKind>(id - kHitType);
            if (kind == index::SearchKind::Custom) {
                more_ = true;
                Recompute();
                if (edit_exts_) SetFocus(edit_exts_);
                return;
            }
            spec_.kind = kind;
            spec_.custom_exts.clear();
            if (edit_exts_) SetWindowTextW(edit_exts_, L"");
        } else if (id >= kHitDate && id < kHitDate + 100) {
            spec_.date = static_cast<app::DatePreset>(id - kHitDate);
        } else if (id >= kHitSize && id < kHitSize + 100) {
            spec_.size = static_cast<app::SizePreset>(id - kHitSize);
        } else if (id >= kHitLocation && id < kHitLocation + 100) {
            const int v = id - kHitLocation;
            if (v == 2) {
                FolderPickerSpec picker;
                picker.selection_outline = selection_outline_;
                picker.title = l10n::Get(I::AdvSearchBrowse);
                picker.initial_path = spec_.custom_folder;
                std::wstring folder;
                if (!ShowFolderPicker(hwnd_, picker, dark_, accent_, folder)) return;
                spec_.custom_folder = folder;
                spec_.location = app::LocationScope::CustomFolder;
            } else {
                spec_.location = v == 1 ? app::LocationScope::CurrentFolder : app::LocationScope::Indexed;
            }
        } else if (id >= kHitNameHow && id < kHitNameHow + 100) {
            spec_.name_how = static_cast<app::NameMatchHow>(id - kHitNameHow);
        } else if (id >= kHitContentMode && id < kHitContentMode + 100) {
            spec_.content_mode = static_cast<index::ContentMatchMode>(id - kHitContentMode);
        }
        Recompute();
    }

    void Complete(bool accepted) {
        if (accepted) {
            Recompute();
            if (scope_error_) return;
            result_.accepted = true;
            result_.query = preview_;
        }
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    int Hit(float x, float y) const {
        if (ContainsRect(CloseRect(), x, y)) return kHitClose;
        if (ContainsRect(search_rect_, x, y)) return kHitSearch;
        if (ContainsRect(cancel_rect_, x, y)) return kHitCancel;
        if (ContainsRect(clear_rect_, x, y)) return kHitClear;
        if (ContainsRect(more_rect_, x, y)) return kHitMore;
        if (Visible(ww_rect_) && ContainsRect(ww_rect_, x, y)) return kHitWholeWord;
        if (Visible(mc_rect_) && ContainsRect(mc_rect_, x, y)) return kHitMatchCase;
        for (int i = 0; i < 2; ++i)
            if (ContainsRect(mode_rect_[i], x, y)) return kHitMode + i;
        for (const auto& pill : pills_)
            if (pill.enabled && ContainsRect(pill.bounds, x, y)) return pill.id;
        return kHitNone;
    }

#ifdef PULSE_WITH_SELFTEST
    // Selftests click targets by id instead of hard-coded coordinates: the
    // centre of hit `id` in client px, or of a field (1000 keyword, 1001
    // extensions, 1002 exclude, 1003 name); with 0x10000 set, a point in the
    // left padding instead. -1 when it is not laid out.
    LRESULT TestHitCenter(WPARAM request) const {
        const int id = static_cast<int>(request & 0xFFFF);
        D2D1_RECT_F r{};
        switch (id) {
        case kHitClose: r = CloseRect(); break;
        case kHitSearch: r = search_rect_; break;
        case kHitCancel: r = cancel_rect_; break;
        case kHitClear: r = clear_rect_; break;
        case kHitMore: r = more_rect_; break;
        case kHitWholeWord: r = ww_rect_; break;
        case kHitMatchCase: r = mc_rect_; break;
        case kHitMode: case kHitMode + 1: r = mode_rect_[id - kHitMode]; break;
        case 1000: r = kw_edit_rect_; break;
        case 1001: r = ext_rect_; break;
        case 1002: r = exclude_rect_; break;
        case 1003: r = name_also_rect_; break;
        default:
            for (const auto& pill : pills_)
                if (pill.id == id && pill.enabled) r = pill.bounds;
            break;
        }
        if (!Visible(r)) return -1;
        const float x = (request & 0x10000) ? r.left + 3.0f : (r.left + r.right) * 0.5f;
        return MAKELRESULT(static_cast<int>(x),
                           static_cast<int>((r.top + r.bottom) * 0.5f));
    }
#endif

    // ---- painting ------------------------------------------------------------------

    void EnsureFormats() {
        if (fmt_scale_ == scale_ && formats_[0]) return;
        ReleaseFormats();
        IDWriteFactory2* factory = compositor_.DwriteFactory();
        if (!factory) return;
        struct Spec { float size; DWRITE_FONT_WEIGHT weight; };
        const Spec specs[kFmtCount] = {
            {15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {13.0f, DWRITE_FONT_WEIGHT_NORMAL},
            {13.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}, {12.5f, DWRITE_FONT_WEIGHT_NORMAL},
            {12.0f, DWRITE_FONT_WEIGHT_NORMAL}, {12.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD}};
        for (int i = 0; i < kFmtCount; ++i) {
            typography::TextFormatSpec spec;
            spec.size = specs[i].size * scale_;
            spec.weight = specs[i].weight;
            typography::CreateTextFormat(factory, spec, &formats_[i]);
        }
        fmt_scale_ = scale_;
    }
    void ReleaseFormats() {
        for (auto*& f : formats_) if (f) { f->Release(); f = nullptr; }
        fmt_scale_ = 0.0f;
    }

    IDWriteTextLayout* MakeLayout(const std::wstring& text, int f, float w, float h, bool ellipsis) const {
        IDWriteFactory2* factory = compositor_.DwriteFactory();
        IDWriteTextFormat* fmt = f >= 0 && f < kFmtCount ? formats_[f] : nullptr;
        if (!factory || !fmt || text.empty()) return nullptr;
        IDWriteTextLayout* layout = nullptr;
        if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), fmt,
                                             std::max(1.0f, w), std::max(1.0f, h), &layout)))
            return nullptr;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        if (ellipsis) {
            DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            IDWriteInlineObject* sign = nullptr;
            factory->CreateEllipsisTrimmingSign(fmt, &sign);
            layout->SetTrimming(&trimming, sign);
            if (sign) sign->Release();
        }
        return layout;
    }

    float Measure(const std::wstring& text, int f) const {
        IDWriteTextLayout* layout = MakeLayout(text, f, 10000.0f, 1000.0f, false);
        if (!layout) return static_cast<float>(text.size()) * 8.0f * scale_;
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        layout->Release();
        return std::ceil(m.widthIncludingTrailingWhitespace);
    }

    // Single line, vertically centred in r, ellipsis at the right; returns the drawn width.
    float Text(const std::wstring& text, int f, const D2D1_RECT_F& r, D2D1_COLOR_F c, bool center = false) {
        IDWriteTextLayout* layout = MakeLayout(text, f, r.right - r.left, r.bottom - r.top, true);
        if (!layout) return 0.0f;
        if (center) layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DWRITE_TEXT_METRICS m{};
        layout->GetMetrics(&m);
        const float y = std::round(r.top + ((r.bottom - r.top) - m.height) * 0.5f);
        brush_->SetColor(c);
        compositor_.DrawTextLayout(D2D1::Point2F(r.left, y), layout, brush_);
        layout->Release();
        return std::min(m.widthIncludingTrailingWhitespace, r.right - r.left);
    }

    void Fill(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        if (c.a <= 0.0f) return;
        brush_->SetColor(c);
        if (radius <= 0.0f) compositor_.Dc()->FillRectangle(r, brush_);
        else compositor_.Dc()->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_);
    }
    void Stroke(const D2D1_RECT_F& r, float radius, D2D1_COLOR_F c) {
        if (c.a <= 0.0f) return;
        const float h = 0.5f * std::max(1.0f, std::floor(scale_));
        const D2D1_RECT_F in = D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h);
        brush_->SetColor(c);
        compositor_.Dc()->DrawRoundedRectangle(D2D1::RoundedRect(in, std::max(0.0f, radius - h), std::max(0.0f, radius - h)),
                                               brush_, h * 2.0f);
    }
    template <size_t N>
    void Icon(const SvgPart (&parts)[N], float view, float x, float y, float size, D2D1_COLOR_F c) {
        DrawSvgParts(compositor_.Dc(), parts, N, view, D2D1::RectF(x, y, x + size, y + size), c);
    }
    void PillIcon(Ico icon, float x, float y, D2D1_COLOR_F c) {
        if (icon == Ico::Globe) Icon(kIcoGlobe, 16, x, y, X(14), c);
        else if (icon == Ico::Folder) Icon(kIcoFolder, 16, x, y, X(14), c);
        else if (icon == Ico::FolderAdd) Icon(kIcoFolderAdd, 16, x, y, X(14), c);
    }

    void DrawPill(const Pill& pill) {
        const bool hovered = hover_ == pill.id && pill.enabled;
        const bool pressed = pressed_ == pill.id && pill.enabled;
        const D2D1_RECT_F& r = pill.bounds;
        const float radius = (r.bottom - r.top) * 0.5f;
        D2D1_COLOR_F fg = pill.enabled ? pal_.text : pal_.text3;
        if (pill.chip) {
            Fill(r, radius, pill.selected ? pal_.accent_soft : (hovered ? pal_.hover : pal_.card));
            Stroke(r, radius, pill.selected ? pal_.accent_line : pal_.line);
            fg = pill.selected ? pal_.accent : pal_.text2;
            Text(pill.text, kF125, r, fg, true);
            return;
        }
        if (pill.selected) {
            Fill(r, radius, pal_.accent_soft);
            Stroke(r, radius, pal_.accent_line);
            fg = pal_.accent;
        } else if (hovered || pressed) {
            Fill(r, radius, pressed ? Mix(pal_.hover, pal_.text, 0.06f) : pal_.hover);
        }
        float x = r.left + X(12);
        if (pill.icon != Ico::None) {
            PillIcon(pill.icon, x, (r.top + r.bottom) * 0.5f - X(7), fg);
            x += X(14 + 6);
        }
        Text(pill.text, pill.selected ? kF13b : kF13, D2D1::RectF(x, r.top, r.right - X(10), r.bottom), fg);
    }

    void DrawFooterSummary(float right) {
        const float top = count_rect_.top;
        bool danger = false;
        const std::wstring count = CountLine(danger);
        if (danger) {
            // The scope warning is the one thing to read: both lines, wrapped.
            IDWriteTextLayout* layout = MakeLayout(count, kF12, right - count_rect_.left, X(40), false);
            if (!layout) return;
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            const float y = std::round((count_rect_.top + count_rect_.bottom - m.height) * 0.5f);
            brush_->SetColor(pal_.danger);
            compositor_.DrawTextLayout(D2D1::Point2F(count_rect_.left, y), layout, brush_);
            layout->Release();
            return;
        }
        // Line 1: the search in words, separators faint; ellipsis on overflow.
        float x = count_rect_.left;
        const D2D1_RECT_F line1 = D2D1::RectF(x, top, right, top + X(18));
        const std::wstring dot = L"\u00b7";
        const float dot_w = Measure(dot, kF13);
        for (size_t i = 0; i < summary_parts_.size() && x < right; ++i) {
            if (i > 0) {
                x += X(6);
                if (x + dot_w > right) break;
                Text(dot, kF13, D2D1::RectF(x, line1.top, x + dot_w, line1.bottom), pal_.text3);
                x += dot_w + X(6);
            }
            x += Text(summary_parts_[i], kF13, D2D1::RectF(x, line1.top, right, line1.bottom), pal_.text);
        }
        // Line 2: the live count, the number in accent.
        if (count.empty()) return;
        const D2D1_RECT_F line2 = D2D1::RectF(count_rect_.left, top + X(19), right, top + X(35));
        const std::wstring number = count_known_ && !count_pending_ && count_ > 0 && !danger && !content_mode_
                                        ? GroupDigits(count_) : std::wstring();
        const size_t at = number.empty() ? std::wstring::npos : count.find(number);
        if (at == std::wstring::npos) {
            Text(count, kF12, line2, danger ? pal_.danger : pal_.text3);
            return;
        }
        float cx = line2.left;
        cx += Text(count.substr(0, at), kF12, D2D1::RectF(cx, line2.top, right, line2.bottom), pal_.text3);
        cx += Text(number, kF12b, D2D1::RectF(cx, line2.top, right, line2.bottom), pal_.accent);
        Text(count.substr(at + number.size()), kF12, D2D1::RectF(cx, line2.top, right, line2.bottom), pal_.text3);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            const auto now = GetTickCount64();
            if (now < next_recovery_) {
                SetTimer(hwnd_, kRecoveryTimer, static_cast<UINT>(next_recovery_ - now), nullptr);
                return;
            }
            // Release all device-bound Painter resources even if a replacement
            // context happens to reuse the old context's address.
            painter_.SetCompositor(nullptr);
            bool recovered = false;
#ifdef PULSE_WITH_SELFTEST
            ++recovery_attempts_;
            if (!GetPropW(hwnd_, L"Pulse.Test.RecoveryFailure"))
#endif
                recovered = compositor_.Recover();
            if (!recovered) {
                next_recovery_ = GetTickCount64() + recovery_delay_;
                SetTimer(hwnd_, kRecoveryTimer, recovery_delay_, nullptr);
                recovery_delay_ = (std::min)(recovery_delay_ * 2, 2000u);
                return;
            }
            KillTimer(hwnd_, kRecoveryTimer);
            next_recovery_ = 0;
            recovery_delay_ = 250;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            painter_.InvalidateTypography();
            // The native EDITs retain their text, undo, focus and selection.
            // Only their presentation resources belong to the lost device.
            LayoutEdits();
            for (const auto edit : VisibleEdits()) InvalidateRect(edit, nullptr, FALSE);
        }
        if (!compositor_.Dc()) return;
        Theme theme = MakeTheme(dark_, accent_);
        if (IsHighContrast()) theme = MakeHighContrastTheme();
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        BeginSurface(compositor_, painter_, theme, dark_, IsHighContrast(), backdrop_, scale_);

        EnsureFormats();
        pal_ = MakePal(dark_, IsHighContrast(), theme, backdrop_ && IsAcrylicBackdrop(hwnd_));
        ID2D1DeviceContext* dc = compositor_.Dc();
        if (FAILED(dc->CreateSolidColorBrush(pal_.text, &brush_))) brush_ = nullptr;
        if (brush_) {
            const float s = scale_;
            const float W = kDlgW * s;

            // Title bar: tile, title, scope, mode switch, close.
            const D2D1_RECT_F tile = R(18, 12, 28, 28);
            Fill(tile, X(8), pal_.accent_soft);
            Icon(kIcoTile, 16, tile.left + X(6), tile.top + X(6), X(16), pal_.accent);
            float x = X(56);
            x += Text(l10n::Get(I::AdvancedSearch), kF15b, D2D1::RectF(x, 0, seg_rect_.left - X(12), X(kTitleH)),
                      pal_.text) + X(10);
            if (x < seg_rect_.left - X(24))
                Text(Format(I::AdvScopeSubtitle, ScopeName()), kF13, D2D1::RectF(x, 0, seg_rect_.left - X(12), X(kTitleH)),
                     pal_.text3);
            Fill(seg_rect_, X(9), pal_.hover);
            const D2D1_RECT_F on = mode_rect_[content_mode_ ? 1 : 0];
            Fill(D2D1::RectF(on.left, on.top + X(1), on.right, on.bottom + X(1)), X(7), Hex(0x000000, dark_ ? 0.25f : 0.08f));
            Fill(on, X(7), pal_.solid);
            Stroke(on, X(7), Alpha(pal_.text, 0.06f));
            const I mode_labels[2] = {I::AdvModeName, I::AdvModeContent};
            for (int i = 0; i < 2; ++i) {
                const D2D1_RECT_F& r = mode_rect_[i];
                const bool chosen = content_mode_ == (i == 1);
                if (!chosen && hover_ == kHitMode + i) Fill(r, X(7), Alpha(pal_.card, 0.5f));
                const D2D1_COLOR_F c = chosen ? pal_.text : pal_.text2;
                if (i == 0) Icon(kIcoModeName, 16, r.left + X(12), r.top + X(7), X(14), c);
                else Icon(kIcoModeContent, 16, r.left + X(12), r.top + X(7), X(14), c);
                Text(l10n::Get(mode_labels[i]), kF13, D2D1::RectF(r.left + X(32), r.top, r.right, r.bottom), c);
            }
            const D2D1_RECT_F close = CloseRect();
            if (hover_ == kHitClose || pressed_ == kHitClose)
                Fill(close, X(8), pressed_ == kHitClose ? Mix(pal_.hover, pal_.text, 0.06f) : pal_.hover);
            Icon(kIcoX, 14, close.left + X(11), close.top + X(11), X(14), pal_.text2);

            // Fields (shared look: focus = 2 px accent line at the bottom).
            auto field = [&](const D2D1_RECT_F& bounds, HWND edit, I placeholder) {
                if (!Visible(bounds) || !edit) return;
                fluent::TextFieldSpec spec;
                spec.bounds = bounds;
                spec.hosted_edit = true;
                spec.suppress_text = !EditText(edit).empty();
                spec.placeholder = l10n::Get(placeholder);
                spec.leading_glyph = edit == edit_kw_ ? L"\xE721" : L"";
                spec.state.focused = GetFocus() == edit;
                painter_.DrawTextField(spec);
            };
            field(kw_rect_, edit_kw_, KeywordHint());
            auto toggle = [&](const D2D1_RECT_F& r, const std::wstring& label, bool on_state, int id) {
                if (!Visible(r)) return;
                if (on_state) Fill(r, X(5), pal_.accent_soft);
                else if (hover_ == id) Fill(r, X(5), pal_.hover);
                Text(label, on_state ? kF12b : kF12, r, on_state ? pal_.accent : pal_.text2, true);
            };
            toggle(ww_rect_, l10n::Get(I::AdvWholeWordShort), spec_.whole_word, kHitWholeWord);
            toggle(mc_rect_, L"Aa", spec_.case_sensitive, kHitMatchCase);

            // "Try" chips.
            Text(l10n::Get(I::AdvTry), kF125, try_label_rect_, pal_.text3);

            // Cards with hairlines between rows.
            for (const auto& card : card_) {
                if (!Visible(card)) continue;
                Fill(card, X(10), pal_.card);
                Stroke(card, X(10), pal_.line);
            }
            const float hair = std::max(1.0f, std::floor(s));
            for (float y : separators_) {
                const D2D1_RECT_F& card = Visible(card_[1]) && y > card_[1].top ? card_[1] : card_[0];
                Fill(D2D1::RectF(card.left + hair, y - hair, card.right - hair, y), 0.0f, pal_.line2);
            }
            for (const auto& row : rows_) Text(row.text, kF13, row.bounds, pal_.text2);
            for (const auto& pill : pills_) DrawPill(pill);
            field(ext_rect_, edit_exts_, I::AdvSearchExtHint);
            field(exclude_rect_, edit_exclude_, ExcludeHint());
            field(name_also_rect_, edit_name_also_, I::AdvNameAlso);

            // More options: chevron, label, and what it holds in faint text.
            {
                const D2D1_COLOR_F c = hover_ == kHitMore ? pal_.text : pal_.text2;
                const float cy = (more_rect_.top + more_rect_.bottom) * 0.5f;
                if (more_) Icon(kIcoChevDown, 12, more_rect_.left, cy - X(6), X(12), c);
                else Icon(kIcoChevRight, 12, more_rect_.left, cy - X(6), X(12), c);
                const float lw = Text(l10n::Get(I::AdvMore), kF13,
                                      D2D1::RectF(more_rect_.left + X(18), more_rect_.top, more_rect_.right, more_rect_.bottom), c);
                if (!more_sum_.empty()) {
                    const float sum_left = more_rect_.left + X(18) + lw + X(16);
                    const float sw = std::min(Measure(more_sum_, kF125), more_rect_.right - sum_left);
                    if (sw > X(40))
                        Text(more_sum_, kF125, D2D1::RectF(more_rect_.right - sw, more_rect_.top, more_rect_.right,
                                                           more_rect_.bottom), pal_.text3);
                }
            }

            // Footer band.
            Fill(D2D1::RectF(0, footer_top_, W, height_px_), 0.0f, pal_.sheet);
            Fill(D2D1::RectF(0, footer_top_, W, footer_top_ + hair), 0.0f, pal_.line2);
            DrawFooterSummary(count_rect_.right);
            fluent::ButtonSpec clear;
            clear.bounds = clear_rect_;
            clear.text = l10n::Get(I::ClearAll);
            clear.kind = fluent::ButtonKind::Transparent;
            clear.state.hovered = hover_ == kHitClear;
            clear.state.pressed = pressed_ == kHitClear;
            painter_.DrawButton(clear);
            fluent::ButtonSpec cancel;
            cancel.bounds = cancel_rect_;
            cancel.text = l10n::Get(I::Cancel);
            cancel.state.hovered = hover_ == kHitCancel;
            cancel.state.pressed = pressed_ == kHitCancel;
            painter_.DrawButton(cancel);
            fluent::ButtonSpec search;
            search.bounds = search_rect_;
            search.text = l10n::Get(I::AdvSearchRun);
            search.kind = fluent::ButtonKind::Primary;
            search.state.hovered = hover_ == kHitSearch;
            search.state.pressed = pressed_ == kHitSearch;
            search.state.enabled = !scope_error_;
            painter_.DrawButton(search);
            brush_->Release();
            brush_ = nullptr;
        }
#ifdef PULSE_WITH_SELFTEST
        if (!snapshot_path_.empty()) {
            // A flip-chain back buffer is no longer the submitted frame after Present.
            const HRESULT drawn = compositor_.Dc()->EndDraw();
            snapshot_saved_ = SUCCEEDED(drawn) && compositor_.SaveSnapshot(snapshot_path_.c_str());
            if (SUCCEEDED(drawn)) compositor_.Present();
            else compositor_.NotifyDeviceLost(drawn);
            return;
        }
#endif
        EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
#ifdef PULSE_WITH_SELFTEST
        static const UINT test_hit_center = RegisterWindowMessageW(L"Pulse.Test.AdvancedHitCenter");
        if (message == test_hit_center) return TestHitCenter(wparam);
#endif
        switch (message) {
        case WM_CREATE: {
            edits_ready_ = false;
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            font_ = typography::CreateEditFont(scale_);
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            edit_kw_ = CreateField(1, content_mode_ ? spec_.content : spec_.name, KeywordHint());
            edit_exts_ = CreateField(2, spec_.kind == index::SearchKind::Custom ? spec_.custom_exts : L"",
                                     I::AdvSearchExtHint);
            edit_exclude_ = CreateField(3, content_mode_ ? spec_.content_exclude : spec_.exclude_name,
                                        ExcludeHint());
            edit_name_also_ = CreateField(4, content_mode_ ? spec_.name : L"", I::AdvNameAlso);
            // Creating a native EDIT can synchronously send EN_CHANGE before its
            // handle is assigned. Do not read a partially constructed form.
            edits_ready_ = edit_kw_ && edit_exts_ && edit_exclude_ && edit_name_also_;
            if (!edits_ready_) return -1;
            return 0;
        }
        case WM_DESTROY:
            KillTimer(hwnd_, kCountTimer);
            KillTimer(hwnd_, kRecoveryTimer);
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            ReleaseFormats();
            compositor_.Shutdown();
            done_ = true;
            return 0;
        case WM_TIMER:
            if (wparam == kRecoveryTimer) {
                KillTimer(hwnd_, kRecoveryTimer);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == kCountTimer) {
                KillTimer(hwnd_, kCountTimer);
                count_timer_ = false;
                if (count_request_ && !content_mode_ && !scope_error_) {
                    counted_query_ = preview_;
                    count_request_(hwnd_, preview_);
                }
                return 0;
            }
            break;
        case kAdvancedSearchCountMessage:
            if (!count_timer_) {  // A newer request is already queued: wait for it.
                count_ = static_cast<size_t>(wparam);
                count_known_ = true;
                count_pending_ = false;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_DPICHANGED: {
            const auto* rc = reinterpret_cast<RECT*>(lparam);
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            Layout();
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, static_cast<int>(kDlgW * scale_),
                         static_cast<int>(std::ceil(height_px_)), SWP_NOZORDER | SWP_NOACTIVATE);
            LayoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            LayoutEdits();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_MOVE:
            compositor_.UpdateTextRenderingParams(
                MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST));
            LayoutEdits();
            return 0;
        case WM_CTLCOLOREDIT: {
            const HDC hdc = reinterpret_cast<HDC>(wparam);
            SetTextColor(hdc, EditTextColor(dark_));
            SetBkColor(hdc, EditBackColor(dark_));
            return reinterpret_cast<LRESULT>(EditBackBrush(edit_brush_));
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) { Complete(true); return 0; }
            if (LOWORD(wparam) == IDCANCEL) { Complete(false); return 0; }
            if (HIWORD(wparam) == EN_CHANGE) Recompute();
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(hwnd_, &ps);
            Render();
            EndPaint(hwnd_, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_NCHITTEST: {
            // The title bar drags, except over the close button and the mode switch.
            const LRESULT hit = BorderlessHitTest(hwnd_, lparam, kTitleH * scale_, CloseRect());
            if (hit == HTCAPTION) {
                POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                ScreenToClient(hwnd_, &pt);
                if (ContainsRect(seg_rect_, static_cast<float>(pt.x), static_cast<float>(pt.y))) return HTCLIENT;
            }
            return hit;
        }
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) {
                hover_ = next;
                SetCursor(LoadCursorW(nullptr, next ? IDC_HAND : IDC_ARROW));
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT && hover_ != kHitNone) {
                SetCursor(LoadCursorW(nullptr, IDC_HAND));
                return TRUE;
            }
            break;
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            HWND edit = nullptr;
            const bool on_toggle = (Visible(ww_rect_) && ContainsRect(ww_rect_, x, y)) ||
                                   (Visible(mc_rect_) && ContainsRect(mc_rect_, x, y));
            if (on_toggle) edit = nullptr;
            else if (ContainsRect(kw_rect_, x, y)) edit = edit_kw_;
            else if (Visible(ext_rect_) && ContainsRect(ext_rect_, x, y)) edit = edit_exts_;
            else if (Visible(exclude_rect_) && ContainsRect(exclude_rect_, x, y)) edit = edit_exclude_;
            else if (Visible(name_also_rect_) && ContainsRect(name_also_rect_, x, y)) edit = edit_name_also_;
            if (edit) {
                // The rounded field includes padding outside the native text line.
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                MapWindowPoints(hwnd_, edit, &point, 1);
                SendMessageW(edit, WM_LBUTTONDOWN, wparam, MAKELPARAM(point.x, point.y));
                return 0;
            }
            pressed_ = Hit(x, y);
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int id = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                               static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (id != pressed || id == kHitNone) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (id == kHitSearch) Complete(true);
            else if (id == kHitCancel || id == kHitClose) Complete(false);
            else Click(id);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) Complete(false);
            else if (wparam == VK_RETURN) Complete(true);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_kw_ = nullptr;
    HWND edit_exts_ = nullptr;
    HWND edit_exclude_ = nullptr;
    HWND edit_name_also_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    app::AdvancedSearchSpec spec_;
    AdvancedSearchDialogResult result_;
    AdvancedSearchCountRequest count_request_;
    std::wstring preview_;
    std::wstring counted_query_;
    std::wstring summary_;
    std::vector<std::wstring> summary_parts_;
    std::wstring more_sum_;
    std::vector<Pill> pills_;
    std::vector<RowLabel> rows_;
    std::vector<float> separators_;
    D2D1_RECT_F kw_rect_{}, kw_edit_rect_{}, ext_rect_{}, exclude_rect_{}, name_also_rect_{};
    D2D1_RECT_F seg_rect_{}, mode_rect_[2]{};
    D2D1_RECT_F try_label_rect_{}, card_[2]{};
    D2D1_RECT_F more_rect_{}, ww_rect_{}, mc_rect_{};
    D2D1_RECT_F search_rect_{}, cancel_rect_{}, clear_rect_{}, count_rect_{};
    IDWriteTextFormat* formats_[kFmtCount]{};
    float fmt_scale_ = 0.0f;
    ID2D1SolidColorBrush* brush_ = nullptr;
    Pal pal_{};
    float footer_top_ = 0.0f;
    float height_px_ = 0.0f;
    size_t count_ = 0;
    bool count_known_ = false;
    bool count_pending_ = false;
    bool count_timer_ = false;
    bool content_mode_ = false;
    bool more_ = false;
    bool dark_ = true;
    bool backdrop_ = false;
    bool done_ = false;
    bool edits_ready_ = false;
    bool scope_error_ = false;
    float scale_ = 1.0f;
    bool selection_outline_ = false;
    ULONGLONG next_recovery_ = 0;
    UINT recovery_delay_ = 250;
#ifdef PULSE_WITH_SELFTEST
    unsigned recovery_attempts_ = 0;
    std::wstring snapshot_path_;
    bool snapshot_saved_ = false;
#endif
    D2D1_COLOR_F accent_{};
    int hover_ = 0;
    int pressed_ = 0;
};

} // namespace

#ifdef PULSE_ADVANCED_QUERY_TEST
AdvancedSearchDialogResult TestAdvancedSearchQuery(app::AdvancedSearchSpec spec, int field,
                                                  const std::wstring& value) {
    AdvancedSearchWindow window;
    return window.TestQuery(std::move(spec), field, value);
}
#endif

#ifdef PULSE_WITH_SELFTEST
void TestAdvancedSearchRecovery(HWND owner, float scale, bool dark, const std::wstring& png,
                               const DialogRecoveryCheck& check) {
    AdvancedSearchWindow window;
    window.TestRecovery(owner, scale, dark, png, check);
}
#endif

AdvancedSearchDialogResult ShowAdvancedSearchDialog(HWND owner, app::AdvancedSearchSpec spec,
                                                    bool dark, D2D1_COLOR_F accent,
                                                    AdvancedSearchCountRequest count, bool selection_outline) {
    AdvancedSearchWindow window;
    return window.Show(owner, std::move(spec), dark, accent, std::move(count), selection_outline);
}

#ifdef PULSE_WITH_SELFTEST
bool TestAdvancedSearchDeviceRecovery(HWND host) {
    AdvancedSearchWindow window;
    return window.TestDeviceRecovery(host);
}
#endif

} // namespace pulse::ui
