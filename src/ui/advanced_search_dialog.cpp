#include "edit_host.h"
#include "../common/windows_compat.h"
#include "advanced_search_dialog.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "fluent_menu.h"
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
#include <string>

#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseAdvancedSearchWindow";
constexpr float kDlgW = 560.0f;
constexpr float kDlgH = 500.0f;
constexpr UINT_PTR kEditCaretTimer = 72;

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return pulse::ui::DipRect(scale, x, y, width, height);
}

bool PickFolderPath(HWND owner, std::wstring& path) {
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog)))) return false;
    dialog->SetTitle(l10n::Get(l10n::StringId::AdvSearchBrowse).c_str());
    FILEOPENDIALOGOPTIONS options = 0;
    dialog->GetOptions(&options);
    dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    if (FAILED(dialog->Show(owner))) return false;
    ComPtr<IShellItem> item;
    if (FAILED(dialog->GetResult(&item))) return false;
    PWSTR folder = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &folder)) || !folder) return false;
    path.assign(folder);
    CoTaskMemFree(folder);
    return !path.empty();
}

std::wstring NameHowLabel(app::NameMatchHow how) {
    switch (how) {
    case app::NameMatchHow::StartsWith: return l10n::Get(l10n::StringId::AdvSearchStarts);
    case app::NameMatchHow::Exact: return l10n::Get(l10n::StringId::AdvSearchExact);
    default: return l10n::Get(l10n::StringId::AdvSearchContains);
    }
}

std::wstring KindLabel(const app::AdvancedSearchSpec& spec) {
    switch (spec.kind) {
    case index::SearchKind::Folder: return l10n::Get(l10n::StringId::KindFolder);
    case index::SearchKind::Document: return l10n::Get(l10n::StringId::KindDocument);
    case index::SearchKind::Image: return l10n::Get(l10n::StringId::KindImage);
    case index::SearchKind::Video: return l10n::Get(l10n::StringId::KindVideo);
    case index::SearchKind::Audio: return l10n::Get(l10n::StringId::KindAudio);
    case index::SearchKind::Archive: return l10n::Get(l10n::StringId::KindArchive);
    case index::SearchKind::Code: return l10n::Get(l10n::StringId::KindCode);
    case index::SearchKind::Custom:
        return spec.custom_exts.empty()
            ? l10n::Get(l10n::StringId::KindCustom) : spec.custom_exts;
    default: return l10n::Get(l10n::StringId::KindAny);
    }
}

std::wstring LocationLabel(app::LocationScope scope) {
    switch (scope) {
    case app::LocationScope::CurrentFolder: return l10n::Get(l10n::StringId::LocationCurrent);
    case app::LocationScope::CustomFolder: return l10n::Get(l10n::StringId::LocationCustom);
    default: return l10n::Get(l10n::StringId::LocationIndexed);
    }
}

std::wstring DateLabel(app::DatePreset preset) {
    switch (preset) {
    case app::DatePreset::Today: return l10n::Get(l10n::StringId::DateToday);
    case app::DatePreset::Yesterday: return l10n::Get(l10n::StringId::DateYesterday);
    case app::DatePreset::ThisWeek: return l10n::Get(l10n::StringId::DateThisWeek);
    case app::DatePreset::ThisMonth: return l10n::Get(l10n::StringId::DateThisMonth);
    case app::DatePreset::ThisYear: return l10n::Get(l10n::StringId::DateThisYear);
    default: return l10n::Get(l10n::StringId::DateAny);
    }
}

std::wstring SizeLabel(app::SizePreset preset) {
    switch (preset) {
    case app::SizePreset::Empty: return l10n::Get(l10n::StringId::SizeEmpty);
    case app::SizePreset::Lt1MB: return l10n::Get(l10n::StringId::SizeLt1MB);
    case app::SizePreset::From1To10MB: return l10n::Get(l10n::StringId::Size1To10MB);
    case app::SizePreset::Gt10MB: return l10n::Get(l10n::StringId::SizeGt10MB);
    default: return l10n::Get(l10n::StringId::SizeAny);
    }
}

std::wstring ModeLabel(index::ContentMatchMode mode) {
    switch (mode) {
    case index::ContentMatchMode::Phrase: return l10n::Get(l10n::StringId::ContentPhrase);
    case index::ContentMatchMode::AnyWord: return l10n::Get(l10n::StringId::ContentAnyWord);
    default: return l10n::Get(l10n::StringId::ContentAllWords);
    }
}

class AdvancedSearchWindow {
public:
    AdvancedSearchDialogResult Show(HWND owner, app::AdvancedSearchSpec spec,
                                    bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        spec_ = std::move(spec);
        dark_ = dark;
        accent_ = accent;
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
        const int height = static_cast<int>(kDlgH * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass,
            l10n::Get(l10n::StringId::AdvancedSearch).c_str(),
            WS_POPUP | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, width, height,
            owner, nullptr, wc.hInstance, this);
        if (!hwnd_) return result_;
        CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        Render();
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdits();
        SetForegroundWindow(hwnd_);
        if (edit_name_) SetFocus(edit_name_);

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
        if (owner_) {
            EnableWindow(owner_, TRUE);
        }
        return result_;
    }

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

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kDlgW - 46, 0, 46, 36); }
    D2D1_RECT_F NameField() const { return Rect(scale_, 20, 68, 328, 32); }
    D2D1_RECT_F NameHowRect() const { return Rect(scale_, 360, 68, 180, 32); }
    D2D1_RECT_F KindRect() const { return Rect(scale_, 20, 128, 140, 32); }
    D2D1_RECT_F ExtField() const { return Rect(scale_, 168, 128, 104, 32); }
    D2D1_RECT_F LocationRect() const { return Rect(scale_, 284, 128, 168, 32); }
    D2D1_RECT_F BrowseRect() const { return Rect(scale_, 460, 128, 80, 32); }
    D2D1_RECT_F DateRect() const { return Rect(scale_, 20, 188, 252, 32); }
    D2D1_RECT_F SizeRect() const { return Rect(scale_, 284, 188, 256, 32); }
    D2D1_RECT_F ContentField() const { return Rect(scale_, 20, 248, 328, 32); }
    D2D1_RECT_F ModeRect() const { return Rect(scale_, 360, 248, 180, 32); }
    D2D1_RECT_F ExcludeField() const { return Rect(scale_, 20, 308, 520, 32); }
    D2D1_RECT_F WholeWordRect() const { return Rect(scale_, 20, 352, 252, 32); }
    D2D1_RECT_F MatchCaseRect() const { return Rect(scale_, 284, 352, 256, 32); }
    D2D1_RECT_F PreviewRect() const { return Rect(scale_, 20, 392, kDlgW - 40, 24); }
    D2D1_RECT_F ErrorRect() const { return Rect(scale_, 20, 418, kDlgW - 40, 28); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kDlgW - 196, kDlgH - 48, 80, 32); }
    D2D1_RECT_F SearchRect() const { return Rect(scale_, kDlgW - 108, kDlgH - 48, 88, 32); }

    D2D1_RECT_F ResetRect() const { return Rect(scale_, 20, kDlgH - 48, 120, 32); }

    HWND EditAt(int id) const {
        if (id == 1) return edit_name_;
        if (id == 2) return edit_content_;
        if (id == 3) return edit_exclude_;
        if (id == 4) return edit_exts_;
        return nullptr;
    }
    int EditId(HWND hwnd) const {
        if (hwnd == edit_name_) return 1;
        if (hwnd == edit_content_) return 2;
        if (hwnd == edit_exclude_) return 3;
        if (hwnd == edit_exts_) return 4;
        return 0;
    }

    D2D1_COLOR_F EditForeground() const {
        return dark_ ? D2D1::ColorF(1.0f, 1.0f, 1.0f)
                     : D2D1::ColorF(26.0f / 255.0f, 26.0f / 255.0f, 26.0f / 255.0f);
    }

    D2D1_COLOR_F EditBackground() const {
        return dark_ ? D2D1::ColorF(30.0f / 255.0f, 30.0f / 255.0f, 30.0f / 255.0f)
                     : D2D1::ColorF(1.0f, 1.0f, 1.0f);
    }

    bool PaintLumaEdit(HWND hwnd) {
        if (!compositor_.LumaTextEnabled()) return false;
        HideCaret(hwnd);
        return compositor_.PresentLumaEdit(hwnd, compositor_.TextFormat(),
                                           EditForeground(), EditBackground());
    }

    HWND CreateField(int id, const std::wstring& text) {
        HWND edit = CreateChildEdit(hwnd_, text.c_str());
        if (!edit) return nullptr;
        SetWindowTheme(edit, L"", L"");
        const auto cue = l10n::Get(id == 4 ? l10n::StringId::AdvSearchExtHint
            : id == 2 ? l10n::StringId::AdvSearchContent
            : id == 3 ? l10n::StringId::AdvSearchExclude : l10n::StringId::AdvSearchName);
        SendMessageW(edit, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(cue.c_str()));
        if (!compositor_.LumaTextEnabled())
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit, EditProc, static_cast<UINT_PTR>(id), reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }

    void PlaceEdit(HWND hwnd, const D2D1_RECT_F& cell) {
        if (!hwnd || !hwnd_) return;
        POINT pt{ static_cast<int>(std::lround(cell.left + 10.0f * scale_)),
                  static_cast<int>(std::lround(cell.top)) };

        const int w = std::max(40, static_cast<int>(std::lround(cell.right - cell.left - 20.0f * scale_)));
        const int cell_h = std::max(18, static_cast<int>(std::lround(cell.bottom - cell.top)));
        int line_h = cell_h;
        if (font_) {
            HDC hdc = GetDC(hwnd);
            HFONT old = static_cast<HFONT>(SelectObject(hdc, font_));
            TEXTMETRICW tm{};
            GetTextMetricsW(hdc, &tm);
            SelectObject(hdc, old);
            ReleaseDC(hwnd, hdc);
            line_h = std::max(1, static_cast<int>(tm.tmHeight));
        }
        line_h = std::min(line_h, cell_h);
        pt.y += std::max(0, (cell_h - line_h) / 2);
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, w, line_h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Layered children need an initial bitmap before they can receive clicks.
        if (compositor_.LumaTextEnabled()) PaintLumaEdit(hwnd);
    }

    void LayoutEdits() {
        if (done_) return;
        PlaceEdit(edit_name_, NameField());
        PlaceEdit(edit_content_, ContentField());
        PlaceEdit(edit_exclude_, ExcludeField());
        PlaceEdit(edit_exts_, ExtField());
    }

    std::wstring EditText(HWND hwnd) const {
        if (!hwnd) return {};
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
        return text;
    }

    void SyncFromEdits() {
        spec_.name = EditText(edit_name_);
        spec_.content = EditText(edit_content_);
        spec_.content_exclude = EditText(edit_exclude_);
        spec_.custom_exts = app::NormalizeExtensionList(EditText(edit_exts_));
        if (!spec_.custom_exts.empty()) spec_.kind = index::SearchKind::Custom;
        else if (spec_.kind == index::SearchKind::Custom) spec_.kind = index::SearchKind::Any;
        preview_ = app::CompileSearchQuery(spec_);
        const auto split = app::SplitSearchQueryText(preview_);
        scope_error_ = split.content.present() && app::ContentSearchNeedsScope(split) &&
                       spec_.location == app::LocationScope::Indexed;
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<AdvancedSearchWindow*>(ref);
        if (!self) return DefSubclassProc(hwnd, msg, wparam, lparam);
        switch (msg) {
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
        case WM_LBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_CAPTURECHANGED:
            if (self->compositor_.LumaTextEnabled()) {
                const LRESULT result = self->compositor_.CallLumaEditMouse(
                    hwnd, msg, wparam, lparam, self->compositor_.TextFormat());
                if (msg != WM_MOUSEMOVE || GetCapture() == hwnd)
                    self->PaintLumaEdit(hwnd);
                if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK)
                    InvalidateRect(self->hwnd_, nullptr, FALSE);
                return result;
            }
            break;
        case WM_PAINT: {
            if (!self->compositor_.LumaTextEnabled()) break;
            HideCaret(hwnd);
            if (!self->PaintLumaEdit(hwnd)) {
                PAINTSTRUCT paint{};
                HDC hdc = BeginPaint(hwnd, &paint);
                RECT rc{};
                GetClientRect(hwnd, &rc);
                if (self->edit_brush_) FillRect(hdc, &rc, self->edit_brush_);
                EndPaint(hwnd, &paint);
            }
            return 0;
        }
        case WM_SETFOCUS: {
            LRESULT result = DefSubclassProc(hwnd, msg, wparam, lparam);
            HideCaret(hwnd);
            SetTimer(hwnd, kEditCaretTimer, GetCaretBlinkTime(), nullptr);
            if (self->compositor_.LumaTextEnabled()) self->PaintLumaEdit(hwnd);
            else InvalidateRect(hwnd, nullptr, FALSE);
            InvalidateRect(self->hwnd_, nullptr, FALSE);
            return result;
        }
        case WM_KILLFOCUS:
            KillTimer(hwnd, kEditCaretTimer);
            InvalidateRect(self->hwnd_, nullptr, FALSE);
            break;
        case WM_TIMER:
            if (wparam == kEditCaretTimer) {
                if (GetCapture() != hwnd) self->PaintLumaEdit(hwnd);
                return 0;
            }
            break;
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
                int id = self->EditId(hwnd);
                id += (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                if (id < 1) id = 4;
                if (id > 4) id = 1;
                if (HWND next = self->EditAt(id)) SetFocus(next);
                return 0;
            }
            break;
        case WM_CHAR:
            if (wparam == VK_RETURN || wparam == VK_ESCAPE || wparam == VK_TAB) return 0;
            break;
        case WM_ERASEBKGND:
            if (self->compositor_.LumaTextEnabled()) return 1;
            break;
        }
        return DefSubclassProc(hwnd, msg, wparam, lparam);
    }

    void DestroyEdits() {
        auto destroy = [](HWND& hwnd) {
            if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
        };
        destroy(edit_name_);
        destroy(edit_content_);
        destroy(edit_exclude_);
        destroy(edit_exts_);
    }

    void Complete(bool accepted) {
        if (accepted) {
            SyncFromEdits();
            if (scope_error_) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            result_.accepted = true;
            result_.query = preview_;
        }
        done_ = true;
        if (hwnd_) { HideComposedDialog(hwnd_, owner_); DestroyWindow(hwnd_); }
    }

    int Hit(float x, float y) const {
        if (pulse::ui::ContainsRect(ResetRect(), x, y)) return 13;
        if (pulse::ui::ContainsRect(CloseRect(), x, y)) return 3;
        if (pulse::ui::ContainsRect(SearchRect(), x, y)) return 1;
        if (pulse::ui::ContainsRect(CancelRect(), x, y)) return 2;
        if (pulse::ui::ContainsRect(NameHowRect(), x, y)) return 4;
        if (pulse::ui::ContainsRect(KindRect(), x, y)) return 5;
        if (pulse::ui::ContainsRect(LocationRect(), x, y)) return 6;
        if (pulse::ui::ContainsRect(BrowseRect(), x, y)) return 7;
        if (pulse::ui::ContainsRect(DateRect(), x, y)) return 8;
        if (pulse::ui::ContainsRect(SizeRect(), x, y)) return 9;
        if (pulse::ui::ContainsRect(ModeRect(), x, y)) return 10;
        if (pulse::ui::ContainsRect(WholeWordRect(), x, y)) return 11;
        if (pulse::ui::ContainsRect(MatchCaseRect(), x, y)) return 12;
        return 0;
    }

    void Choose(int id) {
        SyncFromEdits();
        if (id == 11 || id == 12) {
            if (id == 11) spec_.whole_word = !spec_.whole_word;
            else spec_.case_sensitive = !spec_.case_sensitive;
        } else if (id == 13) {
            const auto current = spec_.current_folder;
            spec_ = {};
            spec_.current_folder = current;
            for (HWND edit : {edit_name_, edit_content_, edit_exclude_, edit_exts_}) SetWindowTextW(edit, L"");
            SetFocus(edit_name_);
        } else {
            const int count = id == 5 ? 9 : id == 8 ? 6 : id == 9 ? 5 : 3;
            const int selected = id == 4 ? static_cast<int>(spec_.name_how)
                : id == 5 ? static_cast<int>(spec_.kind) : id == 6 ? static_cast<int>(spec_.location)
                : id == 8 ? static_cast<int>(spec_.date) : id == 9 ? static_cast<int>(spec_.size)
                : static_cast<int>(spec_.content_mode);
            std::vector<FluentMenuItem> items;
            for (int i = 0; i < count; ++i) {
                FluentMenuItem item;
                item.command = i + 1;
                item.radio_group = true;
                item.checked = item.radio = i == selected;
                auto choice = spec_;
                choice.kind = static_cast<index::SearchKind>(i);
                choice.custom_exts.clear();
                item.text = id == 4 ? NameHowLabel(static_cast<app::NameMatchHow>(i))
                    : id == 5 ? KindLabel(choice) : id == 6 ? LocationLabel(static_cast<app::LocationScope>(i))
                    : id == 8 ? DateLabel(static_cast<app::DatePreset>(i))
                    : id == 9 ? SizeLabel(static_cast<app::SizePreset>(i))
                    : ModeLabel(static_cast<index::ContentMatchMode>(i));
                if (id == 6 && i == static_cast<int>(app::LocationScope::CurrentFolder))
                    item.enabled = !spec_.current_folder.empty();
                items.push_back(std::move(item));
            }
            const auto rect = id == 4 ? NameHowRect() : id == 5 ? KindRect() : id == 6 ? LocationRect()
                : id == 8 ? DateRect() : id == 9 ? SizeRect() : ModeRect();
            POINT anchor{static_cast<LONG>(rect.left), static_cast<LONG>(rect.bottom + 4 * scale_)};
            ClientToScreen(hwnd_, &anchor);
            anchor.x -= FluentMenu::kShadowMargin;
            anchor.y -= FluentMenu::kShadowMargin;
            FluentMenu menu;
            if (!menu.Create(hwnd_, &compositor_, scale_)) return;
            menu.SetTheme(dark_, accent_);
            const int command = menu.TrackPopup(anchor, std::move(items));
            if (!command) return;
            const int value = command - 1;
            if (id == 4) spec_.name_how = static_cast<app::NameMatchHow>(value);
            else if (id == 5) {
                spec_.kind = static_cast<index::SearchKind>(value);
                if (spec_.kind != index::SearchKind::Custom) SetWindowTextW(edit_exts_, L"");
                else SetFocus(edit_exts_);
            } else if (id == 6) {
                if (value == static_cast<int>(app::LocationScope::CustomFolder)) {
                    std::wstring folder;
                    if (!PickFolderPath(hwnd_, folder)) return;
                    spec_.custom_folder = folder;
                }
                spec_.location = static_cast<app::LocationScope>(value);
            } else if (id == 8) spec_.date = static_cast<app::DatePreset>(value);
            else if (id == 9) spec_.size = static_cast<app::SizePreset>(value);
            else spec_.content_mode = static_cast<index::ContentMatchMode>(value);
        }
        preview_ = app::CompileSearchQuery(spec_);
        const auto split = app::SplitSearchQueryText(preview_);
        scope_error_ = split.content.present() && app::ContentSearchNeedsScope(split) &&
                       spec_.location == app::LocationScope::Indexed;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void DrawButton(const D2D1_RECT_F& bounds, const std::wstring& text, bool hover,
                    bool selected = false) {
        fluent::ButtonSpec spec;
        spec.bounds = bounds;
        spec.text = text;
        spec.kind = fluent::ButtonKind::Toggle;
        spec.drop_down = bounds.left != BrowseRect().left;
        spec.state.hovered = hover;
        spec.state.selected = selected;
        spec.state.checked = selected;
        painter_.DrawButton(spec);
    }

    void Render() {
        if (!compositor_.Dc()) return;
        Theme theme = MakeTheme(dark_, accent_);
        if (IsHighContrast()) theme = MakeHighContrastTheme();
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        BeginSurface(compositor_, painter_, theme, dark_, IsHighContrast(), backdrop_, scale_);

        painter_.DrawText(l10n::Get(l10n::StringId::AdvancedSearch),
                          Rect(scale_, 20, 8, 300, 24), compositor_.HeaderFormat(), theme.text);

        auto label = [&](l10n::StringId id, float x, float y) {
            painter_.DrawText(l10n::Get(id), Rect(scale_, x, y, 200, 18),
                              compositor_.SmallFormat(), theme.text_secondary);
        };
        label(l10n::StringId::AdvSearchName, 20, 50);
        label(l10n::StringId::AdvSearchKind, 20, 110);
        label(l10n::StringId::AdvSearchExtensions, 168, 110);
        label(l10n::StringId::AdvSearchLocation, 284, 110);
        label(l10n::StringId::AdvSearchDate, 20, 170);
        label(l10n::StringId::AdvSearchSize, 284, 170);
        label(l10n::StringId::AdvSearchContent, 20, 230);
        label(l10n::StringId::AdvSearchExclude, 20, 290);

        auto field = [&](const D2D1_RECT_F& bounds, HWND edit, l10n::StringId placeholder) {
            fluent::TextFieldSpec spec;
            spec.bounds = bounds;
            spec.hosted_edit = true;
            spec.suppress_text = !EditText(edit).empty();
            spec.placeholder = l10n::Get(placeholder);
            spec.state.focused = GetFocus() == edit;
            painter_.DrawTextField(spec);
        };
        field(NameField(), edit_name_, l10n::StringId::AdvSearchName);
        field(ContentField(), edit_content_, l10n::StringId::AdvSearchContent);
        field(ExcludeField(), edit_exclude_, l10n::StringId::AdvSearchExclude);
        field(ExtField(), edit_exts_, l10n::StringId::AdvSearchExtHint);

        DrawButton(NameHowRect(), NameHowLabel(spec_.name_how), hover_ == 4);
        DrawButton(KindRect(), KindLabel(spec_), hover_ == 5,
                   spec_.kind != index::SearchKind::Any);
        DrawButton(LocationRect(), LocationLabel(spec_.location), hover_ == 6,
                   spec_.location != app::LocationScope::Indexed);
        DrawButton(BrowseRect(), l10n::Get(l10n::StringId::AdvSearchBrowse), hover_ == 7);
        DrawButton(DateRect(), DateLabel(spec_.date), hover_ == 8,
                   spec_.date != app::DatePreset::Any);
        DrawButton(SizeRect(), SizeLabel(spec_.size), hover_ == 9,
                   spec_.size != app::SizePreset::Any);
        DrawButton(ModeRect(), ModeLabel(spec_.content_mode), hover_ == 10);
        fluent::ControlState whole_word;
        whole_word.checked = spec_.whole_word;
        whole_word.hovered = hover_ == 11;
        painter_.DrawCheckBox(WholeWordRect(), l10n::Get(l10n::StringId::AdvSearchWholeWord), whole_word);
        fluent::ControlState match_case;
        match_case.checked = spec_.case_sensitive;
        match_case.hovered = hover_ == 12;
        painter_.DrawCheckBox(MatchCaseRect(), l10n::Get(l10n::StringId::AdvSearchMatchCase), match_case);
        const auto location = spec_.location == app::LocationScope::CustomFolder ? spec_.custom_folder
            : spec_.location == app::LocationScope::CurrentFolder ? spec_.current_folder : L"";
        painter_.DrawText(location, PreviewRect(), compositor_.SmallFormat(), theme.text_secondary);
        fluent::ButtonSpec reset;
        reset.bounds = ResetRect();
        reset.text = l10n::Get(l10n::StringId::ClearAll);
        reset.state.hovered = hover_ == 13;
        painter_.DrawButton(reset);
        if (scope_error_) {
            painter_.DrawText(l10n::Get(l10n::StringId::AdvancedSearchNeedScopeMessage),
                              ErrorRect(), compositor_.SmallFormat(), theme.danger);
        }

        fluent::ButtonSpec cancel;
        cancel.bounds = CancelRect();
        cancel.text = l10n::Get(l10n::StringId::Cancel);
        cancel.state.hovered = hover_ == 2;
        painter_.DrawButton(cancel);
        fluent::ButtonSpec search;
        search.bounds = SearchRect();
        search.text = l10n::Get(l10n::StringId::AdvSearchRun);
        search.kind = fluent::ButtonKind::Primary;
        search.state.hovered = hover_ == 1;
        painter_.DrawButton(search);

        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 3;
        close_state.pressed = pressed_ == 3;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close,
                                    {}, close_state);
        EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: {
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            backdrop_ = ApplyBackdrop(hwnd_, dark_);
            const int height = -std::max(1, static_cast<int>(std::lround(14.0f * scale_)));
            const wchar_t* family = typography::PreferredTextFamily();
            font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                DEFAULT_PITCH | FF_DONTCARE, family);
            if (!font_) {
                font_ = CreateFontW(height, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                    DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
            }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            edit_brush_ = CreateSolidBrush(dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            edit_name_ = CreateField(1, spec_.name);
            edit_content_ = CreateField(2, spec_.content);
            edit_exclude_ = CreateField(3, spec_.content_exclude);
            edit_exts_ = CreateField(4, spec_.custom_exts);
            preview_ = app::CompileSearchQuery(spec_);
            return 0;
        }
        case WM_DESTROY:
            if (font_) { DeleteObject(font_); font_ = nullptr; }
            if (edit_brush_) { DeleteObject(edit_brush_); edit_brush_ = nullptr; }
            compositor_.Shutdown();
            done_ = true;
            return 0;
        case WM_DPICHANGED: {
            const auto* rc = reinterpret_cast<RECT*>(lparam);
            scale_ = static_cast<float>(HIWORD(wparam)) / 96.0f;
            SetWindowPos(hwnd_, nullptr, rc->left, rc->top, rc->right - rc->left,
                         rc->bottom - rc->top, SWP_NOZORDER | SWP_NOACTIVATE);
            compositor_.RecreateTextFormats(scale_);
            painter_.SetScale(scale_);
            if (compositor_.Dc()) compositor_.Resize(rc->right - rc->left, rc->bottom - rc->top);
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
            SetTextColor(hdc, dark_ ? RGB(255, 255, 255) : RGB(26, 26, 26));
            SetBkColor(hdc, dark_ ? RGB(30, 30, 30) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(edit_brush_);
        }
        case WM_COMMAND:
            if (LOWORD(wparam) == IDOK) {
                Complete(true);
                return 0;
            }
            if (LOWORD(wparam) == IDCANCEL) {
                Complete(false);
                return 0;
            }
            if (HIWORD(wparam) == EN_CHANGE) {
                SyncFromEdits();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
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
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) {
                hover_ = next;
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const float x = static_cast<float>(GET_X_LPARAM(lparam));
            const float y = static_cast<float>(GET_Y_LPARAM(lparam));
            HWND edit = nullptr;
            if (ContainsRect(NameField(), x, y)) edit = edit_name_;
            else if (ContainsRect(ContentField(), x, y)) edit = edit_content_;
            else if (ContainsRect(ExcludeField(), x, y)) edit = edit_exclude_;
            else if (ContainsRect(ExtField(), x, y)) edit = edit_exts_;
            if (edit) {
                // The rounded field includes padding outside the native text line.
                POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
                MapWindowPoints(hwnd_, edit, &point, 1);
                SendMessageW(edit, WM_LBUTTONDOWN, wparam, MAKELPARAM(point.x, point.y));
                return 0;
            }
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            pressed_ = hit;
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
            if (id != pressed) {
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (id == 1) Complete(true);
            else if (id == 2 || id == 3) Complete(false);
            else if (id == 7) {
                std::wstring folder;
                if (PickFolderPath(hwnd_, folder)) {
                    spec_.custom_folder = folder;
                    spec_.location = app::LocationScope::CustomFolder;
                    preview_ = app::CompileSearchQuery(spec_);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            } else if (id >= 4) {
                Choose(id);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
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
    HWND edit_name_ = nullptr;
    HWND edit_content_ = nullptr;
    HWND edit_exclude_ = nullptr;
    HWND edit_exts_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    app::AdvancedSearchSpec spec_;
    AdvancedSearchDialogResult result_;
    std::wstring preview_;
    bool dark_ = true;
    bool backdrop_ = false;
    bool done_ = false;
    bool scope_error_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_{};
    int hover_ = 0;
    int pressed_ = 0;
};

} // namespace

AdvancedSearchDialogResult ShowAdvancedSearchDialog(HWND owner, app::AdvancedSearchSpec spec,
                                                    bool dark, D2D1_COLOR_F accent) {
    AdvancedSearchWindow window;
    return window.Show(owner, std::move(spec), dark, accent);
}

} // namespace pulse::ui
