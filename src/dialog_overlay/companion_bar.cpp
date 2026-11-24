#include "companion_bar.h"

#include <algorithm>
#include <utility>

namespace pulse::dialog_overlay {
namespace {

constexpr wchar_t kBarClassName[] = L"PulseDialogCompanionBarWnd";
constexpr int kCmdPickViaPulse = 1000;
constexpr int kCmdQuickJumpBase = 1100;
constexpr int kBarHeightBase = 34;

int ScaleDpi(int px, UINT dpi) noexcept {
    return MulDiv(px, static_cast<int>(dpi ? dpi : 96u), 96);
}

UINT WindowDpi(HWND hwnd) noexcept {
    using GetDpiForWindowFn = UINT (WINAPI*)(HWND);
    static auto fn = []() -> GetDpiForWindowFn {
        HMODULE u32 = GetModuleHandleW(L"user32.dll");
        return u32 ? reinterpret_cast<GetDpiForWindowFn>(GetProcAddress(u32, "GetDpiForWindow")) : nullptr;
    }();
    return (fn && hwnd) ? fn(hwnd) : 96u;
}

} // namespace

CompanionBar::~CompanionBar() { Destroy(); }

bool CompanionBar::Create(HINSTANCE instance) noexcept {
    Destroy();
    instance_ = instance ? instance : GetModuleHandleW(nullptr);

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &CompanionBar::WndProc;
    wc.hInstance = instance_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = kBarClassName;
    RegisterClassExW(&wc);

    hwnd_ = CreateWindowExW(
        WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kBarClassName,
        L"Pulse Quick Jump",
        WS_POPUP | WS_BORDER | WS_CLIPSIBLINGS,
        0, 0, 320, kBarHeightBase,
        nullptr, nullptr, instance_, this);
    if (!hwnd_) return false;

    NONCLIENTMETRICSW ncm{};
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0)) {
        font_ = CreateFontIndirectW(&ncm.lfMessageFont);
    }
    return true;
}

void CompanionBar::Destroy() noexcept {
    Detach();
    if (hwnd_) {
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (font_) {
        DeleteObject(font_);
        font_ = nullptr;
    }
}

void CompanionBar::AttachTo(const NativeDialogInfo& target, std::vector<QuickJumpEntry> entries,
                            PickViaPulseFn on_pick_via_pulse) {
    if (!hwnd_ || !target.dialog_hwnd || !IsWindow(target.dialog_hwnd)) return;
    target_ = target;
    entries_ = std::move(entries);
    on_pick_via_pulse_ = std::move(on_pick_via_pulse);
    RebuildButtons();
    UpdatePosition();
    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
}

void CompanionBar::UpdatePosition() noexcept {
    if (!hwnd_ || !target_.dialog_hwnd) return;
    if (!IsWindow(target_.dialog_hwnd) || !IsWindowVisible(target_.dialog_hwnd) || IsIconic(target_.dialog_hwnd)) {
        ShowWindow(hwnd_, SW_HIDE);
        return;
    }
    RECT rc{};
    if (!GetWindowRect(target_.dialog_hwnd, &rc)) return;
    const UINT dpi = WindowDpi(target_.dialog_hwnd);
    const int bar_h = ScaleDpi(kBarHeightBase, dpi);
    const int bar_w = std::max(ScaleDpi(280, dpi), static_cast<int>(rc.right - rc.left));
    int y = rc.top - bar_h - ScaleDpi(2, dpi);
    if (y < 0) y = rc.bottom + ScaleDpi(2, dpi);
    SetWindowPos(hwnd_, HWND_TOPMOST, rc.left, y, bar_w, bar_h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void CompanionBar::Detach() noexcept {
    for (HWND btn : buttons_) {
        if (btn && IsWindow(btn)) DestroyWindow(btn);
    }
    buttons_.clear();
    target_ = {};
    entries_.clear();
    on_pick_via_pulse_ = {};
    if (hwnd_ && IsWindow(hwnd_)) {
        ShowWindow(hwnd_, SW_HIDE);
    }
}

void CompanionBar::RebuildButtons() {
    for (HWND btn : buttons_) {
        if (btn && IsWindow(btn)) DestroyWindow(btn);
    }
    buttons_.clear();
    if (!hwnd_) return;

    const UINT dpi = WindowDpi(target_.dialog_hwnd ? target_.dialog_hwnd : hwnd_);
    int x = ScaleDpi(6, dpi);
    const int y = ScaleDpi(4, dpi);
    const int h = ScaleDpi(24, dpi);
    const int gap = ScaleDpi(6, dpi);

    auto add_btn = [&](const wchar_t* text, int id, int width) {
        HWND b = CreateWindowExW(
            0, L"BUTTON", text,
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            x, y, width, h, hwnd_,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance_, nullptr);
        if (b) {
            if (font_) SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
            buttons_.push_back(b);
            x += width + gap;
        }
    };

    if (on_pick_via_pulse_) {
        add_btn(L"Pulse...", kCmdPickViaPulse, ScaleDpi(76, dpi));
    }
    const size_t max_entries = std::min<size_t>(entries_.size(), 5);
    for (size_t i = 0; i < max_entries; ++i) {
        add_btn(entries_[i].label.c_str(), kCmdQuickJumpBase + static_cast<int>(i), ScaleDpi(92, dpi));
    }
}

void CompanionBar::OnButtonClick(int command_id) {
    if (!target_.dialog_hwnd || !IsWindow(target_.dialog_hwnd)) {
        Detach();
        return;
    }
    if (command_id == kCmdPickViaPulse && on_pick_via_pulse_) {
        std::wstring chosen;
        if (on_pick_via_pulse_(target_, chosen) && !chosen.empty()) {
            DriveNativeDialogNavigation(target_, chosen, true);
        }
        return;
    }
    if (command_id >= kCmdQuickJumpBase) {
        const size_t idx = static_cast<size_t>(command_id - kCmdQuickJumpBase);
        if (idx < entries_.size()) {
            DriveNativeDialogNavigation(target_, entries_[idx].path, true);
        }
    }
}

LRESULT CALLBACK CompanionBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    CompanionBar* self = nullptr;
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<CompanionBar*>(cs->lpCreateParams);
        if (self) self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<CompanionBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    return self ? self->HandleMessage(hwnd, msg, wparam, lparam) : DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CompanionBar::HandleMessage(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        // Strictly prevent stealing focus from the underlying native dialog.
        return MA_NOACTIVATE;
    case WM_COMMAND:
        if (HIWORD(wparam) == BN_CLICKED) {
            OnButtonClick(LOWORD(wparam));
            return 0;
        }
        break;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

ModeBOutcome RunModeBProxy(const NativeDialogInfo& info,
                           const std::function<bool(std::wstring& out_path)>& pick_fn) noexcept {
    ModeBOutcome out{};
    if (!info.mode_b_eligible || info.has_custom_controls || !pick_fn ||
        !info.dialog_hwnd || !IsWindow(info.dialog_hwnd) || !info.filename_edit_hwnd) {
        return out;
    }

    RECT orig_rect{};
    if (!GetWindowRect(info.dialog_hwnd, &orig_rect)) return out;
    const LONG_PTR orig_ex = GetWindowLongPtrW(info.dialog_hwnd, GWL_EXSTYLE);
    const bool had_layered = (orig_ex & WS_EX_LAYERED) != 0;

    auto restore_native = [&]() noexcept {
        if (!IsWindow(info.dialog_hwnd)) return;
        if (!had_layered) {
            SetWindowLongPtrW(info.dialog_hwnd, GWL_EXSTYLE, orig_ex);
        } else {
            SetLayeredWindowAttributes(info.dialog_hwnd, 0, 255, LWA_ALPHA);
        }
        SetWindowPos(info.dialog_hwnd, nullptr, orig_rect.left, orig_rect.top,
                     orig_rect.right - orig_rect.left, orig_rect.bottom - orig_rect.top,
                     SWP_NOZORDER | SWP_SHOWWINDOW);
        out.restored_native = true;
    };

    // Hide off-screen with alpha=0 while keeping the native window enabled/modal so
    // the caller application's message loop stays completely healthy.
    SetWindowLongPtrW(info.dialog_hwnd, GWL_EXSTYLE, orig_ex | WS_EX_LAYERED);
    SetLayeredWindowAttributes(info.dialog_hwnd, 0, 0, LWA_ALPHA);
    SetWindowPos(info.dialog_hwnd, nullptr, -32000, -32000, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

    std::wstring chosen;
    bool picked = false;
    try {
        picked = pick_fn(chosen);
    } catch (...) {
        picked = false;
    }
    out.handled = true;

    if (!picked || chosen.empty()) {
        // User cancelled or picker failed: cleanly restore native dialog or cancel it.
        restore_native();
        CancelNativeDialog(info);
        return out;
    }

    // Inject result into Edit(1148) + click IDOK.
    if (!DriveNativeDialogNavigation(info, chosen, true)) {
        restore_native();
        return out;
    }
    out.accepted = true;
    return out;
}

} // namespace pulse::dialog_overlay
