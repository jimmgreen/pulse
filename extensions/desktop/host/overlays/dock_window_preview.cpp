// dock_window_preview.cpp - see dock_window_preview.h.
#include "host/overlays/dock_window_preview.h"

#include "../../../../src/common/localization.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <iterator>

namespace pulse::desktop::overlays {

namespace {

constexpr wchar_t kClassName[] = L"PulseDockWindowPreview";
constexpr UINT_PTR kTimerWatch = 1;
constexpr size_t kMaxWindows = 12;
// DWM window attributes by value, so older SDK headers build too (unknown ones fail harmlessly).
constexpr DWORD kDwmaUseDarkMode = 20;
constexpr DWORD kDwmaCornerPreference = 33;
constexpr DWORD kDwmaExcludedFromPeek = 12;
constexpr DWORD kDwmCornerRound = 2;

int Px(float v) { return static_cast<int>(std::lround(v)); }

} // namespace

// ---------------------------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------------------------

DockPreviewLayout LayoutDockWindowPreview(const std::vector<SIZE>& source_sizes, const RECT& icon_screen,
                                          const RECT& work_screen, float scale, uint8_t dock_edge) {
    DockPreviewLayout out;
    const size_t n = source_sizes.size();
    if (n == 0) return out;
    const float s = std::clamp(scale, 0.5f, 4.0f);
    const float pad = 8.0f * s, gap = 6.0f * s, cp = 6.0f * s, title_h = 28.0f * s, margin = 8.0f * s;
    const bool vertical = dock_edge == 1 || dock_edge == 2;
    const float avail = vertical ? static_cast<float>(work_screen.bottom - work_screen.top) - 2.0f * margin
                                 : static_cast<float>(work_screen.right - work_screen.left) - 2.0f * margin;
    const float box_w0 = kDockPreviewThumbW * s, box_h0 = kDockPreviewThumbH * s;
    // Length of one card along the row (or column) for a box factor f.
    auto along = [&](float f) { return vertical ? title_h + box_h0 * f + 2.0f * cp : box_w0 * f + 2.0f * cp; };
    auto total = [&](size_t k, float f) {
        return static_cast<float>(k) * along(f) + static_cast<float>(k > 0 ? k - 1 : 0) * gap + 2.0f * pad;
    };
    float f = 1.0f;
    if (total(n, 1.0f) > avail) {
        const float per = (avail - 2.0f * pad + gap) / static_cast<float>(n) - gap;   // room per card
        const float box_room = per - (vertical ? title_h + 2.0f * cp : 2.0f * cp);
        f = std::clamp(box_room / (vertical ? box_h0 : box_w0), kDockPreviewMinFactor, 1.0f);
    }
    size_t shown = n;
    while (shown > 1 && total(shown, f) > avail) --shown;
    out.shown = shown;

    const float box_w = box_w0 * f, box_h = box_h0 * f;
    const float card_w = box_w + 2.0f * cp, card_h = title_h + box_h + 2.0f * cp;
    const float panel_w = vertical ? card_w + 2.0f * pad : total(shown, f);
    const float panel_h = vertical ? total(shown, f) : card_h + 2.0f * pad;
    const float icon_cx = (icon_screen.left + icon_screen.right) * 0.5f;
    const float icon_cy = (icon_screen.top + icon_screen.bottom) * 0.5f;
    const float wl = static_cast<float>(work_screen.left) + margin, wr = static_cast<float>(work_screen.right) - margin;
    const float wt = static_cast<float>(work_screen.top) + margin, wb = static_cast<float>(work_screen.bottom) - margin;
    float x = 0.0f, y = 0.0f;
    if (!vertical) {
        x = std::clamp(icon_cx - panel_w * 0.5f, wl, std::max(wl, wr - panel_w));
        y = std::max(wt, static_cast<float>(icon_screen.top) - 10.0f * s - panel_h);
    } else {
        x = dock_edge == 1 ? static_cast<float>(icon_screen.right) + 10.0f * s
                           : static_cast<float>(icon_screen.left) - 10.0f * s - panel_w;
        y = std::clamp(icon_cy - panel_h * 0.5f, wt, std::max(wt, wb - panel_h));
    }
    out.panel = RECT{Px(x), Px(y), Px(x + panel_w), Px(y + panel_h)};

    for (size_t i = 0; i < shown; ++i) {
        const float step = static_cast<float>(i) * (along(f) + gap);
        const float cl = vertical ? x + pad : x + pad + step;
        const float ct = vertical ? y + pad + step : y + pad;
        const RECT card{Px(cl), Px(ct), Px(cl + card_w), Px(ct + card_h)};
        out.cards.push_back(card);
        const float close_sz = 22.0f * s;
        const RECT close{Px(cl + card_w - cp - close_sz), Px(ct + cp - 1.0f * s), Px(cl + card_w - cp),
                         Px(ct + cp - 1.0f * s + close_sz)};
        out.closes.push_back(close);
        out.titles.push_back(RECT{Px(cl + cp + 22.0f * s), Px(ct + cp), close.left - Px(4.0f * s),
                                  Px(ct + cp + title_h - 8.0f * s)});
        // The thumbnail keeps the window's shape inside its box.
        const SIZE src = source_sizes[i];
        const float aspect = (src.cx > 0 && src.cy > 0) ? static_cast<float>(src.cx) / static_cast<float>(src.cy) : 1.6f;
        float tw = box_w, th = box_w / aspect;
        if (th > box_h) { th = box_h; tw = box_h * aspect; }
        tw = std::max(tw, 8.0f);
        th = std::max(th, 8.0f);
        const float bx = cl + cp + (box_w - tw) * 0.5f;
        const float by = ct + cp + title_h + (box_h - th) * 0.5f;
        out.thumbs.push_back(RECT{Px(bx), Px(by), Px(bx + tw), Px(by + th)});
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Hover timing
// ---------------------------------------------------------------------------------------------

DockPreviewHover::Action DockPreviewHover::Update(std::wstring_view target, bool over_preview, uint64_t now) {
    if (!suppressed_.empty() && target != suppressed_) suppressed_.clear();
    const std::wstring_view t = (!suppressed_.empty() && target == suppressed_) ? std::wstring_view{} : target;
    if (!shown_.empty()) {
        if (!t.empty() && t != shown_) {   // gliding along the Dock swaps the preview at once
            shown_.assign(t);
            candidate_.clear();
            leave_at_ = 0;
            return Action::Show;
        }
        if (t == shown_ || over_preview) {
            leave_at_ = 0;
            return Action::None;
        }
        if (!leave_at_) {
            leave_at_ = now + kLeaveMs;
            return Action::None;
        }
        if (now >= leave_at_) {
            shown_.clear();
            leave_at_ = 0;
            return Action::Close;
        }
        return Action::None;
    }
    if (t.empty()) {
        candidate_.clear();
        enter_at_ = 0;
        return Action::None;
    }
    if (candidate_ != t) {
        candidate_.assign(t);
        enter_at_ = now + kEnterMs;
        return Action::None;
    }
    if (now >= enter_at_) {
        shown_ = candidate_;
        candidate_.clear();
        enter_at_ = 0;
        return Action::Show;
    }
    return Action::None;
}

void DockPreviewHover::Dismissed(std::wstring_view key) {
    shown_.clear();
    candidate_.clear();
    enter_at_ = leave_at_ = 0;
    suppressed_.assign(key);
}

// ---------------------------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------------------------

struct DockWindowPreview::Impl {
    HWND hwnd = nullptr;
    WindowFn activate_fn, close_fn;
    DockWindowPreviewRequest req;
    std::vector<HWND> windows;
    std::vector<HTHUMBNAIL> thumbs;
    std::vector<bool> iconic;
    std::vector<std::wstring> titles;
    DockPreviewLayout layout;
    bool open = false;
    int hover = -1;
    bool hover_close = false;
    bool tracking = false;

    ~Impl() {
        ReleaseThumbnails();
        if (hwnd && IsWindow(hwnd)) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
    }

    bool EnsureWindow() {
        if (hwnd) return true;
        static ATOM atom = 0;
        if (!atom) {
            WNDCLASSEXW wc{sizeof(wc)};
            wc.lpfnWndProc = &Impl::WndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClassName;
            atom = RegisterClassExW(&wc);
            if (!atom && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        }
        hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClassName, L"", WS_POPUP,
                               0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), this);
        if (!hwnd) return false;
        const DWORD corner = kDwmCornerRound;
        DwmSetWindowAttribute(hwnd, kDwmaCornerPreference, &corner, sizeof(corner));
        const BOOL exclude = TRUE;
        DwmSetWindowAttribute(hwnd, kDwmaExcludedFromPeek, &exclude, sizeof(exclude));
        return true;
    }

    void ReleaseThumbnails() {
        for (HTHUMBNAIL t : thumbs)
            if (t) DwmUnregisterThumbnail(t);
        thumbs.clear();
    }

    static SIZE NormalSize(HWND w) {
        WINDOWPLACEMENT wp{sizeof(wp)};
        if (GetWindowPlacement(w, &wp)) {
            return SIZE{wp.rcNormalPosition.right - wp.rcNormalPosition.left,
                        wp.rcNormalPosition.bottom - wp.rcNormalPosition.top};
        }
        return SIZE{0, 0};
    }

    static HICON WindowIcon(HWND w, bool big) {
        DWORD_PTR res = 0;
        if (SendMessageTimeoutW(w, WM_GETICON, big ? ICON_BIG : ICON_SMALL2, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &res) && res)
            return reinterpret_cast<HICON>(res);
        if (!big && SendMessageTimeoutW(w, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 60, &res) && res)
            return reinterpret_cast<HICON>(res);
        HICON h = reinterpret_cast<HICON>(GetClassLongPtrW(w, big ? GCLP_HICON : GCLP_HICONSM));
        if (!h) h = reinterpret_cast<HICON>(GetClassLongPtrW(w, big ? GCLP_HICONSM : GCLP_HICON));
        return h ? h : LoadIconW(nullptr, IDI_APPLICATION);
    }

    static std::wstring Title(HWND w) {
        wchar_t buf[512]{};
        const int n = GetWindowTextW(w, buf, static_cast<int>(std::size(buf)));
        return n > 0 ? std::wstring(buf, static_cast<size_t>(n)) : std::wstring{};
    }

    bool Show(const DockWindowPreviewRequest& r) {
        ReleaseThumbnails();
        req = r;
        if (req.scale < 0.5f) req.scale = 1.0f;
        windows.assign(r.windows.begin(), r.windows.begin() + static_cast<std::ptrdiff_t>(std::min(r.windows.size(), kMaxWindows)));
        hover = -1;
        hover_close = false;
        if (windows.empty()) {
            Close();
            return false;
        }
        std::vector<SIZE> sizes(windows.size(), SIZE{0, 0});
        iconic.assign(windows.size(), false);
        titles.assign(windows.size(), std::wstring{});
        if (!req.headless) {
            if (!EnsureWindow()) return false;
            const BOOL dark = req.dark ? TRUE : FALSE;
            DwmSetWindowAttribute(hwnd, kDwmaUseDarkMode, &dark, sizeof(dark));
            thumbs.assign(windows.size(), nullptr);
            for (size_t i = 0; i < windows.size(); ++i) {
                HWND w = windows[i];
                titles[i] = Title(w);
                iconic[i] = IsIconic(w) != FALSE;
                sizes[i] = NormalSize(w);
                if (iconic[i]) continue;   // a minimised window has nothing live to show; its icon stands in
                HTHUMBNAIL t = nullptr;
                if (SUCCEEDED(DwmRegisterThumbnail(hwnd, w, &t)) && t) {
                    thumbs[i] = t;
                    SIZE src{};
                    if (SUCCEEDED(DwmQueryThumbnailSourceSize(t, &src)) && src.cx > 0 && src.cy > 0) sizes[i] = src;
                }
            }
        }
        layout = LayoutDockWindowPreview(sizes, req.icon_screen, req.work_screen, req.scale, req.dock_edge);
        open = true;
        if (req.headless) return true;
        // Windows that did not fit give their thumbnails back.
        for (size_t i = layout.shown; i < thumbs.size(); ++i) {
            if (thumbs[i]) DwmUnregisterThumbnail(thumbs[i]);
            thumbs[i] = nullptr;
        }
        const RECT& p = layout.panel;
        SetWindowPos(hwnd, HWND_TOPMOST, p.left, p.top, p.right - p.left, p.bottom - p.top,
                     SWP_NOACTIVATE | SWP_SHOWWINDOW);
        for (size_t i = 0; i < layout.shown && i < thumbs.size(); ++i) {
            if (!thumbs[i]) continue;
            DWM_THUMBNAIL_PROPERTIES tp{};
            tp.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_OPACITY | DWM_TNP_SOURCECLIENTAREAONLY;
            const RECT& t = layout.thumbs[i];
            tp.rcDestination = RECT{t.left - p.left, t.top - p.top, t.right - p.left, t.bottom - p.top};
            tp.fVisible = TRUE;
            tp.opacity = 255;
            tp.fSourceClientAreaOnly = FALSE;
            DwmUpdateThumbnailProperties(thumbs[i], &tp);
        }
        InvalidateRect(hwnd, nullptr, FALSE);
        SetTimer(hwnd, kTimerWatch, 600, nullptr);
        return true;
    }

    void Close() noexcept {
        ReleaseThumbnails();
        if (hwnd) {
            KillTimer(hwnd, kTimerWatch);
            ShowWindow(hwnd, SW_HIDE);
        }
        open = false;
        hover = -1;
        hover_close = false;
        tracking = false;
    }

    // Closed windows leave; changed titles repaint.
    void Watch() {
        if (!open) return;
        std::vector<HWND> alive;
        for (HWND w : windows)
            if (IsWindow(w) && IsWindowVisible(w)) alive.push_back(w);
        if (alive.size() != windows.size()) {
            DockWindowPreviewRequest again = req;
            again.windows = alive;
            if (!Show(again)) Close();
            return;
        }
        bool changed = false;
        for (size_t i = 0; i < windows.size(); ++i) {
            const bool now_iconic = IsIconic(windows[i]) != FALSE;
            if (now_iconic != iconic[i]) {   // minimised or restored meanwhile: rebuild its thumbnail
                Show(req);
                return;
            }
            std::wstring t = Title(windows[i]);
            if (t != titles[i]) {
                titles[i] = std::move(t);
                changed = true;
            }
        }
        if (changed) InvalidateRect(hwnd, nullptr, FALSE);
    }

    int HitCard(POINT client, bool* on_close) const {
        const POINT sp{client.x + layout.panel.left, client.y + layout.panel.top};
        for (size_t i = 0; i < layout.shown; ++i) {
            if (!PtInRect(&layout.cards[i], sp)) continue;
            if (on_close) *on_close = PtInRect(&layout.closes[i], sp) != FALSE;
            return static_cast<int>(i);
        }
        if (on_close) *on_close = false;
        return -1;
    }

    void RemoveWindow(size_t i) {
        DockWindowPreviewRequest again = req;
        again.windows = windows;
        again.windows.erase(again.windows.begin() + static_cast<std::ptrdiff_t>(i));
        if (again.windows.empty() || !Show(again)) Close();
    }

    void Paint(HDC dc) {
        const RECT& p = layout.panel;
        const int w = p.right - p.left, h = p.bottom - p.top;
        if (w <= 0 || h <= 0) return;
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, w, h);
        HGDIOBJ old_bmp = SelectObject(mem, bmp);
        const float s = req.scale;
        const COLORREF bg = req.dark ? RGB(0x24, 0x24, 0x26) : RGB(0xF4, 0xF4, 0xF5);
        const COLORREF card_hover = req.dark ? RGB(0x3A, 0x3A, 0x3D) : RGB(0xE2, 0xE3, 0xE6);
        const COLORREF ink = req.dark ? RGB(0xF2, 0xF2, 0xF2) : RGB(0x1A, 0x1A, 0x1C);
        const COLORREF ink2 = req.dark ? RGB(0xA8, 0xA8, 0xAD) : RGB(0x60, 0x60, 0x66);
        const COLORREF box = req.dark ? RGB(0x1A, 0x1A, 0x1C) : RGB(0xE9, 0xE9, 0xEC);
        HBRUSH bg_brush = CreateSolidBrush(bg);
        const RECT all{0, 0, w, h};
        FillRect(mem, &all, bg_brush);
        DeleteObject(bg_brush);

        NONCLIENTMETRICSW ncm{sizeof(ncm)};
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        LOGFONTW lf = ncm.lfMessageFont;
        lf.lfHeight = -Px(12.0f * s);
        lf.lfWeight = FW_NORMAL;
        lf.lfQuality = CLEARTYPE_QUALITY;
        HFONT font = CreateFontIndirectW(&lf);
        HGDIOBJ old_font = SelectObject(mem, font);
        SetBkMode(mem, TRANSPARENT);

        auto local = [&](const RECT& r) { return RECT{r.left - p.left, r.top - p.top, r.right - p.left, r.bottom - p.top}; };
        const int radius = Px(12.0f * s);
        for (size_t i = 0; i < layout.shown && i < windows.size(); ++i) {
            const RECT card = local(layout.cards[i]);
            if (static_cast<int>(i) == hover) {
                HBRUSH hb = CreateSolidBrush(card_hover);
                HGDIOBJ ob = SelectObject(mem, hb);
                HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
                RoundRect(mem, card.left, card.top, card.right, card.bottom, radius, radius);
                SelectObject(mem, op);
                SelectObject(mem, ob);
                DeleteObject(hb);
            }
            const int icon_px = Px(16.0f * s);
            const RECT title = local(layout.titles[i]);
            DrawIconEx(mem, title.left - Px(22.0f * s), (title.top + title.bottom - icon_px) / 2, WindowIcon(windows[i], false),
                       icon_px, icon_px, 0, nullptr, DI_NORMAL);
            RECT tr = title;
            SetTextColor(mem, ink);
            const std::wstring& t = titles[i];
            DrawTextW(mem, t.c_str(), static_cast<int>(t.size()), &tr, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);

            const RECT thumb = local(layout.thumbs[i]);
            if (iconic[i] || !thumbs.size() || !thumbs[i]) {
                // Minimised (or DWM refused): a quiet tile with the app's big icon.
                HBRUSH bb = CreateSolidBrush(box);
                HGDIOBJ ob = SelectObject(mem, bb);
                HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
                RoundRect(mem, thumb.left, thumb.top, thumb.right, thumb.bottom, Px(8.0f * s), Px(8.0f * s));
                SelectObject(mem, op);
                SelectObject(mem, ob);
                DeleteObject(bb);
                const int big = Px(32.0f * s);
                const int cx = (thumb.left + thumb.right) / 2, cy = (thumb.top + thumb.bottom) / 2 - Px(8.0f * s);
                DrawIconEx(mem, cx - big / 2, cy - big / 2, WindowIcon(windows[i], true), big, big, 0, nullptr, DI_NORMAL);
                if (iconic[i]) {
                    RECT mr{thumb.left, cy + big / 2 + Px(4.0f * s), thumb.right, cy + big / 2 + Px(22.0f * s)};
                    SetTextColor(mem, ink2);
                    const std::wstring& m = l10n::Get(l10n::StringId::DockPreviewMinimized);
                    DrawTextW(mem, m.c_str(), static_cast<int>(m.size()), &mr, DT_SINGLELINE | DT_CENTER | DT_VCENTER | DT_NOPREFIX);
                }
            }
            if (static_cast<int>(i) == hover) {   // × only on the card under the pointer, as on the taskbar
                const RECT cr = local(layout.closes[i]);
                if (hover_close) {
                    HBRUSH rb = CreateSolidBrush(RGB(0xC4, 0x2B, 0x1C));
                    HGDIOBJ ob = SelectObject(mem, rb);
                    HGDIOBJ op = SelectObject(mem, GetStockObject(NULL_PEN));
                    RoundRect(mem, cr.left, cr.top, cr.right, cr.bottom, Px(6.0f * s), Px(6.0f * s));
                    SelectObject(mem, op);
                    SelectObject(mem, ob);
                    DeleteObject(rb);
                }
                HPEN pen = CreatePen(PS_SOLID, std::max(1, Px(1.2f * s)), hover_close ? RGB(255, 255, 255) : ink);
                HGDIOBJ op = SelectObject(mem, pen);
                const int cx = (cr.left + cr.right) / 2, cy = (cr.top + cr.bottom) / 2, k = Px(4.5f * s);
                MoveToEx(mem, cx - k, cy - k, nullptr);
                LineTo(mem, cx + k + 1, cy + k + 1);
                MoveToEx(mem, cx + k, cy - k, nullptr);
                LineTo(mem, cx - k - 1, cy + k + 1);
                SelectObject(mem, op);
                DeleteObject(pen);
            }
        }
        BitBlt(dc, 0, 0, w, h, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old_font);
        DeleteObject(font);
        SelectObject(mem, old_bmp);
        DeleteObject(bmp);
        DeleteDC(mem);
    }

    void OnMouse(UINT msg, POINT pt) {
        bool on_close = false;
        const int hit = HitCard(pt, &on_close);
        switch (msg) {
        case WM_MOUSEMOVE:
            if (!tracking) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                tracking = TrackMouseEvent(&tme) != FALSE;
            }
            if (hit != hover || on_close != hover_close) {
                hover = hit;
                hover_close = on_close;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            break;
        case WM_LBUTTONUP:
            if (hit < 0) break;
            if (on_close) {
                const HWND w = windows[static_cast<size_t>(hit)];
                if (close_fn) close_fn(w);
                RemoveWindow(static_cast<size_t>(hit));
            } else {
                const HWND w = windows[static_cast<size_t>(hit)];
                Close();
                if (activate_fn) activate_fn(w);
            }
            break;
        case WM_MBUTTONUP:
            if (hit < 0) break;
            {
                const HWND w = windows[static_cast<size_t>(hit)];
                if (close_fn) close_fn(w);
                RemoveWindow(static_cast<size_t>(hit));
            }
            break;
        default:
            break;
        }
    }

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
        }
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (!self) return DefWindowProcW(h, msg, wp, lp);
        switch (msg) {
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(h, &ps);
            self->Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_TIMER:
            if (wp == kTimerWatch) self->Watch();
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP:
        case WM_MBUTTONUP:
            self->OnMouse(msg, POINT{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))});
            return 0;
        case WM_LBUTTONDOWN:
        case WM_MBUTTONDOWN:
            return 0;
        case WM_MOUSELEAVE:
            self->tracking = false;
            if (self->hover != -1) {
                self->hover = -1;
                self->hover_close = false;
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEACTIVATE:
            return MA_NOACTIVATE;
        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }
};

DockWindowPreview::DockWindowPreview() : impl_(std::make_unique<Impl>()) {}
DockWindowPreview::~DockWindowPreview() = default;
void DockWindowPreview::SetActivateHandler(WindowFn fn) { impl_->activate_fn = std::move(fn); }
void DockWindowPreview::SetCloseHandler(WindowFn fn) { impl_->close_fn = std::move(fn); }
bool DockWindowPreview::Show(const DockWindowPreviewRequest& request) { return impl_->Show(request); }
void DockWindowPreview::Close() noexcept { impl_->Close(); }
bool DockWindowPreview::IsOpen() const noexcept { return impl_->open; }
bool DockWindowPreview::IsOpenFor(std::wstring_view key) const noexcept { return impl_->open && impl_->req.key == key; }
bool DockWindowPreview::ContainsScreenPoint(POINT pt) const noexcept {
    return impl_->open && PtInRect(&impl_->layout.panel, pt) != FALSE;
}
HWND DockWindowPreview::Hwnd() const noexcept { return impl_->hwnd; }
const DockPreviewLayout& DockWindowPreview::Layout() const noexcept { return impl_->layout; }
const std::vector<HWND>& DockWindowPreview::Windows() const noexcept { return impl_->windows; }

} // namespace pulse::desktop::overlays
