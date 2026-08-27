#include "batch_rename_dialog.h"
#include "FluentTokens.h"
#include "fluent_components.h"
#include "typography.h"
#include "ui_compositor.h"
#include "../common/localization.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

#pragma comment(lib, "uxtheme.lib")

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseBatchRenameWindow";
constexpr float kDlgW = 620.0f;
constexpr float kDlgH = 588.0f;
constexpr UINT_PTR kEditCaretTimer = 71;
constexpr wchar_t kChipNumbered[] = L"{name} ({n}){ext}";
constexpr wchar_t kChipPadded[] = L"{name}_{n:3}{ext}";
constexpr wchar_t kChipExt[] = L"{name}.jpg";

float ScaleDip(float scale, float value) { return value * scale; }

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return D2D1::RectF(ScaleDip(scale, x), ScaleDip(scale, y),
                       ScaleDip(scale, x + width), ScaleDip(scale, y + height));
}

bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

bool ApplyBackdrop(HWND hwnd, bool dark) {
    UpdateWindowTheme(hwnd, dark);
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const bool high_contrast = IsHighContrast();
    DWORD backdrop = high_contrast ? DWMSBT_NONE : DWMSBT_TABBEDWINDOW;
    HRESULT backdrop_result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                                     &backdrop, sizeof(backdrop));
    if (FAILED(backdrop_result) && backdrop == DWMSBT_TABBEDWINDOW) {
        backdrop = DWMSBT_MAINWINDOW;
        backdrop_result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                                &backdrop, sizeof(backdrop));
    }
    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return !high_contrast && SUCCEEDED(backdrop_result);
}

void CenterOwnedWindow(HWND hwnd, HWND owner, int width, int height) {
    RECT anchor{};
    if (!owner || !GetWindowRect(owner, &anchor)) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        anchor = monitor.rcWork;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    HMONITOR monitor_handle = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitor{ sizeof(monitor) };
    GetMonitorInfoW(monitor_handle, &monitor);
    const int work_left = static_cast<int>(monitor.rcWork.left);
    const int work_top = static_cast<int>(monitor.rcWork.top);
    const int work_right = static_cast<int>(monitor.rcWork.right);
    const int work_bottom = static_cast<int>(monitor.rcWork.bottom);
    x = std::clamp(x, work_left, (std::max)(work_left, work_right - width));
    y = std::clamp(y, work_top, (std::max)(work_top, work_bottom - height));
    SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& client_buttons) {
    POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    RECT window{};
    GetWindowRect(hwnd, &window);
    const UINT dpi = GetDpiForWindow(hwnd);
    const int frame_x = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                      + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int frame_y = GetSystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                      + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const bool left = point.x < window.left + frame_x;
    const bool right = point.x >= window.right - frame_x;
    const bool top = point.y < window.top + frame_y;
    const bool bottom = point.y >= window.bottom - frame_y;
    if (top && left) return HTTOPLEFT;
    if (top && right) return HTTOPRIGHT;
    if (bottom && left) return HTBOTTOMLEFT;
    if (bottom && right) return HTBOTTOMRIGHT;
    if (left) return HTLEFT;
    if (right) return HTRIGHT;
    if (top) return HTTOP;
    if (bottom) return HTBOTTOM;
    ScreenToClient(hwnd, &point);
    if (point.y >= 0 && point.y < title_height &&
        !Contains(client_buttons, static_cast<float>(point.x), static_cast<float>(point.y)))
        return HTCAPTION;
    return HTCLIENT;
}

void BeginSurface(Compositor& compositor, fluent::Painter& painter,
                  const Theme& theme, bool dark, bool high_contrast, bool backdrop_enabled,
                  float scale) {
    auto* dc = compositor.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    painter.BeginFrame(theme, high_contrast);
    D2D1_COLOR_F tint = theme.bg;
    tint.a = high_contrast || !backdrop_enabled ? 1.0f : (dark ? 0.76f : 0.82f);
    const float width = static_cast<float>(compositor.Width());
    const float height = static_cast<float>(compositor.Height());
    painter.FillRoundedRect(D2D1::RectF(0, 0, width, height), 0, tint);
    const float inset = 0.5f;
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, width - inset, height - inset),
                              12.0f * scale, theme.stroke_card);
}

void EndSurface(Compositor& compositor) {
    const HRESULT hr = compositor.Dc()->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        compositor.NotifyDeviceLost(hr);
        return;
    }
    compositor.Present();
}

std::wstring StatusLabel(app::BatchRenameStatus status) {
    switch (status) {
    case app::BatchRenameStatus::Ok: return l10n::Get(l10n::StringId::BatchRenameWillRename);
    case app::BatchRenameStatus::Unchanged: return l10n::Get(l10n::StringId::Unchanged);
    case app::BatchRenameStatus::Empty:
    case app::BatchRenameStatus::Invalid: return l10n::Get(l10n::StringId::InvalidName);
    case app::BatchRenameStatus::Collision: return l10n::Get(l10n::StringId::NameCollision);
    }
    return {};
}

class BatchRenameWindow {
public:
    BatchRenameDialogResult Show(HWND owner, const std::vector<std::wstring>& paths,
                                 bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        paths_ = paths;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(GetDpiForWindow(owner ? owner : GetDesktopWindow())) / 96.0f;
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
            l10n::Get(l10n::StringId::BatchRename).c_str(),
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return result_;
        CenterOwnedWindow(hwnd_, owner_, width, height);
        if (owner_) EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        LayoutEdits();
        SetForegroundWindow(hwnd_);
        if (edit_find_) SetFocus(edit_find_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            const bool edit_msg = EditId(message.hwnd) != 0;
            if (!edit_msg && IsDialogMessageW(hwnd_, &message)) continue;
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        DestroyEdits();
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        if (owner_) {
            EnableWindow(owner_, TRUE);
            SetActiveWindow(owner_);
        }
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<BatchRenameWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<BatchRenameWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, kDlgW - 46, 0, 46, 36); }
    D2D1_RECT_F FindField() const { return Rect(scale_, 20, 68, 280, 32); }
    D2D1_RECT_F ReplaceField() const { return Rect(scale_, 320, 68, 280, 32); }
    D2D1_RECT_F IncludeExtRect() const { return Rect(scale_, 430, 40, 170, 24); }
    bool UsesIndex() const { return app::BatchRenamePatternUsesIndex(rule_.pattern); }
    D2D1_RECT_F PatternField() const {
        return UsesIndex() ? Rect(scale_, 20, 128, 424, 32)
                           : Rect(scale_, 20, 128, 580, 32);
    }
    D2D1_RECT_F StartField() const { return Rect(scale_, 456, 128, 144, 32); }
    D2D1_RECT_F ChipRect(int index) const {
        constexpr float widths[] = {92.0f, 100.0f, 100.0f};
        float x = 20.0f;
        for (int i = 0; i < index; ++i) x += widths[i] + 8.0f;
        return Rect(scale_, x, 168, widths[index], 28);
    }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, kDlgW - 196, kDlgH - 48, 80, 32); }
    D2D1_RECT_F ApplyRect() const { return Rect(scale_, kDlgW - 108, kDlgH - 48, 88, 32); }
    D2D1_RECT_F PreviewBounds() const {
        return Rect(scale_, 20, 260, kDlgW - 40, kDlgH - 260 - 56);
    }
    D2D1_RECT_F SummaryRect() const {
        return Rect(scale_, 20, kDlgH - 48, kDlgW - 228, 32);
    }

    HWND EditAt(int id) const {
        switch (id) {
        case 1: return edit_find_;
        case 2: return edit_replace_;
        case 3: return edit_pattern_;
        case 4: return edit_number_;
        }
        return nullptr;
    }

    int EditId(HWND hwnd) const {
        if (hwnd == edit_find_) return 1;
        if (hwnd == edit_replace_) return 2;
        if (hwnd == edit_pattern_) return 3;
        if (hwnd == edit_number_) return 4;
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

    HWND CreateField(int id, const std::wstring& text, bool number = false) {
        HWND edit = CreateWindowExW(
            WS_EX_LAYERED | WS_EX_TOOLWINDOW, L"EDIT", text.c_str(),
            WS_POPUP | ES_AUTOHSCROLL | (number ? ES_NUMBER : 0),
            0, 0, 0, 0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!edit) return nullptr;
        SetWindowTheme(edit, L"", L"");
        if (!compositor_.LumaTextEnabled())
            SetLayeredWindowAttributes(edit, 0, 255, LWA_ALPHA);
        if (font_) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        SetWindowSubclass(edit, EditProc, static_cast<UINT_PTR>(id),
                          reinterpret_cast<DWORD_PTR>(this));
        return edit;
    }

    void PlaceEdit(HWND hwnd, const D2D1_RECT_F& cell, bool visible) {
        if (!hwnd || !hwnd_) return;
        if (!visible) {
            ShowWindow(hwnd, SW_HIDE);
            EnableWindow(hwnd, FALSE);
            return;
        }
        POINT pt{ static_cast<int>(std::lround(cell.left + 10.0f * scale_)),
                  static_cast<int>(std::lround(cell.top)) };
        ClientToScreen(hwnd_, &pt);
        const int w = std::max(40, static_cast<int>(std::lround(
            cell.right - cell.left - 20.0f * scale_)));
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
        EnableWindow(hwnd, TRUE);
        SetWindowPos(hwnd, HWND_TOP, pt.x, pt.y, w, line_h,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }

    void LayoutEdits() {
        PlaceEdit(edit_find_, FindField(), true);
        PlaceEdit(edit_replace_, ReplaceField(), true);
        PlaceEdit(edit_pattern_, PatternField(), true);
        PlaceEdit(edit_number_, StartField(), UsesIndex());
    }

    static LRESULT CALLBACK EditProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam,
                                     UINT_PTR, DWORD_PTR ref) {
        auto* self = reinterpret_cast<BatchRenameWindow*>(ref);
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
                const int id = self->EditId(hwnd);
                const int dir = (GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1;
                const int last = self->UsesIndex() ? 4 : 3;
                int next = id + dir;
                if (next < 1) next = last;
                if (next > last) next = 1;
                if (HWND target = self->EditAt(next)) SetFocus(target);
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
            if (hwnd) {
                DestroyWindow(hwnd);
                hwnd = nullptr;
            }
        };
        destroy(edit_find_);
        destroy(edit_replace_);
        destroy(edit_pattern_);
        destroy(edit_number_);
    }

    std::wstring EditText(HWND hwnd) const {
        if (!hwnd) return {};
        const int n = GetWindowTextLengthW(hwnd);
        std::wstring text(static_cast<size_t>(n), L'\0');
        if (n > 0) GetWindowTextW(hwnd, text.data(), n + 1);
        return text;
    }

    void SyncRuleFromEdits() {
        rule_.find = EditText(edit_find_);
        rule_.replace = EditText(edit_replace_);
        rule_.pattern = EditText(edit_pattern_);
        rule_.prefix.clear();
        rule_.suffix.clear();
        rule_.insert_number = false;
        const std::wstring number = EditText(edit_number_);
        rule_.start = number.empty() ? 1 : _wtoi(number.c_str());
        if (rule_.start < 0) rule_.start = 0;
        items_ = app::PreviewBatchRename(paths_, rule_);
        rename_count_ = 0;
        unchanged_count_ = 0;
        collision_count_ = 0;
        for (const auto& item : items_) {
            if (item.status == app::BatchRenameStatus::Ok) ++rename_count_;
            else if (item.status == app::BatchRenameStatus::Unchanged) ++unchanged_count_;
            else if (item.status == app::BatchRenameStatus::Collision) ++collision_count_;
        }
        can_apply_ = rename_count_ > 0;
    }

    void ApplyChip(const wchar_t* pattern) {
        if (edit_pattern_) SetWindowTextW(edit_pattern_, pattern);
        SyncRuleFromEdits();
        LayoutEdits();
        if (edit_pattern_) SetFocus(edit_pattern_);
    }

    void Complete(bool accepted) {
        if (accepted) {
            SyncRuleFromEdits();
            if (!can_apply_) return;
            result_.accepted = true;
            result_.rule = rule_;
            result_.items = items_;
        }
        done_ = true;
        if (hwnd_) DestroyWindow(hwnd_);
    }

    int Hit(float x, float y) const {
        if (Contains(CloseRect(), x, y)) return 3;
        if (Contains(ApplyRect(), x, y)) return 1;
        if (Contains(CancelRect(), x, y)) return 2;
        if (Contains(IncludeExtRect(), x, y)) return 4;
        if (Contains(ChipRect(0), x, y)) return 5;
        if (Contains(ChipRect(1), x, y)) return 6;
        if (Contains(ChipRect(2), x, y)) return 7;
        if (Contains(FindField(), x, y)) return 10;
        if (Contains(ReplaceField(), x, y)) return 11;
        if (Contains(PatternField(), x, y)) return 12;
        if (UsesIndex() && Contains(StartField(), x, y)) return 13;
        return 0;
    }

    void DrawFieldFrame(const D2D1_RECT_F& bounds, HWND edit, int hit_id, bool enabled) {
        fluent::ControlState state{};
        state.enabled = enabled;
        state.focused = enabled && GetFocus() == edit;
        state.hovered = hover_ == hit_id;
        painter_.DrawTextFieldFrame(bounds, state, true);
    }

    void Render() {
        if (compositor_.NeedsRecovery()) {
            if (!compositor_.Recover()) return;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
        }
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);

        painter_.DrawText(l10n::Get(l10n::StringId::BatchRename),
                          Rect(scale_, 16, 0, 360, 36), compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 3;
        close_state.pressed = pressed_ == 3;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close,
                                    {}, close_state);
        painter_.FillRoundedRect(Rect(scale_, 0, 36, kDlgW, 1), 0, theme.stroke_divider);

        painter_.DrawText(l10n::Get(l10n::StringId::Find),
                          Rect(scale_, 20, 48, 280, 20), compositor_.SmallFormat(),
                          theme.text_secondary);
        painter_.DrawText(l10n::Get(l10n::StringId::Replace),
                          Rect(scale_, 320, 48, 100, 20), compositor_.SmallFormat(),
                          theme.text_secondary);
        fluent::ControlState include_state{};
        include_state.hovered = hover_ == 4;
        include_state.pressed = pressed_ == 4;
        include_state.checked = rule_.replace_in_extension;
        painter_.DrawCheckBox(IncludeExtRect(),
                              l10n::Get(l10n::StringId::BatchRenameIncludeExt),
                              include_state);
        DrawFieldFrame(FindField(), edit_find_, 10, true);
        DrawFieldFrame(ReplaceField(), edit_replace_, 11, true);

        painter_.DrawText(l10n::Get(l10n::StringId::BatchRenamePattern),
                          Rect(scale_, 20, 108, 280, 20), compositor_.SmallFormat(),
                          theme.text_secondary);
        if (UsesIndex()) {
            painter_.DrawText(l10n::Get(l10n::StringId::BatchRenameStartFrom),
                              Rect(scale_, 456, 108, 144, 20), compositor_.SmallFormat(),
                              theme.text_secondary);
        }
        DrawFieldFrame(PatternField(), edit_pattern_, 12, true);
        if (UsesIndex()) DrawFieldFrame(StartField(), edit_number_, 13, true);

        const wchar_t* chip_patterns[] = { kChipNumbered, kChipPadded, kChipExt };
        const l10n::StringId chip_labels[] = {
            l10n::StringId::BatchRenameChipNumber,
            l10n::StringId::BatchRenameChipPadded,
            l10n::StringId::BatchRenameChipExt,
        };
        for (int i = 0; i < 3; ++i) {
            fluent::ControlState chip_state{};
            chip_state.hovered = hover_ == 5 + i;
            chip_state.pressed = pressed_ == 5 + i;
            chip_state.checked = rule_.pattern == chip_patterns[i];
            painter_.DrawButton({ ChipRect(i), l10n::Get(chip_labels[i]), {},
                                  chip_state.checked ? fluent::ButtonKind::Primary
                                                     : fluent::ButtonKind::Standard,
                                  chip_state });
        }
        painter_.DrawText(l10n::Get(l10n::StringId::BatchRenamePatternHint),
                          Rect(scale_, 20, 204, kDlgW - 40, 48), compositor_.SmallFormat(),
                          theme.text_secondary);

        const auto preview = PreviewBounds();
        painter_.FillRoundedRect(preview, 8.0f * scale_, theme.fill_input);
        painter_.StrokeRoundedRect(preview, 8.0f * scale_, theme.stroke_card);
        const float row_h = 22.0f * scale_;
        const float inner_top = preview.top + 8.0f * scale_;
        const float inner_bottom = preview.bottom - 8.0f * scale_;
        const float pad = 12.0f * scale_;
        const float status_w = 90.0f * scale_;
        const float arrow_w = 22.0f * scale_;
        const float names_w = std::max(40.0f, preview.right - preview.left - pad * 2.0f
                                               - status_w - arrow_w);
        const float old_right = preview.left + pad + names_w * 0.5f;
        const float arrow_right = old_right + arrow_w;
        const float new_right = preview.right - pad - status_w;
        const int visible = std::max(1, static_cast<int>((inner_bottom - inner_top) / row_h));
        const int max_scroll = std::max(0, static_cast<int>(items_.size()) - visible);
        scroll_ = std::clamp(scroll_, 0, max_scroll);
        for (int i = 0; i < visible; ++i) {
            const int index = scroll_ + i;
            if (index >= static_cast<int>(items_.size())) break;
            const auto& item = items_[static_cast<size_t>(index)];
            const float y = inner_top + static_cast<float>(i) * row_h;
            D2D1_COLOR_F color = theme.text;
            if (item.status != app::BatchRenameStatus::Ok &&
                item.status != app::BatchRenameStatus::Unchanged)
                color = theme.danger;
            else if (item.status == app::BatchRenameStatus::Unchanged)
                color = theme.text_secondary;
            painter_.DrawText(item.original_name,
                              D2D1::RectF(preview.left + pad, y, old_right, y + row_h),
                              compositor_.SmallFormat(), theme.text_secondary);
            painter_.DrawText(L"→",
                              D2D1::RectF(old_right, y, arrow_right, y + row_h),
                              compositor_.SmallFormat(), theme.text_secondary);
            painter_.DrawText(item.new_name,
                              D2D1::RectF(arrow_right, y, new_right, y + row_h),
                              compositor_.SmallFormat(), color);
            painter_.DrawText(StatusLabel(item.status),
                              D2D1::RectF(new_right, y, preview.right - pad, y + row_h),
                              compositor_.SmallFormat(), color);
        }

        wchar_t summary[128]{};
        swprintf_s(summary, l10n::Get(l10n::StringId::BatchRenameSummary).c_str(),
                   rename_count_, unchanged_count_, collision_count_);
        painter_.DrawText(summary, SummaryRect(), compositor_.SmallFormat(),
                          theme.text_secondary);

        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 2;
        cancel_state.pressed = pressed_ == 2;
        painter_.DrawButton({ CancelRect(), l10n::Get(l10n::StringId::Cancel), {},
                              fluent::ButtonKind::Transparent, cancel_state });

        const auto apply = ApplyRect();
        const D2D1_COLOR_F apply_fill = !can_apply_
            ? theme.fill_input
            : (hover_ == 1 || pressed_ == 1 ? theme.accent_hover : theme.accent);
        painter_.FillRoundedRect(apply, theme.radius_control * scale_, apply_fill);
        painter_.DrawText(l10n::Get(l10n::StringId::Apply), apply, compositor_.SmallFormat(),
                          can_apply_ ? theme.accent_text : theme.text_secondary,
                          fluent::HorizontalAlignment::Center);

        EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE: {
            scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
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
            edit_find_ = CreateField(1, {});
            edit_replace_ = CreateField(2, {});
            edit_pattern_ = CreateField(3, {});
            edit_number_ = CreateField(4, L"1", true);
            SyncRuleFromEdits();
            return 0;
        }
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, 36 * scale_, CloseRect());
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
            if (HIWORD(wparam) == EN_CHANGE) {
                SyncRuleFromEdits();
                LayoutEdits();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            if (delta > 0) scroll_ = std::max(0, scroll_ - 3);
            else scroll_ += 3;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_MOUSEMOVE: {
            const int next = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                 static_cast<float>(GET_Y_LPARAM(lparam)));
            if (next != hover_) { hover_ = next; InvalidateRect(hwnd_, nullptr, FALSE); }
            TRACKMOUSEEVENT track{ sizeof(track), TME_LEAVE, hwnd_, 0 };
            TrackMouseEvent(&track);
            return 0;
        }
        case WM_MOUSELEAVE:
            hover_ = 0;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONDOWN: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            if (hit >= 10 && hit <= 13) {
                if (HWND target = EditAt(hit - 9)) SetFocus(target);
                pressed_ = 0;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            pressed_ = hit;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit == 1) Complete(true);
                else if (hit == 2 || hit == 3) Complete(false);
                else if (hit == 4) {
                    rule_.replace_in_extension = !rule_.replace_in_extension;
                    SyncRuleFromEdits();
                } else if (hit == 5) ApplyChip(kChipNumbered);
                else if (hit == 6) ApplyChip(kChipPadded);
                else if (hit == 7) ApplyChip(kChipExt);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (wparam == VK_ESCAPE) {
                Complete(false);
                return 0;
            }
            if (wparam == VK_RETURN) {
                Complete(true);
                return 0;
            }
            break;
        case WM_CLOSE:
            Complete(false);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd_, &paint);
            Render();
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_DESTROY:
            DestroyEdits();
            if (font_) {
                DeleteObject(font_);
                font_ = nullptr;
            }
            if (edit_brush_) {
                DeleteObject(edit_brush_);
                edit_brush_ = nullptr;
            }
            compositor_.Shutdown();
            hwnd_ = nullptr;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    HWND edit_find_ = nullptr;
    HWND edit_replace_ = nullptr;
    HWND edit_pattern_ = nullptr;
    HWND edit_number_ = nullptr;
    HFONT font_ = nullptr;
    HBRUSH edit_brush_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    std::vector<std::wstring> paths_;
    app::BatchRenameRule rule_;
    std::vector<app::BatchRenameItem> items_;
    BatchRenameDialogResult result_;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool done_ = false;
    bool can_apply_ = false;
    float scale_ = 1.0f;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    int hover_ = 0;
    int pressed_ = 0;
    int scroll_ = 0;
    int rename_count_ = 0;
    int unchanged_count_ = 0;
    int collision_count_ = 0;
};

} // namespace

BatchRenameDialogResult ShowBatchRenameDialog(HWND owner,
                                              const std::vector<std::wstring>& paths,
                                              bool dark,
                                              D2D1_COLOR_F accent) {
    BatchRenameWindow window;
    return window.Show(owner, paths, dark, accent);
}

} // namespace pulse::ui
