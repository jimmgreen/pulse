#include "floating_shelf_window.h"
#include "typography.h"
#include "ui_motion.h"
#include "window_helpers.h"
#include "../app/drag_shake_watcher.h"
#include "../app/shelf_snippets.h"
#include "../common/localization.h"
#include "../common/text_format.h"
#include <dwmapi.h>
#include <ole2.h>
#include <shellscalingapi.h>
#include <windowsx.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {
template<class T> using ShelfPtr = Microsoft::WRL::ComPtr<T>;
using l10n::StringId;

constexpr float kShelfWidthDip = 272.0f;
constexpr float kCornerRadiusDip = 12.0f;
constexpr float kBandDip = 3.0f;
constexpr float kHeaderDip = 38.0f;
constexpr UINT_PTR kAnimTimer = 1;
constexpr UINT_PTR kAutoHideTimer = 2;
constexpr UINT_PTR kDropReadyTimer = 3;
constexpr UINT_PTR kSwitchMenuTimer = 4;
constexpr int kEditId = 401;

D2D1_COLOR_F Rgba(uint32_t rgb, float a = 1.0f) { return D2D1::ColorF(rgb, a); }
D2D1_COLOR_F AlphaOf(D2D1_COLOR_F c, float a) { c.a = a; return c; }

bool PtIn(const D2D1_RECT_F& r, float x, float y) noexcept {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

std::wstring FillOne(std::wstring s, const wchar_t* key, const std::wstring& val) {
    if (const size_t at = s.find(key); at != std::wstring::npos)
        s.replace(at, wcslen(key), val);
    return s;
}

float ScaleForPoint(POINT pt) noexcept {
    if (HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST)) {
        UINT dx = 96, dy = 96;
        if (SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) && dx > 0)
            return static_cast<float>(dx) / 96.0f;
    }
    return 1.0f;
}

RECT WorkAreaForPoint(POINT pt) noexcept {
    MONITORINFO mi{sizeof(mi)};
    if (HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
        mon && GetMonitorInfoW(mon, &mi)) {
        return mi.rcWork;
    }
    RECT rc{0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &rc, 0);
    return rc;
}

float MeasureOneLine(IDWriteFactory2* dw, IDWriteTextFormat* fmt, const std::wstring& text) {
    if (!dw || !fmt || text.empty()) return 0.0f;
    ShelfPtr<IDWriteTextLayout> layout;
    if (FAILED(dw->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                    fmt, 4000.0f, 200.0f, &layout)) || !layout)
        return 0.0f;
    layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TEXT_METRICS m{};
    return SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
}

void DrawTextBox(Compositor* comp, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* br,
                 const std::wstring& text, const D2D1_RECT_F& rc,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING,
                 bool two_lines = false) {
    const float w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (!comp || !comp->Dc() || !fmt || !br || text.empty() || w <= 1.0f || h <= 1.0f) return;
    IDWriteFactory2* factory = comp->DwriteFactory();
    if (!factory) return;
    if (!two_lines) {
        auto fg = br->GetColor();
        fg.a *= br->GetOpacity();
        if (!IsHighContrast() && comp->DrawLumaText(text, fmt, rc, fg, D2D1::ColorF(0, 0.0f), align))
            return;
    }
    ShelfPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()),
                                         fmt, w, h, &layout)) || !layout)
        return;
    layout->SetWordWrapping(two_lines ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    layout->SetTextAlignment(align);
    layout->SetParagraphAlignment(two_lines ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                            : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    DWRITE_TRIMMING trimming{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
    ShelfPtr<IDWriteInlineObject> ellipsis;
    factory->CreateEllipsisTrimmingSign(layout.Get(), &ellipsis);
    layout->SetTrimming(&trimming, ellipsis.Get());
    comp->DrawTextLayout(D2D1::Point2F(rc.left, rc.top), layout.Get(), br, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

enum class HitKind : int {
    None = 0,
    HeadDrag,
    ShelfSwitch,
    Pin,
    More,
    Dock,
    Pile,
    Expand,
    StaleRemove,
    ReleaseGo,
    ReleaseMenu,
    SendChip,
    SendPick,
    FilterChip,
    ViewToggle,
    Collapse,
    ItemCard,
    ItemRemove,
    GroupOpen,
    ClearCheck,
    ComparePair,
};

struct HitZone {
    D2D1_RECT_F rc{};
    HitKind kind = HitKind::None;
    int sub = -1;
    uint64_t id = 0;
    std::wstring path;
};

} // namespace

struct FloatingShelfWindow::Impl {
    class ShelfOleDropTarget final : public IDropTarget {
    public:
        explicit ShelfOleDropTarget(Impl* owner) : owner_(owner) {}
        IFACEMETHODIMP QueryInterface(REFIID riid, void** out) override {
            if (!out) return E_POINTER;
            if (riid == IID_IUnknown || riid == IID_IDropTarget) {
                *out = static_cast<IDropTarget*>(this);
                AddRef();
                return S_OK;
            }
            *out = nullptr;
            return E_NOINTERFACE;
        }
        IFACEMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
        IFACEMETHODIMP_(ULONG) Release() override {
            const ULONG r = --ref_;
            if (r == 0) delete this;
            return r;
        }
        IFACEMETHODIMP DragEnter(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) override {
            if (!effect) return E_POINTER;
            source_effect_ = StagingEffect(*effect);
            *effect = DROPEFFECT_NONE;
            if (!owner_) return S_OK;
            const auto kind = source_effect_ ? app::ProbeShelfDropPayload(obj) : app::ShelfDropPayloadKind::None;
            if (kind == app::ShelfDropPayloadKind::None || !owner_->cb.accept_drop) {
                owner_->OnOleDragLeave();
                return S_OK;
            }
            if (owner_->OnOleDragEnter(kind == app::ShelfDropPayloadKind::Content, key_state, pt))
                *effect = source_effect_;
            return S_OK;
        }
        IFACEMETHODIMP DragOver(DWORD key_state, POINTL pt, DWORD* effect) override {
            if (!effect) return E_POINTER;
            *effect = owner_ && owner_->ole_dropping &&
                owner_->OnOleDragOver(key_state, pt) ? source_effect_ : DROPEFFECT_NONE;
            return S_OK;
        }
        IFACEMETHODIMP DragLeave() override {
            source_effect_ = DROPEFFECT_NONE;
            if (owner_) owner_->OnOleDragLeave();
            return S_OK;
        }
        IFACEMETHODIMP Drop(IDataObject* obj, DWORD key_state, POINTL pt, DWORD* effect) override {
            if (!effect) return E_POINTER;
            const DWORD accepted_effect = source_effect_;
            source_effect_ = DROPEFFECT_NONE;
            *effect = DROPEFFECT_NONE;
            if (owner_ && owner_->ole_dropping &&
                accepted_effect && owner_->OnOleDrop(obj, key_state, pt))
                *effect = accepted_effect;
            return S_OK;
        }
        void Detach() noexcept { owner_ = nullptr; }
    private:
        // A staged move records intent; it must never tell the drag source to delete.
        static DWORD StagingEffect(DWORD allowed) noexcept {
            if (allowed & DROPEFFECT_COPY) return DROPEFFECT_COPY;
            if (allowed & DROPEFFECT_LINK) return DROPEFFECT_LINK;
            return DROPEFFECT_NONE;
        }
        LONG ref_ = 1;
        Impl* owner_ = nullptr;
        DWORD source_effect_ = DROPEFFECT_NONE;
    };

    FloatingShelfCallbacks cb;
    HWND hwnd = nullptr;
    HWND edit = nullptr;
    WNDPROC orig_edit_proc = nullptr;
    HFONT edit_font = nullptr;
    HBRUSH edit_brush = nullptr;
    ShelfOleDropTarget* drop_target = nullptr;
    bool ole_initialized = false;

    Compositor compositor;
    ThumbnailCache thumbs{8ull * 1024ull * 1024ull, 64};
    ShellIconCache icons;
    ShelfPtr<ID2D1SolidColorBrush> brush;

    // Cached text formats (rebuilt when `fmt_scale != scale`).
    float fmt_scale = 0.0f;
    ShelfPtr<IDWriteTextFormat> fmt_title;    // 12.5 semibold
    ShelfPtr<IDWriteTextFormat> fmt_btn;      // 12.5 bold
    ShelfPtr<IDWriteTextFormat> fmt_count;    // 15 semibold
    ShelfPtr<IDWriteTextFormat> fmt_row;      // 12 normal
    ShelfPtr<IDWriteTextFormat> fmt_sub;      // 11.5 normal
    ShelfPtr<IDWriteTextFormat> fmt_sub_bold; // 11.5 semibold
    ShelfPtr<IDWriteTextFormat> fmt_grid;     // 11 normal (2-line)
    ShelfPtr<IDWriteTextFormat> fmt_tag;      // 10.5 normal
    ShelfPtr<IDWriteTextFormat> fmt_badge;    // 10.5 bold
    ShelfPtr<IDWriteTextFormat> fmt_mv;       // 9.5 bold

    bool dark = false;
    WindowEffect effect = WindowEffect::Acrylic;
    D2D1_COLOR_F accent{0.0f, 0.404f, 0.753f, 1.0f};
    bool backdrop = false;
    float scale = 1.0f;

    bool visible = false;
    bool expanded = false;
    bool view_grid = true;
    bool renaming = false;
    bool tracking_mouse = false;
    bool drop_ready = false;
    bool ole_dropping = false;
    bool drop_from_shelf = false;
    bool drop_is_content = false;
    int drop_target_zone = -1;
    POINTL drop_screen{};
    DWORD drop_keys = 0;
    float min_drop_h_dip = 0.0f;
    bool auto_hide_pending = false;
    POINT switch_menu_anchor{};

    // Window move drag state.
    bool moving_window = false;
    POINT move_offset{0, 0};
    HitZone pressed;
    POINT press_screen{};

    // Scroll & hover state.
    float items_scroll_dip = 0.0f;
    float items_content_dip = 0.0f;
    HitKind hover_kind = HitKind::None;
    int hover_sub = -1;
    uint64_t hover_id = 0;
    float fan_spread = 0.0f;
    ULONGLONG pop_start_ms = 0;

    TrayDeckView cached_view{};
    std::vector<HitZone> hits;

    ~Impl() { Shutdown(); }

    void EnsureFormats() {
        if (!compositor.DwriteFactory() || (fmt_scale == scale && fmt_title)) return;
        fmt_scale = scale;
        auto make = [&](float pt, DWRITE_FONT_WEIGHT w, ShelfPtr<IDWriteTextFormat>& out, bool wrap = false) {
            out.Reset();
            typography::CreateTextFormat(compositor.DwriteFactory(),
                {typography::FontRole::Text, pt * scale, w}, out.GetAddressOf());
            if (out) {
                out->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_CHARACTER : DWRITE_WORD_WRAPPING_NO_WRAP);
                out->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR
                                                : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            }
        };
        make(12.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmt_title);
        make(12.5f, DWRITE_FONT_WEIGHT_BOLD, fmt_btn);
        make(15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmt_count);
        make(12.0f, DWRITE_FONT_WEIGHT_NORMAL, fmt_row);
        make(11.5f, DWRITE_FONT_WEIGHT_NORMAL, fmt_sub);
        make(11.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, fmt_sub_bold);
        make(11.0f, DWRITE_FONT_WEIGHT_NORMAL, fmt_grid, true);
        make(10.5f, DWRITE_FONT_WEIGHT_NORMAL, fmt_tag);
        make(10.5f, DWRITE_FONT_WEIGHT_BOLD, fmt_badge);
        make(9.5f, DWRITE_FONT_WEIGHT_BOLD, fmt_mv);
    }

    bool EnsureWindow() {
        if (hwnd && IsWindow(hwnd)) return true;
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = StaticWndProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"PulseFloatingShelfWindow";
        RegisterClassExW(&wc);

        hwnd = CreateWindowExW(
            WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOREDIRECTIONBITMAP,
            wc.lpszClassName, L"Pulse Shelf", WS_POPUP,
            0, 0, 272, 180, nullptr, nullptr, wc.hInstance, this);
        if (!hwnd) return false;

        ole_initialized = SUCCEEDED(OleInitialize(nullptr));
        if (ole_initialized) {
            drop_target = new ShelfOleDropTarget(this);
            if (FAILED(RegisterDragDrop(hwnd, drop_target))) {
                drop_target->Release();
                drop_target = nullptr;
            }
        }

        ApplyDwmChrome();
        return true;
    }

    void ApplyDwmChrome() {
        if (!hwnd) return;
        BOOL is_dark = dark ? TRUE : FALSE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &is_dark, sizeof(is_dark));
        const int corner = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
        const BOOL no_trans = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &no_trans, sizeof(no_trans));
        const WindowEffect shelf_effect = effect == WindowEffect::None || !SystemAllowsPopupAcrylic()
            ? WindowEffect::None : WindowEffect::Acrylic;
        backdrop = ApplyWindowEffect(hwnd, shelf_effect, dark);
        if (edit_brush) { DeleteObject(edit_brush); edit_brush = nullptr; }
        edit_brush = CreateSolidBrush(dark ? RGB(36, 40, 50) : RGB(255, 255, 255));
    }

    bool EnsureGraphics(int w_px, int h_px) {
        if (!hwnd) return false;
        if (compositor.NeedsRecovery()) {
            brush.Reset();
            icons.Reset();
            thumbs.Reset();
            if (!compositor.Recover()) return false;
        } else if (!compositor.Dc()) {
            if (!compositor.Init(hwnd)) return false;
        }
        if (compositor.Width() != w_px || compositor.Height() != h_px) {
            compositor.Resize(w_px, h_px);
        }
        ID2D1DeviceContext* dc = compositor.Dc();
        if (!dc) return false;
        dc->SetDpi(96.0f, 96.0f); // We work in physical pixels scaled explicitly by `scale`.
        icons.SetDeviceContext(dc);
        icons.SetScale(scale);
        icons.SetNotifyWindow(hwnd);
        thumbs.SetDeviceContext(dc);
        thumbs.SetNotifyWindow(hwnd);
        EnsureFormats();
        if (!brush) dc->CreateSolidColorBrush(Rgba(0xFFFFFF), &brush);
        return brush != nullptr;
    }

    static float FilterBarHeightDip(const TrayDeckView& d) {
        int filters = 1;
        for (int f = 1; f < 5; ++f) {
            if (d.filter_counts[f] > 0 || d.filter == f) ++filters;
        }
        return filters >= 4 ? 53.0f : 28.0f;
    }

    float ComputeTargetHeightDip(const TrayDeckView& d) {
        const float top_chrome = kBandDip + kHeaderDip; // 41 DIP
        if (ole_dropping || drop_ready) {
            return std::max(min_drop_h_dip, top_chrome + 162.0f);
        }
        if (d.total_count <= 0) {
            return top_chrome + 128.0f; // Empty dashed box
        }
        const float note_extra = d.snippet_note.empty() ? 0.0f : 18.0f;
        if (!expanded) {
            // Pile (66) + Release (32 + note) + Send chips (40)
            return top_chrome + 66.0f + 32.0f + note_extra + 42.0f;
        }
        // Expanded: bar (28) + items (up to 232) + sel (28) + release (32 + note) + bottom pad (10)
        float items_h = 44.0f;
        if (view_grid) {
            const int rows = std::max(1, (static_cast<int>(d.open_rows.size()) + 2) / 3);
            items_h = static_cast<float>(rows) * 86.0f + static_cast<float>(std::max(0, rows - 1)) * 6.0f + 6.0f;
        } else {
            int groups = 0;
            for (const auto& r : d.open_rows) if (r.group_start) ++groups;
            items_h = static_cast<float>(groups) * 26.0f + static_cast<float>(d.open_rows.size()) * 30.0f + 6.0f;
        }
        items_content_dip = items_h;
        return top_chrome + FilterBarHeightDip(d) + std::clamp(items_h, 64.0f, 232.0f) +
            30.0f + 32.0f + note_extra + 10.0f;
    }

    void BuildView() {
        if (cb.build_view) {
            TrayDeckView view = cb.build_view(expanded);
            if (view.active_shelf_id != cached_view.active_shelf_id || view.filter != cached_view.filter)
                items_scroll_dip = 0.0f;
            cached_view = std::move(view);
            cached_view.view_grid = view_grid;
        }
    }

    void SyncSizeAndRender(bool clamp_to_workarea = true) {
        if (!hwnd || !visible) return;
        BuildView();
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        POINT monitor_point{wr.left + (wr.right - wr.left) / 2, wr.top + 20};
        if (moving_window) GetCursorPos(&monitor_point);
        scale = ScaleForPoint(monitor_point);
        const RECT wa = WorkAreaForPoint(monitor_point);
        const float h_dip = ComputeTargetHeightDip(cached_view);
        const int w_px = std::clamp(static_cast<int>(std::lround(kShelfWidthDip * scale)),
                                    1, std::max(1, static_cast<int>(wa.right - wa.left) - 12));
        const int h_px = std::clamp(static_cast<int>(std::lround(h_dip * scale)),
                                    1, std::max(1, static_cast<int>(wa.bottom - wa.top) - 12));
        int nx = wr.left, ny = wr.top;
        if (clamp_to_workarea && !moving_window) {
            nx = std::clamp(nx, static_cast<int>(wa.left + 6), std::max(static_cast<int>(wa.left + 6), static_cast<int>(wa.right - w_px - 6)));
            ny = std::clamp(ny, static_cast<int>(wa.top + 6), std::max(static_cast<int>(wa.top + 6), static_cast<int>(wa.bottom - h_px - 6)));
        }
        if (nx != wr.left || ny != wr.top || (wr.right - wr.left) != w_px || (wr.bottom - wr.top) != h_px) {
            SetWindowPos(hwnd, HWND_TOPMOST, nx, ny, w_px, h_px, SWP_NOACTIVATE);
        }
        RenderFrame();
    }

    void RenderFrame() {
        if (!hwnd || !visible) return;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const int w_px = cr.right - cr.left, h_px = cr.bottom - cr.top;
        if (w_px <= 0 || h_px <= 0 || !EnsureGraphics(w_px, h_px)) return;

        ID2D1DeviceContext* dc = compositor.Dc();
        const float s = scale;
        const float W = static_cast<float>(w_px), H = static_cast<float>(h_px);
        const Theme th = MakeTheme(dark, accent);
        const ShelfColors p = MakeShelfColors(dark, IsHighContrast(), th);
        auto B = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); return brush.Get(); };

        TrayDeckView& d = cached_view;
        d.fan_spread = fan_spread;
        d.drop_content = drop_is_content;
        d.drop_hover_target = drop_target_zone;
        hits.clear();
        auto add_hit = [&](const D2D1_RECT_F& r, HitKind k, int sub = -1, uint64_t id = 0, std::wstring path = {}) {
            if (r.right > r.left && r.bottom > r.top)
                hits.push_back({r, k, sub, id, std::move(path)});
        };
        auto is_hot = [&](HitKind k, int sub = -1, uint64_t id = 0) {
            if (hover_kind != k) return false;
            if (id != 0) return hover_id == id;
            if (sub >= 0) return hover_sub == sub;
            return true;
        };

        dc->BeginDraw();
        dc->Clear(D2D1::ColorF(0, 0.0f));

        // Strict rounded-corner clip around the ENTIRE shell (top 3px color band,
        // hover fills, scroll area) so nothing can ever bleed past the 12 DIP radius!
        const float rad = kCornerRadiusDip * s;
        const D2D1_RECT_F shell_rc = D2D1::RectF(0.0f, 0.0f, W, H);
        ShelfPtr<ID2D1Factory> factory;
        dc->GetFactory(&factory);
        ShelfPtr<ID2D1RoundedRectangleGeometry> shell_clip;
        if (factory) {
            factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(shell_rc, rad, rad), &shell_clip);
        }

        const ULONGLONG now = motion::NowMs();
        const float pop_t = pop_start_ms ? std::clamp(static_cast<float>(now - pop_start_ms) / 220.0f, 0.0f, 1.0f) : 1.0f;
        const float pop_alpha = pop_t < 1.0f ? (1.0f - (1.0f - pop_t) * (1.0f - pop_t)) : 1.0f;
        if (shell_clip) {
            dc->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), shell_clip.Get(),
                D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), pop_alpha), nullptr);
        }

        // Surface tint (translucent over DWM Acrylic when available)
        D2D1_COLOR_F surf = p.shelf_bg;
        if (!IsHighContrast() && backdrop && compositor.UsesTransparentComposition()) {
            surf.a = dark ? 0.72f : 0.76f;
        } else {
            surf = p.shelf_solid;
        }
        dc->FillRectangle(shell_rc, B(surf));

        // 1. Top 3 DIP color band (.fs-band), cleanly clipped to rounded top corners!
        dc->FillRectangle(D2D1::RectF(0.0f, 0.0f, W, kBandDip * s), B(Rgba(d.active_shelf_color)));

        // 2. Header (38 DIP)
        const float head_top = kBandDip * s;
        const float head_bot = head_top + kHeaderDip * s;
        const float head_cy = (head_top + head_bot) * 0.5f;
        add_hit(D2D1::RectF(0.0f, 0.0f, W, head_bot), HitKind::HeadDrag);

        const float ib = 24.0f * s;
        const D2D1_RECT_F btn_dock = D2D1::RectF(W - 6.0f * s - ib, head_cy - ib * 0.5f, W - 6.0f * s, head_cy + ib * 0.5f);
        const D2D1_RECT_F btn_more = D2D1::RectF(btn_dock.left - 2.0f * s - ib, btn_dock.top, btn_dock.left - 2.0f * s, btn_dock.bottom);
        const D2D1_RECT_F btn_pin  = D2D1::RectF(btn_more.left - 2.0f * s - ib, btn_more.top, btn_more.left - 2.0f * s, btn_more.bottom);

        auto draw_ib = [&](const D2D1_RECT_F& r, ShelfGlyph g, bool hot, bool active_on) {
            if (active_on) dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * s, 6.0f * s), B(p.sel));
            else if (hot) dc->FillRoundedRectangle(D2D1::RoundedRect(r, 6.0f * s, 6.0f * s), B(p.ib_hot));
            DrawShelfGlyph(dc, B(active_on ? p.acc2 : (hot ? p.strong : p.sub)), g, r, 14.5f * s);
        };
        draw_ib(btn_pin, ShelfGlyph::Pin, is_hot(HitKind::Pin), d.pinned);
        draw_ib(btn_more, ShelfGlyph::More, is_hot(HitKind::More), false);
        draw_ib(btn_dock, ShelfGlyph::Dock, is_hot(HitKind::Dock), false);
        add_hit(btn_pin, HitKind::Pin);
        add_hit(btn_more, HitKind::More);
        add_hit(btn_dock, HitKind::Dock);

        const float tag_limit = btn_pin.left - 4.0f * s;
        const float tag_reserve = d.active_shelf_temp
            ? MeasureOneLine(compositor.DwriteFactory(), fmt_tag.Get(), l10n::Get(StringId::ShelfClearedOnExit)) + 14.0f * s
            : 0.0f;
        const D2D1_RECT_F switch_btn_rc = MeasureShelfSwitchButton(d, 6.0f * s, head_cy,
            std::min(146.0f * s, tag_limit - 6.0f * s - tag_reserve), s,
            [&](const std::wstring& t) { return MeasureOneLine(compositor.DwriteFactory(), fmt_title.Get(), t); });
        if (!renaming) {
            DrawShelfSwitchButton(dc, &compositor, brush.Get(), fmt_title.Get(), fmt_tag.Get(),
                                  d, p, switch_btn_rc, s, is_hot(HitKind::ShelfSwitch),
                                  true, tag_limit);
            add_hit(switch_btn_rc, HitKind::ShelfSwitch);
        }

        // 3. Body
        const D2D1_RECT_F body_rc = D2D1::RectF(0.0f, head_bot, W, H);
        if (ole_dropping || drop_ready) {
            const ShelfDropGeom dg = LayoutShelfDropLayer(d, body_rc, s,
                [&](const std::wstring& t) { return MeasureOneLine(compositor.DwriteFactory(), fmt_row.Get(), t); });
            drop_target_zone = HitDropLayer(dg, drop_keys, drop_screen);
            d.drop_hover_target = drop_target_zone;
            DrawShelfDropLayer(dc, &compositor, brush.Get(), fmt_title.Get(), fmt_sub.Get(),
                               fmt_row.Get(), d, p, dg, s, dark);
        } else if (d.total_count <= 0) {
            // Empty dashed card (.fs-empty)
            const D2D1_RECT_F box = D2D1::RectF(10.0f * s, head_bot + 2.0f * s, W - 10.0f * s, H - 10.0f * s);
            ShelfPtr<ID2D1StrokeStyle> dash;
            if (factory) {
                const float dashes[] = {3.0f, 2.5f};
                factory->CreateStrokeStyle(D2D1::StrokeStyleProperties(
                    D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_ROUND,
                    D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_CUSTOM, 0.0f), dashes, 2, &dash);
            }
            dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 10.0f * s, 10.0f * s),
                                     B(p.half_edge), 1.5f * s, dash.Get());
            const float cx = (box.left + box.right) * 0.5f;
            const float cy = (box.top + box.bottom) * 0.5f;
            DrawShelfGlyph(dc, B(p.acc2), ShelfGlyph::Tray,
                           D2D1::RectF(cx - 11.0f * s, cy - 30.0f * s, cx + 11.0f * s, cy - 8.0f * s), 22.0f * s);
            DrawTextBox(&compositor, fmt_title.Get(), B(p.strong), l10n::Get(StringId::ShelfEmptyTitle),
                        D2D1::RectF(box.left + 8.0f * s, cy - 4.0f * s, box.right - 8.0f * s, cy + 15.0f * s),
                        DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawTextBox(&compositor, fmt_sub.Get(), B(p.faint), l10n::Get(StringId::ShelfEmptySub),
                        D2D1::RectF(box.left + 8.0f * s, cy + 16.0f * s, box.right - 8.0f * s, cy + 52.0f * s),
                        DWRITE_TEXT_ALIGNMENT_CENTER, true);
        } else if (!expanded) {
            // Collapsed Pile + Meta + Expand + Release + Send chips
            const float pile_y = head_bot + 2.0f * s;
            const D2D1_RECT_F pile_rc = D2D1::RectF(12.0f * s, pile_y, 12.0f * s + 66.0f * s, pile_y + 58.0f * s);
            const D2D1_RECT_F exp_rc  = D2D1::RectF(W - 8.0f * s - 28.0f * s, pile_y + 15.0f * s,
                                                    W - 8.0f * s, pile_y + 43.0f * s);
            const D2D1_RECT_F meta_rc = D2D1::RectF(pile_rc.right + 10.0f * s, pile_y + 4.0f * s,
                                                    exp_rc.left - 4.0f * s, pile_y + 56.0f * s);
            DrawShelfPile(dc, &compositor, fmt_badge.Get(), fmt_badge.Get(), &thumbs, &icons,
                          d, p, p.shelf_solid, pile_rc, s);
            add_hit(pile_rc, HitKind::Pile);

            D2D1_RECT_F stale_rm{};
            DrawShelfMeta(&compositor, brush.Get(), fmt_count.Get(), fmt_row.Get(),
                          fmt_sub.Get(), fmt_sub_bold.Get(), d, p, meta_rc, s,
                          is_hot(HitKind::StaleRemove), &stale_rm);
            if (stale_rm.right > stale_rm.left) add_hit(stale_rm, HitKind::StaleRemove);

            if (is_hot(HitKind::Expand))
                dc->FillRoundedRectangle(D2D1::RoundedRect(exp_rc, 7.0f * s, 7.0f * s), B(p.ib_hot));
            DrawShelfGlyph(dc, B(is_hot(HitKind::Expand) ? p.strong : p.sub), ShelfGlyph::Up, exp_rc, 14.0f * s);
            add_hit(exp_rc, HitKind::Expand);

            const float rel_y = pile_y + 64.0f * s;
            const D2D1_RECT_F sp = D2D1::RectF(10.0f * s, rel_y, W - 10.0f * s, rel_y + 32.0f * s);
            const D2D1_RECT_F dd = D2D1::RectF(sp.right - 30.0f * s, sp.top, sp.right, sp.bottom);
            const D2D1_RECT_F go = D2D1::RectF(sp.left, sp.top, dd.left, sp.bottom);
            float after_rel = sp.bottom;
            DrawShelfReleaseButton(dc, &compositor, brush.Get(), fmt_btn.Get(), fmt_badge.Get(),
                                   fmt_tag.Get(), d, p, p.shelf_solid, sp, go, dd, s, false,
                                   is_hot(HitKind::ReleaseGo), is_hot(HitKind::ReleaseMenu), &after_rel);
            if (!d.target.empty() && d.release_copy_count + d.release_move_count > 0)
                add_hit(go, HitKind::ReleaseGo);
            add_hit(dd, HitKind::ReleaseMenu);

            // "送到 [📁 文档] [📁 桌面] [＋]"
            const float sy = after_rel + 8.0f * s;
            const std::wstring send_lbl = l10n::Get(StringId::TraySendTo);
            const float slw = MeasureOneLine(compositor.DwriteFactory(), fmt_sub.Get(), send_lbl);
            DrawTextBox(&compositor, fmt_sub.Get(), B(p.faint), send_lbl,
                        D2D1::RectF(10.0f * s, sy, 10.0f * s + slw + 2.0f, sy + 24.0f * s));
            float cx = 10.0f * s + slw + 6.0f * s;
            const float pick_w = 28.0f * s;
            const float available = W - 10.0f * s - cx - pick_w;
            const auto destinations = std::count_if(d.dests.begin(), d.dests.end(),
                [](const TrayDestView& dest) { return !dest.missing; });
            const float chip_max = available / (destinations > 1 ? 2.0f : 1.0f) - 5.0f * s;
            int shown = 0;
            for (int di = 0; di < static_cast<int>(d.dests.size()) && shown < 2; ++di) {
                const auto& dest = d.dests[static_cast<size_t>(di)];
                if (dest.missing) continue;
                const float tw = std::min(88.0f * s, MeasureOneLine(compositor.DwriteFactory(), fmt_row.Get(), dest.label));
                const float cw = std::min(chip_max, 35.0f * s + tw);
                if (cw < 45.0f * s) break;
                const D2D1_RECT_F c = D2D1::RectF(cx, sy, cx + cw, sy + 24.0f * s);
                const bool chot = is_hot(HitKind::SendChip, di);
                dc->FillRoundedRectangle(D2D1::RoundedRect(c, 12.0f * s, 12.0f * s), B(chot ? p.chip_hot : p.chip));
                const float fx = c.left + 7.0f * s, fy = sy + 8.0f * s;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx, fy - 1.8f * s, fx + 6.0f * s, fy + 2.0f * s),
                                                           1.2f * s, 1.2f * s), B(p.folder));
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fx, fy, fx + 13.0f * s, fy + 9.5f * s),
                                                           2.0f * s, 2.0f * s), B(p.folder));
                DrawTextBox(&compositor, fmt_row.Get(), B(chot ? p.strong : p.text), dest.label,
                            D2D1::RectF(fx + 19.0f * s, c.top, c.right - 8.0f * s, c.bottom));
                add_hit(c, HitKind::SendChip, di, 0, dest.path);
                cx += cw + 5.0f * s;
                ++shown;
            }
            const D2D1_RECT_F pick_rc = D2D1::RectF(cx, sy, cx + pick_w, sy + 24.0f * s);
            dc->FillRoundedRectangle(D2D1::RoundedRect(pick_rc, 12.0f * s, 12.0f * s),
                                     B(is_hot(HitKind::SendPick) ? p.chip_hot : p.chip));
            DrawShelfGlyph(dc, B(is_hot(HitKind::SendPick) ? p.strong : p.sub), ShelfGlyph::Plus, pick_rc, 13.0f * s);
            add_hit(pick_rc, HitKind::SendPick);
        } else {
            // Expanded View (Filter bar + Grid/List + Selection bar + Release)
            DrawExpandedBody(dc, d, p, head_bot, W, H, s);
        }

        if (shell_clip) dc->PopLayer();

        // Crisp 1px inset border inside the rounded corners so nothing bleeds outside!
        const D2D1_RECT_F edge_rc = D2D1::RectF(0.5f, 0.5f, W - 0.5f, H - 0.5f);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(edge_rc, std::max(0.0f, rad - 0.5f), std::max(0.0f, rad - 0.5f)),
                                 B(p.shelf_edge), 1.0f);

        const HRESULT hr = dc->EndDraw();
        if (FAILED(hr)) {
            compositor.NotifyDeviceLost(hr);
            brush.Reset();
        } else {
            compositor.Present();
        }

        const float target_spread = (hover_kind == HitKind::Pile) ? 1.0f : 0.0f;
        const bool animating = (pop_t < 1.0f) || (std::abs(fan_spread - target_spread) > 0.01f) ||
                               (d.bump > 0.0f && d.bump < 1.0f);
        if (animating) SetTimer(hwnd, kAnimTimer, 16, nullptr);
        else KillTimer(hwnd, kAnimTimer);
    }

    void DrawExpandedBody(ID2D1DeviceContext* dc, const TrayDeckView& d, const ShelfColors& p,
                          float y_top, float W, float H, float s) {
        auto B = [&](const D2D1_COLOR_F& c) { brush->SetColor(c); return brush.Get(); };
        auto add_hit = [&](const D2D1_RECT_F& r, HitKind k, int sub = -1, uint64_t id = 0, std::wstring path = {}) {
            if (r.right > r.left && r.bottom > r.top)
                hits.push_back({r, k, sub, id, std::move(path)});
        };
        auto is_hot = [&](HitKind k, int sub = -1, uint64_t id = 0) {
            if (hover_kind != k) return false;
            if (id != 0) return hover_id == id;
            if (sub >= 0) return hover_sub == sub;
            return true;
        };

        // 1. Filter & View bar (.fs-bar, height 28 DIP)
        const float bar_cy = y_top + 13.0f * s;
        const float ib = 24.0f * s;
        const D2D1_RECT_F btn_col  = D2D1::RectF(W - 6.0f * s - ib, bar_cy - ib * 0.5f, W - 6.0f * s, bar_cy + ib * 0.5f);
        const D2D1_RECT_F btn_view = D2D1::RectF(btn_col.left - 2.0f * s - ib, btn_col.top, btn_col.left - 2.0f * s, btn_col.bottom);

        if (is_hot(HitKind::ViewToggle))
            dc->FillRoundedRectangle(D2D1::RoundedRect(btn_view, 6.0f * s, 6.0f * s), B(p.ib_hot));
        DrawShelfGlyph(dc, B(is_hot(HitKind::ViewToggle) ? p.strong : p.sub),
                       view_grid ? ShelfGlyph::List : ShelfGlyph::Grid, btn_view, 13.0f * s);
        add_hit(btn_view, HitKind::ViewToggle);

        if (is_hot(HitKind::Collapse))
            dc->FillRoundedRectangle(D2D1::RoundedRect(btn_col, 6.0f * s, 6.0f * s), B(p.ib_hot));
        DrawShelfGlyph(dc, B(is_hot(HitKind::Collapse) ? p.strong : p.sub), ShelfGlyph::Down, btn_col, 13.5f * s);
        add_hit(btn_col, HitKind::Collapse);

        static constexpr StringId kFltIds[5] = {
            StringId::TrayFilterAll, StringId::TrayFilterImages,
            StringId::TrayFilterDocs, StringId::ShelfFilterSnippets, StringId::TrayFilterOther
        };
        const float bar_h = FilterBarHeightDip(d);
        const bool filters_on_second_row = bar_h > 28.0f;
        const float filter_cy = bar_cy + (filters_on_second_row ? 24.0f * s : 0.0f);
        const float filter_right = filters_on_second_row ? W - 8.0f * s : btn_view.left - 4.0f * s;
        std::wstring filter_labels[5];
        float filter_widths[5]{};
        float total_width = 0.0f;
        int filter_count = 0;
        for (int f = 0; f < 5; ++f) {
            if (f > 0 && d.filter_counts[f] <= 0 && f != d.filter) continue;
            filter_labels[f] = l10n::Get(kFltIds[f]) + L" " + std::to_wstring(d.filter_counts[f]);
            IDWriteTextFormat* format = f == d.filter ? fmt_badge.Get() : fmt_tag.Get();
            filter_widths[f] = MeasureOneLine(compositor.DwriteFactory(), format, filter_labels[f]) + 12.0f * s;
            total_width += filter_widths[f];
            ++filter_count;
        }
        float fx = 10.0f * s;
        const float filter_gap = 3.0f * s;
        const float width_factor = std::clamp((filter_right - fx - filter_gap * (filter_count - 1)) / total_width,
                                              0.0f, 1.0f);
        for (int f = 0; f < 5; ++f) {
            if (filter_labels[f].empty()) continue;
            const float cw = filter_widths[f] * width_factor;
            const D2D1_RECT_F c = D2D1::RectF(fx, filter_cy - 11.0f * s, fx + cw, filter_cy + 11.0f * s);
            const bool on = d.filter == f;
            const bool fhot = is_hot(HitKind::FilterChip, f);
            dc->FillRoundedRectangle(D2D1::RoundedRect(c, 11.0f * s, 11.0f * s),
                                     B(on ? p.sel_strong : (fhot ? p.ib_hot : p.chip)));
            DrawTextBox(&compositor, on ? fmt_badge.Get() : fmt_tag.Get(),
                        B(on ? p.acc2 : p.sub), filter_labels[f], c, DWRITE_TEXT_ALIGNMENT_CENTER);
            add_hit(c, HitKind::FilterChip, f);
            fx += cw + filter_gap;
        }

        // 2. Footer positions from bottom up
        const float note_h = d.snippet_note.empty() ? 0.0f : 18.0f * s;
        const float sp_bot = H - 10.0f * s - note_h;
        const float sp_top = sp_bot - 32.0f * s;
        const float sel_top = sp_top - 30.0f * s;
        const float list_top = y_top + bar_h * s;
        const float list_bot = std::max(list_top, sel_top - 2.0f * s);
        const D2D1_RECT_F items_viewport_px = D2D1::RectF(0.0f, list_top, W, list_bot);
        auto add_item_hit = [&](D2D1_RECT_F r, HitKind kind, int index, uint64_t id, const std::wstring& path) {
            r.top = std::max(r.top, list_top);
            r.bottom = std::min(r.bottom, list_bot);
            add_hit(r, kind, index, id, path);
        };

        const float max_scroll = std::max(0.0f, items_content_dip - (list_bot - list_top) / s);
        items_scroll_dip = std::clamp(items_scroll_dip, 0.0f, max_scroll);

        // 3. Scrollable items area (Grid or Source-Grouped List)
        dc->PushAxisAlignedClip(items_viewport_px, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        const float scroll_px = items_scroll_dip * s;
        if (view_grid) {
            const float pad_x = 8.0f * s, gap = 6.0f * s;
            const float cell_w = (W - pad_x * 2.0f - gap * 2.0f) / 3.0f;
            const float cell_h = 86.0f * s;
            for (size_t i = 0; i < d.open_rows.size(); ++i) {
                const auto& row = d.open_rows[i];
                const int col = static_cast<int>(i % 3);
                const int r = static_cast<int>(i / 3);
                const float x0 = pad_x + static_cast<float>(col) * (cell_w + gap);
                const float y0 = list_top + 2.0f * s + static_cast<float>(r) * (cell_h + gap) - scroll_px;
                const D2D1_RECT_F cell = D2D1::RectF(x0, y0, x0 + cell_w, y0 + cell_h);
                if (cell.bottom < list_top || cell.top > list_bot) continue;

                const bool rm_hot = is_hot(HitKind::ItemRemove, -1, row.id);
                const bool card_hot = rm_hot || is_hot(HitKind::ItemCard, -1, row.id);
                if (row.checked) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(cell, 9.0f * s, 9.0f * s), B(p.sel));
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(
                        cell.left + 0.75f * s, cell.top + 0.75f * s, cell.right - 0.75f * s, cell.bottom - 0.75f * s),
                        8.5f * s, 8.5f * s), B(p.acc2), 1.5f * s);
                } else if (card_hot) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(cell, 9.0f * s, 9.0f * s), B(p.ib_hot));
                }

                const D2D1_RECT_F th_slot = D2D1::RectF(cell.left, cell.top + 9.0f * s, cell.right, cell.top + 49.0f * s);
                DrawShelfItemThumb(dc, &compositor, fmt_badge.Get(), &thumbs, &icons,
                                   row.path, row.name, row.domain, row.attrs, row.is_dir,
                                   !row.exists, row.content_kind, row.id, th_slot, 38.0f * s, s);
                if (!row.exists) {
                    DrawTextBox(&compositor, fmt_title.Get(), B(p.warn), L"\u26A0",
                                D2D1::RectF(cell.left, cell.top + 22.0f * s, cell.right, cell.top + 42.0f * s),
                                DWRITE_TEXT_ALIGNMENT_CENTER);
                }

                const std::wstring gname = FormatShelfGridName(row.name, row.content_kind == 0 && !row.is_dir);
                DrawTextBox(&compositor, fmt_grid.Get(), B(row.exists ? p.text : p.faint), gname,
                            D2D1::RectF(cell.left + 3.0f * s, cell.top + 52.0f * s,
                                        cell.right - 3.0f * s, cell.bottom - 3.0f * s),
                            DWRITE_TEXT_ALIGNMENT_CENTER, true);

                // Checkbox (top-left, visible when checked or hovered)
                if (row.checked || card_hot) {
                    const D2D1_RECT_F ck = D2D1::RectF(cell.left + 5.0f * s, cell.top + 5.0f * s,
                                                        cell.left + 21.0f * s, cell.top + 21.0f * s);
                    if (row.checked) {
                        dc->FillRoundedRectangle(D2D1::RoundedRect(ck, 5.0f * s, 5.0f * s), B(p.acc2));
                        DrawShelfGlyph(dc, B(p.on_acc), ShelfGlyph::Check, ck, 10.0f * s);
                    } else {
                        dc->DrawRoundedRectangle(D2D1::RoundedRect(ck, 5.0f * s, 5.0f * s), B(p.ck_edge), 1.5f * s);
                    }
                }

                // Move badge (top-right when not hovered) or Remove button (top-right on hover)
                const D2D1_RECT_F rm_rc = D2D1::RectF(cell.right - 22.0f * s, cell.top + 4.0f * s,
                                                       cell.right - 4.0f * s, cell.top + 22.0f * s);
                if (card_hot) {
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((rm_rc.left + rm_rc.right) * 0.5f,
                                                                (rm_rc.top + rm_rc.bottom) * 0.5f),
                                                  9.0f * s, 9.0f * s),
                                    B(rm_hot ? p.danger : p.rm_bg));
                    DrawShelfGlyph(dc, B(rm_hot ? p.on_acc : p.sub), ShelfGlyph::Close, rm_rc, 9.0f * s);
                } else if (row.move_intent && row.content_kind == 0) {
                    const D2D1_RECT_F mv_rc = D2D1::RectF(cell.right - 21.0f * s, cell.top + 5.0f * s,
                                                          cell.right - 5.0f * s, cell.top + 20.0f * s);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(mv_rc, 4.0f * s, 4.0f * s), B(p.move));
                    DrawTextBox(&compositor, fmt_mv.Get(), B(p.on_acc), l10n::Get(StringId::ShelfMoveBadge),
                                mv_rc, DWRITE_TEXT_ALIGNMENT_CENTER);
                }

                add_item_hit(cell, HitKind::ItemCard, static_cast<int>(i), row.id, row.path);
                if (card_hot) add_item_hit(rm_rc, HitKind::ItemRemove, static_cast<int>(i), row.id, row.path);
            }
        } else {
            // Source-Grouped List
            float y = list_top + 2.0f * s - scroll_px;
            for (size_t i = 0; i < d.open_rows.size(); ++i) {
                const auto& row = d.open_rows[i];
                if (row.group_start) {
                    const D2D1_RECT_F gh = D2D1::RectF(8.0f * s, y, W - 8.0f * s, y + 26.0f * s);
                    if (gh.bottom >= list_top && gh.top <= list_bot) {
                        const float fy = gh.top + 10.0f * s;
                        dc->FillRoundedRectangle(D2D1::RoundedRect(
                            D2D1::RectF(gh.left + 6.0f * s, fy, gh.left + 18.0f * s, fy + 9.0f * s),
                            2.0f * s, 2.0f * s), B(p.folder));
                        float open_left = gh.right - 6.0f * s;
                        if (!row.group_path.empty()) {
                            const std::wstring op = l10n::Get(StringId::TrayGroupOpen);
                            const float ow = MeasureOneLine(compositor.DwriteFactory(), fmt_tag.Get(), op) + 6.0f * s;
                            const D2D1_RECT_F orc = D2D1::RectF(gh.right - 6.0f * s - ow, gh.top + 4.0f * s,
                                                                gh.right - 6.0f * s, gh.bottom);
                            open_left = orc.left - 4.0f * s;
                            const bool ohot = is_hot(HitKind::GroupOpen, static_cast<int>(i));
                            DrawTextBox(&compositor, fmt_tag.Get(), B(ohot ? p.acc2 : p.faint), op,
                                        orc, DWRITE_TEXT_ALIGNMENT_TRAILING);
                            add_item_hit(orc, HitKind::GroupOpen, static_cast<int>(i), 0, row.group_path);
                        }
                        DrawTextBox(&compositor, fmt_badge.Get(), B(p.faint), row.group,
                                    D2D1::RectF(gh.left + 24.0f * s, gh.top + 4.0f * s, open_left, gh.bottom));
                    }
                    y += 26.0f * s;
                }

                const D2D1_RECT_F rr = D2D1::RectF(8.0f * s, y, W - 8.0f * s, y + 30.0f * s);
                y += 30.0f * s;
                if (rr.bottom < list_top || rr.top > list_bot) continue;

                const bool rm_hot = is_hot(HitKind::ItemRemove, -1, row.id);
                const bool rhot = rm_hot || is_hot(HitKind::ItemCard, -1, row.id);
                if (row.checked) dc->FillRoundedRectangle(D2D1::RoundedRect(rr, 7.0f * s, 7.0f * s), B(p.sel));
                else if (rhot) dc->FillRoundedRectangle(D2D1::RoundedRect(rr, 7.0f * s, 7.0f * s), B(p.ib_hot));

                const float cy = (rr.top + rr.bottom) * 0.5f;
                const D2D1_RECT_F ck = D2D1::RectF(rr.left + 6.0f * s, cy - 8.0f * s, rr.left + 22.0f * s, cy + 8.0f * s);
                if (row.checked) {
                    dc->FillRoundedRectangle(D2D1::RoundedRect(ck, 5.0f * s, 5.0f * s), B(p.acc2));
                    DrawShelfGlyph(dc, B(p.on_acc), ShelfGlyph::Check, ck, 10.0f * s);
                } else {
                    dc->DrawRoundedRectangle(D2D1::RoundedRect(ck, 5.0f * s, 5.0f * s), B(p.ck_edge), 1.5f * s);
                }

                const D2D1_RECT_F th_rc = D2D1::RectF(ck.right + 7.0f * s, cy - 9.0f * s, ck.right + 25.0f * s, cy + 9.0f * s);
                DrawShelfItemThumb(dc, &compositor, fmt_badge.Get(), &thumbs, &icons,
                                   row.path, row.name, row.domain, row.attrs, row.is_dir,
                                   !row.exists, row.content_kind, row.id, th_rc, 18.0f * s, s);

                const D2D1_RECT_F rm_rc = D2D1::RectF(rr.right - 24.0f * s, cy - 9.0f * s, rr.right - 6.0f * s, cy + 9.0f * s);
                float right_limit = rr.right - 6.0f * s;
                if (rhot) {
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((rm_rc.left + rm_rc.right) * 0.5f, cy),
                                                  9.0f * s, 9.0f * s), B(rm_hot ? p.danger : p.rm_bg));
                    DrawShelfGlyph(dc, B(rm_hot ? p.on_acc : p.sub), ShelfGlyph::Close, rm_rc, 9.0f * s);
                    right_limit = rm_rc.left - 6.0f * s;
                } else if (!row.size.empty()) {
                    const float sw = MeasureOneLine(compositor.DwriteFactory(), fmt_tag.Get(), row.size) + 4.0f * s;
                    DrawTextBox(&compositor, fmt_tag.Get(), B(p.faint), row.size,
                                D2D1::RectF(right_limit - sw, rr.top, right_limit, rr.bottom),
                                DWRITE_TEXT_ALIGNMENT_TRAILING);
                    right_limit -= sw + 6.0f * s;
                }
                if (row.move_intent && row.content_kind == 0) {
                    const D2D1_RECT_F mv_rc = D2D1::RectF(right_limit - 16.0f * s, cy - 7.5f * s, right_limit, cy + 7.5f * s);
                    dc->FillRoundedRectangle(D2D1::RoundedRect(mv_rc, 4.0f * s, 4.0f * s), B(p.move));
                    DrawTextBox(&compositor, fmt_mv.Get(), B(p.on_acc), l10n::Get(StringId::ShelfMoveBadge),
                                mv_rc, DWRITE_TEXT_ALIGNMENT_CENTER);
                    right_limit = mv_rc.left - 6.0f * s;
                }
                std::wstring nm = row.name;
                if (!row.exists) nm += L" (" + l10n::Get(StringId::ShelfMissingTag) + L")";
                DrawTextBox(&compositor, fmt_row.Get(), B(row.exists ? p.text : p.faint), nm,
                            D2D1::RectF(th_rc.right + 7.0f * s, rr.top, right_limit, rr.bottom));

                add_item_hit(rr, HitKind::ItemCard, static_cast<int>(i), row.id, row.path);
                if (rhot) add_item_hit(rm_rc, HitKind::ItemRemove, static_cast<int>(i), row.id, row.path);
            }
        }
        dc->PopAxisAlignedClip();

        // Thin scrollbar when overflowing
        if (max_scroll > 1.0f && list_bot - list_top > 20.0f * s) {
            const float view_h = list_bot - list_top;
            const float total_h = items_content_dip * s;
            const float thumb_h = std::max(20.0f * s, view_h * (view_h / total_h));
            const float thumb_y = list_top + (view_h - thumb_h) * (items_scroll_dip / max_scroll);
            dc->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(W - 4.5f * s, thumb_y, W - 2.0f * s, thumb_y + thumb_h),
                1.25f * s, 1.25f * s), B(p.ck_edge));
        }

        // 4. Selection info + Compare button (.fs-sel)
        dc->DrawLine(D2D1::Point2F(10.0f * s, sel_top), D2D1::Point2F(W - 10.0f * s, sel_top), B(p.div), 1.0f);
        const D2D1_RECT_F sel_rc = D2D1::RectF(12.0f * s, sel_top + 2.0f * s, W - 12.0f * s, sp_top - 2.0f * s);
        if (d.checked > 0) {
            std::wstring pre = FillOne(l10n::Get(StringId::TraySelectedFormat), L"{n}", std::to_wstring(d.checked)) + L" \u00B7 ";
            const float pw = MeasureOneLine(compositor.DwriteFactory(), fmt_sub.Get(), pre);
            DrawTextBox(&compositor, fmt_sub.Get(), B(p.sub), pre,
                        D2D1::RectF(sel_rc.left, sel_rc.top, sel_rc.left + pw + 2.0f, sel_rc.bottom));
            const std::wstring cancel = l10n::Get(StringId::Cancel);
            const float cw = MeasureOneLine(compositor.DwriteFactory(), fmt_sub_bold.Get(), cancel) + 4.0f * s;
            const D2D1_RECT_F crc = D2D1::RectF(sel_rc.left + pw, sel_rc.top, sel_rc.left + pw + cw, sel_rc.bottom);
            DrawTextBox(&compositor, fmt_sub_bold.Get(), B(is_hot(HitKind::ClearCheck) ? p.strong : p.acc2), cancel, crc);
            add_hit(crc, HitKind::ClearCheck);

            if (d.checked == 2) {
                const std::wstring cmp_lbl = l10n::Get(StringId::TrayCompareThese);
                const float bw = MeasureOneLine(compositor.DwriteFactory(), fmt_sub.Get(), cmp_lbl) + 16.0f * s;
                const float cy = (sel_rc.top + sel_rc.bottom) * 0.5f;
                const D2D1_RECT_F brc = D2D1::RectF(sel_rc.right - bw, cy - 11.0f * s, sel_rc.right, cy + 11.0f * s);
                dc->FillRoundedRectangle(D2D1::RoundedRect(brc, 8.0f * s, 8.0f * s),
                                         B(is_hot(HitKind::ComparePair) ? AlphaOf(p.acc2, 0.36f) : p.sel_strong));
                DrawTextBox(&compositor, fmt_sub.Get(), B(p.strong), cmp_lbl, brc, DWRITE_TEXT_ALIGNMENT_CENTER);
                add_hit(brc, HitKind::ComparePair);
            }
        } else {
            const std::wstring all_lbl = FillOne(l10n::Get(StringId::TrayNoneCheckedFormat), L"{n}", std::to_wstring(d.live_total));
            DrawTextBox(&compositor, fmt_sub.Get(), B(p.sub), all_lbl, sel_rc);
        }

        // 5. Release button
        const D2D1_RECT_F sp = D2D1::RectF(10.0f * s, sp_top, W - 10.0f * s, sp_bot);
        const D2D1_RECT_F dd = D2D1::RectF(sp.right - 30.0f * s, sp.top, sp.right, sp.bottom);
        const D2D1_RECT_F go = D2D1::RectF(sp.left, sp.top, dd.left, sp.bottom);
        DrawShelfReleaseButton(dc, &compositor, brush.Get(), fmt_btn.Get(), fmt_badge.Get(),
                               fmt_tag.Get(), d, p, p.shelf_solid, sp, go, dd, s, false,
                               is_hot(HitKind::ReleaseGo), is_hot(HitKind::ReleaseMenu));
        if (!d.target.empty() && d.release_copy_count + d.release_move_count > 0)
            add_hit(go, HitKind::ReleaseGo);
        add_hit(dd, HitKind::ReleaseMenu);
    }

    const HitZone* HitAt(float x, float y) const {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it) {
            if (PtIn(it->rc, x, y)) return &*it;
        }
        return nullptr;
    }

    void UpdateHover(float x, float y) {
        const HitZone* z = HitAt(x, y);
        const HitKind nk = z ? z->kind : HitKind::None;
        const int nsub = z ? z->sub : -1;
        const uint64_t nid = z ? z->id : 0;
        if (nk != hover_kind || nsub != hover_sub || nid != hover_id) {
            hover_kind = nk;
            hover_sub = nsub;
            hover_id = nid;
            RenderFrame();
        }
    }

    // --- OLE Drop Target callbacks ---
    bool OnOleDragEnter(bool is_content, DWORD key_state, POINTL pt) {
        if (!visible) return false;
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        min_drop_h_dip = static_cast<float>(wr.bottom - wr.top) / std::max(0.5f, scale);
        drop_ready = false;
        KillTimer(hwnd, kDropReadyTimer);
        KillTimer(hwnd, kSwitchMenuTimer);
        ole_dropping = true;
        drop_from_shelf = app::IsShelfOwnDragActive();
        drop_is_content = is_content;
        drop_target_zone = -1;
        drop_keys = key_state;
        drop_screen = pt;
        SyncSizeAndRender();
        return drop_target_zone >= 0;
    }

    bool OnOleDragOver(DWORD key_state, POINTL pt) {
        const int prev = drop_target_zone;
        UpdateDropZone(key_state, pt);
        if (drop_target_zone != prev) RenderFrame();
        return drop_target_zone >= 0;
    }

    void ResetDropState() {
        ole_dropping = false;
        drop_from_shelf = false;
        drop_ready = false;
        drop_is_content = false;
        drop_target_zone = -1;
        min_drop_h_dip = 0.0f;
        KillTimer(hwnd, kDropReadyTimer);
    }

    void OnOleDragLeave() {
        if (!ole_dropping && !drop_ready) return;
        ResetDropState();
        SyncSizeAndRender();
    }

    bool OnOleDrop(IDataObject* obj, DWORD key_state, POINTL pt) {
        UpdateDropZone(key_state, pt);
        const int zone = drop_target_zone;
        const bool shift_move = !drop_is_content && (key_state & MK_SHIFT) != 0;
        ResetDropState();
        const bool ok = zone >= 0 && cb.accept_drop && cb.accept_drop(obj, zone, shift_move);
        SyncSizeAndRender();
        ReconcileAutoHide(false);
        return ok;
    }

    static bool DragButtonDown() noexcept {
        return ((GetAsyncKeyState(VK_LBUTTON) | GetAsyncKeyState(VK_RBUTTON)) & 0x8000) != 0;
    }

    void UpdateDropZone(DWORD key_state, POINTL screen_pt) {
        drop_keys = key_state;
        drop_screen = screen_pt;
        RECT cr{};
        GetClientRect(hwnd, &cr);
        const float s = scale;
        const D2D1_RECT_F body_rc = D2D1::RectF(0.0f, (kBandDip + kHeaderDip) * s,
                                                static_cast<float>(cr.right), static_cast<float>(cr.bottom));
        cached_view.drop_content = drop_is_content;
        const ShelfDropGeom dg = LayoutShelfDropLayer(cached_view, body_rc, s,
            [&](const std::wstring& t) { return MeasureOneLine(compositor.DwriteFactory(), fmt_row.Get(), t); });
        drop_target_zone = HitDropLayer(dg, key_state, screen_pt);
    }

    int HitDropLayer(const ShelfDropGeom& layout, DWORD key_state, POINTL screen_pt) const {
        POINT cp{screen_pt.x, screen_pt.y};
        if (!ScreenToClient(hwnd, &cp)) return -1;
        const int zone = HitTestShelfDropLayer(layout, static_cast<float>(cp.x), static_cast<float>(cp.y),
                                               drop_is_content, (key_state & MK_SHIFT) != 0);
        return drop_from_shelf && zone < 2 ? -1 : zone;
    }

    void ReconcileAutoHide(bool arm) {
        if (cached_view.pinned || cached_view.live_total > 0 || !visible) {
            auto_hide_pending = false;
            KillTimer(hwnd, kAutoHideTimer);
        } else if (arm && !auto_hide_pending) {
            auto_hide_pending = SetTimer(hwnd, kAutoHideTimer, 1500, nullptr) != 0;
        }
    }

    // --- Inline Rename ---
    static LRESULT CALLBACK EditSubclassProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        if (self) {
            if (msg == WM_KEYDOWN) {
                if (wp == VK_RETURN) { self->FinishInlineRename(true); return 0; }
                if (wp == VK_ESCAPE) { self->FinishInlineRename(false); return 0; }
            } else if (msg == WM_KILLFOCUS) {
                LRESULT r = CallWindowProcW(self->orig_edit_proc, h, msg, wp, lp);
                self->FinishInlineRename(true);
                return r;
            }
        }
        return self && self->orig_edit_proc ? CallWindowProcW(self->orig_edit_proc, h, msg, wp, lp)
                                            : DefWindowProcW(h, msg, wp, lp);
    }

    void StartRename(const std::wstring& initial) {
        if (!hwnd || !visible) return;
        renaming = true;
        const float s = scale;
        const int x = static_cast<int>(std::lround(8.0f * s));
        const int y = static_cast<int>(std::lround((kBandDip + 6.0f) * s));
        const int w = static_cast<int>(std::lround(132.0f * s));
        const int h = static_cast<int>(std::lround(26.0f * s));
        if (!edit) {
            edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                   WS_CHILD | ES_AUTOHSCROLL,
                                   x, y, w, h, hwnd, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditId)),
                                   GetModuleHandleW(nullptr), nullptr);
            if (edit) {
                SendMessageW(edit, EM_SETLIMITTEXT, 16, 0);
                SetWindowLongPtrW(edit, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
                orig_edit_proc = reinterpret_cast<WNDPROC>(
                    SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditSubclassProc)));
            }
        }
        if (!edit) { renaming = false; return; }
        if (edit_font) { DeleteObject(edit_font); edit_font = nullptr; }
        edit_font = CreateFontW(-static_cast<int>(std::lround(12.5f * s)), 0, 0, 0, FW_SEMIBOLD,
                                FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                                CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        if (edit_font) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(edit_font), TRUE);
        SetWindowPos(edit, HWND_TOP, x, y, w, h, SWP_SHOWWINDOW);
        SetWindowTextW(edit, initial.c_str());
        SetForegroundWindow(hwnd);
        SetFocus(edit);
        SendMessageW(edit, EM_SETSEL, 0, -1);
        RenderFrame();
    }

    void FinishInlineRename(bool commit) {
        if (!renaming) return;
        renaming = false;
        std::wstring val;
        if (edit) {
            wchar_t buf[64]{};
            GetWindowTextW(edit, buf, ARRAYSIZE(buf));
            val = buf;
            ShowWindow(edit, SW_HIDE);
        }
        if (commit && cb.commit_rename && !val.empty())
            cb.commit_rename(val);
        SyncSizeAndRender(false);
    }

    // --- Window Procedure ---
    static LRESULT CALLBACK StaticWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        Impl* self = nullptr;
        if (msg == WM_NCCREATE) {
            const auto* cs = reinterpret_cast<const CREATESTRUCTW*>(lp);
            self = static_cast<Impl*>(cs->lpCreateParams);
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
            self->hwnd = h;
        } else {
            self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        }
        return self ? self->WndProc(h, msg, wp, lp) : DefWindowProcW(h, msg, wp, lp);
    }

    LRESULT WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
        switch (msg) {
        case WM_NCCALCSIZE:
            if (wp) return 0;
            break;
        case WM_NCACTIVATE:
            // Keep DWM Acrylic frosted even when non-activated above other apps.
            return DefWindowProcW(h, msg, TRUE, -1);
        case WM_MOUSEACTIVATE:
            return (ole_dropping || drop_ready) ? MA_NOACTIVATE : MA_ACTIVATE;
        case WM_ERASEBKGND:
            return 1;
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wp);
            SetTextColor(hdc, dark ? RGB(242, 242, 242) : RGB(48, 59, 80));
            SetBkColor(hdc, dark ? RGB(36, 40, 50) : RGB(255, 255, 255));
            return reinterpret_cast<LRESULT>(edit_brush);
        }
        case WM_DPICHANGED: {
            const float new_scale = static_cast<float>(HIWORD(wp)) / 96.0f;
            if (moving_window && scale > 0.0f) {
                move_offset.x = std::lround(move_offset.x * (new_scale / scale));
                move_offset.y = std::lround(move_offset.y * (new_scale / scale));
            }
            scale = new_scale;
            const auto* sug = reinterpret_cast<const RECT*>(lp);
            if (sug) {
                SetWindowPos(h, HWND_TOPMOST, sug->left, sug->top,
                             sug->right - sug->left, sug->bottom - sug->top,
                             SWP_NOACTIVATE | SWP_NOZORDER);
            }
            SyncSizeAndRender(false);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps{};
            BeginPaint(h, &ps);
            RenderFrame();
            EndPaint(h, &ps);
            return 0;
        }
        case WM_TIMER: {
            if (wp == kAnimTimer) {
                const float target_spread = (hover_kind == HitKind::Pile) ? 1.0f : 0.0f;
                fan_spread += (target_spread - fan_spread) * 0.28f;
                if (std::abs(target_spread - fan_spread) < 0.01f) fan_spread = target_spread;
                if (cached_view.bump > 0.0f && cached_view.bump < 1.0f) BuildView();
                RenderFrame();
                return 0;
            }
            if (wp == kAutoHideTimer) {
                if (auto_hide_pending && !ole_dropping && !drop_ready && !moving_window && !renaming &&
                    pressed.kind == HitKind::None && !DragButtonDown()) {
                    BuildView();
                    ReconcileAutoHide(false);
                    if (auto_hide_pending) {
                        Hide();
                        if (cb.auto_hidden) cb.auto_hidden();
                    }
                }
                return 0;
            }
            if (wp == kDropReadyTimer) {
                if (drop_ready && !DragButtonDown()) OnOleDragLeave();
                return 0;
            }
            if (wp == kSwitchMenuTimer) {
                KillTimer(h, kSwitchMenuTimer);
                if (visible && !renaming && !drop_ready && !ole_dropping && cb.open_switch_menu)
                    cb.open_switch_menu(switch_menu_anchor);
                return 0;
            }
            break;
        }
        case WM_MOUSEMOVE: {
            if (!tracking_mouse) {
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
                tracking_mouse = true;
            }
            if (moving_window || pressed.kind != HitKind::None) {
                if ((wParamHasLeft(wp)) == false && (GetAsyncKeyState(VK_LBUTTON) & 0x8000) == 0) {
                    if (moving_window) EndWindowMove();
                    else CancelPress();
                    return 0;
                }
                POINT cur{};
                GetCursorPos(&cur);
                if (!moving_window) {
                    if (std::abs(cur.x - press_screen.x) > std::max(1, GetSystemMetrics(SM_CXDRAG) / 2) ||
                        std::abs(cur.y - press_screen.y) > std::max(1, GetSystemMetrics(SM_CYDRAG) / 2)) {
                        const HitZone hit = std::move(pressed);
                        pressed = {};
                        if (hit.kind == HitKind::HeadDrag) {
                            RECT wr{};
                            GetWindowRect(h, &wr);
                            moving_window = true;
                            move_offset = {press_screen.x - wr.left, press_screen.y - wr.top};
                            app::SetShelfOwnDragActive(true);
                        } else {
                            if (GetCapture() == h) ReleaseCapture();
                            if (cb.start_drag_out && hit.kind == HitKind::Pile && cached_view.live_total > 0)
                                cb.start_drag_out({});
                            if (cb.start_drag_out && hit.kind == HitKind::ItemCard) {
                                const auto row = std::find_if(cached_view.open_rows.begin(), cached_view.open_rows.end(),
                                    [&](const TrayOpenRow& entry) { return entry.id == hit.id; });
                                if (row != cached_view.open_rows.end() && row->exists)
                                    cb.start_drag_out(row->checked ? std::vector<uint64_t>{} : std::vector<uint64_t>{hit.id});
                            }
                            return 0;
                        }
                    } else {
                        return 0;
                    }
                }
                SetWindowPos(h, HWND_TOPMOST, cur.x - move_offset.x, cur.y - move_offset.y,
                             0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
                if (cb.dragging_window) cb.dragging_window(cur);
                return 0;
            }
            UpdateHover(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking_mouse = false;
            if (hover_kind != HitKind::None) {
                hover_kind = HitKind::None;
                hover_sub = -1;
                hover_id = 0;
                SetTimer(h, kAnimTimer, 16, nullptr);
                RenderFrame();
            }
            return 0;
        case WM_MOUSEWHEEL: {
            if (expanded && !ole_dropping && !drop_ready) {
                const float steps = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wp)) / static_cast<float>(WHEEL_DELTA);
                items_scroll_dip = std::max(0.0f, items_scroll_dip - steps * 44.0f);
                RenderFrame();
                POINT cursor{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                if (ScreenToClient(h, &cursor))
                    UpdateHover(static_cast<float>(cursor.x), static_cast<float>(cursor.y));
            }
            return 0;
        }
        case WM_LBUTTONDBLCLK: {
            const float x = static_cast<float>(GET_X_LPARAM(lp));
            const float y = static_cast<float>(GET_Y_LPARAM(lp));
            if (const HitZone* z = HitAt(x, y); z && z->kind == HitKind::ShelfSwitch) {
                KillTimer(h, kSwitchMenuTimer);
                StartRename(cached_view.active_shelf_name);
                return 0;
            }
            break;
        }
        case WM_LBUTTONDOWN: {
            if (ole_dropping || drop_ready) return 0;
            KillTimer(h, kSwitchMenuTimer);
            if (renaming) FinishInlineRename(true);
            const float x = static_cast<float>(GET_X_LPARAM(lp));
            const float y = static_cast<float>(GET_Y_LPARAM(lp));
            POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &sp);
            const HitZone* z = HitAt(x, y);
            if (!z) return 0;
            pressed = *z;
            press_screen = sp;
            SetCapture(h);
            return 0;
        }
        case WM_LBUTTONUP:
            if (moving_window) {
                EndWindowMove();
            } else if (pressed.kind != HitKind::None) {
                const HitZone hit = std::move(pressed);
                const auto* over = HitAt(static_cast<float>(GET_X_LPARAM(lp)), static_cast<float>(GET_Y_LPARAM(lp)));
                const bool activate = over && hit.kind == over->kind && hit.sub == over->sub && hit.id == over->id;
                CancelPress();
                if (activate) ActivateHit(h, hit);
            }
            return 0;
        case WM_CANCELMODE:
            CancelPress();
            if (moving_window) EndWindowMove();
            return 0;
        case WM_CAPTURECHANGED:
            if (reinterpret_cast<HWND>(lp) != h) {
                pressed = {};
                if (moving_window) EndWindowMove();
            }
            break;
        case WM_RBUTTONUP: {
            KillTimer(h, kSwitchMenuTimer);
            POINT sp{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &sp);
            if (cb.open_more_menu) cb.open_more_menu(sp);
            return 0;
        }
        case WM_KEYDOWN: {
            if (renaming) break;
            const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
            if (wp == VK_ESCAPE) {
                KillTimer(h, kSwitchMenuTimer);
                if (moving_window) EndWindowMove();
                else if (pressed.kind != HitKind::None) CancelPress();
                else if (expanded) {
                    expanded = false;
                    if (cb.clear_checks) cb.clear_checks();
                    SyncSizeAndRender();
                } else {
                    Hide();
                    if (cb.dock_to_sidebar) cb.dock_to_sidebar();
                }
                return 0;
            }
            if (ctrl && !shift && wp == 'V') {
                if (cb.paste_clipboard) cb.paste_clipboard();
                return 0;
            }
            if (ctrl && shift && wp == 'S') {
                if (cb.toggle_shelf) cb.toggle_shelf();
                return 0;
            }
            break;
        }
        default:
            break;
        }
        return DefWindowProcW(h, msg, wp, lp);
    }

    void ActivateHit(HWND h, const HitZone& hit) {
        switch (hit.kind) {
        case HitKind::ShelfSwitch: {
            POINT anchor{static_cast<LONG>(hit.rc.left), static_cast<LONG>(hit.rc.bottom)};
            ClientToScreen(h, &anchor);
            POINT click = press_screen;
            ScreenToClient(h, &click);
            if (static_cast<float>(click.x) < hit.rc.right - 21.0f * scale) {
                // Give a second click on the name a chance to become inline rename.
                switch_menu_anchor = anchor;
                if (SetTimer(h, kSwitchMenuTimer, GetDoubleClickTime(), nullptr)) break;
            }
            if (cb.open_switch_menu) cb.open_switch_menu(anchor);
            break;
        }
        case HitKind::Pin:
            if (cb.toggle_pin) cb.toggle_pin();
            SyncSizeAndRender(false);
            ReconcileAutoHide(false);
            break;
        case HitKind::More: {
            POINT anchor{static_cast<LONG>(hit.rc.right), static_cast<LONG>(hit.rc.bottom)};
            ClientToScreen(h, &anchor);
            if (cb.open_more_menu) cb.open_more_menu(anchor);
            break;
        }
        case HitKind::Dock:
            if (cb.dock_to_sidebar) cb.dock_to_sidebar();
            break;
        case HitKind::Pile:
        case HitKind::Expand:
            expanded = true;
            items_scroll_dip = 0.0f;
            SetForegroundWindow(h);
            SyncSizeAndRender(true);
            break;
        case HitKind::Collapse:
            expanded = false;
            if (cb.clear_checks) cb.clear_checks();
            SyncSizeAndRender(true);
            break;
        case HitKind::ViewToggle:
            view_grid = !view_grid;
            items_scroll_dip = 0.0f;
            if (cb.view_changed) cb.view_changed(view_grid);
            SyncSizeAndRender(true);
            break;
        case HitKind::FilterChip:
            items_scroll_dip = 0.0f;
            if (cb.filter_changed) cb.filter_changed(hit.sub);
            SyncSizeAndRender(true);
            break;
        case HitKind::ItemRemove:
            if (cb.remove_item) cb.remove_item(hit.id);
            break;
        case HitKind::ItemCard:
            if (cb.toggle_check) cb.toggle_check(hit.id);
            SyncSizeAndRender(false);
            break;
        case HitKind::GroupOpen:
            if (cb.open_source_folder && !hit.path.empty()) cb.open_source_folder(hit.path);
            break;
        case HitKind::ClearCheck:
            if (cb.clear_checks) cb.clear_checks();
            SyncSizeAndRender(false);
            break;
        case HitKind::ComparePair:
            if (cb.compare_checked) cb.compare_checked();
            break;
        case HitKind::StaleRemove:
            if (cb.remove_stale) cb.remove_stale();
            break;
        case HitKind::ReleaseGo:
            if (cb.release_to_current) cb.release_to_current();
            break;
        case HitKind::ReleaseMenu: {
            POINT anchor{static_cast<LONG>(hit.rc.right), static_cast<LONG>(hit.rc.bottom)};
            ClientToScreen(h, &anchor);
            if (cb.open_release_menu) cb.open_release_menu(anchor);
            break;
        }
        case HitKind::SendChip:
            if (cb.send_to_folder && !hit.path.empty()) cb.send_to_folder(hit.path);
            break;
        case HitKind::SendPick:
            if (cb.pick_send_folder) cb.pick_send_folder();
            break;
        default:
            SetForegroundWindow(h);
            break;
        }
    }

    void CancelPress() {
        pressed = {};
        if (!moving_window && GetCapture() == hwnd) ReleaseCapture();
    }

    static bool wParamHasLeft(WPARAM wp) noexcept { return (wp & MK_LBUTTON) != 0; }

    void EndWindowMove() {
        if (!moving_window) return;
        moving_window = false;
        app::SetShelfOwnDragActive(false);
        if (GetCapture() == hwnd) ReleaseCapture();
        POINT cur{};
        GetCursorPos(&cur);
        SyncSizeAndRender();
        RECT wr{};
        GetWindowRect(hwnd, &wr);
        if (cb.finish_drag_window) cb.finish_drag_window(cur, {wr.left, wr.top});
    }

    void Hide() {
        if (!hwnd) { visible = false; return; }
        visible = false;
        FinishInlineRename(false);
        KillTimer(hwnd, kAnimTimer);
        KillTimer(hwnd, kAutoHideTimer);
        KillTimer(hwnd, kSwitchMenuTimer);
        auto_hide_pending = false;
        if (moving_window) app::SetShelfOwnDragActive(false);
        moving_window = false;
        ResetDropState();
        CancelPress();
        if (tracking_mouse) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_CANCEL | TME_LEAVE, hwnd, 0};
            TrackMouseEvent(&tme);
            tracking_mouse = false;
        }
        hover_kind = HitKind::None;
        hover_sub = -1;
        hover_id = 0;
        fan_spread = 0.0f;
        ShowWindow(hwnd, SW_HIDE);
        compositor.HibernateHiddenSurface();
    }

    void Shutdown() {
        Hide();
        if (hwnd) {
            if (drop_target) {
                drop_target->Detach();
                RevokeDragDrop(hwnd);
                drop_target->Release();
                drop_target = nullptr;
            }
            if (edit && orig_edit_proc) {
                SetWindowLongPtrW(edit, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(orig_edit_proc));
                orig_edit_proc = nullptr;
            }
            DestroyWindow(hwnd);
            hwnd = nullptr;
            edit = nullptr;
        }
        if (ole_initialized) {
            OleUninitialize();
            ole_initialized = false;
        }
        if (edit_font) { DeleteObject(edit_font); edit_font = nullptr; }
        if (edit_brush) { DeleteObject(edit_brush); edit_brush = nullptr; }
        brush.Reset();
        icons.Reset();
        thumbs.Reset();
        compositor.Shutdown();
    }
};

FloatingShelfWindow::FloatingShelfWindow() : impl_(std::make_unique<Impl>()) {}
FloatingShelfWindow::~FloatingShelfWindow() = default;

void FloatingShelfWindow::SetCallbacks(FloatingShelfCallbacks cb) {
    impl_->cb = std::move(cb);
}

void FloatingShelfWindow::SetAppearance(bool dark, WindowEffect effect, D2D1_COLOR_F accent, bool view_grid) {
    const bool changed = (impl_->dark != dark || impl_->effect != effect);
    impl_->dark = dark;
    impl_->effect = effect;
    impl_->accent = accent;
    impl_->view_grid = view_grid;
    if (impl_->hwnd && changed) impl_->ApplyDwmChrome();
    if (impl_->visible) impl_->SyncSizeAndRender(false);
}

bool FloatingShelfWindow::ShowAt(int screen_x, int screen_y, bool expanded, bool enter_drop_ready) {
    if (!impl_->EnsureWindow()) return false;
    const bool was_visible = impl_->visible;
    if (impl_->expanded != expanded) impl_->items_scroll_dip = 0.0f;
    impl_->expanded = expanded;
    const POINT anchor{screen_x, screen_y};
    impl_->scale = ScaleForPoint(anchor);
    if (enter_drop_ready) {
        screen_x -= static_cast<int>(std::lround(70.0f * impl_->scale));
        screen_y -= static_cast<int>(std::lround(100.0f * impl_->scale));
    }
    impl_->BuildView();
    if (!impl_->ole_dropping && enter_drop_ready && impl_->drop_target && Impl::DragButtonDown()) {
        impl_->drop_ready = SetTimer(impl_->hwnd, kDropReadyTimer, 60, nullptr) != 0;
        POINT cursor{};
        GetCursorPos(&cursor);
        impl_->drop_screen = {cursor.x, cursor.y};
        impl_->drop_keys = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ? MK_SHIFT : 0;
    }
    const float h_dip = impl_->ComputeTargetHeightDip(impl_->cached_view);
    const RECT wa = WorkAreaForPoint(anchor);
    const int w_px = std::clamp(static_cast<int>(std::lround(kShelfWidthDip * impl_->scale)),
                                1, std::max(1, static_cast<int>(wa.right - wa.left) - 12));
    const int h_px = std::clamp(static_cast<int>(std::lround(h_dip * impl_->scale)),
                                1, std::max(1, static_cast<int>(wa.bottom - wa.top) - 12));
    const int x = std::clamp(screen_x, static_cast<int>(wa.left + 6),
                             std::max(static_cast<int>(wa.left + 6), static_cast<int>(wa.right - w_px - 6)));
    const int y = std::clamp(screen_y, static_cast<int>(wa.top + 6),
                             std::max(static_cast<int>(wa.top + 6), static_cast<int>(wa.bottom - h_px - 6)));

    impl_->visible = true;
    KillTimer(impl_->hwnd, kAutoHideTimer);
    impl_->auto_hide_pending = false;
    if (!was_visible && motion::SystemAnimationsEnabled()) {
        impl_->pop_start_ms = motion::NowMs();
    } else {
        impl_->pop_start_ms = 0;
    }

    SetWindowPos(impl_->hwnd, HWND_TOPMOST, x, y, w_px, h_px,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
    impl_->RenderFrame();
    if (!enter_drop_ready && !impl_->ole_dropping && !Impl::DragButtonDown()) SetForegroundWindow(impl_->hwnd);
    return true;
}

bool FloatingShelfWindow::ShowTearOff(POINT cursor_screen) {
    const float s = ScaleForPoint(cursor_screen);
    const int ox = static_cast<int>(std::lround(70.0f * s));
    const int oy = static_cast<int>(std::lround(20.0f * s));
    if (!ShowAt(cursor_screen.x - ox, cursor_screen.y - oy, false, false)) return false;
    RECT wr{};
    GetWindowRect(impl_->hwnd, &wr);
    impl_->moving_window = true;
    impl_->move_offset = {cursor_screen.x - wr.left, cursor_screen.y - wr.top};
    app::SetShelfOwnDragActive(true);
    SetCapture(impl_->hwnd);
    return true;
}

void FloatingShelfWindow::Hide() { impl_->Hide(); }
void FloatingShelfWindow::Shutdown() { impl_->Shutdown(); }
bool FloatingShelfWindow::Visible() const noexcept { return impl_->visible; }
bool FloatingShelfWindow::Expanded() const noexcept { return impl_->expanded; }

void FloatingShelfWindow::SetExpanded(bool expanded) {
    if (impl_->expanded == expanded) return;
    impl_->expanded = expanded;
    impl_->items_scroll_dip = 0.0f;
    if (impl_->visible) impl_->SyncSizeAndRender(true);
}

HWND FloatingShelfWindow::Hwnd() const noexcept { return impl_->hwnd; }

RECT FloatingShelfWindow::WindowBounds() const noexcept {
    RECT r{};
    if (impl_->hwnd) GetWindowRect(impl_->hwnd, &r);
    return r;
}

void FloatingShelfWindow::BeginInlineRename(const std::wstring& current_name) {
    impl_->StartRename(current_name);
}

void FloatingShelfWindow::Refresh(bool arm_auto_hide_when_empty) {
    if (!impl_->visible || !impl_->hwnd) return;
    impl_->SyncSizeAndRender();
    impl_->ReconcileAutoHide(arm_auto_hide_when_empty);
}

} // namespace pulse::ui
