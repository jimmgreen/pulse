#include "../common/windows_compat.h"
#include "window_helpers.h"

#include <algorithm>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace pulse::ui {

float ScaleDip(float scale, float value) { return value * scale; }

D2D1_RECT_F DipRect(float scale, float x, float y, float width, float height) {
    return D2D1::RectF(ScaleDip(scale, x), ScaleDip(scale, y),
                       ScaleDip(scale, x + width), ScaleDip(scale, y + height));
}

bool ContainsRect(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& client_buttons) {
    POINT point{ GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam) };
    RECT window{};
    GetWindowRect(hwnd, &window);
    const UINT dpi = pulse::compat::WindowDpi(hwnd);
    const int frame_x = pulse::compat::SystemMetricsForDpi(SM_CXSIZEFRAME, dpi)
                      + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
    const int frame_y = pulse::compat::SystemMetricsForDpi(SM_CYSIZEFRAME, dpi)
                      + pulse::compat::SystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
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
        !ContainsRect(client_buttons, static_cast<float>(point.x), static_cast<float>(point.y)))
        return HTCAPTION;
    return HTCLIENT;
}

bool ApplyBackdrop(HWND hwnd, bool dark) {
    UpdateWindowTheme(hwnd, dark);
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const bool high_contrast = IsHighContrast();
    DWORD backdrop = high_contrast ? DWMSBT_NONE : DWMSBT_TABBEDWINDOW;
    HRESULT result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                            &backdrop, sizeof(backdrop));
    if (FAILED(result) && backdrop == DWMSBT_TABBEDWINDOW) {
        backdrop = DWMSBT_MAINWINDOW;
        result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE,
                                       &backdrop, sizeof(backdrop));
    }
    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return !high_contrast && SUCCEEDED(result);
}

void CenterOwnedWindow(HWND hwnd, HWND owner, int width, int height,
                       bool clamp_to_work_area) {
    RECT anchor{};
    if (!owner || !GetWindowRect(owner, &anchor)) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor);
        anchor = monitor.rcWork;
    }
    int x = anchor.left + ((anchor.right - anchor.left) - width) / 2;
    int y = anchor.top + ((anchor.bottom - anchor.top) - height) / 2;
    if (clamp_to_work_area) {
        MONITORINFO monitor{ sizeof(monitor) };
        GetMonitorInfoW(MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST), &monitor);
        x = std::clamp(x, static_cast<int>(monitor.rcWork.left),
                       (std::max)(static_cast<int>(monitor.rcWork.left),
                                  static_cast<int>(monitor.rcWork.right) - width));
        y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
                       (std::max)(static_cast<int>(monitor.rcWork.top),
                                  static_cast<int>(monitor.rcWork.bottom) - height));
    }
    SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

void BeginSurface(Compositor& compositor, fluent::Painter& painter,
                  const Theme& theme, bool dark, bool high_contrast,
                  bool backdrop_enabled, float scale) {
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

} // namespace pulse::ui
