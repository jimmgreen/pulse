// ui_renderer.cpp — Chrome orchestration (title, toolbar, status, render).
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {
MainRenderer::MainRenderer() = default;

void MainRenderer::SetCompositor(Compositor* comp) {
    ClearTextWidthCache();
    sized_icon_formats_.clear();
    empty_state_svg_.reset();
    no_selection_svg_.reset();
    recent_empty_svg_.reset();
    starred_empty_svg_.reset();
    exclude_empty_svg_.reset();
    fluent_svgs_.clear();
    fluent_svg_failed_.clear();
    empty_state_svg_dc_.reset();
    compositor_ = comp;
    material_.SetCompositor(comp);
    painter_.SetCompositor(comp);
    if (!comp) {
        icon_cache_.Reset();
        thumbnail_cache_.Reset();
        preview_handler_.Reset();
        preview_mono_format_.reset();
    }
    else { icon_cache_.SetDeviceContext(comp->Dc()); thumbnail_cache_.SetDeviceContext(comp->Dc()); }
}

void MainRenderer::InvalidateTypography() {
    ClearTextWidthCache();
    sized_icon_formats_.clear();
    preview_mono_format_.reset();
    painter_.InvalidateTypography();
}
void MainRenderer::SetIconNotifyWindow(HWND hwnd) {
    notify_hwnd_ = hwnd;
    icon_cache_.SetNotifyWindow(hwnd);
    thumbnail_cache_.SetNotifyWindow(hwnd);
    preview_handler_.SetNotifyWindow(hwnd);
}

void MainRenderer::SetScale(float scale) {
    if (scale_ != scale) {
        preview_mono_format_.reset();
        sized_icon_formats_.clear();
        ClearTextWidthCache();
    }
    scale_ = scale;
    title_bar_height_ = kTitleBarHeight * scale;
    toolbar_height_ = 44.0f * scale;
    status_height_ = 28.0f * scale;
    sidebar_width_ = 224.0f * scale;
    pane_header_height_ = 40.0f * scale;
    column_header_height_ = 32.0f * scale;
    row_height_ = row_height_dip_ * scale;
    margin_ = 4.0f * scale;
    control_gap_ = 4.0f * scale;
    painter_.SetScale(scale);
    icon_cache_.SetScale(scale);
}

float MainRenderer::EffectiveSidebarWidth(float window_width) const {
    return window_width < 900.0f * scale_ ? 48.0f * scale_ : sidebar_width_;
}

D2D1_RECT_F MainRenderer::ContentRect(float w, float h) const {
    float left = EffectiveSidebarWidth(w) + margin_;
    float top = title_bar_height_ + toolbar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    return D2D1::RectF(left, top, w - margin_ - DetailsPanelWidth(w), bottom);
}
D2D1_RECT_F MainRenderer::TitleBarRect(float w) const {
    return D2D1::RectF(0, 0, w, title_bar_height_);
}

D2D1_RECT_F MainRenderer::ToolbarRect(float w) const {
    return D2D1::RectF(0, title_bar_height_, w, title_bar_height_ + toolbar_height_);
}

D2D1_RECT_F MainRenderer::AddressBarRect(float w) const {
    const bool compact = w < 900.0f * scale_;
    const float nav_count = compact ? 3.0f : 4.0f;
    float x = margin_ + 34.0f * scale_ * nav_count + margin_;
    float right = w - margin_;
    float actions = NewButtonWidthPx(compact) + margin_;
    if (!compact) {
        actions += (12.0f + 5.0f * kCommandIconStepDip + 12.0f + 2.0f * kCommandIconStepDip)
            * scale_;
    }
    float addrW = std::max(104.0f * scale_, right - x - actions);
    return D2D1::RectF(x, title_bar_height_ + 4 * scale_,
                       x + addrW, title_bar_height_ + toolbar_height_ - 4 * scale_);
}

float MainRenderer::NewButtonWidthPx(bool compact) const {
    if (compact) return kCommandIconButtonDip * scale_;
    return painter_.MeasureButtonWidth(
        pulse::l10n::Get(pulse::l10n::StringId::New), kIconAdd, true);
}

std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path) {
    std::vector<BreadcrumbSegment> out;
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    if (p.empty()) {
        // Empty path = This PC: a single segment that navigates to "".
        BreadcrumbSegment seg;
        seg.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        seg.path = L"";
        out.push_back(seg);
        return out;
    }
    if (p == L"pulse:recycle") {
        BreadcrumbSegment pc;
        pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        pc.path = L"";
        out.push_back(pc);
        BreadcrumbSegment bin;
        bin.text = pulse::l10n::Get(pulse::l10n::StringId::RecycleBin);
        bin.path = L"pulse:recycle";
        out.push_back(bin);
        return out;
    }

    std::wstring prefix; // full path of the segments emitted so far
    size_t i = 0;
    if (p.size() >= 2 && p[1] == L':') {
        // Explorer-style: drives live under This PC.
        BreadcrumbSegment pc;
        pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        pc.path = L"";
        out.push_back(pc);
        // Drive root, e.g. "C:\": single segment with the drive icon text.
        prefix = p.substr(0, 2);
        BreadcrumbSegment seg;
        seg.text = prefix;
        seg.path = prefix + L"\\";
        out.push_back(seg);
        i = 2;
        while (i < p.size() && p[i] == L'\\') ++i;
        prefix += L"\\";
    } else if (p.size() >= 2 && p[0] == L'\\' && p[1] == L'\\') {
        // UNC: split the root into a server segment and a share segment.
        auto s3 = p.find(L'\\', 2);           // after server
        std::wstring server = (s3 == std::wstring::npos) ? p.substr(2)
                                                         : p.substr(2, s3 - 2);
        BreadcrumbSegment srv;
        srv.text = server;
        srv.path = L"\\\\" + server;
        out.push_back(srv);
        prefix = srv.path;
        if (s3 == std::wstring::npos) {
            i = p.size();
        } else {
            auto s4 = p.find(L'\\', s3 + 1);  // after share
            std::wstring share = (s4 == std::wstring::npos) ? p.substr(s3 + 1)
                                                            : p.substr(s3 + 1, s4 - s3 - 1);
            if (!share.empty()) {
                BreadcrumbSegment seg;
                seg.text = share;
                seg.path = prefix + L"\\" + share;
                out.push_back(seg);
                prefix = seg.path;
            }
            i = (s4 == std::wstring::npos) ? p.size() : s4 + 1;
        }
    }
    while (i <= p.size() && i < p.size()) {
        auto sep = p.find(L'\\', i);
        std::wstring part = (sep == std::wstring::npos) ? p.substr(i)
                                                        : p.substr(i, sep - i);
        if (!part.empty()) {
            if (!prefix.empty() && prefix.back() != L'\\') prefix += L'\\';
            prefix += part;
            BreadcrumbSegment seg;
            seg.text = part;
            seg.path = prefix;
            out.push_back(seg);
        }
        if (sep == std::wstring::npos) break;
        i = sep + 1;
    }
    return out;
}

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out) const {
    out.clear();
    auto segments = SplitBreadcrumb(vm.path);
    if (segments.empty()) return;
    D2D1_RECT_F addr = AddressBarRect(w);
    const float segPad = 8.0f * scale_;
    const float chevronW = 14.0f * scale_;
    const float hint = painter_.OmnibarHintReservePx();
    const float avail = std::max(0.0f, addr.right - addr.left - 2 * margin_ - hint);

    IDWriteFactory3* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->AddressFormat() : nullptr;

    struct Measured { float w; };
    std::vector<float> widths(segments.size(), 0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        widths[i] = MeasureTextWidth(dwrite, fmt, segments[i].text) + segPad * 2;
        total += widths[i] + (i ? chevronW : 0.0f);
    }
    // Collapse leading segments until the rest fits (last segment always kept).
    size_t first = 0;
    while (total > avail && first + 1 < segments.size()) {
        total -= widths[first] + chevronW;
        ++first;
    }

    float x = addr.left + margin_;
    for (size_t i = first; i < segments.size(); ++i) {
        if (i > first) x += chevronW;
        BreadcrumbPlaced p;
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + widths[i], addr.bottom - 2 * scale_);
        p.text = segments[i].text;
        p.path = segments[i].path;
        out.push_back(p);
        x += widths[i];
    }
}
void MainRenderer::DrawTextRect(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt,
    ID2D1SolidColorBrush* br, std::wstring_view text, float x, float y, float w, float h,
    D2D1_DRAW_TEXT_OPTIONS opts) {
    D2D1_RECT_F rc = typography::SnapVerticalBounds(D2D1::RectF(x, y, x + w, y + h));
    if (!IsHighContrast() && compositor_ && br && compositor_->DrawLumaText(
            text, fmt, rc, br->GetColor(), text_background_, fmt->GetTextAlignment())) {
        return;
    }
    dc->DrawText(text.data(), (UINT32)text.size(), fmt, &rc, br, opts, DWRITE_MEASURING_MODE_NATURAL);
}
void MainRenderer::UpdateBrushes(const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    MakeBrush(dc, theme.bg, brBg_);
    MakeBrush(dc, theme.text, brText_);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    MakeBrush(dc, theme.text_disabled, brTextDisabled_);
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.fill_pressed, brFillPressed_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    MakeBrush(dc, theme.fill_input, brFillInput_);
    MakeBrush(dc, theme.stroke_card, brStrokeCard_);
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    MakeBrush(dc, theme.accent, brAccent_);
    MakeBrush(dc, theme.accent_hover, brAccentHover_);
    MakeBrush(dc, theme.accent_text, brAccentText_);
    MakeBrush(dc, theme.danger, brDanger_);
    MakeBrush(dc, theme.danger_hover, brDangerHover_);
    MakeBrush(dc, theme.scrollbar_thumb, brScrollbar_);
    MakeBrush(dc, theme.icon_folder, brIconFolder_);
    MakeBrush(dc, theme.icon_file, brIconFile_);
    MakeBrush(dc, theme.fps_bg, brFpsBg_);
    MakeBrush(dc, theme.fps_text, brFpsText_);
}

void MainRenderer::DrawIconText(float x, float y, float w, float h,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& color, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteTextFormat* iconFmt = compositor_->IconFormat();
    std::wstring txt = glyph;
    IDWriteTextFormat* fmt = iconFmt;
    if (!fmt) {
        fmt = compositor_->TextFormat();
        txt = fallback;
    }
    if (iconFmt && size_factor != 1.0f) {
        const int key = static_cast<int>(std::lround(size_factor * 1000.0f));
        auto cached = sized_icon_formats_.find(key);
        if (cached == sized_icon_formats_.end()) {
            ComPtr<IDWriteTextFormat> created;
            const float size = 16.0f * scale_ * size_factor;
            typography::CreateTextFormat(compositor_->DwriteFactory(),
                {typography::FontRole::Icon, size}, &created);
            if (created.get()) {
                created->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                created->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                cached = sized_icon_formats_.emplace(key, std::move(created)).first;
            }
        }
        if (cached != sized_icon_formats_.end()) fmt = cached->second.get();
    }
    MakeBrush(dc, color, brText_);
    DrawTextRect(dc, fmt, brText_.get(), txt, x, y, w, h);
}

void MainRenderer::DrawButton(const D2D1_RECT_F& rc, const Theme& theme, const D2D1_COLOR_F& bg,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& fg, bool /*round_right*/, bool /*round_left*/, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (bg.a > 0.0f) { // transparent = resting state; hover fill is drawn on interaction only
        MakeBrush(dc, bg, brFillHover_);
        float r = theme.radius_control * scale_;
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, r);
    }
    DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, glyph, fallback, fg,
        size_factor);
}

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
    UpdateBrushes(theme);
    text_background_ = theme.bg;
    painter_.BeginFrame(theme, IsHighContrast());

    // None effect + selected image: the wallpaper itself covers the window
    // base; a scrim plus the translucent panels (backdrop_enabled) let it
    // show through.
    const bool image_mode = vm.window_effect == WindowEffect::None &&
                            !vm.background_image.empty() && !IsHighContrast();
    bool backdrop_drawn = false;
    if (image_mode) {
        backdrop_drawn = material_.DrawSourceCover(dc, rect, vm.background_image);
    } else {
        backdrop_drawn = material_.DrawBackdrop(
            dc, rect, vm.window_effect, vm.dark,
            (vm.window_effect == WindowEffect::None) ? std::wstring{} : vm.background_image);
    }
    D2D1_COLOR_F micaTint = theme.bg;
    if (image_mode) {
        // Decode failure must fall back to opaque, never a hole to the desktop.
        micaTint.a = backdrop_drawn ? (vm.dark ? 0.55f : 0.60f) : 1.0f;
    } else if (backdrop_drawn) {
        micaTint.a = 0.0f;
    } else if (vm.backdrop_enabled) {
        micaTint.a = vm.dark ? 0.72f : 0.78f;
    }
    if (micaTint.a > 0.0f) {
        MakeBrush(dc, micaTint, brBg_);
        FillRect(dc, brBg_.get(), rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top);
    }

    DrawTitleBar(vm, rect, theme);
    if (vm.settings_open) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        DrawSettings(vm, rect, theme);
        DrawStatusBar(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        DrawSidebar(vm, rect, theme);
        DrawPane(vm, rect, theme);
        if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
        else preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                                   theme.bg, theme.text, false);
        DrawStatusBar(vm, rect, theme);
    }

    // Drag action badge (ui.md §7.8): tooltip-style flyout near the cursor.
    if (!vm.drag_badge.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt,
                                     vm.drag_badge);
        float bw = tw + 20 * scale_;
        float bh = 24 * scale_;
        float bx = std::min(vm.drag_badge_x + 14 * scale_, rect.right - bw - margin_);
        float by = std::min(vm.drag_badge_y + 16 * scale_, rect.bottom - bh - margin_);
        D2D1_RECT_F brc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), brc.left, brc.top, bw, bh, 4 * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(brc, 4 * scale_, 4 * scale_), brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        const D2D1_COLOR_F saved_badge_bg = text_background_;
        text_background_ = BlendOver(theme.surface_flyout, theme.bg);
        DrawTextRect(dc, fmt, brText_.get(), vm.drag_badge, bx + 10 * scale_, by, tw, bh);
        text_background_ = saved_badge_bg;
    }

    if (vm.drag_badge.empty() && !vm.tooltip_text.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        const float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt,
                                           vm.tooltip_text);
        const float bw = std::min(tw + 20.0f * scale_, rect.right - 16.0f * scale_);
        const float bh = 28.0f * scale_;
        const float bx = std::clamp(vm.tooltip_x + 12.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.right - bw - 8.0f * scale_));
        const float by = std::clamp(vm.tooltip_y + 18.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.bottom - bh - 8.0f * scale_));
        const D2D1_RECT_F tipRc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), bx, by, bw, bh, 4.0f * scale_);
        MakeBrush(dc, theme.stroke_card, brStrokeCard_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(tipRc, 4.0f * scale_, 4.0f * scale_),
            brStrokeCard_.get(), 1.0f);
        MakeBrush(dc, theme.text, brText_);
        const D2D1_COLOR_F saved_tip_bg = text_background_;
        text_background_ = BlendOver(theme.surface_flyout, theme.bg);
        DrawTextRect(dc, fmt, brText_.get(), vm.tooltip_text,
            bx + 10.0f * scale_, by, bw - 20.0f * scale_, bh);
        text_background_ = saved_tip_bg;
    }

}

ID2D1Bitmap* MainRenderer::LogoBitmap() {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return nullptr;
    if (logo_bitmap_.get() && logo_dc_ == dc && std::abs(logo_scale_ - scale_) <= 0.001f) {
        return logo_bitmap_.get();
    }
    logo_bitmap_.reset();
    logo_dc_ = nullptr;
    // Decode well above the on-screen size so the mark stays crisp at high DPI.
    const int px = std::max(32, static_cast<int>(48.0f * scale_ + 0.5f));
    HICON icon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr),
        MAKEINTRESOURCEW(IDI_PULSE), IMAGE_ICON, px, px, LR_DEFAULTCOLOR));
    if (!icon) return nullptr;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmap> wicBitmap;
    ComPtr<IWICFormatConverter> converter;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&wic))) &&
        SUCCEEDED(wic->CreateBitmapFromHICON(icon, &wicBitmap)) &&
        SUCCEEDED(wic->CreateFormatConverter(&converter)) &&
        SUCCEEDED(converter->Initialize(wicBitmap.get(), GUID_WICPixelFormat32bppPBGRA,
                                        WICBitmapDitherTypeNone, nullptr, 0.0,
                                        WICBitmapPaletteTypeMedianCut))) {
        dc->CreateBitmapFromWicBitmap(converter.get(), nullptr, &logo_bitmap_);
    }
    DestroyIcon(icon);
    if (logo_bitmap_.get()) {
        logo_dc_ = dc;
        logo_scale_ = scale_;
    }
    return logo_bitmap_.get();
}

void MainRenderer::DrawTitleBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float y = 0.0f;
    const float h = title_bar_height_;
    const float right = rect.right;
    const bool compact = TitleBarCompact(rect.right, scale_, vm.tabs.size());

    // Product mark: the packaged app icon; the monogram is the fallback.
    float x = 12.0f * scale_;
    const float mark = 20.0f * scale_;
    const float markY = (h - mark) * 0.5f;
    if (ID2D1Bitmap* logo = LogoBitmap()) {
        dc->DrawBitmap(logo, D2D1::RectF(x, markY, x + mark, markY + mark), 1.0f,
                       D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC, nullptr, nullptr);
    } else {
        MakeBrush(dc, theme.accent, brAccent_);
        dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x + mark * 0.5f, markY + mark * 0.5f),
                                      mark * 0.5f, mark * 0.5f), brAccent_.get());
        ComPtr<IDWriteTextFormat> markFmt;
        typography::CreateTextFormat(compositor_->DwriteFactory(),
            {typography::FontRole::Display, 11.0f * scale_, DWRITE_FONT_WEIGHT_SEMI_BOLD},
            &markFmt);
        if (markFmt.get()) {
            markFmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            markFmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            MakeBrush(dc, theme.accent_text, brAccentText_);
            DrawTextRect(dc, markFmt.get(), brAccentText_.get(), L"P", x, markY, mark, mark);
        }
    }
    x += mark + 8.0f * scale_;
    if (!compact) {
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, compositor_->HeaderFormat(), brTextSecondary_.get(), L"Pulse",
            x, 0.0f, 64.0f * scale_, h);
        x += 72.0f * scale_;
    } else {
        x += 4.0f * scale_;
    }

    const float ctrlW = 46.0f * scale_;
    const TitleChrome chrome = MakeTitleChrome(right, scale_, h);

    const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
    const float tabH = strip.h;
    const float tabY = strip.y;
    auto drawTab = [&](size_t i, float left, bool raised) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = pinned ? kTabPinnedW * scale_ : strip.w;
        const bool hovered = IsHovered(vm, HitTestResult::Tab, static_cast<int>(i)) ||
                             IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i));
        const bool connect = active || raised;
        ChromeTabShape shape;
        shape.top_radius = theme.radius_control * scale_;
        shape.bottom_radius = 8.0f * scale_;
        shape.connect_bottom = connect;
        const float tabTop = tabY;
        const float tabBottom = connect ? (h + 1.0f) : (tabY + tabH);
        const D2D1_RECT_F tabRc = D2D1::RectF(left, tabTop, left + tabW, tabBottom);
        if (raised) {
            D2D1_RECT_F shadow = tabRc;
            shadow.left += 1.0f * scale_;
            shadow.top += 2.0f * scale_;
            shadow.right -= 1.0f * scale_;
            shadow.bottom += 1.0f * scale_;
            MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, vm.dark ? 0.09f : 0.07f), brFillPressed_);
            FillChromeTab(dc, brFillPressed_.get(), shadow, shape);
            D2D1_COLOR_F fill = theme.header_bg;
            if (vm.backdrop_enabled) fill.a = vm.dark ? 0.78f : 0.84f;
            MakeBrush(dc, fill, brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else if (active) {
            D2D1_COLOR_F fill = theme.header_bg;
            if (vm.backdrop_enabled) fill.a = vm.dark ? 0.78f : 0.84f;
            MakeBrush(dc, fill, brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else {
            // Grouped tabs get a tinted body; ungrouped keep the stock look.
            const bool has_color = vm.tabs[i].color_rgb != 0;
            if (has_color) {
                D2D1_COLOR_F tint = HexColor(vm.tabs[i].color_rgb);
                tint.a *= hovered ? 0.16f : 0.10f;
                MakeBrush(dc, tint, brFillHover_);
            } else {
                MakeBrush(dc, hovered ? theme.fill_hover : theme.tab_bg, brFillHover_);
            }
            FillChromeTab(dc, brFillHover_.get(), tabRc, shape);
        }
        // Grouped tabs draw the same top strip as the ungrouped active tab,
        // just in their group color instead of the accent.
        const float r = shape.top_radius;
        const bool has_color = vm.tabs[i].color_rgb != 0;
        if (has_color) {
            D2D1_COLOR_F line = HexColor(vm.tabs[i].color_rgb);
            if (!active) line.a *= 0.55f;
            MakeBrush(dc, line, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        } else if (active) {
            MakeBrush(dc, theme.accent, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        }
        if (pinned) {
            // Chrome pinned tab: centered icon, no title, no close button.
            DrawIconText(left, tabY, tabW, tabH,
                vm.tabs[i].title.empty() ? kIconFolder
                    : vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                        ? kIconSettings : kIconFolder, L"[]",
                active ? theme.icon_folder : theme.text_secondary, 0.85f);
            return;
        }
        DrawIconText(left + 6.0f * scale_, tabY, 16.0f * scale_, tabH,
            vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                ? kIconSettings : kIconFolder, L"[]",
            active ? theme.icon_folder : theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_;
        DrawTabTitle(dc, compositor_->DwriteFactory(), compositor_->TabFormat(), brText_.get(),
                     vm.tabs[i].title, titleLeft, tabY,
                     std::max(0.0f, tabW - 24.0f * scale_ - closeReserve), tabH, scale_);
        if (show_close) {
            const float closeY = tabY + (tabH - closeSz) * 0.5f;
            const float closeX = left + tabW - closePad - closeSz;
            if (IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i))) {
                MakeBrush(dc, theme.fill_hover, brFillPressed_);
                FillRoundedRect(dc, brFillPressed_.get(), closeX, closeY, closeSz, closeSz, r);
            }
            DrawIconText(closeX, closeY, closeSz, closeSz,
                kIconCloseSmall, L"x", theme.text_secondary, 0.62f);
        }
    };
    const int dragI = vm.tab_drag_index;
    // Group chips sit at run starts: Edge-style solid blocks, not pill badges.
    auto brighten = [&](const D2D1_COLOR_F& c) {
        // Toward white (dark theme) or black (light) for readable chip text.
        D2D1_COLOR_F out = c;
        const float t = 0.35f;
        const float target = vm.dark ? 1.0f : 0.0f;
        out.r += (target - out.r) * t;
        out.g += (target - out.g) * t;
        out.b += (target - out.b) * t;
        out.a = 1.0f;
        return out;
    };
    for (const auto& chip : strip.chips) {
        if (chip.group < 0 || chip.group >= static_cast<int>(vm.tab_groups.size())) continue;
        const TabGroupView& gv = vm.tab_groups[static_cast<size_t>(chip.group)];
        const D2D1_COLOR_F gc = HexColor(gv.color_rgb);
        const float ch = strip.h - 8.0f * scale_;
        // Chip drag: the group's chip floats with its run (alone if collapsed).
        float chipLeft = chip.left + gv.x_offset;
        if (vm.tab_drag_chip && dragI >= 0 && dragI < static_cast<int>(vm.tabs.size()) &&
            vm.tabs[static_cast<size_t>(dragI)].group == chip.group) {
            chipLeft = gv.collapsed ? vm.tab_drag_x
                                    : vm.tab_drag_x - chip.width - 4.0f * scale_;
        }
        const D2D1_RECT_F rc = D2D1::RectF(chipLeft, strip.y + 4.0f * scale_,
                                           chipLeft + chip.width,
                                           strip.y + 4.0f * scale_ + ch);
        const bool chip_hovered = IsHovered(vm, HitTestResult::TabGroup, chip.group);
        const bool named = !gv.name.empty();
        D2D1_COLOR_F fill = gc;
        fill.a *= named ? (chip_hovered ? 0.42f : 0.32f)
                        : (chip_hovered ? 1.0f : 0.85f);
        MakeBrush(dc, fill, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, chip.width, ch,
                        5.0f * scale_);
        if (named) {
            MakeBrush(dc, brighten(gc), brText_);
            DrawTextRect(dc, compositor_->SmallFormat(), brText_.get(), gv.name,
                rc.left + 8.0f * scale_, rc.top, chip.width - 16.0f * scale_, ch);
        }
    }
    int activeI = -1;
    const int dragN = std::max(1, vm.tab_drag_count);
    auto inDragRun = [&](int i) { return dragI >= 0 && i >= dragI && i < dragI + dragN; };
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].hidden) continue;
        if (inDragRun(static_cast<int>(i))) continue;
        if (vm.tabs[i].active) {
            activeI = static_cast<int>(i);
            continue;
        }
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        drawTab(i, left, false);
    }
    if (activeI >= 0 && !inDragRun(activeI) && !vm.tabs[static_cast<size_t>(activeI)].hidden) {
        const size_t i = static_cast<size_t>(activeI);
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        const float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        drawTab(i, left, false);
    }
    // The dragged run floats as one block (browser group drag); collapsed
    // members stay hidden and do not take float width.
    if (dragI >= 0 && dragI < static_cast<int>(vm.tabs.size())) {
        float floatX = vm.tab_drag_x;
        for (int k = 0; k < dragN && dragI + k < static_cast<int>(vm.tabs.size()); ++k) {
            if (vm.tabs[static_cast<size_t>(dragI + k)].hidden) continue;
            drawTab(static_cast<size_t>(dragI + k), floatX, true);
            floatX += strip.pitch;
        }
    }

    auto tab_left_at = [&](int i) -> float {
        if (inDragRun(i)) return vm.tab_drag_x + static_cast<float>(i - dragI) * strip.pitch;
        const float extra = i < static_cast<int>(strip.extra.size())
            ? strip.extra[static_cast<size_t>(i)] : 0.0f;
        return strip.x0
            + (static_cast<float>(i) + vm.tabs[static_cast<size_t>(i)].x_offset) * strip.pitch
            + extra;
    };
    const int connected = dragI >= 0 ? dragI : activeI;
    if (connected >= 0 && connected < static_cast<int>(vm.tabs.size())) {
        const float connW = vm.tabs[static_cast<size_t>(connected)].pinned
            ? kTabPinnedW * scale_ : strip.w;
        const float shoulder = 8.0f * scale_;
        const float cut_l = tab_left_at(connected) - shoulder;
        const float cut_r = tab_left_at(connected) + connW + shoulder;
        if (cut_l > 0.0f)
            FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, cut_l, 1.0f);
        if (cut_r < rect.right)
            FillRect(dc, brStrokeDivider_.get(), cut_r, h - 1.0f, rect.right - cut_r, 1.0f);
    } else {
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, rect.right, 1.0f);
    }

    x = strip.end_x;
    // New tab button follows the final rest slot (not the sliding tabs).
    D2D1_RECT_F newRc = D2D1::RectF(x, tabY, x + 32 * scale_, tabY + tabH);
    DrawButton(newRc, theme, IsHovered(vm, HitTestResult::TabNew) ? theme.fill_hover : kTransparent,
        kIconAdd, L"+", theme.text_secondary, true, true);

    const D2D1_RECT_F settingsRc = D2D1::RectF(chrome.settings_left, tabY,
        chrome.settings_left + chrome.settings_w, tabY + tabH);
    DrawButton(settingsRc, theme,
        IsHovered(vm, HitTestResult::SettingsButton) ? theme.fill_hover : kTransparent,
        kIconSettings, L"S", theme.text_secondary, true, true);

    const D2D1_RECT_F themeRc = D2D1::RectF(chrome.theme_left, tabY,
        chrome.theme_left + chrome.theme_w, tabY + tabH);
    DrawButton(themeRc, theme, IsHovered(vm, HitTestResult::ThemeToggle) ? theme.fill_hover : kTransparent,
        kIconTheme, L"T", theme.text_secondary, true, true);

    // Window controls, right-aligned in Win11 order: min, max/restore, close.
    const float ctrlY = y;
    const float ctrlH = h;
    float cx = right;
    cx -= ctrlW;
    D2D1_RECT_F closeRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Close)) {
        MakeBrush(dc, theme.danger, brDanger_);
        FillRect(dc, brDanger_.get(), closeRc.left, closeRc.top, ctrlW, ctrlH);
    }
    DrawIconText(closeRc.left, closeRc.top, ctrlW, ctrlH, kIconClose, L"x",
        IsHovered(vm, HitTestResult::Close) ? HexColor(0xFFFFFF) : theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F maxRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Maximize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), maxRc.left, maxRc.top, ctrlW, ctrlH);
    }
    DrawIconText(maxRc.left, maxRc.top, ctrlW, ctrlH,
        vm.maximized ? kIconRestore : kIconMaximize, vm.maximized ? L"[]" : L"\u25A1",
        theme.text, 0.66f);
    cx -= ctrlW;
    D2D1_RECT_F minRc = D2D1::RectF(cx, ctrlY, cx + ctrlW, ctrlY + ctrlH);
    if (IsHovered(vm, HitTestResult::Minimize)) {
        MakeBrush(dc, theme.fill_hover, brFillHover_);
        FillRect(dc, brFillHover_.get(), minRc.left, minRc.top, ctrlW, ctrlH);
    }
    DrawIconText(minRc.left, minRc.top, ctrlW, ctrlH, kIconMinimize, L"_", theme.text, 0.66f);
}

void MainRenderer::DrawToolbar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const float y = title_bar_height_;
    const float h = toolbar_height_;
    const bool compact = rect.right < 900.0f * scale_;
    float x = margin_;
    const float commandHeight = kCommandIconButtonDip * scale_;
    const float commandTop = y + (h - commandHeight) * 0.5f;
    const float commandBottom = commandTop + commandHeight;

    D2D1_COLOR_F toolbarBackground = theme.header_bg;
    if (vm.backdrop_enabled) toolbarBackground.a = vm.dark ? 0.78f : 0.84f;
    MakeBrush(dc, toolbarBackground, brFillInput_);
    FillRect(dc, brFillInput_.get(), 0.0f, y, rect.right, h);
    FillRect(dc, brStrokeDivider_.get(), 0.0f, y + h - 1.0f, rect.right, 1.0f);

    auto navBtn = [&](const wchar_t* glyph, const wchar_t* fallback, bool enabled,
                      HitTestResult::Region region) {
        D2D1_RECT_F rc = D2D1::RectF(x, commandTop,
                                     x + kCommandIconButtonDip * scale_, commandBottom);
        const bool hovered = IsHovered(vm, region) && vm.hover_control_index < 0;
        DrawButton(rc, theme, hovered ? theme.fill_hover : kTransparent,
            glyph, fallback, enabled ? theme.text : theme.text_disabled, true, true, 0.8f);
        x += kCommandIconStepDip * scale_;
    };
    navBtn(kIconBack, L"<", vm.can_go_back, HitTestResult::NavBack);
    if (!compact) navBtn(kIconForward, L">", vm.can_go_forward, HitTestResult::NavForward);
    navBtn(kIconUp, L"^", true, HitTestResult::NavUp);
    navBtn(kIconRefresh, L"R", true, HitTestResult::NavRefresh);

    x += margin_;

    // Breadcrumb address bar: segments clickable, empty area -> edit mode.
    D2D1_RECT_F addrRc = AddressBarRect(rect.right);
    fluent::ControlState addrState{};
    addrState.focused = vm.address_editing;
    addrState.hovered = !vm.address_editing && IsHovered(vm, HitTestResult::AddressBar);
    painter_.DrawTextFieldFrame(addrRc, addrState);
    if (!vm.address_editing) {
        std::vector<BreadcrumbPlaced> placed;
        BreadcrumbLayout(vm.pane, rect.right, placed);
        for (size_t i = 0; i < placed.size(); ++i) {
            const auto& seg = placed[i];
            if ((int)i == vm.breadcrumb_drop) {
                // Drop target: accent 2px stroke (ui.md §5.2 rule 7).
                dc->DrawRoundedRectangle(
                    D2D1::RoundedRect(seg.rc, theme.radius_control * scale_, theme.radius_control * scale_),
                    brAccent_.get(), 2.0f * scale_);
            } else if ((int)i == vm.breadcrumb_hover) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), seg.rc.left, seg.rc.top,
                    seg.rc.right - seg.rc.left, seg.rc.bottom - seg.rc.top,
                    theme.radius_control * scale_);
            }
            if (i > 0) {
                // Chevron separator.
                float chX = seg.rc.left - 14.0f * scale_;
                DrawIconText(chX, addrRc.top, 14.0f * scale_, addrRc.bottom - addrRc.top,
                    kIconChevronRight, L">", theme.text_secondary, 0.55f);
            }
            MakeBrush(dc, theme.text, brText_);
            // Width measured exactly; let the ink use the right padding as slack
            // so the trailing glyph is not shaved by the clip rect.
            DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), seg.text,
                seg.rc.left + 8 * scale_, seg.rc.top, seg.rc.right - seg.rc.left - 8 * scale_,
                seg.rc.bottom - seg.rc.top);
        }
        if (placed.empty() && !vm.pane.path.empty()) {
            MakeBrush(dc, theme.text, brText_);
            DrawTextRect(dc, compositor_->AddressFormat(), brText_.get(), vm.pane.path,
                addrRc.left + 12.0f * scale_, addrRc.top,
                addrRc.right - addrRc.left - 12.0f * scale_ - painter_.OmnibarHintReservePx(),
                addrRc.bottom - addrRc.top);
        }
        const bool skip_search =
            !IsHighContrast() && EnsureFluentSvg(IDR_FLUENT_OMNIBAR_SEARCH_SVG);
        const D2D1_RECT_F search_rc = painter_.DrawOmnibarHints(addrRc, skip_search);
        if (skip_search) {
            DrawFluentSvg(IDR_FLUENT_OMNIBAR_SEARCH_SVG, search_rc, 1.0f);
        }
    }
    x = addrRc.right + margin_;

    // Quiet New: standard bordered button + Color add, same weight as op icons.
    const std::wstring new_label = pulse::l10n::Get(pulse::l10n::StringId::New);
    const float newW = NewButtonWidthPx(compact);
    D2D1_RECT_F newRc = D2D1::RectF(x, commandTop, x + newW, commandBottom);
    fluent::ButtonSpec neu;
    neu.bounds = newRc;
    neu.text = compact ? std::wstring_view{} : std::wstring_view{new_label};
    neu.glyph = kIconAdd;
    neu.kind = fluent::ButtonKind::Standard;
    neu.icon_only = compact;
    neu.drop_down = !compact;
    neu.state.hovered = IsHovered(vm, HitTestResult::NewButton);
    neu.skip_glyph = !IsHighContrast() && EnsureFluentSvg(IDR_FLUENT_ADD_SVG);
    painter_.DrawButton(neu);
    if (neu.skip_glyph) {
        DrawFluentSvg(IDR_FLUENT_ADD_SVG, painter_.ButtonGlyphRect(newRc, compact), 1.0f);
    }
    x = newRc.right + margin_;

    const bool hasSelection = vm.pane.selected_count > 0;
    auto opBtn = [&](int svg_id, const wchar_t* glyph, const wchar_t* fallback, bool enabled,
                     HitTestResult::Region region) {
        D2D1_RECT_F rc = D2D1::RectF(x, commandTop,
                                     x + kCommandIconButtonDip * scale_, commandBottom);
        if (IsHovered(vm, region)) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top,
                rc.right - rc.left, rc.bottom - rc.top, theme.radius_control * scale_);
        }
        const float icon = 20.0f * scale_;
        const float pad_x = (rc.right - rc.left - icon) * 0.5f;
        const float pad_y = (rc.bottom - rc.top - icon) * 0.5f;
        const D2D1_RECT_F icon_rc = D2D1::RectF(rc.left + pad_x, rc.top + pad_y,
                                                rc.right - pad_x, rc.bottom - pad_y);
        const float opacity = enabled ? 1.0f : 0.4f;
        if (IsHighContrast() || !DrawFluentSvg(svg_id, icon_rc, opacity)) {
            DrawIconText(rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                glyph, fallback, enabled ? theme.text : theme.text_disabled, 0.8f);
        }
        x += kCommandIconStepDip * scale_;
    };
    if (!compact) {
        x += 4.0f * scale_;
        FillRect(dc, brStrokeDivider_.get(), x, y + 10.0f * scale_, 1.0f, h - 20.0f * scale_);
        x += 7.0f * scale_;
        opBtn(IDR_FLUENT_CUT_SVG, kIconCut, L"Cut", hasSelection, HitTestResult::Cut);
        opBtn(IDR_FLUENT_COPY_SVG, kIconCopy, L"Copy", hasSelection, HitTestResult::Copy);
        opBtn(IDR_FLUENT_PASTE_SVG, kIconPaste, L"Paste", true, HitTestResult::Paste);
        opBtn(IDR_FLUENT_RENAME_SVG, kIconRename, L"Ren", hasSelection, HitTestResult::Rename);
        opBtn(IDR_FLUENT_DELETE_SVG, kIconDelete, L"Del", hasSelection, HitTestResult::Delete);
        x += 4.0f * scale_;
        FillRect(dc, brStrokeDivider_.get(), x, y + 10.0f * scale_, 1.0f, h - 20.0f * scale_);
        x += 7.0f * scale_;
        opBtn(IDR_FLUENT_APPS_SVG, kIconSplit, L"Spl", true, HitTestResult::SplitButton);
        opBtn(vm.details_visible ? IDR_FLUENT_PANEL_CLOSE_SVG : IDR_FLUENT_PANEL_SVG,
              vm.details_visible ? kIconDetailsClose : kIconDetailsOpen,
              L"Det", true, HitTestResult::DetailsToggle);
    }
}
void MainRenderer::DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    float y = rect.bottom - status_height_;
    D2D1_COLOR_F statusBackground = theme.status_bg;
    if (vm.backdrop_enabled) statusBackground.a = vm.dark ? 0.76f : 0.82f;
    MakeBrush(dc, statusBackground, brFillInput_);
    FillRect(dc, brFillInput_.get(), 0, y, rect.right, status_height_);
    FillRect(dc, brStrokeDivider_.get(), 0, y, rect.right, 1);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.status.status_text,
        margin_, y, rect.right * 0.30f, status_height_);
    const bool compact = rect.right < 900.0f * scale_;
    std::wstring selectionText = vm.status.selection_text;
    if (compact && vm.pane.selected_count > 0) {
        wchar_t buf[64];
        swprintf_s(buf,
            pulse::l10n::Get(pulse::l10n::StringId::SelectedCountFormat).c_str(),
            vm.pane.selected_count);
        selectionText = buf;
    }
    DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), selectionText,
        rect.right * 0.30f, y, rect.right * 0.18f, status_height_);

    float rightReserved = 170.0f * scale_;
    if (!vm.status.performance_text.empty()) {
        const std::wstring& perfText = rect.right < 1100.0f * scale_
            ? vm.status.performance_compact_text : vm.status.performance_text;
        const float perfWidth = std::min(rect.right * 0.50f,
            MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), perfText)
                + 16.0f * scale_);
        rightReserved = perfWidth + margin_;
        IDWriteTextFormat* perfFormat = compositor_->SmallFormat();
        perfFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, perfFormat, brTextSecondary_.get(), perfText,
            rect.right - rightReserved, y, perfWidth, status_height_);
        perfFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), vm.status.mode_text,
            rect.right - rightReserved, y, rightReserved - 10.0f * scale_, status_height_);
    }

    // Ops task summary + self-drawn Fluent progress bar.
    float taskX = rect.right * 0.48f;
    float taskRight = rect.right - rightReserved - margin_;
    if (!vm.status.task_text.empty() || vm.status.task_progress >= 0.0f) {
        float barW = (vm.status.task_progress >= 0.0f) ? (100 * scale_ + margin_ * 2) : 0.0f;
        MakeBrush(dc, theme.accent, brAccentText_);
        DrawTextRect(dc, compositor_->SmallFormat(), brAccentText_.get(), vm.status.task_text,
            taskX, y, std::max(0.0f, taskRight - taskX - barW), status_height_);
        if (barW > 0.0f) {
            float trackH = 4 * scale_;
            float trackX = taskRight - 100 * scale_;
            float trackY = y + (status_height_ - trackH) * 0.5f;
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            FillRoundedRect(dc, brStrokeCard_.get(), trackX, trackY, 100 * scale_, trackH, trackH * 0.5f);
            float fillW = 100 * scale_ * std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
            if (fillW > trackH) {
                MakeBrush(dc, theme.accent, brAccent_);
                FillRoundedRect(dc, brAccent_.get(), trackX, trackY, fillW, trackH, trackH * 0.5f);
            }
        }
    }

}
MainRenderer::TabStripMetrics MainRenderer::ComputeTabStrip(
    const WindowViewModel& vm, float window_w) const {
    TabStripMetrics m;
    const bool compact = TitleBarCompact(window_w, scale_, vm.tabs.size());
    const TitleChrome chrome = MakeTitleChrome(window_w, scale_, title_bar_height_);
    m.x0 = compact ? 44.0f * scale_ : 112.0f * scale_;
    const float tabsRight = chrome.settings_left - 8.0f * scale_;

    // Group chips: one at the start of each consecutive same-group run. Their
    // widths come out of the strip budget before tabs are sized; positions
    // are resolved in the second pass once the tab pitch is known.
    const float chipGap = 4.0f * scale_;
    float chipsTotal = 0.0f;
    IDWriteTextFormat* chipFmt = compositor_ ? compositor_->SmallFormat() : nullptr;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (!runStart) continue;
        float cw = 8.0f * scale_; // unnamed group: slim color bar
        if (g < static_cast<int>(vm.tab_groups.size()) &&
            !vm.tab_groups[static_cast<size_t>(g)].name.empty()) {
            const float tw = std::min(88.0f * scale_,
                MeasureTextWidth(compositor_->DwriteFactory(), chipFmt,
                                 vm.tab_groups[static_cast<size_t>(g)].name));
            cw = tw + 16.0f * scale_; // Edge-style block: text + side padding
        }
        TabStripMetrics::Chip chip;
        chip.width = cw;
        chip.group = g;
        chipsTotal += cw + chipGap;
        m.chips.push_back(chip);
    }
    m.extra.assign(vm.tabs.size(), 0.0f);

    const float available = std::max(0.0f, tabsRight - m.x0 - 36.0f * scale_ - chipsTotal);
    size_t visibleCount = 0;
    size_t pinnedCount = 0; // visible pinned tabs get a fixed narrow slot
    for (const auto& t : vm.tabs) {
        if (t.hidden) continue;
        if (t.pinned) ++pinnedCount; else ++visibleCount;
    }
    const float pinnedW = kTabPinnedW * scale_;
    const float pinnedTotal = static_cast<float>(pinnedCount) * (pinnedW + control_gap_);
    m.w = visibleCount == 0 ? 0.0f
        : std::min(kTabMaxW * scale_, std::max(kTabMinW * scale_,
            std::max(0.0f, available - pinnedTotal)
                / static_cast<float>(visibleCount) - control_gap_));
    m.y = 4.0f * scale_;
    m.h = title_bar_height_ - 8.0f * scale_;
    m.pitch = m.w + control_gap_;

    // Final pass: per-tab extra offset + definitive chip positions. Collapsed
    // members contribute zero width (chip stays visible at the fold point);
    // pinned tabs use the fixed narrow slot.
    float acc = 0.0f;
    size_t chipIdx = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (runStart && chipIdx < m.chips.size()) {
            TabStripMetrics::Chip& chip = m.chips[chipIdx++];
            chip.left = m.x0 + static_cast<float>(i) * m.pitch + acc;
            acc += chip.width + chipGap;
        }
        m.extra[i] = acc;
        if (vm.tabs[i].hidden) acc -= m.pitch;
        else if (vm.tabs[i].pinned) acc += pinnedW - m.w; // narrower than a slot
    }
    m.end_x = m.x0 + chipsTotal + static_cast<float>(pinnedCount) *
        (pinnedW + control_gap_) + static_cast<float>(visibleCount) * m.pitch;
    return m;
}

} // namespace pulse::ui
