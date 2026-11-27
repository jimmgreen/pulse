#include "picker_saved_toast.h"
#include "../common/windows_compat.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <windowsx.h>

namespace pulse::ui {
namespace {
constexpr wchar_t kClass[] = L"PulsePickerSavedToast";
constexpr wchar_t kTargetProperty[] = L"Pulse.PickerSavedToast.Target";
constexpr UINT kAction = WM_APP + 0x3d1;
constexpr UINT kDismiss = WM_APP + 0x3d2;
constexpr UINT_PTR kWatch = 0x50535457;
constexpr ULONGLONG kLifetime = 15000;
thread_local HWND active_toast = nullptr;

struct State {
    HWND target = nullptr;
    NotificationToast toast;
    Compositor compositor;
    Theme theme{};
    float scale = 1;
    bool ready = false;
    bool action_pending = false;
    ULONGLONG expires = 0;
    std::vector<SnackAction> actions;
    std::wstring title, detail;
};

bool ValidTarget(const State& s) {
    return IsWindow(s.target) && GetPropW(s.target, kTargetProperty) == &s;
}

void Populate(HWND hwnd, State& s) {
    std::vector<SnackAction> buttons = s.actions;
    fluent::Painter painter;
    painter.SetCompositor(&s.compositor);
    painter.SetScale(s.scale);
    painter.BeginFrame(s.theme, IsHighContrast());
    const float card_width = (std::min)(360.0f * s.scale,
        static_cast<float>(s.compositor.Width()) - 32.0f * s.scale);
    const float budget = (card_width - 60.0f * s.scale -
        (buttons.size() > 1 ? 8.0f * s.scale : 0.0f)) /
        static_cast<float>((std::max)(size_t{1}, buttons.size()));
    for (size_t i = 0; i < buttons.size(); ++i) {
        auto& button = buttons[i];
        // The shared card lays buttons in one row. Fit each label to its half
        // rather than letting a long first label push the second off the card.
        if (painter.MeasureButtonWidth(button.label) > budget) {
            while (!button.label.empty() &&
                painter.MeasureButtonWidth(button.label + L"\x2026") > budget) {
                button.label.pop_back();
                if (!button.label.empty() && button.label.back() >= 0xd800 && button.label.back() <= 0xdbff)
                    button.label.pop_back();
            }
            button.label += L"\x2026";
        }
        button.message = kAction;
        button.wparam = static_cast<WPARAM>(i);
    }
    s.toast.ShowActionCard(hwnd, fluent::InfoBarKind::Success, s.title, s.detail, std::move(buttons));
}

void Draw(HWND hwnd, State& s) {
    if (!s.ready) return;
    if (!s.compositor.Dc() && !s.compositor.Recover()) return;
    auto* dc = s.compositor.Dc();
    dc->BeginDraw();
    dc->Clear(s.compositor.UsesTransparentComposition() ? D2D1::ColorF(0, 0) : s.theme.bg);
    s.toast.Draw(s.compositor, s.theme, s.scale, IsHighContrast());
    if (SUCCEEDED(dc->EndDraw())) s.compositor.Present();
    const auto card = s.toast.Bounds();
    // Transparent margins must not intercept input from the application below.
    const int pad = static_cast<int>(std::ceil(8.0f * s.scale));
    HRGN region = CreateRoundRectRgn(static_cast<int>(card.left) - pad,
        static_cast<int>(card.top) - pad, static_cast<int>(card.right) + pad,
        static_cast<int>(card.bottom) + pad, pad * 2, pad * 2);
    if (region && !SetWindowRgn(hwnd, region, FALSE)) DeleteObject(region);
}

void Place(HWND hwnd, State& s) {
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromWindow(s.target, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    const auto& work = monitor.rcWork;
    const int width = (std::min)(static_cast<int>(std::ceil(392.0f * s.scale)), static_cast<int>(work.right - work.left));
    const int height = (std::min)(static_cast<int>(std::ceil(240.0f * s.scale)), static_cast<int>(work.bottom - work.top));
    SetWindowPos(hwnd, HWND_TOPMOST, work.right - width, work.bottom - height,
        width, height, SWP_NOACTIVATE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        s = static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(s));
    }
    if (!s) return DefWindowProcW(hwnd, message, wparam, lparam);
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_ERASEBKGND: return 1;
    case WM_CLOSE:
    case kDismiss:
        DestroyWindow(hwnd);
        return 0;
    case kAction:
        if (s->action_pending && wparam < s->actions.size() && ValidTarget(*s)) {
            s->action_pending = false;
            const auto& action = s->actions[static_cast<size_t>(wparam)];
            if (action.message) PostMessageW(s->target, action.message, action.wparam, 0);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kWatch) {
            if (!ValidTarget(*s) || GetTickCount64() >= s->expires) DestroyWindow(hwnd);
            return 0;
        }
        break;
    case WM_SIZE:
        if (s->ready) {
            s->compositor.Resize(LOWORD(lparam), HIWORD(lparam));
            Populate(hwnd, *s);
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    case WM_DPICHANGED:
        s->scale = static_cast<float>(HIWORD(wparam)) / 96.0f;
        s->compositor.RecreateTextFormats(s->scale);
        Place(hwnd, *s);
        return 0;
    case WM_DISPLAYCHANGE:
        Place(hwnd, *s);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        Draw(hwnd, *s);
        EndPaint(hwnd, &paint);
        return 0;
    }
    case WM_NCDESTROY:
        if (ValidTarget(*s)) RemovePropW(s->target, kTargetProperty);
        if (active_toast == hwnd) active_toast = nullptr;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        delete s;
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    if (s->ready && s->toast.HandleMessage(hwnd, message, wparam, lparam)) {
        if (!s->toast.IsVisible()) {
            // NotificationToast queues its action during button-up. Keep the
            // host alive until that message is delivered, then close it once.
            s->action_pending = message == WM_LBUTTONUP;
            PostMessageW(hwnd, kDismiss, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
}

HWND ShowPickerSavedToast(HWND target, bool dark, D2D1_COLOR_F accent,
    std::wstring title, std::wstring detail, std::vector<SnackAction> actions) {
    if (!IsWindow(target) || GetWindowThreadProcessId(target, nullptr) != GetCurrentThreadId()) return nullptr;
    if (active_toast && IsWindow(active_toast)) DestroyWindow(active_toast);
    WNDCLASSW wc{};
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return nullptr;
    auto state = std::make_unique<State>();
    state->target = target;
    state->scale = static_cast<float>(compat::WindowDpi(target)) / 96.0f;
    state->theme = IsHighContrast() ? MakeHighContrastTheme() : MakeTheme(dark, accent);
    state->title = std::move(title);
    state->detail = std::move(detail);
    if (actions.size() > 2) actions.resize(2);
    state->actions = std::move(actions);
    state->expires = GetTickCount64() + kLifetime;
    State* s = state.release();
    const HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP,
        kClass, L"", WS_POPUP, 0, 0, 392, 240, nullptr, nullptr, wc.hInstance, s);
    if (!hwnd) return nullptr; // WM_NCDESTROY owns the state after WM_NCCREATE.
    active_toast = hwnd;
    if (!SetPropW(target, kTargetProperty, s)) { DestroyWindow(hwnd); return nullptr; }
    Place(hwnd, *s);
    s->compositor.SetSyncInterval(0);
    if (!s->compositor.Init(hwnd)) { DestroyWindow(hwnd); return nullptr; }
    s->compositor.RecreateTextFormats(s->scale);
    s->ready = true;
    Populate(hwnd, *s);
    if (!SetTimer(hwnd, kWatch, 250, nullptr)) { DestroyWindow(hwnd); return nullptr; }
    Draw(hwnd, *s);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    return hwnd;
}

#ifdef PULSE_PICKER_SAVED_TOAST_TEST
namespace picker_saved_toast_test {
bool Snapshot(HWND hwnd, const wchar_t* path) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) return false;
    Draw(hwnd, *s);
    return s->compositor.SaveSnapshot(path);
}
D2D1_RECT_F Bounds(HWND hwnd) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return s ? s->toast.Bounds() : D2D1_RECT_F{};
}
void Expire(HWND hwnd) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (s) { s->expires = 0; SendMessageW(hwnd, WM_TIMER, kWatch, 0); }
}
void Resize(HWND hwnd, int width, float scale) {
    auto* s = reinterpret_cast<State*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s) return;
    s->scale = scale;
    s->compositor.RecreateTextFormats(scale);
    SetWindowPos(hwnd, nullptr, 0, 0, width, static_cast<int>(240.0f * scale), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}
}
#endif
}
