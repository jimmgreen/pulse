// drag_ghost.cpp - see drag_ghost.h.
#include "drag_ghost.h"

#include <objbase.h> // gdiplus.h needs this first
#undef min
#undef max
#include <gdiplus.h>

#include <algorithm>
#include <cmath>
#include <mutex>

#pragma comment(lib, "gdiplus.lib")

namespace pulse::ui {
namespace {

constexpr float kCornerRadius = 6.0f;
constexpr float kIconSize = 16.0f;
constexpr float kPadding = 11.0f;
constexpr float kGap = 8.0f;
constexpr float kCloseWidth = 16.0f;
// The card's window class, published so the drop target search can skip it: the
// card sits under the cursor by design and must never be mistaken for a window.
constexpr wchar_t kGhostClass[] = L"PulseTabDragGhost";

std::once_flag g_register_ghost_once;

// GDI+ has to be started before any of its objects exist, and the UI process
// never had a reason to start it: only Pulse.Preview.exe rasterizes metafiles.
// Without this the card's drawing calls fail silently and the window stays
// transparent.
void EnsureGdiplus() {
    static std::once_flag once;
    std::call_once(once, [] {
        Gdiplus::GdiplusStartupInput input;
        ULONG_PTR token = 0;
        Gdiplus::GdiplusStartup(&token, &input, nullptr);
        // Never shut down: the process owns the ghost until it exits.
    });
}

LRESULT CALLBACK GhostProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void RegisterGhostClassOnce() {
    std::call_once(g_register_ghost_once, [] {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = GhostProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kGhostClass;
        RegisterClassExW(&wc);
    });
}

void AddRoundedRect(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    const float d = radius * 2.0f;
    path.AddArc(rect.X, rect.Y, d, d, 180.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270.0f, 90.0f);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0.0f, 90.0f);
    path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}

// A folder glyph, drawn instead of shelled out for an icon: querying the shell
// on the UI thread is exactly what the drag path must not do.
void DrawFolderGlyph(Gdiplus::Graphics& g, const Gdiplus::RectF& box, bool dark) {
    const float tab_h = box.Height * 0.22f;
    const float tab_w = box.Width * 0.42f;
    Gdiplus::GraphicsPath body;
    AddRoundedRect(body, Gdiplus::RectF(box.X, box.Y + tab_h, box.Width,
                                        box.Height - tab_h),
                   box.Height * 0.10f);
    Gdiplus::Color fill = dark ? Gdiplus::Color(255, 224, 178, 96)
                               : Gdiplus::Color(255, 233, 176, 72);
    Gdiplus::Color edge = dark ? Gdiplus::Color(255, 168, 124, 46)
                               : Gdiplus::Color(255, 186, 134, 40);
    Gdiplus::SolidBrush brush(fill);
    Gdiplus::Pen pen(edge, std::max(1.0f, box.Height * 0.05f));
    Gdiplus::GraphicsPath tab;
    AddRoundedRect(tab, Gdiplus::RectF(box.X, box.Y, tab_w, tab_h * 2.0f),
                   box.Height * 0.06f);
    g.FillPath(&brush, &tab);
    g.FillPath(&brush, &body);
    g.DrawPath(&pen, &body);
}

} // namespace

bool TabDragGhost::EnsureWindow() {
    if (hwnd_ && IsWindow(hwnd_)) return true;
    hwnd_ = nullptr;
    RegisterGhostClassOnce();
    hwnd_ = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT | WS_EX_LAYERED,
        kGhostClass, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
        GetModuleHandleW(nullptr), nullptr);
    return hwnd_ != nullptr;
}

void TabDragGhost::Show(float scale, const std::wstring& title, bool dark,
                        int anchor_x, int anchor_y) {
    if (!EnsureWindow()) return;
    EnsureGdiplus();
    anchor_x_ = std::max(0, anchor_x);
    anchor_y_ = std::max(0, anchor_y);
    const float s = std::clamp(scale, 0.5f, 4.0f);
    const float padding = kPadding * s;
    const float icon = kIconSize * s;
    const float gap = kGap * s;
    const float close_w = kCloseWidth * s;

    Gdiplus::Font font(L"Segoe UI", 12.5f * s, Gdiplus::FontStyleRegular, Gdiplus::UnitPixel);
    Gdiplus::StringFormat format;
    format.SetTrimming(Gdiplus::StringTrimmingEllipsisCharacter);
    format.SetFormatFlags(Gdiplus::StringFormatFlagsNoWrap);
    format.SetLineAlignment(Gdiplus::StringAlignmentCenter);

    // Measure through a throwaway graphics on the screen, then allocate the
    // card exactly as wide as its label needs (within reason).
    HDC measure_dc = GetDC(nullptr);
    float text_w = 90.0f * s;
    if (measure_dc) {
        Gdiplus::Graphics measure(measure_dc);
        Gdiplus::RectF box;
        if (measure.MeasureString(title.c_str(), -1, &font, Gdiplus::PointF(0, 0),
                                  &format, &box) == Gdiplus::Ok &&
            box.Width > 0.0f) {
            text_w = std::clamp(box.Width, 60.0f * s, 260.0f * s);
        }
        ReleaseDC(nullptr, measure_dc);
    }

    width_ = static_cast<int>(std::lround(padding + icon + gap + text_w + gap + close_w + padding));
    height_ = static_cast<int>(std::lround(30.0f * s));
    if (width_ <= 0 || height_ <= 0) return;

    // UpdateLayeredWindow wants premultiplied alpha, and Gdiplus::Bitmap::
    // GetHBITMAP hands back a straight-alpha DIB: draw into a DIB section of our
    // own instead, which GDI+ writes premultiplied as long as it is PARGB.
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width_;
    info.bmiHeader.biHeight = -height_; // top-down, matches GDI+ rows
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dib || !bits) {
        if (dib) DeleteObject(dib);
        return;
    }

    Gdiplus::Bitmap bitmap(width_, height_, width_ * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE*>(bits));
    Gdiplus::Graphics g(&bitmap);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAlias);
    g.Clear(Gdiplus::Color(0, 0, 0, 0));

    const Gdiplus::RectF card(0.5f, 0.5f, static_cast<float>(width_) - 1.0f,
                              static_cast<float>(height_) - 1.0f);
    Gdiplus::GraphicsPath card_path;
    AddRoundedRect(card_path, card, kCornerRadius * s);
    Gdiplus::SolidBrush card_fill(dark ? Gdiplus::Color(255, 44, 44, 44)
                                       : Gdiplus::Color(255, 249, 249, 249));
    Gdiplus::Pen card_edge(dark ? Gdiplus::Color(255, 82, 82, 82)
                                : Gdiplus::Color(255, 205, 205, 205),
                           std::max(1.0f, s));
    g.FillPath(&card_fill, &card_path);
    g.DrawPath(&card_edge, &card_path);

    const float icon_x = padding;
    const float icon_y = (height_ - icon) * 0.5f;
    DrawFolderGlyph(g, Gdiplus::RectF(icon_x, icon_y, icon, icon), dark);

    Gdiplus::SolidBrush text_brush(dark ? Gdiplus::Color(255, 240, 240, 240)
                                        : Gdiplus::Color(255, 26, 26, 26));
    const float text_x = icon_x + icon + gap;
    g.DrawString(title.c_str(), -1, &font,
                 Gdiplus::RectF(text_x, 0.0f, text_w, static_cast<float>(height_)),
                 &format, &text_brush);

    // The close button Explorer draws on its drag image: decoration, the drop is
    // the only thing that acts.
    const float cross_x = static_cast<float>(width_) - padding - close_w * 0.5f;
    const float cross_y = static_cast<float>(height_) * 0.5f;
    const float arm = close_w * 0.22f;
    Gdiplus::Pen cross_pen(dark ? Gdiplus::Color(255, 170, 170, 170)
                                : Gdiplus::Color(255, 120, 120, 120),
                           std::max(1.0f, 1.2f * s));
    g.DrawLine(&cross_pen, cross_x - arm, cross_y - arm, cross_x + arm, cross_y + arm);
    g.DrawLine(&cross_pen, cross_x - arm, cross_y + arm, cross_x + arm, cross_y - arm);

    // Push the painted card to the layered window.
    HDC screen = GetDC(nullptr);
    if (!screen) {
        DeleteObject(dib);
        return;
    }
    HDC mem = CreateCompatibleDC(screen);
    HGDIOBJ old = SelectObject(mem, dib);
    POINT source{0, 0};
    SIZE size{width_, height_};
    POINT destination{0, 0};
    BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
    SetWindowPos(hwnd_, nullptr, 0, 0, width_, height_,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    UpdateLayeredWindow(hwnd_, screen, &destination, &size, mem, &source, 0, &blend,
                        ULW_ALPHA);
    SelectObject(mem, old);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    DeleteObject(dib);
}

void TabDragGhost::Follow(POINT screen_point) {
    if (!hwnd_ || width_ <= 0 || height_ <= 0) return;
    // The card keeps the pointer exactly where it was inside the tab, so the
    // drag reads as picking the tab up rather than as dragging a badge.
    SetWindowPos(hwnd_, nullptr, screen_point.x - anchor_x_,
                 screen_point.y - anchor_y_, width_, height_,
                 SWP_NOZORDER | SWP_NOACTIVATE | (visible_ ? 0u : SWP_SHOWWINDOW));
    visible_ = true;
}

void TabDragGhost::Hide() {
    if (!hwnd_) return;
    if (visible_) ShowWindow(hwnd_, SW_HIDE);
    visible_ = false;
}

} // namespace pulse::ui
