// settings_window.cpp — Fluent settings (this round: 右键菜单 only).
#include "settings_window.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <string>

#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2
#endif
#ifndef DWMSBT_TABBEDWINDOW
#define DWMSBT_TABBEDWINDOW 4
#endif

namespace pulse::ui {
namespace {

constexpr wchar_t kClass[] = L"PulseSettingsWindow";
constexpr float kWinW = 520.0f;
constexpr float kWinH = 640.0f;
constexpr float kTitleH = 36.0f;
constexpr float kPad = 20.0f;

float Dip(float scale, float v) { return v * scale; }

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
    x = std::clamp(x, static_cast<int>(monitor.rcWork.left),
                   (std::max)(static_cast<int>(monitor.rcWork.left),
                              static_cast<int>(monitor.rcWork.right) - width));
    y = std::clamp(y, static_cast<int>(monitor.rcWork.top),
                   (std::max)(static_cast<int>(monitor.rcWork.top),
                              static_cast<int>(monitor.rcWork.bottom) - height));
    SetWindowPos(hwnd, HWND_TOP, x, y, width, height, SWP_NOACTIVATE | SWP_NOOWNERZORDER);
}

LRESULT BorderlessHitTest(HWND hwnd, LPARAM lparam, float title_height,
                          const D2D1_RECT_F& close) {
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
    if (fluent::Painter::Contains(close, static_cast<float>(point.x),
                                  static_cast<float>(point.y)))
        return HTCLIENT;
    if (point.y >= 0 && point.y < title_height) return HTCAPTION;
    return HTCLIENT;
}

struct GroupInfo {
    ipc::CtxMenuGroup group;
    const wchar_t* title;
    const wchar_t* desc;
};

constexpr GroupInfo kGroups[] = {
    { ipc::CtxMenuGroup::Software, L"软件功能",
      L"第三方软件的子菜单和类型动词（合并 PDF、CAD 打开…）。默认保留。" },
    { ipc::CtxMenuGroup::OpenWith, L"打开方式",
      L"注册表「用某应用打开」和「打开方式…」。COM 重复项默认关闭。" },
    { ipc::CtxMenuGroup::Share, L"分享",
      L"发送到、即时通讯分享。默认隐藏。" },
    { ipc::CtxMenuGroup::System, L"系统项",
      L"壁纸、旋转、快捷方式、以前的版本等。默认隐藏。" },
    { ipc::CtxMenuGroup::Print, L"打印",
      L"打印。默认保留。" },
};

} // namespace

SettingsWindow::SettingsWindow() : painter_(&compositor_) {}
SettingsWindow::~SettingsWindow() { Destroy(); }

bool SettingsWindow::Create(HWND owner, app::ContextMenuPrefs* prefs) {
    if (hwnd_) {
        prefs_ = prefs;
        return true;
    }
    owner_ = owner;
    prefs_ = prefs;
    dark_ = ShouldUseDarkMode(ThemeMode::Auto);

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!GetClassInfoExW(wc.hInstance, kClass, &wc)) RegisterClassExW(&wc);
    const UINT owner_dpi = owner_ ? GetDpiForWindow(owner_) : 96;
    const int width = MulDiv(static_cast<int>(kWinW), owner_dpi, 96);
    const int height = MulDiv(static_cast<int>(kWinH), owner_dpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kClass, L"设置",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner_, nullptr,
        wc.hInstance, this);
    return hwnd_ != nullptr;
}

void SettingsWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void SettingsWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void SettingsWindow::Show(bool activate) {
    if (!hwnd_) return;
    RebuildHits();
    if (!positioned_) {
        RECT rect{};
        GetWindowRect(hwnd_, &rect);
        CenterOwnedWindow(hwnd_, owner_, rect.right - rect.left, rect.bottom - rect.top);
        positioned_ = true;
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOOWNERZORDER |
                 (activate ? 0 : SWP_NOACTIVATE) | SWP_SHOWWINDOW);
    if (activate) SetForegroundWindow(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void SettingsWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool SettingsWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void SettingsWindow::ApplyWindowTheme() {
    backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
}

void SettingsWindow::ClampScroll() {
    const float view = (std::max)(0.0f, static_cast<float>(compositor_.Height()) - Dip(scale_, kTitleH));
    const float max_scroll = (std::max)(0.0f, content_h_ - view);
    scroll_ = std::clamp(scroll_, 0.0f, max_scroll);
}

void SettingsWindow::RebuildHits() {
    hits_.clear();
    if (!prefs_) return;
    const float s = (std::max)(scale_, 0.001f);
    const float width = static_cast<float>(compositor_.Width());
    const float pad = Dip(s, kPad);
    const float title = Dip(s, kTitleH);
    const float close_w = Dip(s, 40.0f);
    hits_.push_back({ D2D1::RectF(width - close_w, 0, width, title), 1, false });

    float y = Dip(s, 16.0f);
    y += Dip(s, 28.0f); // page title
    y += Dip(s, 22.0f); // subtitle
    y += Dip(s, 8.0f);

    int item_id = 100;
    for (int g = 0; g < 5; ++g) {
        size_t n = 0;
        if (prefs_) {
            for (const auto& seen : prefs_->seen)
                if (ipc::GroupOf(seen.category) == kGroups[g].group) ++n;
        }
        const float header = Dip(s, 56.0f);
        const float desc = Dip(s, 36.0f);
        const float row_h = Dip(s, 36.0f);
        const float card_h = header + desc + (n ? n * row_h : 0) + Dip(s, 8.0f);
        const float card_left = pad;
        const float card_right = width - pad;
        const float switch_w = Dip(s, 56.0f);
        hits_.push_back({ D2D1::RectF(card_right - switch_w - Dip(s, 8.0f), y,
                                      card_right - Dip(s, 8.0f), y + header),
                          10 + g, true });
        float iy = y + header + desc;
        if (prefs_) {
            for (const auto& seen : prefs_->seen) {
                if (ipc::GroupOf(seen.category) != kGroups[g].group) continue;
                hits_.push_back({ D2D1::RectF(card_left, iy, card_right, iy + row_h),
                                  item_id++, true });
                iy += row_h;
            }
        }
        y += card_h + Dip(s, 12.0f);
    }

    hits_.push_back({ D2D1::RectF(pad, y, pad + Dip(s, 120.0f), y + Dip(s, 32.0f)), 2, true });
    y += Dip(s, 48.0f);
    content_h_ = y;
    ClampScroll();
}

int SettingsWindow::HitTest(float x, float y) const {
    const float title = Dip(scale_, kTitleH);
    for (const auto& hit : hits_) {
        float hy = hit.rect.top;
        float hb = hit.rect.bottom;
        if (hit.scrolled) {
            hy = hit.rect.top + title - scroll_;
            hb = hit.rect.bottom + title - scroll_;
            if (y < title) continue;
        }
        if (x >= hit.rect.left && x < hit.rect.right && y >= hy && y < hb)
            return hit.id;
    }
    return 0;
}

void SettingsWindow::OnClick(int hit) {
    if (!prefs_ || hit == 0) return;
    if (hit == 1) {
        Hide();
        return;
    }
    if (hit == 2) {
        prefs_->ResetToDefaults();
        prefs_->Save();
        RebuildHits();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (hit >= 10 && hit < 15) {
        const auto group = kGroups[hit - 10].group;
        prefs_->SetGroupEnabled(group, !prefs_->GroupEnabled(group));
        prefs_->Save();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
    if (hit >= 100) {
        int id = 100;
        for (int g = 0; g < 5; ++g) {
            for (const auto& seen : prefs_->seen) {
                if (ipc::GroupOf(seen.category) != kGroups[g].group) continue;
                if (id == hit) {
                    const bool on = prefs_->ItemEnabled(seen.key, seen.category, seen.from_com);
                    prefs_->SetItemEnabled(seen.key, !on);
                    prefs_->Save();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                ++id;
            }
        }
    }
}

void SettingsWindow::Render() {
    if (!compositor_.Dc() || !prefs_) return;
    const bool high_contrast = IsHighContrast();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    auto* dc = compositor_.Dc();
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0.0f));
    painter_.BeginFrame(theme, high_contrast);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    D2D1_COLOR_F tint = theme.bg;
    tint.a = high_contrast || !backdrop_enabled_ ? 1.0f : (dark_ ? 0.76f : 0.82f);
    painter_.FillRoundedRect(D2D1::RectF(0, 0, width, height), 0, tint);
    painter_.StrokeRoundedRect(D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f),
                               12.0f * scale_, theme.stroke_card);

    const float s = (std::max)(scale_, 0.001f);
    const float title = Dip(s, kTitleH);
    const float pad = Dip(s, kPad);
    const float close_w = Dip(s, 40.0f);
    const auto close = D2D1::RectF(width - close_w, 0, width, title);
    painter_.DrawText(L"设置", D2D1::RectF(pad, 0, close.left, title),
                      compositor_.SmallFormat(), theme.text);
    fluent::ControlState close_state{};
    close_state.hovered = hover_ == 1;
    close_state.pressed = pressed_ == 1;
    painter_.DrawTitleBarButton(close, fluent::TitleBarButtonRole::Close, {}, close_state);
    painter_.FillRoundedRect(D2D1::RectF(0, title, width, title + 1.0f), 0, theme.stroke_divider);

    dc->PushAxisAlignedClip(D2D1::RectF(0, title, width, height), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    const float origin = title - scroll_;

    painter_.DrawText(L"右键菜单",
                      D2D1::RectF(pad, origin + Dip(s, 16.0f), width - pad, origin + Dip(s, 44.0f)),
                      compositor_.HeaderFormat(), theme.text);
    painter_.DrawText(L"选择融合区显示哪些 Explorer 动词。改动在下次右键生效。",
                      D2D1::RectF(pad, origin + Dip(s, 44.0f), width - pad, origin + Dip(s, 66.0f)),
                      compositor_.SmallFormat(), theme.text_secondary);

    float y = origin + Dip(s, 78.0f);
    int item_id = 100;
    const float switch_w = Dip(s, 42.0f);
    const float switch_h = Dip(s, 32.0f);

    for (int g = 0; g < 5; ++g) {
        std::vector<const app::SeenMenuItem*> rows;
        for (const auto& seen : prefs_->seen)
            if (ipc::GroupOf(seen.category) == kGroups[g].group) rows.push_back(&seen);
        const float header = Dip(s, 56.0f);
        const float desc = Dip(s, 36.0f);
        const float row_h = Dip(s, 36.0f);
        const float card_h = header + desc + (rows.empty() ? 0 : rows.size() * row_h) + Dip(s, 8.0f);
        const auto card = D2D1::RectF(pad, y, width - pad, y + card_h);
        painter_.FillRoundedRect(card, Dip(s, 8.0f), theme.fill_input);
        painter_.StrokeRoundedRect(card, Dip(s, 8.0f), theme.stroke_card);

        painter_.DrawText(kGroups[g].title,
                          D2D1::RectF(card.left + Dip(s, 16.0f), y + Dip(s, 10.0f),
                                      card.right - Dip(s, 64.0f), y + Dip(s, 32.0f)),
                          compositor_.TextFormat(), theme.text);
        fluent::ControlState group_state{};
        group_state.checked = prefs_->GroupEnabled(kGroups[g].group);
        group_state.hovered = hover_ == 10 + g;
        group_state.pressed = pressed_ == 10 + g;
        painter_.DrawSwitch(D2D1::RectF(card.right - Dip(s, 16.0f) - switch_w,
                                        y + (header - switch_h) * 0.5f,
                                        card.right - Dip(s, 16.0f),
                                        y + (header + switch_h) * 0.5f),
                            L"", group_state);
        painter_.DrawText(kGroups[g].desc,
                          D2D1::RectF(card.left + Dip(s, 16.0f), y + header - Dip(s, 4.0f),
                                      card.right - Dip(s, 16.0f), y + header + desc - Dip(s, 8.0f)),
                          compositor_.SmallFormat(), theme.text_secondary);

        float iy = y + header + desc;
        for (const auto* row : rows) {
            if (hover_ == item_id)
                painter_.FillRoundedRect(D2D1::RectF(card.left + Dip(s, 4.0f), iy,
                                                     card.right - Dip(s, 4.0f), iy + row_h),
                                         Dip(s, 4.0f), theme.fill_hover);
            painter_.DrawText(row->text,
                              D2D1::RectF(card.left + Dip(s, 16.0f), iy,
                                          card.right - Dip(s, 64.0f), iy + row_h),
                              compositor_.SmallFormat(), theme.text);
            fluent::ControlState item_state{};
            item_state.checked = prefs_->ItemEnabled(row->key, row->category, row->from_com);
            item_state.hovered = hover_ == item_id;
            item_state.pressed = pressed_ == item_id;
            painter_.DrawSwitch(D2D1::RectF(card.right - Dip(s, 16.0f) - switch_w,
                                            iy + (row_h - switch_h) * 0.5f,
                                            card.right - Dip(s, 16.0f),
                                            iy + (row_h + switch_h) * 0.5f),
                                L"", item_state);
            ++item_id;
            iy += row_h;
        }
        y += card_h + Dip(s, 12.0f);
    }

    fluent::ControlState restore{};
    restore.hovered = hover_ == 2;
    restore.pressed = pressed_ == 2;
    painter_.DrawButton({ D2D1::RectF(pad, y, pad + Dip(s, 120.0f), y + Dip(s, 32.0f)),
                          L"恢复默认", {}, fluent::ButtonKind::Standard, restore });

    dc->PopAxisAlignedClip();

    const float view = height - title;
    if (content_h_ > view + 1.0f) {
        fluent::ScrollbarSpec bar;
        bar.viewport = D2D1::RectF(width - Dip(s, 10.0f), title, width - Dip(s, 2.0f), height);
        bar.offset = scroll_;
        bar.viewport_extent = view;
        bar.content_extent = content_h_;
        bar.expand_progress = 1.0f;
        painter_.DrawScrollbar(bar);
    }

    compositor_.Dc()->EndDraw();
    compositor_.Present();
}

LRESULT CALLBACK SettingsWindow::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<SettingsWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<SettingsWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT SettingsWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
        ApplyWindowTheme();
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        RebuildHits();
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const float width = static_cast<float>(compositor_.Width());
        const float title = Dip(scale_, kTitleH);
        const float close_w = Dip(scale_, 40.0f);
        return BorderlessHitTest(hwnd_, lparam, title,
            D2D1::RectF(width - close_w, 0, width, title));
    }
    case WM_SIZE:
        if (compositor_.Dc() && wparam != SIZE_MINIMIZED) {
            compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
            RebuildHits();
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_DPICHANGED: {
        scale_ = HIWORD(wparam) / 96.0f;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        const auto* suggested = reinterpret_cast<RECT*>(lparam);
        SetWindowPos(hwnd_, nullptr, suggested->left, suggested->top,
                     suggested->right - suggested->left, suggested->bottom - suggested->top,
                     SWP_NOZORDER | SWP_NOOWNERZORDER);
        RebuildHits();
        return 0;
    }
    case WM_MOUSEWHEEL: {
        const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
        scroll_ -= static_cast<float>(delta) / 120.0f * Dip(scale_, 48.0f);
        ClampScroll();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int next = HitTest(static_cast<float>(GET_X_LPARAM(lparam)),
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
    case WM_LBUTTONDOWN:
        pressed_ = HitTest(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
        const int pressed = pressed_;
        pressed_ = 0;
        ReleaseCapture();
        if (hit == pressed) OnClick(hit);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;
    case WM_CLOSE:
        Hide();
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
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

} // namespace pulse::ui
