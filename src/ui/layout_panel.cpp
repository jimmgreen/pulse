#include "layout_panel.h"

#include "../common/localization.h"
#include "../common/windows_compat.h"
#include "typography.h"

#include <dwmapi.h>
#include <d2d1effects.h>
#include <windowsx.h>
#include <algorithm>
#include <cmath>

namespace pulse::ui {
namespace {

constexpr wchar_t kLayoutPanelClass[] = L"PulseLayoutPanel";
// ui_modes_mockup .fly: 388 wide, 16 padding; the rest follows the CSS rows.
constexpr float kWidthDip = 388.0f;
constexpr float kPadDip = 16.0f;
constexpr float kHeaderDip = 40.0f;        // h4 15px + p 12.5px
constexpr float kPresetGapDip = 8.0f;
constexpr float kCardLabelDip = 18.0f;
constexpr float kSectionDip = 38.0f;       // h6: 14 margin + 16 line + 8
constexpr float kToggleDip = 62.0f;
constexpr float kToggleGapDip = 6.0f;
constexpr float kRowDip = 36.0f;           // .orow: 10 margin + 26 segment
constexpr int kShadowMarginPx = 20;          // = FluentMenu::kShadowMargin
constexpr UINT_PTR kHoverTimer = 1;
constexpr float kHoverMs = 140.0f;
// CLSID_D2D1Shadow (d2d1.lib does not export the effect CLSIDs), as in color_picker.cpp.
constexpr GUID kLpShadowEffectClsid = {0xC67EA361, 0x1863, 0x4e69, {0x89, 0xDB, 0x69, 0x5D, 0x3E, 0x9D, 0x5B, 0x53}};
constexpr float kFooterDip = 50.0f;        // .ff: 10 + 30 button + 10

using Sid = pulse::l10n::StringId;

bool InsideRect(const D2D1_RECT_F& rc, float x, float y) {
    return x >= rc.left && x < rc.right && y >= rc.top && y < rc.bottom;
}

bool HighContrastOn() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) &&
           (hc.dwFlags & HCF_HIGHCONTRASTON);
}

D2D1_COLOR_F LpHex(uint32_t rgb, float a = 1.0f) {
    return D2D1::ColorF(rgb, a);
}
D2D1_COLOR_F LpAlpha(D2D1_COLOR_F c, float a) {
    c.a = a;
    return c;
}
D2D1_COLOR_F LpMix(D2D1_COLOR_F a, D2D1_COLOR_F b, float k) {
    return D2D1_COLOR_F{a.r * k + b.r * (1 - k), a.g * k + b.g * (1 - k), a.b * k + b.b * (1 - k), 1};
}

// a -> b by t, alpha included (hover transitions).
D2D1_COLOR_F LpLerp(D2D1_COLOR_F a, D2D1_COLOR_F b, float t) {
    return D2D1_COLOR_F{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

constexpr int kSegmentOptions[kLayoutPanelSegmentCount] = {3, 3, 2, 3};
constexpr Sid kSegmentLabels[kLayoutPanelSegmentCount] = {
    Sid::LayoutRowSidebar, Sid::LayoutRowCommands, Sid::LayoutRowAddress, Sid::LayoutRowDensity};
constexpr Sid kSegmentText[kLayoutPanelSegmentCount][3] = {
    {Sid::LayoutSegDocked, Sid::LayoutSegFloating, Sid::LayoutSegHidden},
    {Sid::LayoutSegFullCommands, Sid::LayoutSegInlineCommands, Sid::LayoutSegCollapsed},
    {Sid::LayoutSegOwnRow, Sid::LayoutSegInTitle, Sid::LayoutSegOwnRow},
    {Sid::LayoutSegCompact, Sid::LayoutSegStandard, Sid::LayoutSegRoomy}};
struct ToggleDef { const wchar_t* glyph; Sid label; };
constexpr ToggleDef kToggles[kLayoutPanelToggleCount] = {
    // One shape per meaning (command_icons kGlyph*): TabStrip, StatusBar, Tray,
    // SelectionBar and FileExt instead of glyphs other commands already show.
    {L"\xF5C7", Sid::LayoutTglTabs},      {L"\xE734", Sid::LayoutTglLite},
    {L"\xE8A0", Sid::LayoutTglDetails},   {L"\xF5C8", Sid::LayoutTglStatus},
    {L"\xE8A1", Sid::LayoutTglTray},      {L"\xE8BF", Sid::LayoutTglZebra},
    {L"\xF5CC", Sid::LayoutTglSelection}, {L"\xF5CE", Sid::LayoutTglExtensions}};
constexpr Sid kModeNames[3] = {Sid::SetupModeZen, Sid::SetupModeStandard, Sid::SetupModePro};
constexpr const wchar_t* kModeKeys[3] = {L"Alt\u21E71", L"Alt\u21E72", L"Alt\u21E73"};

}  // namespace

LayoutPanel::~LayoutPanel() {
    if (hwnd_ && IsWindow(hwnd_)) {
        compositor_.Shutdown();
        DestroyWindow(hwnd_);
    }
    hwnd_ = nullptr;
}

bool LayoutPanel::IsOpen() const noexcept {
    return hwnd_ && IsWindowVisible(hwnd_);
}

Theme LayoutPanel::CurrentTheme() const {
    return HighContrastOn() ? MakeHighContrastTheme() : MakeTheme(dark_, accent_);
}

float LayoutPanel::PanelHeightDip() const {
    const float card_w = (kWidthDip - 2 * kPadDip - 2 * kPresetGapDip) / 3;
    const float mini_h = (card_w - 12.0f) * 10.0f / 16.0f;
    const float card_h = 6.0f + mini_h + 7.0f + kCardLabelDip + 8.0f;
    return kPadDip + kHeaderDip + 12.0f + card_h + kSectionDip + 2 * kToggleDip + kToggleGapDip +
           kLayoutPanelSegmentCount * kRowDip + 14.0f + kFooterDip;
}

bool LayoutPanel::EnsureWindow(HWND owner) {
    if (hwnd_) return true;
    static bool registered = false;
    HINSTANCE instance = GetModuleHandleW(nullptr);
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &LayoutPanel::WndProc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kLayoutPanelClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        registered = true;
    }
    CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOOLWINDOW, kLayoutPanelClass, L"",
                    WS_POPUP, 0, 0, 100, 100, owner, nullptr, instance, this);
    return hwnd_ != nullptr;
}

void LayoutPanel::PositionPanel() {
    if (!hwnd_) return;
    HMONITOR mon = MonitorFromPoint(anchor_, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const UINT dpi = pulse::compat::WindowDpi(owner_ ? owner_ : hwnd_);
    const float old_scale = scale_;
    scale_ = static_cast<float>(dpi) / 96.0f;
    if (old_scale != scale_) {
        compositor_.RecreateTextFormats(scale_);
        painter_.SetScale(scale_);
        RecreateFormats();
    }
    // The card keeps the old placement; the window adds the shadow border.
    margin_ = compositor_.UsesTransparentComposition() ? kShadowMarginPx : 0;
    const int w = static_cast<int>(std::lround(kWidthDip * scale_));
    const int h = static_cast<int>(std::lround(PanelHeightDip() * scale_));
    const int margin = static_cast<int>(std::lround(8.0f * scale_));
    int x = anchor_.x - w;
    x = std::clamp(x, static_cast<int>(mi.rcWork.left) + margin,
                   (std::max)(static_cast<int>(mi.rcWork.left) + margin, static_cast<int>(mi.rcWork.right) - margin - w));
    int y = anchor_.y;
    if (y + h > mi.rcWork.bottom - margin) y = (std::max)(static_cast<int>(mi.rcWork.top) + margin,
                                                         static_cast<int>(mi.rcWork.bottom) - margin - h);
    SetWindowPos(hwnd_, HWND_TOP, x - margin_, y - margin_, w + 2 * margin_, h + 2 * margin_, SWP_NOACTIVATE);
}

void LayoutPanel::ApplyTheme() {
    if (!hwnd_) return;
    const BOOL dark = dark_ ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd_, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &dark, sizeof(dark));
    // With the drawn card (shadow border) DWM must not round or outline the
    // window; the opaque fallback keeps the DWM rounded corners.
    const DWORD corner = margin_ > 0 ? 1 /* DWMWCP_DONOTROUND */ : 2 /* DWMWCP_ROUND */;
    DwmSetWindowAttribute(hwnd_, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &corner, sizeof(corner));
}

void LayoutPanel::Show(HWND owner, POINT anchor, bool dark, D2D1_COLOR_F accent,
                       const LayoutPanelState& state, LayoutPanelCallbacks callbacks) {
    owner_ = owner;
    anchor_ = anchor;
    dark_ = dark;
    accent_ = accent;
    state_ = state;
    callbacks_ = std::move(callbacks);
    closing_ = false;
    hover_ = Hit{};
    pressed_ = Hit{};
    hover_t_ = {};
    if (!EnsureWindow(owner)) return;
    if (owner && GetWindow(hwnd_, GW_OWNER) != owner)
        SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    PositionPanel();
    ApplyTheme();
    Layout();
    ShowWindow(hwnd_, SW_SHOW);
    SetForegroundWindow(hwnd_);
    SetFocus(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LayoutPanel::Update(const LayoutPanelState& state, bool dark, D2D1_COLOR_F accent) {
    state_ = state;
    const bool theme_changed = dark != dark_;
    dark_ = dark;
    accent_ = accent;
    if (!IsOpen()) return;
    if (theme_changed) ApplyTheme();
    InvalidateRect(hwnd_, nullptr, FALSE);
}

void LayoutPanel::Hide() {
    if (closing_) return;
    closing_ = true;
    const bool was_visible = IsOpen();
    if (hwnd_) {
        KillTimer(hwnd_, kHoverTimer);
        ShowWindow(hwnd_, SW_HIDE);
    }
    hide_tick_ = GetTickCount64();
    if (was_visible && callbacks_.on_closed) {
        auto fn = std::move(callbacks_.on_closed);
        callbacks_.on_closed = nullptr;
        fn();
    }
}

void LayoutPanel::RecreateFormats() {
    auto* factory = compositor_.DwriteFactory();
    if (!factory) return;
    // The main window's type ramp (Painter / Compositor): the shared family,
    // the interface font size, and the same token sizes.
    auto make = [&](typography::FontRole role, DWRITE_FONT_WEIGHT weight, float dip, ComPtr<IDWriteTextFormat>& out) {
        out.reset();
        if (FAILED(typography::CreateTextFormat(factory, {role, dip * scale_, weight}, &out)) || !out.get())
            return;
        out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        out->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    using typography::FontRole;
    make(FontRole::Display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f, title_);   // = HeaderFormat
    make(FontRole::Text, DWRITE_FONT_WEIGHT_NORMAL, 13.0f, body_);          // = Painter nav
    make(FontRole::Text, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, caption_);       // = Painter caption
    make(FontRole::Text, DWRITE_FONT_WEIGHT_SEMI_BOLD, 12.0f, caption_bold_);
    make(FontRole::Text, DWRITE_FONT_WEIGHT_NORMAL, 12.0f, small_);         // = SmallFormat
    make(FontRole::Text, DWRITE_FONT_WEIGHT_NORMAL, 11.0f, kbd_);
}

float LayoutPanel::Measure(std::wstring_view text, IDWriteTextFormat* format) const {
    auto* factory = compositor_.DwriteFactory();
    if (!factory || !format || text.empty()) return 0.0f;
    // Measure as it is drawn: LumaText when the UI text uses it.
    if (!HighContrastOn()) {
        float luma_w = 0.0f;
        if (const_cast<Compositor&>(compositor_).MeasureLumaText(text, format, luma_w)) return luma_w;
    }
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(factory->CreateTextLayout(text.data(), static_cast<UINT32>(text.size()), format, 4096.0f,
                                         256.0f, &layout)) || !layout.get())
        return 0.0f;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace;
}

void LayoutPanel::Layout() {
    const float s = scale_;
    const float pad = kPadDip * s;
    const float w = kWidthDip * s;
    const float m = Margin();  // card origin inside the shadow border
    const float left = m + pad, right = m + w - pad;
    float y = m + pad;
    // Header: 恢复 sits at the right, level with the title.
    const float restore_w = Measure(l10n::Get(Sid::LayoutPanelRestore), caption_.get()) + 12.0f * s;
    restore_rc_ = D2D1::RectF(right - restore_w, y, right, y + 24.0f * s);
    y += kHeaderDip * s + 12.0f * s;
    // Preset cards.
    const float card_w = (w - 2 * pad - 2 * kPresetGapDip * s) / 3;
    const float mini_h = (card_w - 12.0f * s) * 10.0f / 16.0f;
    const float card_h = 6.0f * s + mini_h + 7.0f * s + kCardLabelDip * s + 8.0f * s;
    for (int i = 0; i < 3; ++i) {
        const float x = left + i * (card_w + kPresetGapDip * s);
        preset_rc_[static_cast<size_t>(i)] = D2D1::RectF(x, y, x + card_w, y + card_h);
    }
    y += card_h;
    section_y_ = y;
    y += kSectionDip * s;
    // Toggle tiles: 4 columns.
    const float tile_w = (w - 2 * pad - 3 * kToggleGapDip * s) / 4;
    for (int i = 0; i < kLayoutPanelToggleCount; ++i) {
        const float x = left + (i % 4) * (tile_w + kToggleGapDip * s);
        const float ty = y + (i / 4) * (kToggleDip + kToggleGapDip) * s;
        toggle_rc_[static_cast<size_t>(i)] = D2D1::RectF(x, ty, x + tile_w, ty + kToggleDip * s);
    }
    y += (2 * kToggleDip + kToggleGapDip) * s;
    // Segment rows: options right-aligned, each 11 px padding around its text.
    for (int r = 0; r < kLayoutPanelSegmentCount; ++r) {
        const float top = y + 10.0f * s, bottom = top + 26.0f * s;
        segment_y_[static_cast<size_t>(r)] = top;
        float widths[3]{};
        float total = 0.0f;
        for (int k = 0; k < kSegmentOptions[r]; ++k) {
            // The chosen label is drawn 13px semibold (Painter::SectionFormat).
            widths[k] = std::ceil(Measure(l10n::Get(kSegmentText[r][k]), caption_bold_.get()) * 1.05f) + 22.0f * s;
            total += widths[k];
        }
        float x = right - total;
        for (int k = 0; k < 3; ++k) {
            auto& rc = segment_rc_[static_cast<size_t>(r)][static_cast<size_t>(k)];
            if (k >= kSegmentOptions[r]) { rc = D2D1::RectF(); continue; }
            rc = D2D1::RectF(x, top, x + widths[k], bottom);
            x += widths[k];
        }
        y += kRowDip * s;
    }
    y += 14.0f * s;
    footer_y_ = y;
    const std::wstring more = l10n::Get(Sid::LayoutPanelMoreSettings);
    const float more_w = painter_.MeasureButtonWidth(more, L"\xE713");
    more_rc_ = D2D1::RectF(right - more_w, y + 10.0f * s, right, y + 40.0f * s);
}

D2D1_RECT_F LayoutPanel::CardRect() const {
    const float m = Margin();
    return D2D1::RectF(m, m, m + std::round(kWidthDip * scale_), m + std::round(PanelHeightDip() * scale_));
}

// Preset-card hover fades in/out over kHoverMs (instant without animations).
void LayoutPanel::StepHover(bool restart) {
    const ULONGLONG now = GetTickCount64();
    BOOL anim = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
    const float dt = restart || !hover_step_tick_ ? 16.0f : static_cast<float>(now - hover_step_tick_);
    hover_step_tick_ = now;
    bool moving = false;
    for (int i = 0; i < 3; ++i) {
        const float target = hover_.kind == HitKind::Preset && hover_.index == i ? 1.0f : 0.0f;
        float& t = hover_t_[static_cast<size_t>(i)];
        if (!anim) t = target;
        else if (t < target) t = (std::min)(target, t + dt / kHoverMs);
        else if (t > target) t = (std::max)(target, t - dt / kHoverMs);
        moving |= t != target;
    }
    if (moving) SetTimer(hwnd_, kHoverTimer, 15, nullptr);
    else KillTimer(hwnd_, kHoverTimer);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// Port of the installer's Wizard::MiniPreview (setup_ui.cpp), itself the
// mockup's .mini CSS: one unit u = width / 120 and a 16:10 window.
void DrawLayoutMiniPreview(ID2D1DeviceContext* dc, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box,
                           int mode, const Theme& theme, bool dark, float scale) {
    struct Cfg { bool sidebar, peek, lite, tabs, merge, toolbar, inline_cmd, status, tray, icons, roomy; };
    static const Cfg kCfg[] = {
        {false, true, true, false, true, false, false, false, false, true, true},     // zen
        {true, false, true, true, false, false, true, false, false, false, false},    // standard
        {true, false, false, true, false, true, false, true, true, false, false},     // pro
    };
    const Cfg& c = kCfg[std::clamp(mode, 0, 2)];
    const float W = box.right - box.left, H = W * 10.f / 16.f, u = W / 120.f;
    const float x0 = box.left, y0 = box.top;
    const D2D1_COLOR_F accent = theme.accent;
    const D2D1_COLOR_F sheet = LpHex(dark ? 0x202020 : 0xF7F8F4), title = LpHex(dark ? 0x1A1A1A : 0xF1F3EE),
        card = LpHex(dark ? 0x272727 : 0xFFFFFF), stroke = dark ? LpHex(0xFFFFFF, .07f) : LpHex(0xDCE3D9),
        track = dark ? LpHex(0xFFFFFF, .059f) : LpHex(0x000000, .047f), text3 = dark ? LpHex(0xFFFFFF, .40f) : LpHex(0x929CAE),
        tone = dark ? LpMix(accent, LpHex(0xFFFFFF), .70f) : LpMix(accent, LpHex(0x000000), .86f),
        flyout = LpHex(dark ? 0x2B2B2B : 0xFFFFFF), place = LpHex(dark ? 0xF3B744 : 0xC7860A),
        folder = LpHex(dark ? 0xF6C462 : 0xF0B84E);
    auto fill = [&](const D2D1_RECT_F& r, D2D1_COLOR_F col, float radius = 0) {
        brush->SetColor(col);
        if (radius > 0) dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush);
        else dc->FillRectangle(r, brush);
    };
    auto stroke_rc = [&](const D2D1_RECT_F& r, D2D1_COLOR_F col, float radius, float width) {
        brush->SetColor(col);
        const float h = width / 2;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + h, r.top + h, r.right - h, r.bottom - h),
                                                   radius, radius), brush, width);
    };
    const D2D1_RECT_F win{x0, y0, x0 + W, y0 + H};
    dc->PushAxisAlignedClip(win, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    fill(win, sheet, 4 * scale);

    const float mt = 9 * u, pad = 3 * u;
    fill({x0, y0, x0 + W, y0 + mt}, title);
    float x = x0 + pad;
    const float mid = y0 + mt / 2;
    fill({x, mid - 2.25f * u, x + 4.5f * u, mid + 2.25f * u}, LpMix(LpHex(0xC3C0FF), LpHex(0x4B3FC6), .4f), 1.2f * u);
    x += 7 * u;
    const float cap_w = 15 * u, right_edge = x0 + W - pad;
    fill({right_edge - cap_w, mid - 1.75f * u, right_edge, mid + 1.75f * u}, track, 1.5f * u);
    auto field = [&](float l, float r, float cy) {
        const D2D1_RECT_F f{l, cy - 2.5f * u, r, cy + 2.5f * u};
        fill(f, card, 1.4f * u);
        stroke_rc(f, stroke, 1.4f * u, (std::max)(.6f, .5f * u));
    };
    if (c.tabs && !c.merge) {
        const D2D1_RECT_F tab{x, y0 + mt - 6.5f * u, x + 20 * u, y0 + mt + 1.6f * u};
        fill(tab, sheet, 1.6f * u);
        fill({tab.left + 2.5f * u, y0 + mt - 1.9f * u, tab.left + 7.5f * u, y0 + mt - 1.f * u}, accent, .45f * u);
    }
    if (c.merge) {
        const float srch = 22 * u, fixed = srch + cap_w + 2 * 2.5f * u;
        const float addr = (std::min)(.48f * W, (std::max)(0.f, (right_edge - x - fixed - 2.5f * u) / 2));
        field(x, x + addr, mid);
        field(x + addr + 2.5f * u, x + addr + 2.5f * u + srch, mid);
    }
    float y = y0 + mt;
    if (!c.merge) {
        const float h = 8 * u, cy = y + h / 2, srch = 22 * u, gap = 2 * u;
        float r = x0 + W - pad;
        if (c.inline_cmd && !c.toolbar) {
            fill({r - 11 * u, cy - 2.5f * u, r, cy + 2.5f * u}, accent, 1.2f * u);
            r -= 11 * u + gap;
        }
        field(r - srch, r, cy);
        field(x0 + pad, r - srch - gap, cy);
        y += h;
    }
    if (c.toolbar) {
        const float h = 8 * u, cy = y + h / 2, gap = 2.2f * u, sq = 3.4f * u;
        float bx = x0 + pad;
        fill({bx, cy - 2.5f * u, bx + 15 * u, cy + 2.5f * u}, accent, 1.2f * u);
        bx += 15 * u + gap;
        auto square = [&](bool colored) {
            fill({bx, cy - sq / 2, bx + sq, cy + sq / 2}, colored ? LpAlpha(tone, .8f) : LpAlpha(text3, .55f), .8f * u);
            bx += sq + gap;
        };
        square(true); square(true); square(false); square(false); square(true);
        bx = x0 + W - pad - 2 * sq - gap;
        square(false); square(false);
        y += h;
    }
    const float status_h = c.status ? 5 * u : 0;
    const D2D1_RECT_F body{x0 + 2.5f * u, y, x0 + W - 2.5f * u, y0 + H - status_h - 2.5f * u};
    float pane_left = body.left;
    if (c.sidebar) {
        const float sw = (body.right - body.left) * (c.lite ? .19f : .22f);
        float sy = body.top + 1.5f * u;
        const float ih = 2.4f * u, gap = 2.1f * u;
        auto item = [&](float frac, D2D1_COLOR_F col, float extra_top = 0) {
            sy += extra_top;
            fill({body.left, sy, body.left + sw * frac, sy + ih}, col, 1.0f);
            sy += ih + gap;
        };
        if (c.lite) {
            const D2D1_COLOR_F t = LpAlpha(tone, .35f);
            for (float f : {.62f, .70f, .56f, .66f, .52f}) item(f, t);
            item(.60f, t, 1.5f * u);
            for (float f : {.86f, .70f}) {
                const float bh = 1.6f * u;
                fill({body.left, sy, body.left + sw * f, sy + bh}, track, 1);
                fill({body.left, sy, body.left + sw * f * .7f, sy + bh}, LpAlpha(place, .7f), 1);
                sy += bh + gap;
            }
        } else {
            const D2D1_COLOR_F t = LpAlpha(text3, .35f), head = LpAlpha(text3, .6f);
            item(.55f, head); item(.70f, t); item(.62f, LpAlpha(accent, .75f)); item(.76f, t);
            item(.55f, head, 1.5f * u); item(.58f, t); item(.66f, t);
            item(.55f, head, 1.5f * u); item(.50f, LpHex(0xE5484D, .6f)); item(.56f, LpHex(0x2FB55A, .6f));
            if (c.tray)
                fill({body.left, body.bottom - 5 * u, body.left + sw, body.bottom}, LpAlpha(accent, dark ? .15f : .095f), 1.2f * u);
        }
        pane_left = body.left + sw + 2 * u;
    }
    const D2D1_RECT_F pane{pane_left, body.top, body.right, body.bottom};
    fill(pane, card, 1.6f * u);
    stroke_rc(pane, stroke, 1.6f * u, (std::max)(.6f, .5f * u));
    if (c.icons) {
        const float p = 3.5f * u, cg = 2.5f * u, rg = 3 * u, ih = 6.5f * u;
        const float cw = (pane.right - pane.left - 2 * p - 4 * cg) / 5;
        for (int i = 0; i < 10; ++i) {
            const float ix = pane.left + p + (i % 5) * (cw + cg), iy = pane.top + p + (i / 5) * (ih + rg);
            if (iy + ih > pane.bottom - 1) break;
            // A folder (tab + body) at its own aspect, centred in the cell,
            // with the name line under it (the mockup's track shadow).
            const float fw = (std::min)(cw, 8.0f * u), fx = ix + (cw - fw) / 2;
            fill({fx, iy, fx + fw * .45f, iy + 2.0f * u}, LpAlpha(folder, .62f), .7f * u);
            fill({fx, iy + 1.1f * u, fx + fw, iy + ih}, LpAlpha(folder, .8f), u);
            const float lw = (std::min)(cw, fw + 1.0f * u);
            fill({ix + (cw - lw) / 2, iy + ih + .8f * u, ix + (cw + lw) / 2, iy + ih + 1.8f * u}, track, .5f * u);
        }
    } else {
        static const float widths[] = {.78f, .62f, .88f, .54f, .70f, .84f, .58f, .66f, .74f, .60f};
        const int rows = c.roomy ? 5 : 7;
        const float gap = (c.roomy ? 3.4f : 2.3f) * u, ih = 2.3f * u, p = 2.5f * u;
        float ry = pane.top + p;
        for (int i = 0; i < rows && ry + ih <= pane.bottom - p * .5f; ++i, ry += ih + gap)
            fill({pane.left + p, ry, pane.left + p + (pane.right - pane.left - 2 * p) * widths[i], ry + ih},
                 i == 2 ? LpAlpha(accent, .7f) : LpAlpha(text3, .32f), 1.0f);
    }
    if (c.status) {
        const float cy = y0 + H - status_h / 2;
        fill({x0 + pad, cy - .9f * u, x0 + pad + 10 * u, cy + .9f * u}, LpAlpha(text3, .4f), 1.0f);
        fill({x0 + pad + 13 * u, cy - .9f * u, x0 + pad + 20 * u, cy + .9f * u}, LpAlpha(text3, .4f), 1.0f);
    }
    if (!c.toolbar) {
        const float fw = 38 * u, fh = 7 * u, fx = x0 + (W - fw) / 2, fy = y0 + H - 4 * u - fh;
        fill({fx, fy + .6f * u, fx + fw, fy + fh + .6f * u}, LpHex(0x000000, dark ? .30f : .10f), 2 * u);
        fill({fx, fy, fx + fw, fy + fh}, flyout, 2 * u);
        stroke_rc({fx, fy, fx + fw, fy + fh}, stroke, 2 * u, (std::max)(.6f, .5f * u));
        float bx = fx + 3 * u;
        const float cy = fy + fh / 2, sq = 3.4f * u;
        fill({bx, cy - sq / 2, bx + 9 * u, cy + sq / 2}, accent, .8f * u);
        bx += 9 * u + 2.5f * u;
        for (int i = 0; i < 3; ++i, bx += sq + 2.5f * u)
            fill({bx, cy - sq / 2, bx + sq, cy + sq / 2}, LpAlpha(tone, .7f), .8f * u);
    }
    if (!c.sidebar && c.peek)
        fill({x0 + .6f * u, y0 + H * .56f - 5 * u, x0 + 1.6f * u, y0 + H * .56f + 5 * u}, LpAlpha(accent, .45f), u);
    dc->PopAxisAlignedClip();
    stroke_rc(win, stroke, 4 * scale, 1.0f);
}

void LayoutPanel::DrawMiniPreview(ID2D1DeviceContext* dc, const D2D1_RECT_F& box, int mode, const Theme& theme) {
    DrawLayoutMiniPreview(dc, brush_.get(), box, mode, theme, dark_, scale_);
}

void LayoutPanel::Render() {
    if (!compositor_.Dc()) return;
    if (compositor_.NeedsRecovery()) {
        if (!compositor_.Recover()) return;
        compositor_.RecreateTextFormats(scale_);
        brush_.reset();
    }
    RECT client{};
    GetClientRect(hwnd_, &client);
    compositor_.Resize((std::max)(1L, client.right), (std::max)(1L, client.bottom));
    Layout();

    auto* dc = compositor_.Dc();
    const bool hc = HighContrastOn();
    const Theme theme = CurrentTheme();
    const float s = scale_;
    // Card rect inside the shadow border; `w`/`h` stay the card edges.
    const D2D1_RECT_F card_rc = CardRect();
    const float cl = card_rc.left, ct = card_rc.top;
    const float w = card_rc.right, h = card_rc.bottom;
    const float pad = kPadDip * s;
    const float line_w = (std::max)(1.0f, std::floor(s));
    // mockup tokens: --track, --sel (accent tint), --accent-text.
    const D2D1_COLOR_F track = hc ? theme.bg : (dark_ ? LpHex(0xFFFFFF, .059f) : LpHex(0x000000, .035f));
    const D2D1_COLOR_F track_edge = hc ? theme.text : theme.stroke_card;
    const D2D1_COLOR_F sel = hc ? theme.fill_selected : LpAlpha(theme.accent, dark_ ? .20f : .11f);
    // Text on the accent tint: the accent itself on light (accent_text is the
    // light-on-dark tone), lifted on dark like Painter::DrawTrackSegment.
    const D2D1_COLOR_F accent_fg = hc ? theme.accent_text : dark_ ? LpMix(theme.accent, LpHex(0xFFFFFF), .7f) : theme.accent;

    dc->BeginDraw();
    dc->SetTransform(D2D1::Matrix3x2F::Identity());
    // Same card as FluentMenu / the color picker: drop shadow from the card
    // silhouette, then Painter::DrawMenuSurface (fill, radius, border).
    const float card_radius = theme.radius_flyout * s;
    const D2D1_COLOR_F card_fill = hc ? theme.surface_flyout : LpHex(dark_ ? 0x2B2B2B : 0xF9F9F9);
    const D2D1_COLOR_F card_border = hc ? theme.stroke_card : dark_ ? LpHex(0xFFFFFF, .055f) : LpHex(0x000000, .055f);
    dc->Clear(margin_ > 0 ? D2D1::ColorF(0, 0, 0, 0) : card_fill);
    if (!brush_.get()) dc->CreateSolidColorBrush(theme.text, &brush_);
    if (margin_ > 0) {
        ComPtr<ID2D1Image> target;
        dc->GetTarget(&target);
        ComPtr<ID2D1CommandList> silhouette;
        if (target.get() && SUCCEEDED(dc->CreateCommandList(&silhouette))) {
            dc->SetTarget(silhouette.get());
            brush_->SetColor(D2D1::ColorF(0, 0, 0, 1.0f));
            dc->FillRoundedRectangle(D2D1::RoundedRect(card_rc, card_radius, card_radius), brush_.get());
            silhouette->Close();
            dc->SetTarget(target.get());
            ComPtr<ID2D1Effect> shadow;
            if (SUCCEEDED(dc->CreateEffect(kLpShadowEffectClsid, &shadow))) {
                shadow->SetInput(0, silhouette.get());
                shadow->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, 7.0f);
                shadow->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.36f));
                dc->DrawImage(shadow.get(), D2D1::Point2F(0.0f, 4.0f * s));
            }
        }
    }
    painter_.SetCompositor(&compositor_);
    painter_.SetScale(s);
    painter_.BeginFrame(theme, hc);
    painter_.DrawMenuSurface(card_rc);

    auto text_at = [&](std::wstring_view txt, const D2D1_RECT_F& rc, IDWriteTextFormat* fmt, const D2D1_COLOR_F& col,
                       DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!fmt || txt.empty() || rc.right <= rc.left) return;
        // LumaText like the main window's text; DirectWrite when it is off or in high contrast.
        if (!hc && compositor_.DrawLumaText(txt, fmt, typography::SnapVerticalBounds(rc), col, card_fill, align))
            return;
        fmt->SetTextAlignment(align);
        brush_->SetColor(col);
        dc->DrawTextW(txt.data(), static_cast<UINT32>(txt.size()), fmt, rc, brush_.get(),
                      D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    };
    auto fill = [&](const D2D1_RECT_F& r, D2D1_COLOR_F col, float radius) {
        brush_->SetColor(col);
        dc->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), brush_.get());
    };
    auto inset_ring = [&](const D2D1_RECT_F& r, D2D1_COLOR_F col, float radius, float width) {
        brush_->SetColor(col);
        const float k = width / 2;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(r.left + k, r.top + k, r.right - k, r.bottom - k),
                                                   radius - k, radius - k), brush_.get(), width);
    };
    const int mode = std::clamp(state_.mode, 0, 2);
    const std::wstring mode_name = l10n::Get(kModeNames[mode]);

    // Header: 界面 / 当前：<b>X</b>, or 自定义 · 基于「X」改了 N 项, and 恢复.
    float y = ct + pad;
    text_at(l10n::Get(Sid::TitleBtnLayout), D2D1::RectF(cl + pad, y, w - pad, y + 20.0f * s), title_.get(), theme.text);
    {
        const float line_top = y + 22.0f * s, line_bottom = line_top + 18.0f * s;
        float x = cl + pad;
        std::wstring prefix = l10n::Get(Sid::LayoutPanelCurrent);
        if (!prefix.empty() && prefix.back() == L':') prefix += L' ';  // "Current: " (zh ends in a full-width colon)
        text_at(prefix, D2D1::RectF(x, line_top, w - pad, line_bottom), caption_.get(), theme.text_secondary);
        x += Measure(prefix, caption_.get());
        const std::wstring bold = state_.changed > 0 ? l10n::Get(Sid::LayoutPanelCustom) : mode_name;
        text_at(bold, D2D1::RectF(x, line_top, w - pad, line_bottom), caption_bold_.get(), theme.text);
        x += Measure(bold, caption_bold_.get());
        if (state_.changed > 0) {
            std::wstring rest = L" " + l10n::Get(Sid::LayoutPanelBasedOn);
            if (const size_t p = rest.find(L"{mode}"); p != std::wstring::npos) rest.replace(p, 6, mode_name);
            if (const size_t p = rest.find(L"{n}"); p != std::wstring::npos) rest.replace(p, 3, std::to_wstring(state_.changed));
            text_at(rest, D2D1::RectF(x, line_top, restore_rc_.left - 4.0f * s, line_bottom), caption_.get(),
                    theme.text_secondary);
            if (hover_.kind == HitKind::Restore) fill(restore_rc_, sel, 5.0f * s);
            text_at(l10n::Get(Sid::LayoutPanelRestore), restore_rc_, caption_.get(), accent_fg,
                    DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    }

    // Preset cards. Hover (eased): a stronger fill and accent-tinted ring,
    // and the thumbnail lifts and grows a little over a soft shadow.
    for (int i = 0; i < 3; ++i) {
        const D2D1_RECT_F rc = preset_rc_[static_cast<size_t>(i)];
        const bool current = i == mode;
        const bool on = current && state_.changed == 0;
        const float lin = hover_t_[static_cast<size_t>(i)];
        const float ht = hc ? (lin > 0.5f ? 1.0f : 0.0f) : 1.0f - (1.0f - lin) * (1.0f - lin) * (1.0f - lin);
        const bool down = pressed_.kind == HitKind::Preset && pressed_.index == i && hover_ == pressed_;
        const float radius = 10.0f * s;
        const D2D1_COLOR_F hover_bg = dark_ ? LpHex(0xFFFFFF, .095f) : LpHex(0x000000, .06f);
        fill(rc, on ? sel : hc && ht > 0 ? theme.fill_hover : LpLerp(track, hover_bg, ht), radius);
        if (on) inset_ring(rc, theme.accent, radius, 1.5f * s);
        else if (current) inset_ring(rc, LpAlpha(theme.accent, .55f + .3f * ht), radius, 1.0f * s);  // .pcard.base
        else inset_ring(rc, hc ? (ht > 0 ? theme.accent : track_edge) : LpLerp(track_edge, LpAlpha(theme.accent, .6f), ht),
                        radius, line_w);
        const D2D1_RECT_F mini = D2D1::RectF(rc.left + 6.0f * s, rc.top + 6.0f * s, rc.right - 6.0f * s, rc.top + 6.0f * s +
                                            (rc.right - rc.left - 12.0f * s) * 10.0f / 16.0f);
        D2D1_RECT_F shown = mini;
        if (!hc && ht > 0.0f) {
            const float grow = (down ? 0.5f : 2.0f) * s * ht, lift = (down ? 0.0f : 1.5f) * s * ht;
            shown = D2D1::RectF(mini.left - grow, mini.top - grow * 10.0f / 16.0f - lift,
                                mini.right + grow, mini.bottom + grow * 10.0f / 16.0f - lift);
            const float r4 = 4.0f * s;
            for (int k = 3; k >= 1; --k) {
                const float spread = static_cast<float>(k) * s;
                fill(D2D1::RectF(shown.left - spread * .5f, shown.top + 2.0f * s, shown.right + spread * .5f,
                                 shown.bottom + 1.5f * s + spread),
                     LpHex(0x000000, (dark_ ? .16f : .06f) * ht), r4 + spread);
            }
        }
        DrawMiniPreview(dc, shown, i, theme);
        if (!hc && ht > 0.0f && !on)
            inset_ring(shown, LpAlpha(theme.accent, .5f * ht), 4.0f * s, line_w);
        const float label_top = mini.bottom + 7.0f * s;
        const D2D1_RECT_F label = D2D1::RectF(rc.left + 8.0f * s, label_top, rc.right - 8.0f * s, label_top + kCardLabelDip * s);
        const float kw = Measure(kModeKeys[i], kbd_.get()) + 8.0f * s;
        const D2D1_RECT_F key = D2D1::RectF(label.right - kw, label.top + 1.0f * s, label.right, label.bottom - 1.0f * s);
        text_at(l10n::Get(kModeNames[i]), D2D1::RectF(label.left, label.top, key.left - 4.0f * s, label.bottom),
                caption_bold_.get(), theme.text);
        fill(key, theme.fill_input, 3.0f * s);
        inset_ring(key, track_edge, 3.0f * s, line_w);
        text_at(kModeKeys[i], key, kbd_.get(), theme.text_secondary, DWRITE_TEXT_ALIGNMENT_CENTER);
        if (on) {
            const D2D1_RECT_F ck = D2D1::RectF(rc.right - 28.0f * s, rc.top + 10.0f * s, rc.right - 10.0f * s, rc.top + 28.0f * s);
            brush_->SetColor(theme.accent);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F((ck.left + ck.right) / 2, (ck.top + ck.bottom) / 2),
                                          9.0f * s, 9.0f * s), brush_.get());
            painter_.DrawGlyph(L"\xE73E", D2D1::RectF(ck.left + 3.0f * s, ck.top + 3.0f * s, ck.right - 3.0f * s,
                                                      ck.bottom - 3.0f * s), D2D1::ColorF(0xFFFFFF));
        }
    }

    // 显示 · 点一下立即生效
    {
        const float top = section_y_ + 14.0f * s, bottom = top + 16.0f * s;
        text_at(l10n::Get(Sid::LayoutPanelShow), D2D1::RectF(cl + pad, top, w - pad, bottom), small_.get(), theme.text_secondary);
        text_at(l10n::Get(Sid::LayoutPanelShowHint), D2D1::RectF(cl + pad, top, w - pad, bottom), small_.get(),
                LpAlpha(theme.text_secondary, .75f), DWRITE_TEXT_ALIGNMENT_TRAILING);
    }
    for (int i = 0; i < kLayoutPanelToggleCount; ++i) {
        const D2D1_RECT_F rc = toggle_rc_[static_cast<size_t>(i)];
        const bool on = state_.toggles[static_cast<size_t>(i)];
        const bool hot = hover_.kind == HitKind::Toggle && hover_.index == i;
        const float radius = 9.0f * s;
        fill(rc, on ? sel : hot ? theme.fill_hover : track, radius);
        inset_ring(rc, on ? LpAlpha(theme.accent, .35f) : track_edge, radius, line_w);
        const D2D1_COLOR_F fg = on ? accent_fg : hot ? theme.text : theme.text_secondary;
        const float cx = (rc.left + rc.right) / 2;
        const float gy = rc.top + 12.0f * s;
        painter_.DrawGlyph(kToggles[i].glyph, D2D1::RectF(cx - 10.0f * s, gy, cx + 10.0f * s, gy + 20.0f * s), fg);
        text_at(l10n::Get(kToggles[i].label), D2D1::RectF(rc.left + 2.0f * s, gy + 24.0f * s, rc.right - 2.0f * s,
                gy + 40.0f * s), small_.get(), fg, DWRITE_TEXT_ALIGNMENT_CENTER);
    }

    // Segment rows.
    for (int r = 0; r < kLayoutPanelSegmentCount; ++r) {
        const float top = segment_y_[static_cast<size_t>(r)];
        const auto& rcs = segment_rc_[static_cast<size_t>(r)];
        text_at(l10n::Get(kSegmentLabels[r]), D2D1::RectF(cl + pad, top, rcs[0].left - 8.0f * s, top + 26.0f * s),
                body_.get(), theme.text);
        const int n = kSegmentOptions[r];
        const D2D1_RECT_F track_rc = D2D1::RectF(rcs[0].left, top, rcs[static_cast<size_t>(n - 1)].right, top + 26.0f * s);
        painter_.DrawSegmentedTrack(track_rc);
        for (int k = 0; k < n; ++k) {
            fluent::SegmentedItemSpec item{};
            item.bounds = rcs[static_cast<size_t>(k)];
            const std::wstring text = l10n::Get(kSegmentText[r][k]);
            item.text = text;
            item.shared_track = true;
            item.position = k == 0 ? fluent::SegmentPosition::First
                          : k == n - 1 ? fluent::SegmentPosition::Last : fluent::SegmentPosition::Middle;
            item.state.selected = state_.segments[static_cast<size_t>(r)] == k;
            item.state.checked = item.state.selected;
            item.state.hovered = hover_.kind == HitKind::Segment && hover_.index == r && hover_.value == k;
            item.state.pressed = item.state.hovered && pressed_ == hover_;
            painter_.DrawTrackSegment(item);
        }
    }

    // Footer strip: sheet background with a top divider.
    {
        // Square top, the card's rounded bottom corners (inset by the border).
        const float in = line_w;
        const D2D1_RECT_F strip = D2D1::RectF(cl + in, footer_y_, w - in, h - in);
        brush_->SetColor(theme.surface_sheet);
        dc->FillRoundedRectangle(D2D1::RoundedRect(strip, card_radius - in, card_radius - in), brush_.get());
        dc->FillRectangle(D2D1::RectF(strip.left, strip.top, strip.right, strip.top + card_radius), brush_.get());
        brush_->SetColor(theme.stroke_divider);
        dc->FillRectangle(D2D1::RectF(cl + in, footer_y_, w - in, footer_y_ + line_w), brush_.get());
        text_at(l10n::Get(Sid::LayoutPanelFootHint), D2D1::RectF(cl + pad, more_rc_.top, more_rc_.left - 8.0f * s, more_rc_.bottom),
                small_.get(), LpAlpha(theme.text_secondary, .75f));
        fluent::ButtonSpec more{};
        more.bounds = more_rc_;
        const std::wstring more_text = l10n::Get(Sid::LayoutPanelMoreSettings);
        more.text = more_text;
        more.glyph = L"\xE713";
        more.state.hovered = hover_.kind == HitKind::MoreSettings;
        more.state.pressed = more.state.hovered && pressed_.kind == HitKind::MoreSettings;
        painter_.DrawButton(more);
    }

    // Re-stroke the card border over the footer strip (DrawMenuSurface's stroke).
    painter_.StrokeRoundedRect(card_rc, card_radius, card_border);
    const HRESULT hr = dc->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET || hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
        compositor_.NotifyDeviceLost(hr);
    else
        compositor_.Present();
}

LayoutPanel::Hit LayoutPanel::HitTest(float x, float y) const {
    if (state_.changed > 0 && InsideRect(restore_rc_, x, y)) return {HitKind::Restore, state_.mode, -1};
    for (int i = 0; i < 3; ++i)
        if (InsideRect(preset_rc_[static_cast<size_t>(i)], x, y)) return {HitKind::Preset, i, -1};
    for (int i = 0; i < kLayoutPanelToggleCount; ++i)
        if (InsideRect(toggle_rc_[static_cast<size_t>(i)], x, y)) return {HitKind::Toggle, i, -1};
    for (int r = 0; r < kLayoutPanelSegmentCount; ++r)
        for (int k = 0; k < kSegmentOptions[r]; ++k)
            if (InsideRect(segment_rc_[static_cast<size_t>(r)][static_cast<size_t>(k)], x, y))
                return {HitKind::Segment, r, k};
    if (InsideRect(more_rc_, x, y)) return {HitKind::MoreSettings, -1, -1};
    return {};
}

void LayoutPanel::OnClick(const Hit& hit) {
    switch (hit.kind) {
    case HitKind::Restore:
    case HitKind::Preset:
        if (callbacks_.on_preset) callbacks_.on_preset(hit.index);
        break;
    case HitKind::Toggle:
        if (callbacks_.on_toggle) callbacks_.on_toggle(static_cast<LayoutPanelToggle>(hit.index));
        break;
    case HitKind::Segment:
        if (callbacks_.on_segment) callbacks_.on_segment(static_cast<LayoutPanelSegment>(hit.index), hit.value);
        break;
    case HitKind::MoreSettings: {
        auto fn = callbacks_.on_more_settings;
        Hide();
        if (fn) fn();
        break;
    }
    case HitKind::None:
        break;
    }
}

LRESULT CALLBACK LayoutPanel::WndProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    LayoutPanel* self = nullptr;
    if (message == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        self = static_cast<LayoutPanel*>(cs->lpCreateParams);
        self->hwnd_ = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    } else {
        self = reinterpret_cast<LayoutPanel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self) return self->HandleMessage(message, wparam, lparam);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT LayoutPanel::HandleMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_CREATE:
        scale_ = static_cast<float>(pulse::compat::WindowDpi(owner_ ? owner_ : hwnd_)) / 96.0f;
        if (!compositor_.Init(hwnd_)) return -1;
        compositor_.SetSyncInterval(0);
        compositor_.RecreateTextFormats(scale_);
        painter_.SetCompositor(&compositor_);
        painter_.SetScale(scale_);
        RecreateFormats();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wparam) == WA_INACTIVE) Hide();
        return 0;
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    case WM_PAINT: {
        PAINTSTRUCT ps{};
        BeginPaint(hwnd_, &ps);
        Render();
        EndPaint(hwnd_, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        if (!tracking_leave_) {
            TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd_, 0};
            tracking_leave_ = TrackMouseEvent(&tme) != FALSE;
        }
        const Hit hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (!(hit == hover_)) {
            const bool preset_changed = hit.kind == HitKind::Preset || hover_.kind == HitKind::Preset;
            hover_ = hit;
            if (preset_changed) StepHover(true);
            else InvalidateRect(hwnd_, nullptr, FALSE);
        }
        SetCursor(LoadCursorW(nullptr, hit.kind == HitKind::None ? IDC_ARROW : IDC_HAND));
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) return TRUE;  // WM_MOUSEMOVE picks the cursor
        break;
    case WM_MOUSELEAVE:
        tracking_leave_ = false;
        if (hover_.kind != HitKind::None) {
            const bool preset = hover_.kind == HitKind::Preset;
            hover_ = Hit{};
            if (preset) StepHover(true);
            else InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return 0;
    case WM_TIMER:
        if (wparam == kHoverTimer) {
            StepHover(false);
            return 0;
        }
        break;
    case WM_NCHITTEST: {
        // The shadow border is click-through (to the owner below, which then
        // takes the activation and closes the panel).
        if (margin_ <= 0) break;
        POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        ScreenToClient(hwnd_, &pt);
        const D2D1_RECT_F card = CardRect();
        if (!InsideRect(card, static_cast<float>(pt.x), static_cast<float>(pt.y))) return HTTRANSPARENT;
        return HTCLIENT;
    }
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        pressed_ = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP: {
        const Hit pressed = pressed_;
        pressed_ = Hit{};
        if (GetCapture() == hwnd_) ReleaseCapture();
        const Hit hit = HitTest(static_cast<float>(GET_X_LPARAM(lparam)), static_cast<float>(GET_Y_LPARAM(lparam)));
        if (pressed.kind != HitKind::None && hit == pressed) OnClick(hit);
        if (IsOpen()) InvalidateRect(hwnd_, nullptr, FALSE);
        return 0;
    }
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE) {
            Hide();
            return 0;
        }
        break;
    case WM_SYSKEYDOWN: {
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if (wparam == L'L' && !shift && !ctrl) {  // Alt+L again closes it
            Hide();
            return 0;
        }
        if (shift && !ctrl && wparam >= L'1' && wparam <= L'3') {
            if (callbacks_.on_preset) callbacks_.on_preset(static_cast<int>(wparam - L'1'));
            return 0;
        }
        if (wparam == VK_F4) {
            Hide();
            return 0;
        }
        break;
    }
    case WM_SYSCHAR:
        return 0;  // no beep for the Alt keys above
    case WM_CAPTURECHANGED:
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd_, message, wparam, lparam);
}

}  // namespace pulse::ui