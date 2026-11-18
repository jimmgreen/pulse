#include "../common/windows_compat.h"
#include "../common/os_version.h"
#include "window_helpers.h"
#include "surface_capture.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wrl/client.h>

namespace pulse::ui {

namespace {
thread_local const wchar_t* surface_capture_path = nullptr;
thread_local bool surface_capture_saved = false;
}

bool CaptureDialogSurface(const std::wstring& path, const std::function<void()>& render) {
    struct Restore {
        const wchar_t* path = surface_capture_path;
        bool saved = surface_capture_saved;
        ~Restore() { surface_capture_path = path; surface_capture_saved = saved; }
    } restore;
    surface_capture_path = path.c_str();
    surface_capture_saved = false;
    render();
    return surface_capture_saved;
}

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

// ---- Windows 10: corners and shadow of our own ------------------------------
// Windows 10 neither rounds a window nor gives a borderless one a rounded
// shadow. The dialogs clip their card to 8 px corners (BeginSurface, the
// corners stay transparent) and a click-through layered window right behind
// each dialog draws a soft rounded shadow. Windows 11 does both itself.
namespace {

constexpr float kCornerRadiusDip = 8.0f;   // DWMWCP_ROUND
constexpr wchar_t kShadowClass[] = L"PulseDialogShadow";
constexpr UINT_PTR kShadowSubclassId = 0x50534844;  // 'PSHD'

bool ForcedOwnCorners() {
    // Lets the Windows 10 path be checked on Windows 11.
    wchar_t value[4]{};
    return GetEnvironmentVariableW(L"PULSE_TEST_OWN_CORNERS", value, 4) && value[0] == L'1';
}

struct WindowShadow {
    HWND window = nullptr;
    bool dark = false;
    bool suppressed = false;   // until the dialog hides (exit animations)
    int width = 0, height = 0;
    UINT dpi = 0;
    bool painted_dark = false;
};

int ShadowSpread(UINT dpi) { return MulDiv(20, static_cast<int>(dpi), 96); }
int ShadowDrop(UINT dpi) { return MulDiv(4, static_cast<int>(dpi), 96); }

HWND CreateShadowWindow(HWND dialog) {
    static const ATOM atom = [] {
        WNDCLASSEXW cls{ sizeof(cls) };
        cls.lpfnWndProc = DefWindowProcW;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = kShadowClass;
        return RegisterClassExW(&cls);
    }();
    if (!atom) return nullptr;
    // Owned like the dialog, so raising the owner keeps the two together.
    const DWORD ex = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW |
        static_cast<DWORD>(GetWindowLongPtrW(dialog, GWL_EXSTYLE) & WS_EX_TOPMOST);
    return CreateWindowExW(ex, kShadowClass, L"", WS_POPUP, 0, 0, 0, 0,
                           GetWindow(dialog, GW_OWNER), nullptr, GetModuleHandleW(nullptr), nullptr);
}

// A black rounded rectangle blurred outward, premultiplied, placed at `origin`.
void PaintShadow(WindowShadow& shadow, int width, int height, UINT dpi, POINT origin) {
    const int spread = ShadowSpread(dpi);
    const int w = width + 2 * spread, h = height + 2 * spread;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = w;
    info.bmiHeader.biHeight = -h;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    const HDC screen = GetDC(nullptr);
    const HDC memory = CreateCompatibleDC(screen);
    const HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (bitmap && bits && memory) {
        auto* pixels = static_cast<uint32_t*>(bits);
        const float radius = kCornerRadiusDip * static_cast<float>(dpi) / 96.0f;
        const float blur = static_cast<float>(spread);
        const float peak = shadow.dark ? 0.50f : 0.22f;
        const float cx = static_cast<float>(w) * 0.5f;
        const float cy = static_cast<float>(h) * 0.5f + static_cast<float>(ShadowDrop(dpi));
        const float hx = static_cast<float>(width) * 0.5f - radius;
        const float hy = static_cast<float>(height) * 0.5f - radius;
        for (int y = 0; y < h; ++y) {
            const float qy = std::fabs(static_cast<float>(y) + 0.5f - cy) - hy;
            for (int x = 0; x < w; ++x) {
                const float qx = std::fabs(static_cast<float>(x) + 0.5f - cx) - hx;
                const float ox = (std::max)(qx, 0.0f), oy = (std::max)(qy, 0.0f);
                // Signed distance to the card's rounded edge.
                const float d = std::sqrt(ox * ox + oy * oy) + (std::min)((std::max)(qx, qy), 0.0f) - radius;
                float alpha = 0.0f;
                if (d <= 0.0f) alpha = peak;
                else if (d < blur) {
                    const float t = d / blur;
                    alpha = peak * std::exp(-4.0f * t * t) * (1.0f - t);
                }
                pixels[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] =
                    static_cast<uint32_t>(alpha * 255.0f + 0.5f) << 24;
            }
        }
        const HGDIOBJ old = SelectObject(memory, bitmap);
        POINT source{};
        SIZE size{ w, h };
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        UpdateLayeredWindow(shadow.window, screen, &origin, &size, memory, &source, 0, &blend, ULW_ALPHA);
        SelectObject(memory, old);
        shadow.width = width;
        shadow.height = height;
        shadow.dpi = dpi;
        shadow.painted_dark = shadow.dark;
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    ReleaseDC(nullptr, screen);
}

void SyncShadow(HWND dialog, WindowShadow& shadow) {
    const bool show = !shadow.suppressed && IsWindowVisible(dialog) && !IsIconic(dialog) && !IsZoomed(dialog);
    if (!show) {
        if (shadow.window && IsWindowVisible(shadow.window)) ShowWindow(shadow.window, SW_HIDE);
        return;
    }
    if (!shadow.window) shadow.window = CreateShadowWindow(dialog);
    if (!shadow.window) return;
    RECT rect{};
    GetWindowRect(dialog, &rect);
    const UINT dpi = pulse::compat::WindowDpi(dialog);
    const int spread = ShadowSpread(dpi);
    const int width = rect.right - rect.left, height = rect.bottom - rect.top;
    const POINT origin{ rect.left - spread, rect.top - spread };
    if (width != shadow.width || height != shadow.height || dpi != shadow.dpi || shadow.dark != shadow.painted_dark)
        PaintShadow(shadow, width, height, dpi, origin);
    // Right behind the dialog, moved along with it.
    SetWindowPos(shadow.window, dialog, origin.x, origin.y, 0, 0,
                 SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER | SWP_SHOWWINDOW);
}

LRESULT CALLBACK ShadowSubclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                UINT_PTR, DWORD_PTR data) {
    auto* shadow = reinterpret_cast<WindowShadow*>(data);
    if (message == WM_WINDOWPOSCHANGED) {
        const LRESULT result = DefSubclassProc(hwnd, message, wparam, lparam);
        if (reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_HIDEWINDOW) shadow->suppressed = false;
        SyncShadow(hwnd, *shadow);
        return result;
    }
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, ShadowSubclass, kShadowSubclassId);
        if (shadow->window) DestroyWindow(shadow->window);
        delete shadow;
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

WindowShadow* FindShadow(HWND hwnd) {
    DWORD_PTR data = 0;
    return GetWindowSubclass(hwnd, ShadowSubclass, kShadowSubclassId, &data)
        ? reinterpret_cast<WindowShadow*>(data) : nullptr;
}

void AttachWindowShadow(HWND hwnd, bool dark) {
    if (WindowShadow* shadow = FindShadow(hwnd)) {
        shadow->dark = dark;
        SyncShadow(hwnd, *shadow);
        return;
    }
    auto* shadow = new WindowShadow;
    shadow->dark = dark;
    if (!SetWindowSubclass(hwnd, ShadowSubclass, kShadowSubclassId, reinterpret_cast<DWORD_PTR>(shadow))) {
        delete shadow;
        return;
    }
    SyncShadow(hwnd, *shadow);
}

// ---- Acrylic: the Pulse window under the dialog, blurred -------------------
// Same material as menu acrylic, on Windows 10 and 11 alike: the plate is
// built from the frame copy of our window right under the dialog and redone
// when the dialog moves or (at most 4 times a second) when that window has
// presented since. Elsewhere the dialog keeps its plain surface.
constexpr UINT_PTR kAcrylicSubclassId = 0x50414352;  // 'PACR'
constexpr ULONGLONG kPlateRefreshMs = 250;

struct DialogPlate {
    HWND hwnd = nullptr;
    RECT key{};
    bool dark = false;
    const Compositor* source = nullptr;
    unsigned long long serial = 0;
    ULONGLONG tick = 0;
    ComPtr<ID2D1Bitmap1> plate;
};
thread_local std::vector<DialogPlate> t_dialog_plates;

DialogPlate* FindPlate(HWND hwnd) {
    for (auto& entry : t_dialog_plates)
        if (entry.hwnd == hwnd) return &entry;
    return nullptr;
}

void DropPlate(HWND hwnd) {
    t_dialog_plates.erase(std::remove_if(t_dialog_plates.begin(), t_dialog_plates.end(),
                                         [hwnd](const DialogPlate& entry) { return entry.hwnd == hwnd; }),
                          t_dialog_plates.end());
}

LRESULT CALLBACK AcrylicSubclass(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR, DWORD_PTR) {
    if (message == WM_WINDOWPOSCHANGED) {
        // Moved: what lies under the dialog changed.
        if (!(reinterpret_cast<const WINDOWPOS*>(lparam)->flags & SWP_NOMOVE) && FindPlate(hwnd))
            InvalidateRect(hwnd, nullptr, FALSE);
    } else if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, AcrylicSubclass, kAcrylicSubclassId);
        DropPlate(hwnd);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

void TrackAcrylic(HWND hwnd) {
    DWORD_PTR data = 0;
    if (!GetWindowSubclass(hwnd, AcrylicSubclass, kAcrylicSubclassId, &data))
        SetWindowSubclass(hwnd, AcrylicSubclass, kAcrylicSubclassId, 0);
}

// Called inside BeginDraw before any layer is pushed (the build switches
// targets). Null: draw the plain surface.
ID2D1Bitmap1* DialogAcrylicPlate(Compositor& compositor, bool dark, float scale) {
    const HWND hwnd = compositor.Hwnd();
    const auto builder = Compositor::GetAcrylicPlateBuilder();
    if (!hwnd || !builder || !Compositor::FrameCopyEnabled()) {
        if (hwnd) DropPlate(hwnd);
        return nullptr;
    }
    RECT card{};
    GetClientRect(hwnd, &card);
    MapWindowPoints(hwnd, nullptr, reinterpret_cast<POINT*>(&card), 2);
    if (card.right <= card.left || card.bottom <= card.top) return nullptr;
    DialogPlate* entry = FindPlate(hwnd);
    if (!entry) {
        t_dialog_plates.push_back({});
        entry = &t_dialog_plates.back();
        entry->hwnd = hwnd;
    }
    Compositor* source = Compositor::FrameSourceUnder(hwnd, card);
    if (!source || !source->FrameCopy()) {
        entry->plate.reset();
        entry->source = nullptr;
        return nullptr;
    }
    const ULONGLONG now = GetTickCount64();
    const bool stale = !entry->plate.get() || !EqualRect(&entry->key, &card) || entry->dark != dark ||
        entry->source != source ||
        (entry->serial != source->FrameCopySerial() && now - entry->tick >= kPlateRefreshMs);
    if (stale) {
        POINT origin{ 0, 0 };
        ComPtr<ID2D1Bitmap1> plate;
        if (!ClientToScreen(source->Hwnd(), &origin) ||
            !builder(compositor.Dc(), source->FrameCopy(), origin, card, scale, dark, plate)) {
            entry->plate.reset();
            entry->source = nullptr;
            return nullptr;
        }
        entry->plate = std::move(plate);
        entry->key = card;
        entry->dark = dark;
        entry->source = source;
        entry->serial = source->FrameCopySerial();
        entry->tick = now;
    }
    return entry->plate.get();
}

// BeginSurface's clip and the edge drawn over everything at EndSurface.
struct OwnCornerFrame {
    bool layer = false;
    D2D1_ROUNDED_RECT edge{};
    D2D1_COLOR_F color{};
};
thread_local OwnCornerFrame g_own_corner_frame;

// ---- Windows 11: the system acrylic ------------------------------------------
constexpr wchar_t kAcrylicProp[] = L"Pulse.DialogAcrylic";
constexpr UINT_PTR kKeepFrostedId = 0x5046524F;  // 'PFRO'

// Settings > Personalization > Colors > Transparency effects.
bool SystemTransparencyEnabled() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"EnableTransparency", RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) {
        return true;
    }
    return value != 0;
}

// DWM flattens acrylic to a solid colour once the window is inactive; telling
// it the frame is still active keeps the dialog frosted behind other windows.
LRESULT CALLBACK KeepFrostedProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam,
                                 UINT_PTR id, DWORD_PTR) {
    if (message == WM_NCACTIVATE) return DefSubclassProc(hwnd, message, TRUE, lparam);
    if (message == WM_NCDESTROY) {
        RemoveWindowSubclass(hwnd, KeepFrostedProc, id);
        RemovePropW(hwnd, kAcrylicProp);
    }
    return DefSubclassProc(hwnd, message, wparam, lparam);
}

} // namespace

bool DrawsOwnCorners() {
    static const bool own = pulse::os::TrueWindowsBuild() < 22000 || ForcedOwnCorners();
    return own;
}

void SuppressWindowShadow(HWND hwnd) {
    if (WindowShadow* shadow = FindShadow(hwnd)) {
        shadow->suppressed = true;
        SyncShadow(hwnd, *shadow);
    }
}

bool ApplyBackdrop(HWND hwnd, bool dark) {
    UpdateWindowTheme(hwnd, dark);
    if (DrawsOwnCorners()) {
        // Windows 10 has no system acrylic: the dialog draws the Pulse window
        // under it, blurred (DialogAcrylicPlate), inside its own corners.
        TrackAcrylic(hwnd);
        // No DWM frame: its square shadow would show around the rounded card.
        const DWORD corner = DWMWCP_DONOTROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const DWORD none = DWMSBT_NONE;
        DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &none, sizeof(none));
        const DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hwnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        AttachWindowShadow(hwnd, dark);
        return false;
    }
    const DWORD corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    const bool solid = IsHighContrast() || !SystemTransparencyEnabled();
    HRESULT result = E_FAIL;
    DWORD backdrop = DWMSBT_NONE;
    if (solid) {
        result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
    } else {
        // Acrylic like the context menus; Mica Alt / Mica where it is missing.
        for (const DWORD type : {DWORD{DWMSBT_TRANSIENTWINDOW}, DWORD{DWMSBT_TABBEDWINDOW},
                                 DWORD{DWMSBT_MAINWINDOW}}) {
            backdrop = type;
            result = DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
            if (SUCCEEDED(result)) break;
        }
    }
    const bool acrylic = !solid && SUCCEEDED(result) && backdrop == DWMSBT_TRANSIENTWINDOW;
    if (acrylic) {
        SetPropW(hwnd, kAcrylicProp, reinterpret_cast<HANDLE>(1));
        SetWindowSubclass(hwnd, KeepFrostedProc, kKeepFrostedId, 0);
        if (GetForegroundWindow() != hwnd) SendMessageW(hwnd, WM_NCACTIVATE, TRUE, 0);
    } else {
        RemovePropW(hwnd, kAcrylicProp);
        RemoveWindowSubclass(hwnd, KeepFrostedProc, kKeepFrostedId);
    }
    MARGINS margins{ -1 };
    DwmExtendFrameIntoClientArea(hwnd, &margins);
    return !solid && SUCCEEDED(result);
}

bool IsAcrylicBackdrop(HWND hwnd) {
    return hwnd && GetPropW(hwnd, kAcrylicProp) != nullptr;
}

D2D1_COLOR_F GlassCard(bool dark) {
    return dark ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.62f);
}

D2D1_COLOR_F GlassSheet(bool dark) {
    return dark ? D2D1::ColorF(24.0f / 255.0f, 26.0f / 255.0f, 25.0f / 255.0f, 0.55f)
                : D2D1::ColorF(236.0f / 255.0f, 238.0f / 255.0f, 233.0f / 255.0f, 0.38f);
}

void ApplyGlass(Theme& theme, bool dark) {
    theme.surface_card = GlassCard(dark);
    theme.surface_sheet = GlassSheet(dark);
}

std::wstring FitTextEnd(const std::wstring& text, float width,
                       const std::function<float(std::wstring_view)>& measure) {
    if (measure(text) <= width) return text;
    size_t lo = 0, hi = text.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (measure(text.substr(0, mid) + L"\u2026") <= width) lo = mid; else hi = mid - 1;
    }
    if (lo > 0 && IS_HIGH_SURROGATE(text[lo - 1])) --lo;
    return text.substr(0, lo) + L"\u2026";
}

std::wstring FitPathMiddle(const std::wstring& text, float width,
                           const std::function<float(std::wstring_view)>& measure) {
    if (text.empty() || measure(text) <= width) return text;
    std::vector<std::wstring> parts;
    for (size_t start = 0;;) {
        const size_t next = text.find(L'\\', start);
        parts.push_back(text.substr(start, next == std::wstring::npos ? std::wstring::npos
                                                                       : next - start));
        if (next == std::wstring::npos) break;
        start = next + 1;
    }
    if (parts.size() < 3 || parts.back().empty()) return FitTextEnd(text, width, measure);
    // A UNC path keeps its server as the root.
    size_t first = 1;
    std::wstring root = parts[0] + L"\\";
    if (parts[0].empty() && parts.size() > 3 && parts[1].empty()) {
        root = L"\\\\" + parts[2] + L"\\";
        first = 3;
    }
    std::wstring tail = parts.back();
    std::wstring best;
    for (size_t i = parts.size() - 1; i-- > first;) {
        const std::wstring candidate = root + L"\u2026\\" + parts[i] + L"\\" + tail;
        if (measure(candidate) > width) break;
        tail = parts[i] + L"\\" + tail;
        best = candidate;
    }
    if (!best.empty()) return best;
    const std::wstring last = root + L"\u2026\\" + parts.back();
    if (measure(last) <= width) return last;
    return FitTextEnd(parts.back(), width, measure);
}

void RedirectStrayModalKey(MSG& message, HWND dialog) {
    if (message.message < WM_KEYFIRST || message.message > WM_KEYLAST || !dialog) return;
    if (message.hwnd == dialog || IsChild(dialog, message.hwnd)) return;
    // Other enabled windows of this thread keep their typing.
    const HWND root = message.hwnd ? GetAncestor(message.hwnd, GA_ROOT) : nullptr;
    if (!root || IsWindowEnabled(root)) return;
    if (GetForegroundWindow() != dialog) SetForegroundWindow(dialog);
    const HWND focus = GetFocus();
    message.hwnd = (focus == dialog || IsChild(dialog, focus)) ? focus : dialog;
    constexpr LPARAM kAltDown = 1 << 29;
    if (!(message.lParam & kAltDown)) {
        if (message.message == WM_SYSKEYDOWN) message.message = WM_KEYDOWN;
        else if (message.message == WM_SYSKEYUP) message.message = WM_KEYUP;
        else if (message.message == WM_SYSCHAR) message.message = WM_CHAR;
    }
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
    ID2D1Bitmap1* plate = high_contrast || surface_capture_path || !DrawsOwnCorners() ? nullptr : DialogAcrylicPlate(compositor, dark, scale);
    dc->Clear(D2D1::ColorF(0, 0.0f));
    painter.BeginFrame(theme, high_contrast);
    D2D1_COLOR_F tint = theme.bg;
    // Acrylic carries its own frost, so the tint stays thin (效果图 ④); Mica
    // needs more to read as a dialog rather than the wallpaper.
    tint.a = high_contrast || surface_capture_path || !backdrop_enabled ? 1.0f
           : IsAcrylicBackdrop(compositor.Hwnd()) ? 0.42f
           : (dark ? 0.76f : 0.82f);
    const float width = static_cast<float>(compositor.Width());
    const float height = static_cast<float>(compositor.Height());
    const float inset = 0.5f;
    const float radius = kCornerRadiusDip * scale;
    g_own_corner_frame = {};
    if (DrawsOwnCorners()) {
        // Everything drawn this frame is clipped to the rounded card; the
        // edge goes on last so footer bands do not cover it.
        Microsoft::WRL::ComPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> shape;
        if (factory && SUCCEEDED(factory->CreateRoundedRectangleGeometry(
                D2D1::RoundedRect(D2D1::RectF(0, 0, width, height), radius, radius), &shape))) {
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shape.Get()), nullptr);
            g_own_corner_frame.layer = true;
        }
        g_own_corner_frame.edge = D2D1::RoundedRect(
            D2D1::RectF(inset, inset, width - inset, height - inset), radius - inset, radius - inset);
        g_own_corner_frame.color = theme.stroke_card;
        g_own_corner_frame.color.a = (std::max)(g_own_corner_frame.color.a, dark ? 0.16f : 0.14f);
        if (plate) dc->DrawBitmap(plate, D2D1::RectF(0, 0, width, height), 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
        else painter.FillRoundedRect(D2D1::RectF(0, 0, width, height), 0, tint);
        return;
    }
    if (plate) dc->DrawBitmap(plate, D2D1::RectF(0, 0, width, height), 1.0f,
                                   D2D1_BITMAP_INTERPOLATION_MODE_NEAREST_NEIGHBOR);
    else painter.FillRoundedRect(D2D1::RectF(0, 0, width, height), 0, tint);
    // DWM rounds the window by 8 px and draws its own edge: ours follows the
    // same curve, or the two part at the corners.
    painter.StrokeRoundedRect(D2D1::RectF(inset, inset, width - inset, height - inset),
                              radius, theme.stroke_card);
}

void EndSurface(Compositor& compositor) {
    auto* dc = compositor.Dc();
    if (g_own_corner_frame.layer) {
        dc->PopLayer();
        Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
        if (SUCCEEDED(dc->CreateSolidColorBrush(g_own_corner_frame.color, &brush)))
            dc->DrawRoundedRectangle(g_own_corner_frame.edge, brush.Get(), 1.0f);
    }
    g_own_corner_frame = {};
    const HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED ||
        hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        compositor.NotifyDeviceLost(hr);
        return;
    }
    if (surface_capture_path) surface_capture_saved = compositor.SaveSnapshot(surface_capture_path);
    else compositor.Present();
}

} // namespace pulse::ui
