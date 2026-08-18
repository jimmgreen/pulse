#include "file_operation_dialog.h"
#include "../common/text_format.h"

#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <string>

namespace pulse::ui {
namespace {

constexpr wchar_t kTransferClass[] = L"PulseFileOperationWindow";
constexpr wchar_t kConflictClass[] = L"PulseFileConflictWindow";
constexpr UINT_PTR kRenderTimer = 1;
constexpr float kDlgW = 460.0f;
constexpr float kTitleH = 36.0f;
constexpr float kPadX = 20.0f;
constexpr float kFooterH = 42.0f;
constexpr float kCollapsedH = 258.0f;
constexpr float kGraphBlockH = 156.0f;
constexpr float kDetailedH = kCollapsedH + kGraphBlockH;

float ScaleDip(float scale, float value) { return value * scale; }

struct TransferChrome {
    D2D1_RECT_F minimize{};
    D2D1_RECT_F close{};
    D2D1_RECT_F details{};
    D2D1_RECT_F pause{};
    D2D1_RECT_F cancel{};
};

TransferChrome MakeTransferChrome(float scale, float width, float height) {
    TransferChrome chrome;
    const float title = ScaleDip(scale, kTitleH);
    const float btn = ScaleDip(scale, 40.0f);
    chrome.close = D2D1::RectF(width - btn, 0, width, title);
    chrome.minimize = D2D1::RectF(width - btn * 2.0f, 0, width - btn, title);
    const float footer_top = height - ScaleDip(scale, kFooterH);
    chrome.details = D2D1::RectF(ScaleDip(scale, kPadX), footer_top + ScaleDip(scale, 7.0f),
                                 ScaleDip(scale, kPadX + 108.0f), height - ScaleDip(scale, 7.0f));
    chrome.pause = D2D1::RectF(width - ScaleDip(scale, 168.0f), footer_top + ScaleDip(scale, 7.0f),
                               width - ScaleDip(scale, 96.0f), height - ScaleDip(scale, 7.0f));
    chrome.cancel = D2D1::RectF(width - ScaleDip(scale, 88.0f), footer_top + ScaleDip(scale, 7.0f),
                                width - ScaleDip(scale, kPadX), height - ScaleDip(scale, 7.0f));
    return chrome;
}

void DrawSpeedGraph(ID2D1DeviceContext* dc, const D2D1_RECT_F& plot,
                    const std::deque<double>& history, D2D1_COLOR_F accent, float scale) {
    if (!dc) return;
    ComPtr<ID2D1SolidColorBrush> grid;
    D2D1_COLOR_F grid_color = accent;
    grid_color.a = 0.08f;
    dc->CreateSolidColorBrush(grid_color, &grid);
    const float step = ScaleDip(scale, 20.0f);
    for (float y = plot.top + ScaleDip(scale, 12.0f); y < plot.bottom; y += step) {
        dc->DrawLine(D2D1::Point2F(plot.left, y), D2D1::Point2F(plot.right, y),
                     grid.get(), 1.0f);
    }
    if (history.size() < 2) return;

    const double max_speed = (std::max)(1.0, *std::max_element(history.begin(), history.end()));
    const float left = plot.left;
    const float right = plot.right;
    const float top = plot.top + ScaleDip(scale, 6.0f);
    const float bottom = plot.bottom;
    const size_t count = history.size();
    auto y_at = [&](size_t index) {
        return bottom - (bottom - top) *
            static_cast<float>(history[index] / max_speed);
    };
    auto x_at = [&](size_t index) {
        return left + (right - left) * static_cast<float>(index)
            / static_cast<float>(count - 1);
    };

    ID2D1Factory* factory = nullptr;
    dc->GetFactory(&factory);
    if (factory) {
        ComPtr<ID2D1PathGeometry> geometry;
        if (SUCCEEDED(factory->CreatePathGeometry(&geometry))) {
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(geometry->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(x_at(0), y_at(0)), D2D1_FIGURE_BEGIN_FILLED);
                for (size_t i = 1; i < count; ++i)
                    sink->AddLine(D2D1::Point2F(x_at(i), y_at(i)));
                sink->AddLine(D2D1::Point2F(x_at(count - 1), bottom));
                sink->AddLine(D2D1::Point2F(x_at(0), bottom));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                sink->Close();
                D2D1_COLOR_F fill_color = accent;
                fill_color.a = 0.28f;
                ComPtr<ID2D1SolidColorBrush> fill;
                dc->CreateSolidColorBrush(fill_color, &fill);
                dc->FillGeometry(geometry.get(), fill.get());
            }
        }
        factory->Release();
    }

    ComPtr<ID2D1SolidColorBrush> line;
    dc->CreateSolidColorBrush(accent, &line);
    for (size_t i = 1; i < count; ++i) {
        dc->DrawLine(D2D1::Point2F(x_at(i - 1), y_at(i - 1)),
                     D2D1::Point2F(x_at(i), y_at(i)), line.get(), 2.0f * scale);
    }
    const float last_x = x_at(count - 1);
    const float last_y = y_at(count - 1);
    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(last_x, last_y),
                                  3.5f * scale, 3.5f * scale), line.get());
}

D2D1_RECT_F Rect(float scale, float x, float y, float width, float height) {
    return D2D1::RectF(ScaleDip(scale, x), ScaleDip(scale, y),
                       ScaleDip(scale, x + width), ScaleDip(scale, y + height));
}

bool Contains(const D2D1_RECT_F& rect, float x, float y) {
    return x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom;
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring_view view = path;
    while (view.size() > 1 && (view.back() == L'\\' || view.back() == L'/'))
        view.remove_suffix(1);
    const size_t slash = view.find_last_of(L"\\/");
    return slash == std::wstring_view::npos ? std::wstring(view)
                                             : std::wstring(view.substr(slash + 1));
}

std::wstring ParentName(const std::wstring& path) {
    std::wstring copy = path;
    while (copy.size() > 1 && (copy.back() == L'\\' || copy.back() == L'/')) copy.pop_back();
    const size_t slash = copy.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return copy;
    return LeafName(copy.substr(0, slash));
}

std::wstring FormatDuration(uint64_t seconds) {
    if (seconds == 0) return L"正在估算";
    if (seconds < 60) return std::to_wstring(seconds) + L" 秒";
    const uint64_t minutes = seconds / 60;
    if (minutes < 60) return std::to_wstring(minutes) + L" 分 "
        + std::to_wstring(seconds % 60) + L" 秒";
    return std::to_wstring(minutes / 60) + L" 小时 "
        + std::to_wstring(minutes % 60) + L" 分";
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
    compositor.Dc()->EndDraw();
    compositor.Present();
}

class ConflictWindow {
public:
    ConflictDialogResult Show(HWND owner, const ops::ConflictItemInfo& conflict,
                              bool dark, D2D1_COLOR_F accent) {
        owner_ = owner;
        conflict_ = conflict;
        dark_ = dark;
        accent_ = accent;
        scale_ = static_cast<float>(GetDpiForWindow(owner)) / 96.0f;

        WNDCLASSEXW wc{ sizeof(wc) };
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpfnWndProc = WndProc;
        wc.lpszClassName = kConflictClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        if (!GetClassInfoExW(wc.hInstance, kConflictClass, &wc)) RegisterClassExW(&wc);

        const int width = static_cast<int>(480 * scale_);
        const int height = static_cast<int>(428 * scale_);
        hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kConflictClass, L"替换或跳过文件",
            WS_POPUP | WS_THICKFRAME | WS_SYSMENU,
            CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner, nullptr,
            wc.hInstance, this);
        if (!hwnd_) return result_;
        CenterOwnedWindow(hwnd_, owner_, width, height);
        EnableWindow(owner_, FALSE);
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);

        MSG message{};
        while (!done_ && GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (IsWindow(hwnd_)) DestroyWindow(hwnd_);
        hwnd_ = nullptr;
        EnableWindow(owner_, TRUE);
        SetActiveWindow(owner_);
        return result_;
    }

private:
    static LRESULT CALLBACK WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<ConflictWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            self = static_cast<ConflictWindow*>(create->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd_ = hwnd;
        }
        return self ? self->Handle(message, wparam, lparam)
                    : DefWindowProcW(hwnd, message, wparam, lparam);
    }

    D2D1_RECT_F CloseRect() const { return Rect(scale_, 434, 0, 46, 44); }
    D2D1_RECT_F CardRect(int index) const { return Rect(scale_, 20, 121 + index * 66.0f, 440, 58); }
    D2D1_RECT_F CheckRect() const { return Rect(scale_, 20, details_ ? 438.0f : 326.0f, 350, 30); }
    D2D1_RECT_F DetailRect() const { return Rect(scale_, 20, details_ ? 486.0f : 374.0f, 110, 32); }
    D2D1_RECT_F CancelRect() const { return Rect(scale_, 362, details_ ? 482.0f : 370.0f, 98, 34); }

    int Hit(float x, float y) const {
        if (Contains(CloseRect(), x, y)) return 7;
        for (int i = 0; i < 3; ++i) if (Contains(CardRect(i), x, y)) return i + 1;
        if (conflict_.remaining > 1 && Contains(CheckRect(), x, y)) return 4;
        if (Contains(DetailRect(), x, y)) return 5;
        if (Contains(CancelRect(), x, y)) return 6;
        return 0;
    }

    void Complete(ops::ConflictChoice choice) {
        result_.choice = choice;
        result_.apply_to_all = apply_all_;
        done_ = true;
        if (hwnd_) DestroyWindow(hwnd_);
    }

    void ToggleDetails() {
        details_ = !details_;
        RECT window{};
        GetWindowRect(hwnd_, &window);
        const int target = static_cast<int>((details_ ? 538 : 428) * scale_);
        SetWindowPos(hwnd_, nullptr, 0, 0, window.right - window.left, target,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOOWNERZORDER);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void DrawCard(const Theme& theme, int index, const wchar_t* title,
                  const wchar_t* subtitle, const wchar_t* glyph,
                  D2D1_COLOR_F icon_color, const wchar_t* shortcut) {
        const auto bounds = CardRect(index);
        fluent::ControlState state{};
        state.hovered = hover_ == index + 1;
        state.pressed = pressed_ == index + 1;
        state.focused = focus_ == index;
        D2D1_COLOR_F fill = dark_ ? HexColor(state.hovered ? 0x474747 : 0x3A3A3A, 0.92f)
                                  : HexColor(state.hovered ? 0xE4E4E4 : 0xEEEEEE, 0.92f);
        painter_.FillRoundedRect(bounds, 10 * scale_, fill);
        painter_.StrokeRoundedRect(bounds, 10 * scale_,
            state.focused ? WithAlpha(theme.accent, 0.35f) : theme.stroke_card,
            state.focused ? 1.5f : 1.0f);
        const auto icon = Rect(scale_, 32, 133 + index * 66.0f, 34, 34);
        painter_.FillRoundedRect(icon, 17 * scale_, icon_color);
        painter_.DrawGlyph(glyph, icon, HexColor(0xFFFFFF));
        painter_.DrawText(title, Rect(scale_, 78, 126 + index * 66.0f, 272, 25),
                          compositor_.TextFormat(), theme.text);
        painter_.DrawText(subtitle, Rect(scale_, 78, 150 + index * 66.0f, 300, 20),
                          compositor_.SmallFormat(), theme.text_secondary);
        fluent::BadgeSpec badge{ Rect(scale_, 382, 139 + index * 66.0f, 58, 22), shortcut,
                                 fluent::BadgeKind::Keycap };
        painter_.DrawBadge(badge);
    }

    void Render() {
        if (!compositor_.Dc()) return;
        const bool high_contrast = IsHighContrast();
        const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
        BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                     scale_);

        painter_.DrawGlyph(L"\xE8C8", Rect(scale_, 14, 11, 22, 22), theme.accent);
        painter_.DrawText(L"替换或跳过文件", Rect(scale_, 42, 0, 260, 44),
                          compositor_.SmallFormat(), theme.text);
        fluent::ControlState close_state{};
        close_state.hovered = hover_ == 7;
        close_state.pressed = pressed_ == 7;
        painter_.DrawTitleBarButton(CloseRect(), fluent::TitleBarButtonRole::Close,
                                    {}, close_state);
        painter_.FillRoundedRect(Rect(scale_, 0, 43, 480, 1), 0, theme.stroke_divider);

        const std::wstring route = ParentName(conflict_.source) + L"  →  "
                                 + ParentName(conflict_.destination);
        painter_.DrawBadge({ Rect(scale_, 20, 58, 250, 24), route,
                             fluent::BadgeKind::Neutral });
        const std::wstring headline = L"目标中已包含“" + LeafName(conflict_.destination) + L"”";
        painter_.DrawText(headline, Rect(scale_, 20, 84, 440, 30),
                          compositor_.HeaderFormat(), theme.text);

        DrawCard(theme, 0, L"替换目标中的文件", L"使用来源中的版本覆盖现有文件",
                 L"\xE73E", HexColor(0x107C10), L"Alt+R");
        DrawCard(theme, 1, L"跳过该文件", L"不做更改，保留目标中的现有文件",
                 L"\xE72A", HexColor(0x0078D4), L"Alt+S");
        DrawCard(theme, 2, L"保留两者", L"自动生成副本名称并继续传输",
                 L"\xE8C8", HexColor(0x6B69D6), L"Alt+K");

        if (conflict_.remaining > 1) {
            fluent::ControlState check{};
            check.checked = apply_all_;
            check.hovered = hover_ == 4;
            check.focused = focus_ == 3;
            const std::wstring label = L"应用于剩余 " + std::to_wstring(conflict_.remaining)
                                     + L" 个冲突";
            painter_.DrawCheckBox(CheckRect(), label, check);
        }
        painter_.FillRoundedRect(Rect(scale_, 0, details_ ? 474.0f : 360.0f, 480, 1), 0,
                                 theme.stroke_divider);
        fluent::ControlState detail_state{};
        detail_state.hovered = hover_ == 5;
        detail_state.focused = focus_ == (conflict_.remaining > 1 ? 4 : 3);
        painter_.DrawButton({ DetailRect(), details_ ? L"简略信息" : L"详细信息",
                              details_ ? L"\xE70E" : L"\xE70D",
                              fluent::ButtonKind::Transparent, detail_state });
        fluent::ControlState cancel_state{};
        cancel_state.hovered = hover_ == 6;
        cancel_state.pressed = pressed_ == 6;
        cancel_state.focused = focus_ == (conflict_.remaining > 1 ? 5 : 4);
        painter_.DrawButton({ CancelRect(), L"取消", {}, fluent::ButtonKind::Standard,
                              cancel_state });

        if (details_) {
            const float top = 326.0f;
            painter_.FillRoundedRect(Rect(scale_, 20, top, 440, 104), 7 * scale_,
                                     dark_ ? HexColor(0x161616, 0.62f) : HexColor(0xFFFFFF, 0.62f));
            painter_.StrokeRoundedRect(Rect(scale_, 20, top, 440, 104), 7 * scale_, theme.stroke_card);
            painter_.DrawText(L"来源", Rect(scale_, 32, top + 8, 190, 20),
                              compositor_.SmallFormat(), theme.accent);
            painter_.DrawText(L"现有", Rect(scale_, 252, top + 8, 190, 20),
                              compositor_.SmallFormat(), theme.text_secondary);
            const std::wstring source_meta = pulse::format::ByteSize(conflict_.source_size) + L"  ·  "
                                           + pulse::format::LocalFileTime(conflict_.source_modified, L"未知");
            const std::wstring target_meta = pulse::format::ByteSize(conflict_.destination_size) + L"  ·  "
                                           + pulse::format::LocalFileTime(conflict_.destination_modified, L"未知");
            painter_.DrawText(source_meta, Rect(scale_, 32, top + 30, 198, 20),
                              compositor_.SmallFormat(), theme.text);
            painter_.DrawText(target_meta, Rect(scale_, 252, top + 30, 196, 20),
                              compositor_.SmallFormat(), theme.text);
            painter_.DrawText(conflict_.source, Rect(scale_, 32, top + 55, 198, 38),
                              compositor_.SmallFormat(), theme.text_secondary);
            painter_.DrawText(conflict_.destination, Rect(scale_, 252, top + 55, 196, 38),
                              compositor_.SmallFormat(), theme.text_secondary);
        }
        EndSurface(compositor_);
    }

    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam) {
        switch (message) {
        case WM_CREATE:
            scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
            backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
            if (!compositor_.Init(hwnd_)) return -1;
            compositor_.RecreateTextFormats(scale_);
            painter_.SetCompositor(&compositor_);
            painter_.SetScale(scale_);
            return 0;
        case WM_NCCALCSIZE:
            return 0;
        case WM_NCHITTEST:
            return BorderlessHitTest(hwnd_, lparam, 44 * scale_, CloseRect());
        case WM_SIZE:
            if (compositor_.Dc()) compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
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
        case WM_LBUTTONDOWN:
            pressed_ = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                           static_cast<float>(GET_Y_LPARAM(lparam)));
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP: {
            const int hit = Hit(static_cast<float>(GET_X_LPARAM(lparam)),
                                static_cast<float>(GET_Y_LPARAM(lparam)));
            const int pressed = pressed_;
            pressed_ = 0;
            ReleaseCapture();
            if (hit == pressed) {
                if (hit >= 1 && hit <= 3) {
                    focus_ = hit - 1;
                    Complete(hit == 1 ? ops::ConflictChoice::Replace
                             : hit == 2 ? ops::ConflictChoice::Skip
                                        : ops::ConflictChoice::KeepBoth);
                } else if (hit == 4) apply_all_ = !apply_all_;
                else if (hit == 5) ToggleDetails();
                else if (hit == 6 || hit == 7) Complete(ops::ConflictChoice::Cancel);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        case WM_KEYDOWN:
            if (GetKeyState(VK_MENU) < 0) {
                if (wparam == 'R') { Complete(ops::ConflictChoice::Replace); return 0; }
                if (wparam == 'S') { Complete(ops::ConflictChoice::Skip); return 0; }
                if (wparam == 'K') { Complete(ops::ConflictChoice::KeepBoth); return 0; }
            }
            if (wparam == VK_ESCAPE) { Complete(ops::ConflictChoice::Cancel); return 0; }
            if (wparam == VK_TAB) {
                const int count = conflict_.remaining > 1 ? 6 : 5;
                focus_ = (focus_ + (GetKeyState(VK_SHIFT) < 0 ? count - 1 : 1)) % count;
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            if (wparam == VK_RETURN || wparam == VK_SPACE) {
                if (focus_ <= 2) Complete(focus_ == 0 ? ops::ConflictChoice::Replace
                                         : focus_ == 1 ? ops::ConflictChoice::Skip
                                                       : ops::ConflictChoice::KeepBoth);
                else if (focus_ == 3 && conflict_.remaining > 1) apply_all_ = !apply_all_;
                else if (focus_ == (conflict_.remaining > 1 ? 4 : 3)) ToggleDetails();
                else Complete(ops::ConflictChoice::Cancel);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            break;
        case WM_SYSKEYDOWN:
            if (wparam == 'R') { Complete(ops::ConflictChoice::Replace); return 0; }
            if (wparam == 'S') { Complete(ops::ConflictChoice::Skip); return 0; }
            if (wparam == 'K') { Complete(ops::ConflictChoice::KeepBoth); return 0; }
            break;
        case WM_CLOSE:
            Complete(ops::ConflictChoice::Cancel);
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
            done_ = true;
            return 0;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    HWND hwnd_ = nullptr;
    HWND owner_ = nullptr;
    Compositor compositor_;
    fluent::Painter painter_;
    ops::ConflictItemInfo conflict_;
    ConflictDialogResult result_;
    D2D1_COLOR_F accent_ = HexColor(0x0078D4);
    float scale_ = 1.0f;
    bool dark_ = false;
    bool backdrop_enabled_ = false;
    bool details_ = false;
    bool apply_all_ = false;
    bool done_ = false;
    int hover_ = 0;
    int pressed_ = 0;
    int focus_ = 0;
};

} // namespace

FileOperationWindow::FileOperationWindow() : painter_(&compositor_) {}
FileOperationWindow::~FileOperationWindow() { Destroy(); }

bool FileOperationWindow::Create(HWND owner, FileOperationCallbacks callbacks) {
    if (hwnd_) return true;
    owner_ = owner;
    callbacks_ = std::move(callbacks);
    dark_ = ShouldUseDarkMode(ThemeMode::Auto);
    accent_ = GetAccentColor();

    WNDCLASSEXW wc{ sizeof(wc) };
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpfnWndProc = WndProc;
    wc.lpszClassName = kTransferClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    if (!GetClassInfoExW(wc.hInstance, kTransferClass, &wc)) RegisterClassExW(&wc);
    const UINT owner_dpi = GetDpiForWindow(owner_);
    const int width = MulDiv(static_cast<int>(kDlgW), owner_dpi, 96);
    const int height = MulDiv(static_cast<int>(kCollapsedH), owner_dpi, 96);
    hwnd_ = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, kTransferClass, L"文件操作",
        WS_POPUP | WS_THICKFRAME | WS_SYSMENU | WS_MINIMIZEBOX,
        CW_USEDEFAULT, CW_USEDEFAULT, width, height, owner_, nullptr,
        wc.hInstance, this);
    return hwnd_ != nullptr;
}

void FileOperationWindow::Destroy() {
    if (hwnd_) DestroyWindow(hwnd_);
    hwnd_ = nullptr;
}

void FileOperationWindow::SetTheme(bool dark, D2D1_COLOR_F accent) {
    dark_ = dark;
    accent_ = accent;
    if (hwnd_) {
        ApplyWindowTheme();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void FileOperationWindow::Update(const ops::OpStatus& status) {
    const bool new_task = status.task_id != status_.task_id;
    status_ = status;
    if (new_task) {
        speed_history_.clear();
    }
    if (hwnd_) InvalidateRect(hwnd_, nullptr, FALSE);
}

void FileOperationWindow::Show(bool activate) {
    if (!hwnd_) return;
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
}

void FileOperationWindow::Hide() {
    if (hwnd_) ShowWindow(hwnd_, SW_HIDE);
}

bool FileOperationWindow::IsVisible() const {
    return hwnd_ && IsWindowVisible(hwnd_);
}

void FileOperationWindow::ApplyWindowTheme() {
    backdrop_enabled_ = ApplyBackdrop(hwnd_, dark_);
}

void FileOperationWindow::ResizeForDetails(bool preserve_center) {
    if (!hwnd_) return;
    RECT rect{};
    GetWindowRect(hwnd_, &rect);
    const int width = rect.right - rect.left;
    const int old_height = rect.bottom - rect.top;
    const int height = static_cast<int>((detailed_ ? kDetailedH : kCollapsedH) * scale_);
    const int y = preserve_center ? rect.top - (height - old_height) / 2 : rect.top;
    SetWindowPos(hwnd_, nullptr, rect.left, y, width, height,
                 SWP_NOZORDER | SWP_NOOWNERZORDER);
}

int FileOperationWindow::HitTestControl(float x, float y) const {
    const auto chrome = MakeTransferChrome(scale_,
        static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()));
    if (Contains(chrome.close, x, y)) return 1;
    if (Contains(chrome.minimize, x, y)) return 2;
    if (Contains(chrome.pause, x, y)) return 3;
    if (Contains(chrome.cancel, x, y)) return 4;
    if (Contains(chrome.details, x, y)) return 5;
    return 0;
}

void FileOperationWindow::RequestClose() {
    if (status_.active && status_.phase != ops::OpPhase::Completed &&
        status_.phase != ops::OpPhase::Failed) {
        if (callbacks_.cancel) callbacks_.cancel();
        return;
    }
    if (callbacks_.dismiss) callbacks_.dismiss();
    Hide();
}

void FileOperationWindow::Render() {
    if (!compositor_.Dc()) return;
    const bool high_contrast = IsHighContrast();
    const Theme theme = high_contrast ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
    BeginSurface(compositor_, painter_, theme, dark_, high_contrast, backdrop_enabled_,
                 scale_);
    const float width = static_cast<float>(compositor_.Width());
    const float height = static_cast<float>(compositor_.Height());
    const float dip_w = width / std::max(scale_, 0.001f);
    const auto chrome = MakeTransferChrome(scale_, width, height);
    const bool failed = status_.phase == ops::OpPhase::Failed;
    const bool paused = status_.phase == ops::OpPhase::Paused;
    const bool waiting = status_.phase == ops::OpPhase::WaitingForConflict;
    const bool scanning = status_.phase == ops::OpPhase::Scanning;
    const bool completed = status_.phase == ops::OpPhase::Completed;
    const bool moving = status_.type == ops::OpType::Move;
    const bool deleting = status_.type == ops::OpType::RecycleDelete
                       || status_.type == ops::OpType::RealDelete;
    const bool restoring = status_.type == ops::OpType::RestoreRecycle;
    const D2D1_COLOR_F sky = dark_ ? HexColor(0x38BDF8) : HexColor(0x0284C7);

    const wchar_t* operation_glyph = deleting ? L"\xE74D" : restoring ? L"\xE777"
        : (moving ? L"\xE7C2" : L"\xE8C8");
    painter_.DrawGlyph(operation_glyph, Rect(scale_, 14, 10, 16, 16), sky);
    std::wstring title;
    if (failed) {
        title = deleting ? L"无法完成删除" : restoring ? L"无法完成还原"
            : moving ? L"无法完成移动" : L"无法完成复制";
    } else if (completed) {
        title = (deleting ? L"已删除 " : restoring ? L"已还原 " : moving ? L"已移动 " : L"已复制 ")
              + std::to_wstring(status_.completed_items) + L" 个项目";
    } else if (paused) {
        title = deleting ? L"已暂停删除" : restoring ? L"已暂停还原"
            : moving ? L"已暂停移动" : L"已暂停复制";
    } else if (scanning) title = L"正在准备文件列表";
    else {
        const uint64_t count = status_.total_items > 0 ? status_.total_items
            : (std::max)(status_.completed_items, static_cast<uint64_t>(1));
        title = (deleting ? L"正在删除 " : restoring ? L"正在还原 " : moving ? L"正在移动 " : L"正在复制 ")
              + std::to_wstring(count) + L" 个项目";
    }
    painter_.DrawText(title, D2D1::RectF(ScaleDip(scale_, 36.0f), 0,
                                         chrome.minimize.left - ScaleDip(scale_, 8.0f),
                                         ScaleDip(scale_, kTitleH)),
                      compositor_.SmallFormat(), theme.text);

    fluent::ControlState minimize{};
    minimize.hovered = hover_ == 2;
    minimize.pressed = pressed_ == 2;
    painter_.DrawTitleBarButton(chrome.minimize, fluent::TitleBarButtonRole::Minimize, {}, minimize);
    fluent::ControlState close{};
    close.hovered = hover_ == 1;
    close.pressed = pressed_ == 1;
    painter_.DrawTitleBarButton(chrome.close, fluent::TitleBarButtonRole::Close, {}, close);
    painter_.FillRoundedRect(D2D1::RectF(0, ScaleDip(scale_, kTitleH), width,
                                         ScaleDip(scale_, kTitleH) + 1.0f), 0, theme.stroke_divider);

    const std::wstring src = status_.source_label.empty() ? L"源" : status_.source_label;
    const std::wstring dst = status_.destination_label.empty() ? L"目标" : status_.destination_label;
    const std::wstring subtitle = deleting
        ? L"正在删除 " + src
        : moving ? L"正在从 " + src + L" 移动到 " + dst
                 : L"正在从 " + src + L" 复制到 " + dst;
    painter_.DrawText(subtitle, Rect(scale_, kPadX, 48, dip_w - kPadX - 72.0f, 18),
                      compositor_.SmallFormat(), theme.text);
    wchar_t percent[32];
    if (status_.percent < 0.0f) percent[0] = 0;
    else swprintf_s(percent, L"%.0f%%", std::clamp(status_.percent, 0.0f, 100.0f));
    painter_.DrawText(percent, Rect(scale_, dip_w - kPadX - 56.0f, 46, 56, 20),
                      compositor_.TextFormat(), failed ? theme.danger : sky,
                      fluent::HorizontalAlignment::Right);

    std::wstring file_line;
    std::wstring badge_text;
    fluent::BadgeKind badge_kind = fluent::BadgeKind::Success;
    if (failed) {
        file_line = status_.last_error.empty() ? L"操作失败" : status_.last_error;
        badge_text = L"失败";
        badge_kind = fluent::BadgeKind::Danger;
    } else if (paused) {
        file_line = L"项目：" + (status_.current_item.empty() ? status_.summary : status_.current_item);
        badge_text = L"已暂停";
        badge_kind = fluent::BadgeKind::Warning;
    } else if (waiting) {
        file_line = L"项目：" + (status_.current_item.empty() ? status_.summary : status_.current_item);
        badge_text = L"等待冲突";
        badge_kind = fluent::BadgeKind::Warning;
    } else if (completed) {
        file_line = status_.summary.empty() ? L"全部项目已完成" : status_.summary;
        badge_text = L"已完成";
        badge_kind = fluent::BadgeKind::Success;
    } else if (scanning) {
        file_line = status_.summary.empty() ? L"正在扫描文件…" : status_.summary;
        badge_text = L"准备中";
        badge_kind = fluent::BadgeKind::Neutral;
    } else {
        const std::wstring item = status_.current_item.empty() ? status_.summary : status_.current_item;
        file_line = L"项目：" + item;
        badge_text = deleting ? L"正在删除" : moving ? L"正在移动" : L"正在复制";
        badge_kind = fluent::BadgeKind::Success;
    }
    const float badge_w = badge_text.size() <= 2 ? 48.0f : 78.0f;
    painter_.DrawText(file_line, Rect(scale_, kPadX, 70, dip_w - kPadX - badge_w - 12.0f, 20),
                      compositor_.SmallFormat(), theme.text);
    painter_.DrawBadge({ Rect(scale_, dip_w - kPadX - badge_w, 70, badge_w, 18),
                         badge_text, badge_kind });

    const auto track = Rect(scale_, kPadX, 98, dip_w - kPadX * 2.0f, 10);
    painter_.FillRoundedRect(track, ScaleDip(scale_, 5.0f),
        dark_ ? HexColor(0xFFFFFF, 0.10f) : HexColor(0x000000, 0.10f));
    const float inner = ScaleDip(scale_, 2.0f);
    D2D1_RECT_F fill = D2D1::RectF(track.left + inner, track.top + inner,
                                   track.right - inner, track.bottom - inner);
    const float value = status_.percent < 0.0f ? 0.0f
        : std::clamp(status_.percent / 100.0f, 0.0f, 1.0f);
    fill.right = fill.left + (fill.right - fill.left) * value;
    D2D1_COLOR_F bar = failed ? theme.danger
        : (paused || waiting) ? HexColor(0xF59E0B)
        : HexColor(0x0EA5E9);
    if (fill.right > fill.left) {
        painter_.FillRoundedRect(fill, ScaleDip(scale_, 3.0f), bar);
    }

    const auto stats = Rect(scale_, kPadX, 118, dip_w - kPadX * 2.0f, 52);
    painter_.FillRoundedRect(stats, ScaleDip(scale_, 12.0f),
        dark_ ? HexColor(0xFFFFFF, 0.05f) : HexColor(0x000000, 0.05f));
    const bool byte_transfer = status_.type == ops::OpType::Copy
        || status_.type == ops::OpType::Move;
    const std::wstring time_text = failed ? L"未完成"
        : completed ? L"已完成"
        : paused ? L"已暂停"
        : waiting ? L"等待处理"
        : !byte_transfer ? L"无需估算"
        : status_.eta_seconds == 0 ? L"正在估算"
        : L"约 " + FormatDuration(status_.eta_seconds);
    const uint64_t remain_items = status_.total_items > status_.completed_items
        ? status_.total_items - status_.completed_items : 0;
    const uint64_t remain_bytes = status_.total_bytes > status_.transferred_bytes
        ? status_.total_bytes - status_.transferred_bytes : 0;
    std::wstring items_text;
    if (failed) items_text = L"0 个";
    else if (completed) items_text = L"0 个";
    else if (status_.total_items == 0) items_text = L"正在计算";
    else if (byte_transfer && status_.total_bytes > 0)
        items_text = std::to_wstring(remain_items) + L" 个 ("
            + pulse::format::ByteSize(remain_bytes) + L")";
    else items_text = std::to_wstring(remain_items) + L" 个";
    const std::wstring speed_text = !byte_transfer ? L"无需估算"
        : completed || failed || paused || waiting ? L"0 B/s"
        : status_.bytes_per_second <= 0.0 ? L"正在估算"
        : pulse::format::ByteSize(static_cast<uint64_t>(status_.bytes_per_second)) + L"/s";
    const D2D1_COLOR_F time_color = failed ? theme.danger : theme.text;
    painter_.DrawText(L"剩余时间", Rect(scale_, 32, 124, 120, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(L"剩余项目", Rect(scale_, 176, 124, 140, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(L"当前速度", Rect(scale_, 328, 124, 110, 14),
                      compositor_.SmallFormat(), theme.text_secondary);
    painter_.DrawText(time_text, Rect(scale_, 32, 140, 136, 22),
                      compositor_.SmallFormat(), time_color);
    painter_.DrawText(items_text, Rect(scale_, 176, 140, 148, 22),
                      compositor_.SmallFormat(), theme.text);
    painter_.DrawText(speed_text, Rect(scale_, 328, 140, 112, 22),
                      compositor_.SmallFormat(), sky);

    if (detailed_) {
        const auto card = Rect(scale_, kPadX, 180, dip_w - kPadX * 2.0f, 140);
        painter_.FillRoundedRect(card, ScaleDip(scale_, 12.0f),
            dark_ ? HexColor(0x000000, 0.30f) : HexColor(0x000000, 0.05f));
        painter_.StrokeRoundedRect(card, ScaleDip(scale_, 12.0f), theme.stroke_card);
        painter_.DrawGlyph(L"\xE8F1", Rect(scale_, 30, 188, 14, 14), sky);
        painter_.DrawText(L"传输速度变化历史", Rect(scale_, 48, 186, 220, 18),
                          compositor_.SmallFormat(), theme.text_secondary);
        const std::wstring peak = L"峰值: " + pulse::format::ByteSize(
            static_cast<uint64_t>(status_.peak_bytes_per_second)) + L"/s";
        painter_.DrawText(peak, Rect(scale_, 280, 186, 156, 18),
                          compositor_.SmallFormat(), theme.text_secondary,
                          fluent::HorizontalAlignment::Right);
        const auto plot = Rect(scale_, 30, 208, dip_w - kPadX * 2.0f - 20.0f, 100);
        painter_.FillRoundedRect(plot, ScaleDip(scale_, 8.0f),
            dark_ ? HexColor(0x0F172A, 0.60f) : HexColor(0xFFFFFF, 0.50f));
        DrawSpeedGraph(compositor_.Dc(), plot, speed_history_, sky, scale_);
    }

    painter_.FillRoundedRect(D2D1::RectF(0, height - ScaleDip(scale_, kFooterH), width,
                                         height - ScaleDip(scale_, kFooterH) + 1.0f),
                             0, theme.stroke_divider);
    const auto footer_bg = D2D1::RectF(0, height - ScaleDip(scale_, kFooterH) + 1.0f,
                                       width, height);
    painter_.FillRoundedRect(footer_bg, 0,
        dark_ ? HexColor(0x202022, 0.92f) : HexColor(0x000000, 0.04f));

    fluent::ControlState detail_state{};
    detail_state.hovered = hover_ == 5;
    detail_state.pressed = pressed_ == 5;
    painter_.DrawButton({ chrome.details, detailed_ ? L"简略信息" : L"详细信息",
        detailed_ ? L"\xE70E" : L"\xE70D", fluent::ButtonKind::Transparent, detail_state });
    fluent::ControlState pause_state{};
    pause_state.enabled = status_.active && !failed && status_.phase != ops::OpPhase::Cancelling
        && status_.phase != ops::OpPhase::WaitingForConflict;
    pause_state.hovered = hover_ == 3;
    pause_state.pressed = pressed_ == 3;
    painter_.DrawButton({ chrome.pause, paused ? L"继续" : L"暂停", {},
                          fluent::ButtonKind::Standard, pause_state });
    fluent::ControlState cancel_state{};
    cancel_state.enabled = status_.phase != ops::OpPhase::Cancelling;
    cancel_state.hovered = hover_ == 4;
    cancel_state.pressed = pressed_ == 4;
    painter_.DrawButton({ chrome.cancel, status_.active ? L"取消" : L"关闭", {},
                          fluent::ButtonKind::Standard, cancel_state });
    EndSurface(compositor_);
}

LRESULT CALLBACK FileOperationWindow::WndProc(HWND hwnd, UINT message,
                                               WPARAM wparam, LPARAM lparam) {
    auto* self = reinterpret_cast<FileOperationWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<FileOperationWindow*>(create->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd_ = hwnd;
    }
    return self ? self->HandleMessage(message, wparam, lparam)
                : DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT FileOperationWindow::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(GetDpiForWindow(hwnd_)) / 96.0f;
        ApplyWindowTheme();
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        SetTimer(hwnd_, kRenderTimer, 250, nullptr);
        return 0;
    case WM_NCCALCSIZE:
        return 0;
    case WM_NCHITTEST: {
        const auto chrome = MakeTransferChrome(scale_,
            static_cast<float>(compositor_.Width()), static_cast<float>(compositor_.Height()));
        return BorderlessHitTest(hwnd_, lparam, ScaleDip(scale_, kTitleH),
            D2D1::RectF(chrome.minimize.left, 0, chrome.close.right, chrome.close.bottom));
    }
    case WM_SIZE:
        if (compositor_.Dc() && wparam != SIZE_MINIMIZED)
            compositor_.Resize(LOWORD(lparam), HIWORD(lparam));
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
        return 0;
    }
    case WM_TIMER:
        if (wparam == kRenderTimer && IsWindowVisible(hwnd_)) {
            if (status_.active) {
                const bool running = status_.phase == ops::OpPhase::Running;
                const bool byte_transfer = status_.type == ops::OpType::Copy
                    || status_.type == ops::OpType::Move;
                speed_history_.push_back(running && byte_transfer
                    ? (std::max)(0.0, status_.bytes_per_second) : 0.0);
                while (speed_history_.size() > 120) speed_history_.pop_front();
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEMOVE: {
        const int next = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
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
        pressed_ = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                  static_cast<float>(GET_Y_LPARAM(lparam)));
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const int hit = HitTestControl(static_cast<float>(GET_X_LPARAM(lparam)),
                                       static_cast<float>(GET_Y_LPARAM(lparam)));
        const int pressed = pressed_;
        pressed_ = 0;
        ReleaseCapture();
        if (hit == pressed) {
            if (hit == 1 || hit == 4) {
                RequestClose();
            } else if (hit == 2) ShowWindow(hwnd_, SW_MINIMIZE);
            else if (hit == 3 && status_.active &&
                     status_.phase != ops::OpPhase::WaitingForConflict &&
                     status_.phase != ops::OpPhase::Cancelling) {
                if (status_.phase == ops::OpPhase::Paused) {
                    if (callbacks_.resume) callbacks_.resume();
                } else if (callbacks_.pause) callbacks_.pause();
            } else if (hit == 5) {
                detailed_ = !detailed_;
                ResizeForDetails(true);
            }
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            RequestClose();
            return 0;
        }
        break;
    case WM_CLOSE:
        RequestClose();
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
        KillTimer(hwnd_, kRenderTimer);
        compositor_.Shutdown();
        hwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

ConflictDialogResult ShowFileConflictDialog(HWND owner,
                                            const ops::ConflictItemInfo& conflict,
                                            bool dark,
                                            D2D1_COLOR_F accent) {
    ConflictWindow window;
    return window.Show(owner, conflict, dark, accent);
}

} // namespace pulse::ui
