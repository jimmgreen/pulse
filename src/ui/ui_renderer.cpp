#include "tab_width_fit.h"
#include "pane_header_icons.h"
#include "tab_rows_layout.h"
#include "tab_pager_layout.h"
// ui_renderer.cpp — Chrome orchestration (title, toolbar, status, render).
#include "legacy_icons.h"
#include "drag_layout.h"
#include "drag_feedback.h"
#include "operation_flight.h"
#include "drag_drop.h"
#include "ui_renderer.h"
#include "window_action_icon.h"
#include "device_crumbs.h"
#include "../fs/device_path.h"
#include "snack_style.h"
#include "command_icons.h"
#include "toolbar_layout.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "../common/display_path.h"
#include "tab_shape.h"
#include "bloom_accent_picker.h"
#include "typography.h"
#include "../app/resource.h"
#ifdef PULSE_STARTUP_TRACE_DIAGNOSTICS
#include "../app/startup_trace.h"
#define PULSE_SCENE_MARK(label) app::startup_trace::Mark(label)
#else
#define PULSE_SCENE_MARK(label) ((void)0)
#endif
#include "../app/places.h"
#include "../common/text_format.h"
#include <windowsx.h>
#include <commctrl.h>
#include <d2d1effects.h>
#include <shlwapi.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <string_view>

namespace pulse::ui {
namespace {
// Tokens the three status-bar mockups use that Theme has no slot for
// (pane_layout / drag_undo / file_ops_mockup.html :root): the accent made
// readable as ink (--accent-fg), its washes, the tertiary text, and the
// success / warning / danger inks with their soft fills.
struct MockupInk {
    D2D1_COLOR_F accent_fg, accent_soft, accent_soft2, text3;
    D2D1_COLOR_F ok, ok_soft, warn, warn_soft, danger, danger_soft;
};
MockupInk MakeMockupInk(const Theme& theme, bool dark) {
    MockupInk ink{};
    const bool high_contrast = IsHighContrast();
    ink.accent_fg = MakeSnackPalette(theme, high_contrast).accent;
    ink.accent_soft = WithAlpha(theme.accent, dark ? 0.17f : 0.095f);
    ink.accent_soft2 = WithAlpha(theme.accent, dark ? 0.28f : 0.17f);
    ink.text3 = theme.text_disabled;
    if (high_contrast) {
        ink.ok = ink.warn = ink.danger = theme.text;
        ink.ok_soft = ink.warn_soft = ink.danger_soft = theme.fill_hover;
        return ink;
    }
    ink.ok = dark ? HexColor(0x6CCB5F) : HexColor(0x0F7B0F);
    ink.ok_soft = dark ? HexColor(0x6CCB5F, 0.14f) : HexColor(0x0F7B0F, 0.10f);
    ink.warn = dark ? HexColor(0xFCE100) : HexColor(0x9D5D00);
    ink.warn_soft = dark ? HexColor(0xFCE100, 0.12f) : HexColor(0x9D5D00, 0.10f);
    ink.danger = dark ? HexColor(0xFF99A4) : HexColor(0xC42B1C);
    ink.danger_soft = dark ? HexColor(0xFF99A4, 0.12f) : HexColor(0xC42B1C, 0.09f);
    return ink;
}
// --shadow of the mockups: 0 8px 24px (dark 28px) plus 0 1px 3px. Not with a
// layer or clip pushed (DrawElevationShadow retargets the context).
void DrawFlyoutShadow(ID2D1DeviceContext* dc, const D2D1_RECT_F& rect, float radius, float scale, bool dark) {
    if (IsHighContrast()) return;
    DrawElevationShadow(dc, rect, radius, (dark ? 14.0f : 12.0f) * scale, 8.0f * scale,
                        dark ? D2D1::ColorF(0x000000, 0.45f) : D2D1::ColorF(0x303B50, 0.16f));
    DrawElevationShadow(dc, rect, radius, 1.5f * scale, 1.0f * scale,
                        dark ? D2D1::ColorF(0x000000, 0.35f) : D2D1::ColorF(0x303B50, 0.10f));
}
} // namespace

MainRenderer::MainRenderer()
    : owned_caches_(std::make_unique<RenderCaches>()), caches_(*owned_caches_) {}

MainRenderer::MainRenderer(RenderCaches& caches) : caches_(caches) {}

void MainRendererDeleter::operator()(MainRenderer* renderer) const noexcept {
    delete renderer;
}

MainRendererPtr MakeMainRenderer(RenderCaches& caches) {
    return MainRendererPtr(new MainRenderer(caches));
}

void MainRenderer::SetCompositor(Compositor* comp) {
    paneHeaderStroke_.reset();
    link_arrow_geometry_.reset();
    tray_shadows_.clear();
    ClearTextWidthCache();
    sized_icon_formats_.clear();
    empty_state_svg_.reset();
    no_selection_svg_.reset();
    recent_empty_svg_.reset();
    starred_empty_svg_.reset();
    exclude_empty_svg_.reset();
    fluent_svgs_.clear();
    fluent_svg_failed_.clear();
    fluent_bitmaps_.clear();
    pane_zoom_motion_ = {};
    content_tab_identity_ = 0;
    fluent_raster_dc_.reset();
    empty_state_svg_dc_.reset();
    app_icon_bitmaps_.clear();
    app_icon_dc_ = nullptr;
    ai_panel_.ReleaseDeviceResources();
    ReleaseAiIcons();
    compositor_ = comp;
    material_.SetCompositor(comp);
    painter_.SetCompositor(comp);
    if (!comp) {
        // Closing one window must not empty the caches the others draw from.
        if (owned_caches_) ResetRenderCaches(*owned_caches_);
        preview_mono_format_.reset();
    }
    else {
        icon_cache_.SetDeviceContext(comp->Dc());
        open_with_icons_.SetDeviceContext(comp->Dc());
        thumbnail_cache_.SetDeviceContext(comp->Dc());
        folder_thumbnail_cache_.SetDeviceContext(comp->Dc());
        details_cache_.SetDeviceContext(comp->Dc());
        hover_cache_.SetDeviceContext(comp->Dc());
        // The hover card: cheap pictures first, and a decode left behind by
        // the pointer is given up after 400 ms when another file waits.
        hover_cache_.SetRequestFlags(ipc::kPreviewRequestFlagCard);
        hover_cache_.SetAbandonStaleMs(400);
    }
}

void MainRenderer::InvalidateTypography() {
    ClearTextWidthCache();
    cell_text_widths_.clear();
    auto_widths_scale_ = -1.0f;
    sized_icon_formats_.clear();
    preview_mono_format_.reset();
    painter_.InvalidateTypography();
}
void MainRenderer::SetIconNotifyWindow(HWND hwnd) {
    // The caches may be shared by every Pulse window (RenderCaches): each
    // renderer registers its own window and leaves the others in place.
    if (notify_hwnd_ && notify_hwnd_ != hwnd) {
        ReleaseWallViews();
        icon_cache_.RemoveNotifyWindow(notify_hwnd_);
        open_with_icons_.RemoveNotifyWindow(notify_hwnd_);
        thumbnail_cache_.RemoveNotifyWindow(notify_hwnd_);
        folder_thumbnail_cache_.RemoveNotifyWindow(notify_hwnd_);
        details_cache_.RemoveNotifyWindow(notify_hwnd_);
        hover_cache_.RemoveNotifyWindow(notify_hwnd_);
    }
    notify_hwnd_ = hwnd;
    if (hwnd) {
        icon_cache_.AddNotifyWindow(hwnd);
        open_with_icons_.AddNotifyWindow(hwnd);
        thumbnail_cache_.AddNotifyWindow(hwnd);
        folder_thumbnail_cache_.AddNotifyWindow(hwnd);
        details_cache_.AddNotifyWindow(hwnd);
        hover_cache_.AddNotifyWindow(hwnd);
    }
}

void MainRenderer::SetScale(float scale) {
    if (scale_ != scale) {
        tray_shadows_.clear();
        preview_mono_format_.reset();
        sized_icon_formats_.clear();
        ClearTextWidthCache();
    }
    scale_ = scale;
    title_bar_height_ = (embedded_ ? embedded_top_dip_ : kTitleBarHeight) * scale;
    toolbar_height_ = ToolbarHeightDip() * scale;
    status_height_ = (embedded_ ? embedded_bottom_dip_ : 28.0f) * scale;
    sidebar_width_ = sidebar_width_dip_ * scale;
    pane_header_height_ = 40.0f * scale;
    column_header_height_ = 32.0f * scale;
    row_height_ = row_height_dip_ * scale;
    margin_ = 4.0f * scale;
    control_gap_ = 4.0f * scale;
    painter_.SetScale(scale);
    icon_cache_.SetScale(scale);
}

void MainRenderer::SetEmbedded(float top_dip, float bottom_dip) {
    embedded_ = true;
    vertical_tabs_ = false;
    favorites_bar_ = false;
    embedded_top_dip_ = top_dip;
    embedded_bottom_dip_ = bottom_dip;
    SetDetailsPanelVisible(false);
    SetScale(scale_);
}

float MainRenderer::SidebarOwnWidth(float window_width) const {
    const float rail = kSidebarRailWidthDip * scale_;
    if (SidebarAutoCollapsed(window_width) || collapse_value_ >= 1.0f) return rail;
    const float full = SidebarFullWidth(window_width);
    return collapse_value_ <= 0.0f ? full : full + (rail - full) * collapse_value_;
}

float MainRenderer::EffectiveSidebarWidth(float window_width) const {
    return SidebarOwnWidth(window_width) + AiDockWidth(window_width);
}

// The title bar (brand, toggle, tabs) keeps the user's sidebar: the docked
// assistant folding the sidebar to its rail does not move it.
float MainRenderer::TitleSidebarWidth(float window_width) const {
    if (ai_rail_ && !user_sidebar_collapsed_ && window_width / scale_ >= kSidebarRailWindowDip)
        return SidebarFullWidth(window_width);
    return SidebarOwnWidth(window_width);
}

float MainRenderer::AiDockWidth(float window_width) const {
    return ai_panel_.DockWidth(window_width, kSidebarRailWidthDip * scale_, scale_);
}

D2D1_RECT_F MainRenderer::AiColumnRect(float w, float h) const {
    const float left = SidebarOwnWidth(w);
    return D2D1::RectF(left, title_bar_height_, left + AiDockWidth(w), h - status_height_);
}

D2D1_RECT_F MainRenderer::AiFloatingArea(float w, float h) const {
    return D2D1::RectF(0.0f, title_bar_height_, w, h - status_height_);
}

void MainRenderer::SetAiPanel(bool open, AiPanelMode mode, bool animate) {
    ai_panel_.SetState(open, mode, animate);
    ai_rail_ = open && mode == AiPanelMode::Docked;
    ApplySidebarCollapse(animate);
}

void MainRenderer::SetSidebarCollapsed(bool on, bool animate) {
    user_sidebar_collapsed_ = on;
    ApplySidebarCollapse(animate);
}

void MainRenderer::ApplySidebarCollapse(bool animate) {
    const bool on = user_sidebar_collapsed_ || ai_rail_;
    if (on == sidebar_collapsed_) return;
    sidebar_collapsed_ = on;
    if (on) SetSidebarPeek(false);
    if (animate && motion::SystemAnimationsEnabled()) {
        collapse_from_ = collapse_value_;
        collapse_start_ = motion::NowMs();
        collapse_anim_ = true;
    } else {
        collapse_anim_ = false;
        collapse_value_ = on ? 1.0f : 0.0f;
    }
}

float MainRenderer::SidebarFullWidth(float window_width) const {
    const float window_dip = window_width / scale_;
    const bool details_open = details_visible_ && window_dip >= kDetailsVisibleWindowDip;
    const float max_dip = MaxSidebarWidthDip(window_dip, details_width_, details_open,
                                             2.0f * margin_ / scale_);
    return (std::min)(sidebar_width_dip_, max_dip) * scale_;
}

// The preferred width is only capped while drawing, so narrowing the window pushes
// the panel back temporarily and widening it restores what the user dragged.
float MainRenderer::DetailsPanelWidth(float window_w) const {
    const float window_dip = window_w / scale_;
    if (window_dip < kDetailsVisibleWindowDip) return 0.0f;
    const float open = details_anim_ ? details_value_ : (details_visible_ ? 1.0f : 0.0f);
    if (open <= 0.0f) return 0.0f;
    const float max_dip = MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                                             (window_dip < kSidebarRailWindowDip || sidebar_collapsed_),
                                             2.0f * margin_ / scale_);
    return std::round(((std::min)(details_width_, max_dip) * scale_ + margin_) * open);
}

float MainRenderer::SidebarMaxWidthDip(float window_w) const {
    const float window_dip = window_w / scale_;
    const bool details_open = details_visible_ && window_dip >= kDetailsVisibleWindowDip;
    return MaxSidebarWidthDip(window_dip, details_width_, details_open, 2.0f * margin_ / scale_);
}

float MainRenderer::DetailsMaxWidthDip(float window_w) const {
    const float window_dip = window_w / scale_;
    return MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                              (window_dip < kSidebarRailWindowDip || sidebar_collapsed_), 2.0f * margin_ / scale_);
}

D2D1_RECT_F MainRenderer::ContentRect(float w, float h) const {
    float left = EffectiveSidebarWidth(w) + margin_;
    float top = title_bar_height_ + toolbar_height_ + margin_;
    float bottom = h - status_height_ - margin_;
    float right = w - margin_ - DetailsPanelWidth(w);
    if (extension_dock_visible_ && right - left >= 688.0f * scale_) right -= 328.0f * scale_;
    return D2D1::RectF(left, top, right, bottom);
}
D2D1_RECT_F MainRenderer::ExtensionDockRect(float w, float h) const {
    const float right = w - margin_ - DetailsPanelWidth(w);
    const float left = EffectiveSidebarWidth(w) + margin_;
    if (right - left < 688.0f * scale_) return {};
    return {right - 320.0f * scale_, title_bar_height_ + toolbar_height_ + margin_,
        right, h - status_height_ - margin_};
}
D2D1_RECT_F MainRenderer::TitleBarRect(float w) const {
    return D2D1::RectF(0, 0, w, title_bar_height_);
}

namespace {
// Width of a toolbar command (icon + label [+ chevron]) sized to its label, so
// Sort, Filter and Group keep even spacing and each chevron sits 4 DIP after its
// text. Layout is queried many times per frame (paint, hit tests): small cache.
float ToolbarCommandWidth(const fluent::Painter& painter, IDWriteTextFormat* format,
                          l10n::StringId id, const wchar_t* glyph, bool drop_down,
                          float scale, float fallback_dip) {
    struct Entry { int id = -1; float scale = 0.0f, font = 0.0f; const void* format = nullptr;
                   std::wstring text; float width = 0.0f; };
    thread_local Entry cache[3];
    const std::wstring text(l10n::Get(id));
    const float font = format ? format->GetFontSize() : 0.0f;
    for (const Entry& e : cache)
        if (e.id == static_cast<int>(id) && e.scale == scale && e.font == font &&
            e.format == format && e.text == text)
            return e.width;
    const float bare = painter.MeasureButtonWidth(L"", glyph, drop_down);
    const float full = painter.MeasureButtonWidth(text, glyph, drop_down);
    // No text measurement available (no device yet): keep the fixed width.
    const float width = format && full > bare ? full : fallback_dip * scale;
    // One slot per button: reuse its slot, else the first free one.
    Entry* slot = &cache[2];
    for (Entry& e : cache)
        if (e.id == static_cast<int>(id) || e.id < 0) { slot = &e; break; }
    *slot = {static_cast<int>(id), scale, font, format, text, width};
    return width;
}
} // namespace

float MainRenderer::ToolbarGroupWidth(float w) const {
    if (toolbar_group_ < 0 || !toolbar_show_group_) return 0.0f;
    // Narrow windows: icon only, like Sort and Filter.
    if (w - EffectiveSidebarWidth(w) < 700.0f * scale_) return 32.0f * scale_;
    if (toolbar_group_ > 0) {
        // Icon 30 + label + 6 + clear button 28 DIP, so the x sits next to the
        // text instead of at the end of a fixed 212 DIP pill.
        IDWriteFactory2* factory = compositor_ ? compositor_->DwriteFactory() : nullptr;
        if (!factory) return 212.0f * scale_;
        const float label = std::ceil(MeasureTextWidth(factory, compositor_->TextFormat(), ToolbarGroupLabel()));
        return std::min(label + 64.0f * scale_, 212.0f * scale_);
    }
    return ToolbarCommandWidth(painter_, compositor_ ? compositor_->TextFormat() : nullptr,
                               l10n::StringId::ToolbarGroup, L"\xF168", true, scale_, 108.0f);
}

ToolbarLayout MainRenderer::ToolbarLayoutAt(float w, float create_width, float filter_expand) const {
    const float left = EffectiveSidebarWidth(w);
    const float group_width = ToolbarGroupWidth(w);
    IDWriteTextFormat* format = compositor_ ? compositor_->TextFormat() : nullptr;
    // Hidden ones pass -1 (MakeToolbarLayout); an opening filter shows anyway.
    const float sort_width = !toolbar_show_sort_ ? -1.0f
        : ToolbarCommandWidth(painter_, format, l10n::StringId::ToolbarSort, L"\xE8CB", true, scale_, 88.0f);
    const float filter_width = !toolbar_show_filter_ ? -1.0f
        : ToolbarCommandWidth(painter_, format, l10n::StringId::ToolbarFilter, kIconFilter, false, scale_, 88.0f);
    const float selection_span = selection_count_ > 0 && !embedded_ && !pin_drop_mode_
        ? SelectionChipSpan(scale_, SelectionChipLabelWidth(w, selection_count_), selection_actions_) : 0.0f;
    ToolbarPinOptions pins;
    pins.view_slots = pin_view_slots_;
    pins.right_from = pin_right_from_;
    pins.view_order = toolbar_view_order_;
    pins.app_from = pin_app_from_;
    pins.drop_mode = pin_drop_mode_ && !embedded_;
    pins.separators = pins.drop_mode ? 0 : pin_separators_;
    pins.label_width = pins.drop_mode ? ToolbarPinLabelWidth() : 0.0f;
    if (!vertical_tabs_)
        return MakeToolbarLayout(w, scale_, title_bar_height_, margin_, create_width, left, filter_expand,
                                 search_hidden_ ? -1.0f : search_min_dip_, group_width, sort_width, filter_width, pin_count_,
                                 selection_span, pins, navigation_can_forward_);
    // Command row directly under the title bar; address row centered in it,
    // stopping short of the settings button.
    ToolbarLayout out = MakeToolbarLayout(w, scale_, title_bar_height_ - 46.0f * scale_, margin_,
                                          create_width, left, filter_expand, 0.0f, group_width, sort_width,
                                          filter_width, pin_count_, selection_span, pins);
    const TitleChrome chrome = MakeTitleChrome(w, scale_, title_bar_height_);
    const float row_top = (title_bar_height_ - 36.0f * scale_) * 0.5f - 4.0f * scale_;
    const ToolbarLayout row = MakeToolbarLayout(chrome.windows_left - 4.0f * scale_, scale_, row_top,
                                                margin_, create_width, left, 0.0f, search_hidden_ ? -1.0f : search_min_dip_,
                                                0, 0, 0, 0, 0, {}, navigation_can_forward_);
    out.navigation = row.navigation;
    out.address = row.address;
    out.search = row.search;
    out.address_text = row.address_text;
    out.navigation_tier = row.navigation_tier;
    out.refresh_inside = row.refresh_inside;
    out.merged_search = row.merged_search;
    return out;
}

// Drop-mode label ahead of the pinned apps: 10 + glyph 16 + 6 + text + 10 DIP.
float MainRenderer::ToolbarPinLabelWidth() const {
    if (pin_drop_label_.empty() || !compositor_ || !compositor_->SmallFormat()) return 0.0f;
    const float text = std::ceil(typography::MeasureLine(compositor_, compositor_->SmallFormat(), pin_drop_label_));
    return text + 42.0f * scale_;
}

ID2D1Bitmap* MainRenderer::AppIconBitmap(const common::MenuIconRef& pixels) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!pixels || !pixels->Valid() || !dc) return nullptr;
    if (dc != app_icon_dc_) {   // new device: the old bitmaps belong to the lost one
        app_icon_bitmaps_.clear();
        app_icon_dc_ = dc;
    }
    for (const auto& entry : app_icon_bitmaps_)
        if (entry.pixels == pixels) return entry.bitmap.get();
    if (app_icon_bitmaps_.size() >= 32) app_icon_bitmaps_.clear();
    AppIconEntry entry;
    entry.pixels = pixels;
    const auto props = D2D1::BitmapProperties(
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc->CreateBitmap(D2D1::SizeU(pixels->width, pixels->height), pixels->bgra.data(),
                                pixels->width * 4u, props, &entry.bitmap)))
        entry.bitmap.reset();  // remembered as missing: no retry every frame
    app_icon_bitmaps_.push_back(std::move(entry));
    return app_icon_bitmaps_.back().bitmap.get();
}

float MainRenderer::TitleBrandWidth(const WindowViewModel& vm, float w) const {
    const float aligned = TitleSidebarWidth(w);
    title_brand_width_ = aligned;
    title_brand_name_ = true;
    if (vertical_tabs_ || embedded_ || !compositor_) return aligned;
    if (w < 360.0f*scale_) { title_brand_name_ = false; return title_brand_width_ = 54.0f*scale_; }
    const float logo = 78 * scale_; // 12 + 24 mark + 4 + 34 toggle + 4
    const bool rail = user_sidebar_collapsed_ || SidebarAutoCollapsed(w);
    if (rail) {
        title_brand_name_ = false;
        return title_brand_width_ = logo;
    }
    const auto* name = vm.window_identity && !vm.window_identity_name.empty() ? &vm.window_identity_name : nullptr;
    const float name_width = MeasureTextWidth(compositor_->DwriteFactory(), compositor_->HeaderFormat(),
        name ? *name : std::wstring(L"Pulse"));
    const float named = 88 * scale_ + (std::min)(name_width + (name ? 22 * scale_ : 0), 140 * scale_);
    float natural = 0, groups = 0;
    int count = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const auto& tab = vm.tabs[i];
        if (tab.group >= 0 && (i == 0 || vm.tabs[i - 1].group != tab.group)) {
            float chip = 8 * scale_;
            if (tab.group < static_cast<int>(vm.tab_groups.size()) && !vm.tab_groups[tab.group].name.empty())
                chip = (std::min)(88 * scale_, MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(),
                    vm.tab_groups[tab.group].name)) + 16 * scale_;
            groups += chip + 4 * scale_;
        }
        if (tab.hidden) continue;
        natural += tab.pinned ? (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_ :
            ((60 + (tab.marker_rgb ? 12 : 0)) * scale_ + MeasureTextWidth(compositor_->DwriteFactory(), compositor_->TabFormat(), tab.title));
        ++count;
    }
    natural += (std::max)(0, count - 1) * 2 * scale_ + groups;
    const float right = MakeTitleChrome(w, scale_, TitleControlsHeight()).windows_left - (8 + 56 + 38) * scale_;

    const float minimum = count * 104 * scale_ + (std::max)(0, count - 1) * 2 * scale_ + groups;
    if (natural <= right - named - margin_ || minimum <= right - named - margin_)
        return title_brand_width_ = named;
    title_brand_name_ = false;
    return title_brand_width_ = logo;
}

D2D1_RECT_F MainRenderer::SidebarToggleRect(float w) const {
    const float size = (!vertical_tabs_ && !embedded_ && w < 360.0f*scale_ ? 24.0f : 32.0f) * scale_;
    const float top = (TitleControlsHeight() - size) * 0.5f;
    if (!vertical_tabs_ && !embedded_ && title_brand_width_ > 0) {
        const float right = title_brand_width_ - 4 * scale_;
        return D2D1::RectF(right - size, top, right, top + size);
    }
    if (user_sidebar_collapsed_ || SidebarAutoCollapsed(w)) {
        const float left = (kSidebarRailWidthDip * scale_ - size) * 0.5f;
        return D2D1::RectF(left, top, left + size, top + size);
    }
    const float right = TitleSidebarWidth(w) - 8.0f * scale_;
    return D2D1::RectF(right - size, top, right, top + size);
}

// Dot and name sit where the product name is, after the mark: 6 dip padding,
// a 10 dip dot, 6 dip gap, the name, 6 dip padding; never under the sidebar
// toggle. Without room for that (the icon rail, a narrow sidebar) a dot sits
// on the lower right corner of the mark, or of the toggle where the toggle
// stands in for the mark.
MainRenderer::WindowIdentityGeometry MainRenderer::WindowIdentityLayout(const WindowViewModel& vm, float w) const {
    const float brand = TitleBrandWidth(vm, w);
    WindowIdentityGeometry g;
    if (!vm.window_identity || vm.window_identity_name.empty() || !compositor_) return g;
    const D2D1_RECT_F toggle = SidebarToggleRect(w);
    const bool has_toggle = toggle.right > toggle.left;
    const bool title_rail = user_sidebar_collapsed_ || SidebarAutoCollapsed(w);
    if (!(has_toggle && title_rail)) {
        const float left = (12.0f + 24.0f + 4.0f) * scale_;
        float limit = brand - 8.0f * scale_;
        if (has_toggle) limit = (std::min)(limit, toggle.left - 4.0f * scale_);
        const float text = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->HeaderFormat(),
                                             vm.window_identity_name);
        const float width = (std::min)(limit - left, 28.0f * scale_ + text);
        if (width >= 40.0f * scale_) {
            const float height = 28.0f * scale_;
            const float top = (TitleControlsHeight() - height) * 0.5f;
            g.hit = D2D1::RectF(left, top, left + width, top + height);
            g.dot_radius = 5.0f * scale_;
            g.dot = D2D1::Point2F(left + 6.0f * scale_ + g.dot_radius, top + height * 0.5f);
            return g;
        }
    }
    g.compact = true;
    g.dot_radius = 4.0f * scale_;
    if (has_toggle && title_rail && (vertical_tabs_ || embedded_)) {
        g.dot = D2D1::Point2F(toggle.right - 3.0f * scale_, toggle.bottom - 3.0f * scale_);
        const float reach = g.dot_radius + 3.0f * scale_;
        g.hit = D2D1::RectF(g.dot.x - reach, g.dot.y - reach, g.dot.x + reach, g.dot.y + reach);
    } else {
        const float mark = 24.0f * scale_;
        const float left = 12.0f * scale_;
        const float top = (TitleControlsHeight() - mark) * 0.5f;
        g.dot = D2D1::Point2F(left + mark - 2.0f * scale_, top + mark - 2.0f * scale_);
        const float pad = 4.0f * scale_;
        g.hit = D2D1::RectF(left - pad, top - pad, left + mark + pad, top + mark + pad);
    }
    return g;
}

D2D1_RECT_F MainRenderer::ToolbarRect(float w) const {
    return D2D1::RectF(EffectiveSidebarWidth(w), title_bar_height_, w, title_bar_height_ + toolbar_height_);
}

D2D1_RECT_F MainRenderer::AddressBarRect(float w) const {
    return ToolbarLayoutAt(w,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_)).address;
}
D2D1_RECT_F MainRenderer::SearchBarRect(float w) const {
    return ToolbarLayoutAt(w,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_)).search;
}
D2D1_RECT_F MainRenderer::NewCommandRect(float w) const {
    return ToolbarLayoutAt(w,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_)).create;
}
D2D1_RECT_F MainRenderer::SplitCommandRect(float w) const {
    return ToolbarLayoutAt(w,NewButtonWidthPx(w-EffectiveSidebarWidth(w)<600*scale_)).commands[5];
}
D2D1_RECT_F MainRenderer::NewArrowRect(float w) const {
    return NewArrowSegment(NewCommandRect(w), w-EffectiveSidebarWidth(w)<600*scale_);
}
// Split New button: the chevron segment is a fixed width at the right edge,
// centred on the chevron that Painter::DrawButton places 8 DIP from the edge.
D2D1_RECT_F MainRenderer::NewArrowSegment(const D2D1_RECT_F& create, bool compact) const {
    if (create.right <= create.left) return create;
    if (new_primary_ == 2) return create;   // menu-only New: the whole button opens the menu
    const float arrow = (compact ? 28.0f : 32.0f) * scale_;
    return D2D1::RectF(std::max(create.left, create.right - arrow), create.top, create.right, create.bottom);
}
float MainRenderer::NewButtonWidthPx(bool compact) const {
    // Compact: icon button + chevron segment at a fixed width, as before no text
    // measuring. The full button labels the folder action and adds a gap so the
    // label never runs into the segment divider.
    if (compact) return (kCommandIconButtonDip + 28.0f) * scale_;
    return painter_.MeasureButtonWidth(NewButtonLabel(), NewButtonGlyph(), true) + 12.0f * scale_;
}
std::wstring MainRenderer::NewButtonLabel() const {
    if (new_primary_ == 2) return pulse::l10n::Get(pulse::l10n::StringId::New);
    if (new_primary_ == 1 && new_last_text_) return pulse::l10n::Get(pulse::l10n::StringId::NewTextDocument);
    return pulse::l10n::Get(pulse::l10n::StringId::NewFolder);
}
const wchar_t* MainRenderer::NewButtonGlyph() const {
    if (new_primary_ == 2) return L"\xE710";                       // Add
    if (new_primary_ == 1 && new_last_text_) return L"\xE8A5";     // Document
    return kNewFolderGlyph;
}

namespace {
DeviceCrumbResolver g_device_crumbs = nullptr;
}  // namespace

void SetDeviceCrumbResolver(DeviceCrumbResolver resolver) { g_device_crumbs = resolver; }

// 此电脑 › Xiaomi 14 › [内部存储] › DCIM › Camera. A one-storage phone skips the
// storage level: its name segment opens the storage, as This PC does.
static void AppendDeviceCrumbs(const std::wstring& path, std::vector<BreadcrumbSegment>& out) {
    BreadcrumbSegment pc;
    pc.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
    pc.path = L"";
    out.push_back(pc);
    const auto where = fs::ParseDevicePath(path);
    if (!where) return;
    DeviceCrumbInfo info;
    if (g_device_crumbs) info = g_device_crumbs(where->device);
    fs::DevicePath walk{where->device, L"", {}};
    BreadcrumbSegment device;
    device.text = info.known && !info.name.empty()
        ? info.name : std::wstring(pulse::l10n::Get(pulse::l10n::StringId::DeviceKindPhone));
    if (info.skips_storage && !where->storage.empty()) walk.storage = where->storage;
    device.path = fs::FormatDevicePath(walk);
    out.push_back(device);
    if (where->storage.empty()) return;
    walk.storage = where->storage;
    if (!info.skips_storage) {
        BreadcrumbSegment storage;
        storage.text = where->storage;
        storage.path = fs::FormatDevicePath(walk);
        out.push_back(storage);
    }
    for (const auto& part : where->parts) {
        walk.parts.push_back(part);
        BreadcrumbSegment seg;
        seg.text = part;
        seg.path = fs::FormatDevicePath(walk);
        out.push_back(seg);
    }
}

std::vector<BreadcrumbSegment> SplitBreadcrumb(const std::wstring& path) {
    std::vector<BreadcrumbSegment> out;
    std::wstring p = path;
    if (p.starts_with(L"\\\\?\\UNC\\")) p = L"\\\\" + p.substr(8);
    else if (p.starts_with(L"\\\\?\\")) p = p.substr(4);
    while (p.size() > 1 && p.back() == L'\\') p.pop_back();
    // The view model carries This PC as its display name (DisplayPath), not
    // "": kept as a folder name, clicking the crumb a second time opened
    // "C:\Program Files\Pulse\此电脑" relative to the working directory.
    if (p.empty() || p == pulse::l10n::Get(pulse::l10n::StringId::ThisPc)) {
        // Empty path = This PC: a single segment that navigates to "".
        BreadcrumbSegment seg;
        seg.text = pulse::l10n::Get(pulse::l10n::StringId::ThisPc);
        seg.path = L"";
        out.push_back(seg);
        return out;
    }
    std::wstring pulse_kind, pulse_rest;
    if (app::ParsePulsePath(p, &pulse_kind, &pulse_rest)) {
        if (pulse_kind == L"device") {
            AppendDeviceCrumbs(p, out);
            return out;
        }
        if (pulse_kind == L"recycle") {
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
        BreadcrumbSegment seg;
        seg.path = path;
        if (pulse_kind == L"search" || pulse_kind == L"saved-search")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Search);
        else if (pulse_kind == L"starred")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::StarredItems);
        else if (pulse_kind == L"recent")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Recent);
        else if (pulse_kind == L"settings")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Settings);
        else if (pulse_kind == L"tag")
            seg.text = pulse_rest.empty()
                ? pulse::l10n::Get(pulse::l10n::StringId::Tag) : pulse_rest;
        else if (pulse_kind == L"tags")
            seg.text = pulse::l10n::Get(pulse::l10n::StringId::Tag);
        else
            seg.text = pulse_kind;
        out.push_back(seg);
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

namespace {
constexpr float kSuggestRowDip = 36.0f;
constexpr float kSuggestPadDip = 5.0f;
constexpr float kSuggestHeadDip = 24.0f;
}

D2D1_RECT_F MainRenderer::AddressSuggestionsRect(float w, size_t count) const {
    const D2D1_RECT_F addr = AddressBarRect(w);
    const float top = addr.bottom + 4.0f * scale_;
    const float h = (2.0f * kSuggestPadDip + kSuggestHeadDip + kSuggestRowDip * static_cast<float>(count)) * scale_;
    return D2D1::RectF(addr.left, top, addr.right, top + h);
}

int MainRenderer::AddressSuggestionAt(float w, size_t count, float x, float y) const {
    if (count == 0) return -1;
    const D2D1_RECT_F rc = AddressSuggestionsRect(w, count);
    if (x < rc.left || x >= rc.right || y < rc.top || y >= rc.bottom) return -1;
    const float rows_top = rc.top + (kSuggestPadDip + kSuggestHeadDip) * scale_;
    if (y < rows_top) return -2;  // the header
    const int row = static_cast<int>((y - rows_top) / (kSuggestRowDip * scale_));
    return row >= 0 && row < static_cast<int>(count) ? row : -2;  // -2: the card's padding
}

void MainRenderer::DrawAddressSuggestions(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                          const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const size_t count = vm.address_suggestions.size();
    const D2D1_RECT_F card = AddressSuggestionsRect(rect.right - rect.left, count);
    const SnackPalette pal = MakeSnackPalette(theme, IsHighContrast());
    DrawSnackPlate(dc, card, 8.0f * scale_, pal, scale_, IsHighContrast());
    const D2D1_COLOR_F saved_bg = text_background_;
    text_background_ = pal.plate;
    IDWriteTextFormat* fmt = compositor_->TextFormat();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const auto measure_small = [&](const std::wstring& t) {
        return MeasureLayoutText(compositor_, compositor_->DwriteFactory(), small_fmt, t);
    };
    const float row_h = kSuggestRowDip * scale_;
    const float rows_top = card.top + (kSuggestPadDip + kSuggestHeadDip) * scale_;
    if (!vm.address_suggestion_header.empty()) {
        // Header: 别名, or why a real subfolder leads.
        MakeBrush(dc, WithAlpha(pal.text_secondary, 0.85f), brText_);
        DrawTextRect(dc, small_fmt, brText_.get(), vm.address_suggestion_header,
                     card.left + 15.0f * scale_, card.top + kSuggestPadDip * scale_,
                     card.right - card.left - 30.0f * scale_, kSuggestHeadDip * scale_);
    }
    for (size_t i = 0; i < count; ++i) {
        const AddressSuggestionView& item = vm.address_suggestions[i];
        const float y = rows_top + row_h * static_cast<float>(i);
        const D2D1_RECT_F row = D2D1::RectF(card.left + 5.0f * scale_, y, card.right - 5.0f * scale_, y + row_h);
        if (static_cast<int>(i) == vm.address_suggestion_selected) {
            MakeBrush(dc, WithAlpha(theme.accent, 0.16f), brAccent_);
            FillRoundedRect(dc, brAccent_.get(), row.left, row.top, row.right - row.left, row_h, 6.0f * scale_);
        }
        if (item.real || !app::AliasStore::Instance().ShowMark()) {
            DrawIconText(row.left + 8.0f * scale_, y, 18.0f * scale_, row_h, kIconFolder, L"[]", theme.icon_folder, 0.95f);
        } else {
            // The alias mark leads: what matched is a name the user gave.
            const float m = kAliasMarkDip * scale_;
            const float mx = std::round(row.left + 10.0f * scale_);
            const float my = std::round(y + (row_h - m) * 0.5f);
            painter_.DrawAliasMark(D2D1::RectF(mx, my, mx + m, my + m));
        }
        float x = row.left + 34.0f * scale_;
        float right = row.right - 10.0f * scale_;
        if (!item.hint.empty()) {
            // Right-aligned hint: 子文件夹 / 按拼音匹配 · Enter 前往.
            const float hint_w = measure_small(item.hint) + 1.0f;
            if (hint_w < (right - x) * 0.45f) {
                MakeBrush(dc, WithAlpha(pal.text_secondary, 0.75f), brText_);
                DrawTextRect(dc, small_fmt, brText_.get(), item.hint, right - hint_w, y, hint_w, row_h);
                right -= hint_w + 12.0f * scale_;
            }
        }
        const float label_w = (std::min)(MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt, item.label) + 1.0f,
                                         (std::max)(0.0f, (right - x) * 0.6f));
        MakeBrush(dc, pal.text, brText_);
        DrawTextRect(dc, fmt, brText_.get(), item.label, x, y, label_w, row_h);
        x += label_w + 10.0f * scale_;
        const float path_w = (std::max)(0.0f, right - x);
        if (path_w > 24.0f * scale_) {
            MakeBrush(dc, pal.text_secondary, brText_);
            DrawTextRect(dc, small_fmt, brText_.get(), FitEndEllipsis(item.path, path_w, measure_small),
                         x, y, path_w, row_h);
        }
    }
    text_background_ = saved_bg;
}

namespace {
constexpr float kTagSuggestRowDip = 30.0f;
}

D2D1_RECT_F MainRenderer::FilterTagSuggestionsRect(const D2D1_RECT_F& anchor, float w, size_t count) const {
    const float width = (std::max)(anchor.right - anchor.left, 240.0f * scale_);
    const float margin = 8.0f * scale_;
    float left = anchor.left;
    if (left + width > w - margin) left = (std::max)(margin, w - margin - width);
    const float top = anchor.bottom + 4.0f * scale_;
    const float h = (2.0f * kSuggestPadDip + kTagSuggestRowDip * static_cast<float>(count)) * scale_;
    return D2D1::RectF(left, top, left + width, top + h);
}

int MainRenderer::FilterTagSuggestionAt(const D2D1_RECT_F& anchor, float w, size_t count, float x, float y) const {
    if (count == 0) return -1;
    const D2D1_RECT_F rc = FilterTagSuggestionsRect(anchor, w, count);
    if (x < rc.left || x >= rc.right || y < rc.top || y >= rc.bottom) return -1;
    const int row = static_cast<int>((y - rc.top - kSuggestPadDip * scale_) / (kTagSuggestRowDip * scale_));
    return y >= rc.top + kSuggestPadDip * scale_ && row >= 0 && row < static_cast<int>(count) ? row : -2;
}

void MainRenderer::DrawFilterTagSuggestions(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                            const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const size_t count = vm.filter_tag_suggestions.size();
    const D2D1_RECT_F card = FilterTagSuggestionsRect(vm.filter_tag_anchor, rect.right - rect.left, count);
    const SnackPalette pal = MakeSnackPalette(theme, IsHighContrast());
    DrawSnackPlate(dc, card, 8.0f * scale_, pal, scale_, IsHighContrast());
    const D2D1_COLOR_F saved_bg = text_background_;
    text_background_ = pal.plate;
    IDWriteTextFormat* fmt = compositor_->TextFormat();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const float row_h = kTagSuggestRowDip * scale_;
    for (size_t i = 0; i < count; ++i) {
        const FilterTagSuggestionView& item = vm.filter_tag_suggestions[i];
        const float y = card.top + kSuggestPadDip * scale_ + row_h * static_cast<float>(i);
        const D2D1_RECT_F row = D2D1::RectF(card.left + 5.0f * scale_, y, card.right - 5.0f * scale_, y + row_h);
        if (static_cast<int>(i) == vm.filter_tag_selected) {
            MakeBrush(dc, WithAlpha(theme.accent, 0.16f), brAccent_);
            FillRoundedRect(dc, brAccent_.get(), row.left, row.top, row.right - row.left, row_h, 6.0f * scale_);
        }
        const float dot = 5.0f * scale_;
        const float cx = row.left + 14.0f * scale_;
        const float cy = y + row_h * 0.5f;
        MakeBrush(dc, item.color, brAccent_);
        FillRoundedRect(dc, brAccent_.get(), cx - dot, cy - dot, dot * 2.0f, dot * 2.0f, dot);
        const std::wstring number = std::to_wstring(item.count);
        const float number_w = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), small_fmt, number) + 1.0f;
        const float right = row.right - 10.0f * scale_;
        MakeBrush(dc, pal.text_secondary, brText_);
        DrawTextRect(dc, small_fmt, brText_.get(), number, right - number_w, y, number_w, row_h);
        const float x = row.left + 30.0f * scale_;
        MakeBrush(dc, pal.text, brText_);
        DrawTextRect(dc, fmt, brText_.get(), item.name, x, y, (std::max)(0.0f, right - number_w - 10.0f * scale_ - x), row_h);
    }
    text_background_ = saved_bg;
}

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out, BreadcrumbStrip* strip) const {
    out.clear();
    if (strip) *strip = BreadcrumbStrip{};
    auto segments = SplitBreadcrumb(vm.is_query_search && vm.has_search_origin ? vm.search_origin : vm.path);
    // 别名: a folder on the way shows its alias; the click still goes to the real path.
    if (!app::AliasStore::Instance().Empty()) {
        for (auto& seg : segments) {
            if (seg.path.empty() || seg.path.starts_with(L"pulse:")) continue;
            if (std::wstring alias = app::AliasStore::Instance().Shown(seg.path); !alias.empty()) {
                seg.text = std::move(alias);
                seg.alias = app::AliasStore::Instance().ShowMark();   // the mark is optional
            }
        }
    }
    if (vm.is_query_search && vm.has_search_origin) {
        BreadcrumbSegment search;
        search.text = vm.search_crumb.empty() ? pulse::l10n::Get(pulse::l10n::StringId::Search) : vm.search_crumb;
        search.path = vm.path;
        segments.push_back(std::move(search));
    }
    // Details tree: a selected child row's folder continues the path in
    // faint segments; a click still navigates there.
    std::vector<BreadcrumbSegment> ghosts;
    const std::wstring& dir = vm.tree_selection_dir;
    const std::wstring& rel = vm.tree_selection_rel;
    if (!vm.is_query_search && !segments.empty() && !rel.empty() && dir.size() > rel.size() &&
        dir.ends_with(rel)) {
        const size_t dir_base = dir.size() - rel.size();
        size_t at = 0;
        while (at < rel.size()) {
            size_t next = rel.find(L'\\', at);
            if (next == std::wstring::npos) next = rel.size();
            BreadcrumbSegment seg;
            seg.text = rel.substr(at, next - at);
            seg.path = dir.substr(0, dir_base + next);
            if (!seg.text.empty()) ghosts.push_back(std::move(seg));
            at = next + 1;
        }
    }
    if (segments.empty()) return;
    D2D1_RECT_F addr = ToolbarLayoutAt(w, 0).address_text;
    const float segPad = 8.0f * scale_;
    const float chevronW = 14.0f * scale_;
    const float hint = FavoritesButtonShown() ? 28.0f * scale_ : 0.0f;   // the ★⌄ button
    const float avail = std::max(0.0f, addr.right - addr.left - 2 * margin_ - hint);

    IDWriteFactory2* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
    IDWriteTextFormat* fmt = compositor_ ? compositor_->AddressFormat() : nullptr;

    const auto navigation = ToolbarLayoutAt(w, 0);
    const size_t keep = navigation.merged_search ? 1 : navigation.navigation_tier == NavigationTier::Narrow ? 2 : 0;
    if (keep && !segments.empty()) {
        const bool folded = segments.size() > keep;
        const size_t first = folded ? segments.size() - keep : 0;
        const float more_width = folded ? (std::min)(24 * scale_, avail) : 0;
        const size_t count = segments.size() - first;
        const float gaps = chevronW * static_cast<float>(count - 1 + (folded ? 1 : 0));
        const bool tail = !vm.is_query_search && (vm.path.empty() || !fs::IsVirtualPath(vm.path));
        float remaining = (std::max)(0.0f, avail - more_width - gaps - (tail ? chevronW : 0));
        float x = addr.left + margin_;
        if (folded) {
            BreadcrumbPlaced more;
            more.text = L"\u2026";
            more.path = vm.is_query_search && vm.has_search_origin ? vm.search_origin : vm.path;
            more.rc = {x, addr.top + 2 * scale_, x + more_width, addr.bottom - 2 * scale_};
            out.push_back(std::move(more));
            x += more_width + chevronW;
        }
        for (size_t i = first; i < segments.size(); ++i) {
            const float natural = MeasureTextWidth(dwrite, fmt, segments[i].text) + segPad * 2 +
                (segments[i].alias ? kCrumbAliasMarkDip * scale_ : 0);
            const float cap = i + 1 < segments.size() ? remaining * 0.45f : remaining;
            const float item_width = (std::min)(natural, cap);
            BreadcrumbPlaced item;
            item.text = segments[i].text; item.path = segments[i].path; item.alias = segments[i].alias;
            item.rc = {x, addr.top + 2 * scale_, x + item_width, addr.bottom - 2 * scale_};
            out.push_back(std::move(item));
            remaining -= item_width; x += item_width + chevronW;
        }
        crumb_scroll_ = crumb_scroll_shown_ = 0;
        if (strip) {
            strip->view = {addr.left + margin_, addr.top, addr.left + margin_ + avail, addr.bottom};
            strip->width = avail;
        }
        return;
    }

    struct Measured { float w; };
    std::vector<float> widths(segments.size(), 0.0f);
    float total = 0.0f;
    for (size_t i = 0; i < segments.size(); ++i) {
        widths[i] = MeasureTextWidth(dwrite, fmt, segments[i].text) + segPad * 2 +
                    (segments[i].alias ? kCrumbAliasMarkDip * scale_ : 0.0f);
        total += widths[i] + (i ? chevronW : 0.0f);
    }
    // The faint tail only shows when all of it fits next to the whole path.
    std::vector<float> ghost_widths(ghosts.size(), 0.0f);
    float ghost_total = 0.0f;
    for (size_t i = 0; i < ghosts.size(); ++i) {
        ghost_widths[i] = MeasureTextWidth(dwrite, fmt, ghosts[i].text) + segPad * 2;
        ghost_total += ghost_widths[i] + chevronW;
    }
    // The faint tail stays only when the current folder and all of it fit
    // in the bar together; otherwise it goes.
    {
        float rest = total;
        for (size_t lead = 0; rest + ghost_total > avail && lead + 1 < segments.size(); ++lead)
            rest -= widths[lead] + chevronW;
        if (ghosts.empty() || rest + ghost_total > avail) {
            ghosts.clear();
            ghost_total = 0.0f;
        }
    }
    // A path longer than the bar is one strip seen through the bar, resting
    // on its end; the wheel scrolls it toward the root (ScrollBreadcrumb).
    const std::wstring key = vm.is_query_search && vm.has_search_origin
        ? vm.search_origin + L"|" + vm.path : vm.path;
    if (key != crumb_scroll_key_) {
        crumb_scroll_key_ = key;
        crumb_scroll_ = crumb_scroll_shown_ = 0.0f;
    }
    const float view_left = addr.left + margin_;
    const bool trailing_arrow = !vm.is_query_search && (vm.path.empty() || !fs::IsVirtualPath(vm.path));
    const float width = total + ghost_total + (trailing_arrow ? chevronW : 0.0f);
    const float overflow = std::max(0.0f, width - avail);
    crumb_scroll_ = std::clamp(crumb_scroll_, 0.0f, overflow);
    const float offset = std::clamp(crumb_scroll_shown_, 0.0f, overflow);
    if (strip) {
        strip->view = D2D1::RectF(view_left, addr.top, view_left + avail, addr.bottom);
        strip->overflow = overflow;
        strip->offset = offset;
        strip->width = width;
    }

    float x = std::round(view_left - (overflow - offset));
    for (size_t i = 0; i < segments.size(); ++i) {
        if (i > 0) x += chevronW;
        BreadcrumbPlaced p;
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + widths[i], addr.bottom - 2 * scale_);
        p.text = segments[i].text;
        p.path = segments[i].path;
        p.alias = segments[i].alias;
        out.push_back(p);
        x += widths[i];
    }
    for (size_t i = 0; i < ghosts.size(); ++i) {
        x += chevronW;
        BreadcrumbPlaced p;
        p.rc = D2D1::RectF(x, addr.top + 2 * scale_, x + ghost_widths[i], addr.bottom - 2 * scale_);
        p.text = std::move(ghosts[i].text);
        p.path = std::move(ghosts[i].path);
        p.ghost = true;
        out.push_back(std::move(p));
        x += ghost_widths[i];
    }
}

bool MainRenderer::ScrollBreadcrumb(const WindowViewModel& vm, float window_w, float x, float y, float steps) {
    if (vm.address_editing || vm.address_searching || steps == 0.0f) return false;
    std::vector<BreadcrumbPlaced> placed;
    BreadcrumbStrip strip;
    BreadcrumbLayout(vm.pane, window_w, placed, &strip);
    const D2D1_RECT_F addr = AddressBarRect(window_w);
    if (strip.overflow <= 0.0f || x < addr.left || x >= strip.view.right || y < addr.top || y >= addr.bottom)
        return false;
    // About one segment a notch; a precision touchpad sends fractions.
    crumb_scroll_ = std::clamp(crumb_scroll_ + steps * 72.0f * scale_, 0.0f, strip.overflow);
    crumb_hover_left_ = 0;
    if (!motion::SystemAnimationsEnabled() || IsHighContrast()) crumb_scroll_shown_ = crumb_scroll_;
    return true;
}

bool MainRenderer::DragScrollBreadcrumb(const WindowViewModel& vm, float window_w, float x) {
    if (vm.address_editing || vm.address_searching) return false;
    std::vector<BreadcrumbPlaced> placed;
    BreadcrumbStrip strip;
    BreadcrumbLayout(vm.pane, window_w, placed, &strip);
    if (strip.overflow <= 0.0f) return false;
    const float zone = 28.0f * scale_;
    float dir = 0.0f;
    if (x < strip.view.left + zone && crumb_scroll_ < strip.overflow) dir = 1.0f;
    else if (x >= strip.view.right - zone && x < strip.view.right && crumb_scroll_ > 0.0f) dir = -1.0f;
    const uint64_t now = GetTickCount64();
    if (dir == 0.0f || now - crumb_drag_tick_ < 90) return false;
    crumb_drag_tick_ = now;
    crumb_scroll_ = std::clamp(crumb_scroll_ + dir * 36.0f * scale_, 0.0f, strip.overflow);
    crumb_hover_left_ = 0;
    if (!motion::SystemAnimationsEnabled() || IsHighContrast()) crumb_scroll_shown_ = crumb_scroll_;
    return true;
}

bool MainRenderer::SettleBreadcrumbScroll(uint64_t now_ms) {
    if (crumb_scroll_ <= 0.0f || crumb_hover_left_ == 0 || now_ms - crumb_hover_left_ < 1500) return false;
    crumb_scroll_ = 0.0f;
    crumb_hover_left_ = 0;
    if (!motion::SystemAnimationsEnabled() || IsHighContrast()) crumb_scroll_shown_ = 0.0f;
    return true;
}
void MainRenderer::DrawTextRect(ID2D1DeviceContext* dc, IDWriteTextFormat* fmt,
    ID2D1SolidColorBrush* br, std::wstring_view text, float x, float y, float w, float h,
    D2D1_DRAW_TEXT_OPTIONS opts) {
    D2D1_RECT_F rc = typography::SnapVerticalBounds(D2D1::RectF(x, y, x + w, y + h));
    if (compositor_ && DrawLegacyIcon(dc, compositor_->DwriteFactory(), text, rc, br, fmt->GetFontSize())) return;
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

// MainRenderer::icon_tone_colors_ holds one colour per tone.
static_assert(static_cast<size_t>(command_icons::Tone::Count) == 8);
void MainRenderer::DrawIconText(float x, float y, float w, float h,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& color, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const auto command = command_icons::FromGlyph(glyph);
    if (command != command_icons::Icon::None) {
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, color, brText_);
        auto bounds = command_icons::CenteredBounds(D2D1::RectF(x, y, x + w, y + h),
                                                     icon_box_dip_ * scale_ * size_factor);
        float width = command_icons::kStroke;
        if (icon_stroke_px_ > 0.0f) {
            // Toolbar: the same line weight at every size (chevrons and x a
            // touch lighter), drawn from a whole pixel so no icon is softer.
            const float side = bounds.right - bounds.left;
            if (side > 0.0f) {
                const float px = side < 15.0f * scale_ ? icon_stroke_px_ * 0.86f : icon_stroke_px_;
                width = px * command_icons::kCanvas / side;
                bounds.left = std::round(bounds.left);
                bounds.top = std::round(bounds.top);
                bounds.right = bounds.left + side;
                bounds.bottom = bounds.top + side;
            }
        }
        const auto tone = icon_tones_ && !IsHighContrast() ? command_icons::ToneOf(command)
                                                           : command_icons::Tone::None;
        ID2D1Brush* accent = nullptr;
        if (tone != command_icons::Tone::None) {
            MakeBrush(dc, icon_tone_colors_[static_cast<size_t>(tone)], brIconTone_);
            accent = brIconTone_.get();
        }
        if (command_icons::DrawTwoTone(dc, brText_.get(), accent, paneHeaderStroke_.get(), command, bounds, 1.0f, width))
            return;
    }
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

namespace {
void UnionInto(D2D1_RECT_F& r, bool& any, const D2D1_RECT_F& a) {
    if (a.right <= a.left || a.bottom <= a.top) return;
    if (!any) {
        r = a;
        any = true;
        return;
    }
    r.left = std::min(r.left, a.left);
    r.top = std::min(r.top, a.top);
    r.right = std::max(r.right, a.right);
    r.bottom = std::max(r.bottom, a.bottom);
}
} // namespace

bool MainRenderer::OverlayUp(const WindowViewModel& vm) const {
    // Everything the tail of Render floats over the window, and the hover card.
    return vm.settings_open || vm.pane.marquee_active || vm.content_drop || vm.drag_ghost.on ||
           !vm.drag_badge.empty() || vm.change_popover.visible ||
           (vm.address_editing && !vm.address_suggestions.empty()) ||
           !vm.filter_tag_suggestions.empty() || vm.teach.visible || !vm.tooltip_text.empty() ||
           group_wheel_.Visible() || window_overview_.IsOpen() || window_switcher_.IsOpen() ||
           ai_panel_.FloatingVisible() ||
           caret_due_ms_ != 0 || hover_frame_ms_ != 0 ||
           hover_card_rect_.right > hover_card_rect_.left;
}

bool MainRenderer::NonHoverMotionActive() const {
    // TickMotion without the list hover plates.
    const uint64_t m = motion::NowMs();
    const ULONGLONG now = GetTickCount64();
    if (sidebar_pill_.Active(m) || settings_nav_pill_.Active(m) || settings_nav_hover_.Active(m) ||
        collapse_anim_ || details_anim_ || ai_panel_.Animating() || ai_panel_.WantsDecorFrame())
        return true;
    for (const auto& motion : list_shift_) if (motion.Active(m)) return true;
    for (const auto& motion : view_morph_) if (motion.Active(m)) return true;
    for (const auto& motion : link_pill_motion_) if (motion.Active(m)) return true;
    if ((task_pill_done_at_ != 0 && now - task_pill_done_at_ < task_pill_hold_ms_ + 400) ||
        (task_pill_pop_at_ != 0 && now - task_pill_pop_at_ < kTaskPillPopMs + 40))
        return true;
    if (ticks_active_ || motion_frame_wanted_) return true;
    if (m < tab_motion_until_ || m < row_leave_until_ || m < mtime_flash_until_ ||
        m < splitter_motion_until_ || m < spring_ring_until_)
        return true;
    return list_loading_active_ || folder_size_anim_active_ || task_pill_spinning_;
}

std::vector<MainRenderer::PaintedSelection> MainRenderer::FrameSelections(const WindowViewModel& vm) {
    std::vector<PaintedSelection> result;
    const auto append = [&](const PaneViewModel& pane) {
        result.push_back({pane.selection_owner, pane.snapshot.get(), pane.selection_revision,
            pane.selected_index, pane.selected_count, pane.all_selected});
    };
    if (vm.pane_slots.empty()) append(vm.pane);
    else for (const auto& slot : vm.pane_slots) append(slot.pane);
    return result;
}

bool MainRenderer::PartialRedrawSafe(const WindowViewModel& vm) const {
    // Hover damage does not cover rows deselected elsewhere in the pane.
    // Compare values captured for the previous frame, never the live set pointer.
    return painted_selections_ == FrameSelections(vm) &&
        !last_frame_overlay_ && !OverlayUp(vm) && !NonHoverMotionActive();
}

bool MainRenderer::HoverRowDamage(int pane_index, int old_source, int new_source,
                                  D2D1_RECT_F* out) const {
    if (!out || pane_index < 0 || pane_index >= static_cast<int>(drawn_rows_.size())) return false;
    if (tree_guides_drawn_[static_cast<size_t>(pane_index)]) return false;
    const auto& rows = drawn_rows_[static_cast<size_t>(pane_index)];
    if (rows.empty()) return false;
    D2D1_RECT_F r{};
    bool any = false;
    for (const int source : {old_source, new_source}) {
        if (source < 0) continue;
        const auto it = std::find_if(rows.begin(), rows.end(),
                                     [&](const auto& row) { return row.first == source; });
        if (it == rows.end()) return false;
        UnionInto(r, any, it->second);
    }
    UnionInto(r, any, list_hover_motion_[static_cast<size_t>(pane_index)].Span());
    if (!any) return false;
    const float margin = 4.0f * scale_;
    *out = D2D1::RectF(r.left - margin, r.top - margin, r.right + margin, r.bottom + margin);
    return true;
}

bool MainRenderer::HoverMotionDamage(D2D1_RECT_F* out) const {
    if (!out) return false;
    const uint64_t m = motion::NowMs();
    D2D1_RECT_F r{};
    bool any = false;
    for (size_t i = 0; i < list_hover_motion_.size(); ++i) {
        if (!list_hover_motion_[i].Active(m)) continue;
        if (i >= drawn_rows_.size() || drawn_rows_[i].empty()) return false;
        UnionInto(r, any, list_hover_motion_[i].Span());
    }
    if (!any) return false;
    const float margin = 4.0f * scale_;
    *out = D2D1::RectF(r.left - margin, r.top - margin, r.right + margin, r.bottom + margin);
    return true;
}

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    const auto perf_start = std::chrono::steady_clock::now();
    frame_perf_ = {};
    PULSE_SCENE_MARK("scene_enter");
    ++motion_frame_;
    asst_drawn_ = false;   // DrawSettingsAssistant sets it again
    last_frame_overlay_ = OverlayUp(vm);
    painted_selections_ = FrameSelections(vm);
    thumbnail_cache_.AdvanceFrame(notify_hwnd_);
    folder_thumbnail_cache_.BeginFrame();
    struct FolderFrameScope {
        FolderThumbnailCache& cache;
        ThumbnailCache& thumbnails;
        bool& motion_wanted;
        ~FolderFrameScope() {
            cache.EndFrame();
            thumbnails.EndInterestFrame();
            if (thumbnails.TakeUploadFrameRequest()) motion_wanted = true;
            if (cache.TakeUploadFrameRequest()) motion_wanted = true;
        }
    } folder_frame{folder_thumbnail_cache_, thumbnail_cache_, motion_frame_wanted_};
    // One timestamp per frame: every glide and the sidebar width agree.
    motion_now_ = motion::NowMs();
    uintptr_t active_tab = 0;
    for (const auto& tab : vm.tabs) if (tab.active) { active_tab = tab.id; break; }
    if (active_tab != content_tab_identity_) {
        // Switching tabs is not folder navigation. Retire the previous tab's
        // row motion before drawing; never morph two unrelated listings.
        nav_enter_ = {}; list_shift_ = {}; view_morph_ = {};
        list_hover_motion_ = {}; item_check_motion_ = {}; link_pill_motion_ = {};
        list_loading_since_ = {}; folder_size_pending_since_ = {};
        content_tab_identity_ = active_tab;
    }
    BeginPaneZoomMotion(vm);
    caret_due_ms_ = 0;
    list_loading_active_ = false;
    folder_size_anim_active_ = false;
    motion_frame_wanted_ = false;
    landing_rects_.clear();
    if (collapse_anim_) {
        // Sampled once per frame so every layout query agrees on the width.
        // Motion v2 "rail": the width rides the smooth spring (seb-sidebar-collapse).
        const float ms = static_cast<float>(motion_now_ - collapse_start_);
        const float target = sidebar_collapsed_ ? 1.0f : 0.0f;
        if (ms >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))) {
            collapse_value_ = target;
            collapse_anim_ = false;
        } else {
            const float p = motion::SpringProgress(motion::spring::Smooth, ms);
            collapse_value_ = std::clamp(collapse_from_ + (target - collapse_from_) * p, 0.0f, 1.0f);
        }
    }
    // Pulse 助手: the docked column width is frame-stable like the sidebar's.
    ai_panel_.Tick(motion_now_);
    if (details_anim_) {
        const float ms = static_cast<float>(motion_now_ - details_anim_start_);
        const float target = details_visible_ ? 1.0f : 0.0f;
        if (ms >= static_cast<float>(motion::SpringSettleMs(motion::spring::Smooth))) {
            details_value_ = target;
            details_anim_ = false;
        } else {
            const float p = motion::SpringProgress(motion::spring::Smooth, ms);
            details_value_ = std::clamp(details_from_ + (target - details_from_) * p, 0.0f, 1.0f);
        }
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
    open_with_icons_.SetDeviceContext(compositor_->Dc());
    UpdateBrushes(theme);
    text_background_ = theme.bg;
    painter_.BeginFrame(theme, IsHighContrast());
    PULSE_SCENE_MARK("scene_resources");

    // None effect + selected image: the (optionally blurred) wallpaper covers
    // the window base; the title scrim, one shared sheet and the pane cards
    // stack over it with the opacities from ComputeLayerAlphas.
    const bool image_mode = vm.window_effect == WindowEffect::None &&
                            !vm.background_image.empty() && !IsHighContrast();
    bool backdrop_drawn = false;
    if (embedded_) {
        // The host window owns the backdrop; only the shared sheet is tinted.
    } else if (image_mode) {
        backdrop_drawn = material_.DrawSourceCover(dc, rect, vm.background_image,
                                                   WallpaperBlurDip(vm.wallpaper_blur) * scale_);
    } else {
        backdrop_drawn = material_.DrawBackdrop(
            dc, rect, vm.window_effect, vm.dark,
            (vm.window_effect == WindowEffect::None) ? std::wstring{} : vm.background_image);
    }
    PULSE_SCENE_MARK("scene_backdrop");
    const LayerAlphas layers = ComputeLayerAlphas(image_mode, backdrop_drawn,
        vm.backdrop_enabled, vm.dark, vm.wallpaper_look, vm.wallpaper_blur, vm.frame_transparency);
    if (!backdrop_drawn && !vm.backdrop_enabled) {
        // Lower pane opacity must reveal the theme canvas when no material or
        // wallpaper exists, instead of exposing an unpainted transparent base.
        MakeBrush(dc, theme.bg, brBg_);
        dc->FillRectangle(rect, brBg_.get());
    }
    sheet_alpha_ = layers.sheet;
    card_alpha_ = layers.card;
    pane_alpha_ = layers.pane;
    if (pane_alpha_ >= 0.0f) {
        // The colour the stacked layers would have given the card.
        const float w = layers.pane_mix[0] + layers.pane_mix[1] + layers.pane_mix[2];
        const auto mix = [&](float D2D1_COLOR_F::*c) {
            return w > 0.0f ? (layers.pane_mix[0] * (theme.surface_title.*c) + layers.pane_mix[1] * (theme.surface_sheet.*c) +
                               layers.pane_mix[2] * (theme.surface_card.*c)) / w
                            : theme.surface_card.*c;
        };
        pane_color_ = D2D1::ColorF(mix(&D2D1_COLOR_F::r), mix(&D2D1_COLOR_F::g), mix(&D2D1_COLOR_F::b), pane_alpha_);
    }
    auto tint_background = [&](D2D1_RECT_F bounds, D2D1_COLOR_F color, float alpha) {
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top || alpha <= 0) return;
        MakeBrush(dc, WithAlpha(color, alpha), brBg_);
        dc->FillRectangle(bounds, brBg_.get());
    };
    if (layers.title > 0.0f) {
        tint_background(rect, theme.surface_title, layers.title);
    }
    // One sheet below the title strip carries the toolbar, sidebar and status
    // bar; the active tab uses the same fill and meets it on a whole pixel so
    // translucent layers never double up into a seam.
    if (sheet_alpha_ > 0.0f) {
        const float sheetTop = std::round(title_bar_height_);
        tint_background({rect.left, sheetTop, rect.right, rect.bottom}, theme.surface_sheet, sheet_alpha_);
    }

    if (!embedded_) DrawTitleBar(vm, rect, theme);
    PULSE_SCENE_MARK("scene_title");
    // Over a wallpaper or Mica the docked AI panel is a card like the pane.
    ai_panel_.SetGlass(!embedded_ && card_alpha_ < 0.999f && !IsHighContrast(), card_alpha_,
                       CardFrameColor(theme, vm.dark, card_alpha_));
    // The not-connected empty state shows the services' logos (only called within Draw).
    ai_panel_.SetMarkPainter([this, &theme](std::string_view id, const D2D1_RECT_F& r) {
        DrawProviderMark(id, {}, r, theme);
    });
    if (vm.settings_open) {
        details_preview_bounds_ = {};
        DrawSettings(vm, rect, theme);
        if (ai_panel_.Visible()) {
            // Docked: the column at the left edge, Settings moved right of it
            // (vm.settings_inset_left); floating: over Settings as elsewhere.
            const float dock = AiDockWidth(rect.right);
            ai_panel_.Draw(compositor_, vm.ai_panel, theme,
                           D2D1::RectF(rect.left, title_bar_height_, rect.left + dock, rect.bottom - status_height_),
                           AiPanelView::DockTargetWidth(rect.right, kSidebarRailWidthDip * scale_, scale_),
                           AiFloatingArea(rect.right, rect.bottom), scale_, motion_now_);
        }
        DrawStatusBar(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        PULSE_SCENE_MARK("scene_toolbar");
        if (favorites_bar_) DrawFavoritesBar(vm, rect, theme);
        const bool peek = SidebarPeekVisible(rect.right);
        if (!peek) DrawSidebar(vm, rect, theme);
        PULSE_SCENE_MARK("scene_sidebar");
        const auto pane_start = std::chrono::steady_clock::now();
        const bool layoutPreview=vm.layout_controls.preview_preset>=0;
        if(layoutPreview) dc->PushLayer(D2D1::LayerParameters(ContentRect(rect.right,rect.bottom),nullptr,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,D2D1::Matrix3x2F::Identity(),.28f),nullptr);
        DrawPane(vm, rect, theme);
        if(layoutPreview) dc->PopLayer();
        DrawPaneZoomMotion(vm, rect);
        frame_perf_.pane_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - pane_start).count());
        PULSE_SCENE_MARK("scene_panes");
        if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
        else details_preview_bounds_ = {};
        if (ai_panel_.Visible()) {
            ai_panel_.Draw(compositor_, vm.ai_panel, theme, AiColumnRect(rect.right, rect.bottom),
                           AiPanelView::DockTargetWidth(rect.right, kSidebarRailWidthDip * scale_, scale_),
                           AiFloatingArea(rect.right, rect.bottom), scale_, motion_now_);
        }
        if (peek) DrawSidebarPeek(vm, rect, theme);
        if (!embedded_) DrawStatusBar(vm, rect, theme);
        if (!peek) DrawTrayCompareCard(vm, theme);
        DrawWorkspacePreview(vm, rect, theme);
    }

    // A tab dragged from another window over the content (multi-window M6d):
    // the area each of its panes would take. What the drop does is in the
    // hint beside the dragged tab (M7h).
    if (vm.content_drop) {
        const float radius = 8.0f * scale_;
        const float inset = 1.0f * scale_;
        for (const D2D1_RECT_F& area : vm.content_drop_rects) {
            MakeBrush(dc, WithAlpha(theme.accent, 0.16f), brAccent_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(area, radius, radius), brAccent_.get());
            MakeBrush(dc, theme.accent, brAccent_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(area.left + inset, area.top + inset,
                                                                   area.right - inset, area.bottom - inset),
                                                       radius, radius),
                                     brAccent_.get(), 2.0f * scale_);
        }
    }

    // The dragged tab (multi-window M7h): its window is hidden while it is
    // over this one, so it follows the cursor as a pill, and a hint below
    // says what letting go does. Both are capsules in the notification style
    // (M7i, toast redesign A): acrylic in the window's own theme.
    if (vm.drag_ghost.on) {
        const DragGhostView& ghost = vm.drag_ghost;
        IDWriteFactory2* const factory = compositor_->DwriteFactory();
        IDWriteTextFormat* const tabFmt = compositor_->TabFormat();
        IDWriteTextFormat* const fmt = compositor_->TextFormat();
        IDWriteTextFormat* const smallFmt = compositor_->SmallFormat();
        const SnackPalette pal = MakeSnackPalette(theme, IsHighContrast());
        const bool high_contrast = IsHighContrast();
        const D2D1_COLOR_F saved_bg = text_background_;
        text_background_ = pal.plate;
        const auto plate = [&](float x, float y, float w, float h, float r) {
            DrawSnackPlate(dc, D2D1::RectF(x, y, x + w, y + h), r, pal, scale_, high_contrast);
        };

        std::wstring panes;
        if (ghost.panes > 1) {
            wchar_t buffer[64]{};
            swprintf_s(buffer, pulse::l10n::Get(pulse::l10n::StringId::DropPaneCount).c_str(), ghost.panes);
            panes = buffer;
        }
        const float titleW = (std::min)(MeasureLayoutText(compositor_, factory, tabFmt, ghost.title), 220.0f * scale_);
        const float chipW = panes.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, smallFmt, panes);
        const float pillH = 28.0f * scale_;
        const float pillR = pillH * 0.5f;
        const float pillW = 30.0f * scale_ + titleW + (chipW > 0.0f ? 7.0f * scale_ + chipW : 0.0f) + 13.0f * scale_;
        const float px = std::round((std::clamp)(ghost.x - 28.0f * scale_, rect.left, (std::max)(rect.left, rect.right - pillW)));
        const float py = std::round((std::clamp)(ghost.y - 14.0f * scale_, rect.top, (std::max)(rect.top, rect.bottom - pillH)));
        plate(px, py, pillW, pillH, pillR);
        DrawIconText(px + 10.0f * scale_, py, 16.0f * scale_, pillH, kIconFolder, L"[]", theme.icon_folder, 0.95f);
        MakeBrush(dc, pal.text, brText_);
        DrawTabTitle(compositor_, tabFmt, brText_.get(), ghost.title, px + 30.0f * scale_, py, titleW, pillH, scale_);
        if (chipW > 0.0f) {
            MakeBrush(dc, pal.accent, brText_);
            DrawTextRect(dc, smallFmt, brText_.get(), panes, px + 30.0f * scale_ + titleW + 7.0f * scale_, py,
                         chipW + 1.0f, pillH);
        }

        if (!ghost.hint.empty()) {
            // The hint in up to three runs: the part that names the spot (a
            // side, a tab number) in the accent color, then a quieter detail
            // after a thin divider.
            const size_t em_at = (std::min)(ghost.em_start, ghost.hint.size());
            const size_t em_len = (std::min)(ghost.em_length, ghost.hint.size() - em_at);
            const std::wstring before = ghost.hint.substr(0, em_at);
            const std::wstring em = ghost.hint.substr(em_at, em_len);
            const std::wstring after = ghost.hint.substr(em_at + em_len);
            const float beforeW = before.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, fmt, before);
            const float emW = em.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, fmt, em);
            const float afterW = after.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, fmt, after);
            const float hintW = beforeW + emW + afterW;
            const float detailW = ghost.detail.empty() ? 0.0f : MeasureLayoutText(compositor_, factory, smallFmt, ghost.detail);
            const float divider = ghost.detail.empty() ? 0.0f : 17.0f * scale_;  // gap, 1 px line, gap
            const float bw = 28.0f * scale_ + hintW + divider + detailW;
            const float bh = 28.0f * scale_;
            const float br = bh * 0.5f;
            float bx = ghost.x + 14.0f * scale_, by = ghost.y + 22.0f * scale_;
            if (bx + bw > rect.right - 6.0f * scale_) bx = ghost.x - bw - 10.0f * scale_;
            if (by + bh > rect.bottom - 6.0f * scale_) by = ghost.y - bh - 22.0f * scale_;
            bx = std::round((std::max)(rect.left + 6.0f * scale_, bx));
            by = std::round((std::max)(rect.top + 6.0f * scale_, by));
            plate(bx, by, bw, bh, br);
            float tx = bx + 14.0f * scale_;
            if (!before.empty()) {
                MakeBrush(dc, pal.text, brText_);
                DrawTextRect(dc, fmt, brText_.get(), before, tx, by, beforeW + 1.0f, bh);
                tx += beforeW;
            }
            if (!em.empty()) {
                MakeBrush(dc, pal.accent, brText_);
                DrawTextRect(dc, fmt, brText_.get(), em, tx, by, emW + 1.0f, bh);
                tx += emW;
            }
            if (!after.empty()) {
                MakeBrush(dc, pal.text, brText_);
                DrawTextRect(dc, fmt, brText_.get(), after, tx, by, afterW + 1.0f, bh);
                tx += afterW;
            }
            if (!ghost.detail.empty()) {
                MakeBrush(dc, pal.divider, brStrokeCard_);
                const float lx = std::round(tx + 8.0f * scale_);
                dc->FillRectangle(D2D1::RectF(lx, by + 8.0f * scale_, lx + 1.0f, by + bh - 8.0f * scale_), brStrokeCard_.get());
                MakeBrush(dc, pal.text_secondary, brText_);
                DrawTextRect(dc, smallFmt, brText_.get(), ghost.detail, lx + 8.0f * scale_, by, detailW + 1.0f, bh);
            }
        }
        text_background_ = saved_bg;
    }

    // Match the drag ghost's separate action pill for external/native drags too.
    if (!vm.drag_badge.empty() && !DragVisualActive()) {
        const std::wstring here_format = l10n::Get(l10n::StringId::DragDropAlreadyHereFormat);
        const auto marker = here_format.find(L"%s");
        const bool cancel = vm.drag_badge == l10n::Get(l10n::StringId::DragHintReleaseCancel);
        const bool blocked = cancel || vm.drag_badge == l10n::Get(l10n::StringId::DragHintNoTarget) ||
            vm.drag_badge == l10n::Get(l10n::StringId::DragDropIntoSelf) ||
            (marker != std::wstring::npos && vm.drag_badge.starts_with(here_format.substr(0, marker)));
        const auto layout = LayoutDragFeedback(compositor_->DwriteFactory(), vm.drag_badge, scale_, blocked);
        const float bx = std::clamp(vm.drag_badge_x + 14 * scale_, rect.left + margin_,
            std::max(rect.left + margin_, rect.right - layout.width - margin_));
        const float by = std::clamp(vm.drag_badge_y + 12 * scale_, rect.top + margin_, rect.bottom - 24 * scale_ - margin_);
        MakeBrush(dc, theme.text, brText_);
        DrawDragFeedback(dc, brText_.get(), layout, bx, by, scale_, vm.dark, blocked,
            !vm.drag_badge_move, cancel, theme.accent, theme.accent_text);
    }

    if (vm.change_popover.visible) {
        const auto rc = ChangePopoverRect(vm.change_popover, rect, scale_);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, 8 * scale_);
        auto content = rc; content.left += 14 * scale_; content.right -= 14 * scale_;
        content.top += 10 * scale_; content.bottom -= 42 * scale_;
        size_t begin = 0;
        for (int line = 0; line < ChangePopoverLineCount(vm.change_popover) && begin < vm.change_popover.summary.size(); ++line) {
            const auto end = vm.change_popover.summary.find(L'\n', begin);
            auto line_rc = content; line_rc.top += line * 24 * scale_; line_rc.bottom = line_rc.top + 24 * scale_;
            painter_.DrawText(std::wstring_view(vm.change_popover.summary).substr(begin,
                end == std::wstring::npos ? end : end - begin), line_rc, compositor_->SmallFormat(), theme.text);
            if (end == std::wstring::npos) break;
            begin = end + 1;
        }
        content.top = rc.bottom - 38 * scale_; content.bottom = rc.bottom - 8 * scale_;
        painter_.DrawText(pulse::l10n::Get(pulse::l10n::StringId::ChangeView), content, compositor_->TextFormat(), theme.accent);
    }
    if (vm.address_editing && !vm.address_searching && !vm.address_suggestions.empty())
        DrawAddressSuggestions(vm, rect, theme);
    if (!vm.filter_tag_suggestions.empty()) DrawFilterTagSuggestions(vm, rect, theme);
    if (vm.layout_controls.visible) {
        // pane_layout_mockup.html: .strip/.sbtn, .zstrip/.mcell, .peek, .gslot/.gcap.
        // Member brushes and the shared dash stroke: nothing is created per frame.
        const auto& c=vm.layout_controls;
        const MockupInk mk=MakeMockupInk(theme,vm.dark);
        if(!paneHeaderStroke_.get())command_icons::CreateStrokeStyle(dc,&paneHeaderStroke_);
        if(!dashStroke_.get()) {
            ID2D1Factory* dash_factory=nullptr;dc->GetFactory(&dash_factory);
            if(dash_factory) {
                D2D1_STROKE_STYLE_PROPERTIES props{};props.dashStyle=D2D1_DASH_STYLE_DASH;
                dash_factory->CreateStrokeStyle(props,nullptr,0,&dashStroke_);dash_factory->Release();
            }
        }
        IDWriteTextFormat* const small_fmt=compositor_->SmallFormat();
        const auto text=[&](const std::wstring& value,D2D1_RECT_F r,const D2D1_COLOR_F& color,IDWriteTextFormat* fmt=nullptr) {
            MakeBrush(dc,color,brAccentText_);
            DrawTextEndEllipsis(compositor_,fmt?fmt:small_fmt,brAccentText_.get(),value,r.left,r.top,r.right-r.left,r.bottom-r.top);
        };
        const auto glyph=[&](command_icons::Icon icon,D2D1_RECT_F r,const D2D1_COLOR_F& color,float size=14.0f) {
            MakeBrush(dc,color,brAccentText_);
            command_icons::Draw(dc,brAccentText_.get(),paneHeaderStroke_.get(),icon,
                command_icons::CenteredBounds(r,size*scale_),1,command_icons::kStroke);
        };
        const auto pictogram=[&](int preset,D2D1_RECT_F r,const D2D1_COLOR_F& color) {
            constexpr command_icons::Icon icons[] = {command_icons::Icon::SplitSingle, command_icons::Icon::Split,
                command_icons::Icon::SplitStacked, command_icons::Icon::SplitThree, command_icons::Icon::SplitFour};
            glyph(icons[std::clamp(preset,0,4)],r,color);
        };
        // .sbtn.on: accent-soft, accent ink, and the 2 DIP bar 9 DIP in from each side.
        const auto on_button=[&](D2D1_RECT_F button) {
            painter_.FillRoundedRect(button,5*scale_,mk.accent_soft);
            painter_.FillRoundedRect({button.left+9*scale_,button.bottom-3*scale_,button.right-9*scale_,button.bottom-scale_},scale_,mk.accent_fg);
        };
        const auto strip=PaneLayoutStripBounds(vm,rect,scale_,compositor_->DwriteFactory(),small_fmt);
        const auto draw_buttons=[&](D2D1_RECT_F r, bool floating) {
            // Inline the buttons sit on the status bar itself; only the hover
            // tray of a collapsed strip is a flyout.
            if(floating) {
                DrawFlyoutShadow(dc,r,7*scale_,scale_,vm.dark);
                painter_.FillRoundedRect(r,7*scale_,theme.surface_flyout);
                painter_.StrokeRoundedRect(r,7*scale_,theme.stroke_card);
            }
            const float divider_x=std::round(r.left+140*scale_)+.5f,cy=(r.top+r.bottom)*.5f;
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            dc->DrawLine({divider_x,cy-7*scale_},{divider_x,cy+7*scale_},brStrokeDivider_.get(),scale_);
            for(int i=0;i<7;++i) {
                const auto button=PaneLayoutButtonBounds(r,i,scale_);
                const bool hot=(i<5&&vm.hover_region==HitTestResult::PaneLayoutPreset&&vm.hover_control_index==i)||
                    (i==5&&vm.hover_region==HitTestResult::PaneLayoutCompare)||(i==6&&vm.hover_region==HitTestResult::PaneLayoutZoom);
                const bool enabled=i<5||(i==5?c.pane_names.size()==2:c.pane_names.size()>1);
                const bool on=i<5?i==c.preset:(i==5&&c.compare&&enabled);
                if(on)on_button(button);
                else if(hot&&enabled)painter_.FillRoundedRect(button,5*scale_,theme.fill_hover);
                const D2D1_COLOR_F ink=!enabled?WithAlpha(theme.text_secondary,theme.text_secondary.a*.35f):
                    on?mk.accent_fg:hot?theme.text:theme.text_secondary;
                if(i<5)pictogram(i,button,ink);
                else glyph(i==5?command_icons::Icon::Compare:command_icons::Icon::PaneExpand,button,ink);
            }
        };
        if(c.zoom_index>=0) {
            // .zstrip: the label pill, the mini-map, the restore button.
            const auto map=PaneLayoutMiniBounds(strip,scale_);
            const float cy=(strip.top+strip.bottom)*.5f;
            const auto badge=D2D1::RectF(strip.left,cy-11*scale_,map.left-6*scale_,cy+11*scale_);
            const bool words=badge.right-badge.left>=PaneZoomLabelWidth(scale_,compositor_->DwriteFactory(),small_fmt,true)-.5f;
            painter_.FillRoundedRect(badge,11*scale_,mk.accent_soft);
            const float icon_left=words?badge.left+8*scale_:(badge.left+badge.right)*.5f-5*scale_;
            glyph(command_icons::Icon::PaneExpand,{icon_left,badge.top,icon_left+10*scale_,badge.bottom},mk.accent_fg,11.0f);
            if(words)text(l10n::Get(l10n::StringId::PaneZoomBadge),{badge.left+23*scale_,badge.top,badge.right-6*scale_,badge.bottom},mk.accent_fg);
            const auto cells=PaneLayoutCells(c.preset,map,2*scale_);
            for(size_t i=0;i<cells.size();++i) {
                const auto cell=cells[i];
                const bool expanded=static_cast<int>(i)==c.zoom_index;
                const bool hot=!expanded&&vm.hover_region==HitTestResult::PaneLayoutCell&&vm.hover_control_index==static_cast<int>(i);
                if(expanded||hot)painter_.FillRoundedRect(cell,3*scale_,expanded?mk.accent_fg:mk.accent_soft2);
                const float inset=.6f*scale_;
                MakeBrush(dc,expanded||hot?mk.accent_fg:mk.text3,brAccentText_);
                dc->DrawRoundedRectangle(D2D1::RoundedRect({cell.left+inset,cell.top+inset,cell.right-inset,cell.bottom-inset},3*scale_,3*scale_),
                    brAccentText_.get(),1.2f*scale_);
                if(static_cast<int>(i)==c.target_index) {
                    MakeBrush(dc,expanded?theme.surface_card:mk.accent_fg,brAccentText_);
                    dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cell.right-4*scale_,cell.top+4*scale_),2*scale_,2*scale_),brAccentText_.get());
                }
            }
            const auto restore=D2D1::RectF(strip.right-26*scale_,cy-11*scale_,strip.right,cy+11*scale_);
            const bool restore_hot=vm.hover_region==HitTestResult::PaneLayoutRestore;
            if(restore_hot)painter_.FillRoundedRect(restore,5*scale_,theme.fill_hover);
            glyph(command_icons::Icon::PaneRestore,restore,restore_hot?theme.text:theme.text_secondary);
        } else if(strip.right-strip.left<100*scale_) {
            // .strip-collapsed: a small caret, then the current layout as an
            // .sbtn.on; hovering opens the full row as a tray.
            const float cy=(strip.top+strip.bottom)*.5f;
            const auto caret=D2D1::RectF(strip.left+scale_,strip.top,strip.left+13*scale_,strip.bottom);
            const auto button=D2D1::RectF(caret.right+scale_,cy-11*scale_,caret.right+27*scale_,cy+11*scale_);
            if(vm.hover_region==HitTestResult::PaneLayoutTray&&!c.tray_open)painter_.FillRoundedRect(strip,7*scale_,theme.fill_hover);
            glyph(command_icons::Icon::ChevronLeft,caret,mk.text3,8.0f);
            on_button(button);
            pictogram(c.preset,button,mk.accent_fg);
        } else draw_buttons(strip,false);
        // .strip::before: 1 DIP divider 10 DIP left of the buttons, 14 DIP tall.
        {
            const float x=std::round(strip.left-10*scale_)+.5f,cy=(strip.top+strip.bottom)*.5f;
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            dc->DrawLine(D2D1::Point2F(x,cy-7*scale_),D2D1::Point2F(x,cy+7*scale_),brStrokeDivider_.get(),scale_);
        }
        if(c.tray_open&&c.zoom_index<0)draw_buttons(PaneLayoutTrayBounds(vm,rect,scale_,compositor_->DwriteFactory(),small_fmt),true);
        if(c.preview_preset>=0&&c.preview_preset<5) {
            // .gslot: dashed accent outline on a faint wash, the pane name on a
            // flyout capsule; .gcap: what the switch does, at the bottom.
            const auto area=ContentRect(rect.right,rect.bottom);
            IDWriteTextFormat* const name_fmt=compositor_->TextFormat();
            const auto capsule=[&](const std::wstring& label,float cx,float cy,float limit,bool name) {
                IDWriteTextFormat* const fmt=name?name_fmt:small_fmt;
                const float pad=(name?12.0f:14.0f)*scale_;
                const float width=std::max(0.0f,std::min(limit,MeasureTextWidth(compositor_->DwriteFactory(),fmt,label)+pad*2));
                if(width<pad*2+8*scale_)return;
                const auto pill=D2D1::RectF(std::round(cx-width*.5f),std::round(cy-15*scale_),std::round(cx+width*.5f),std::round(cy+15*scale_));
                DrawFlyoutShadow(dc,pill,15*scale_,scale_,vm.dark);
                painter_.FillRoundedRect(pill,15*scale_,theme.surface_flyout);
                text(label,{pill.left+pad,pill.top,pill.right-pad,pill.bottom},name?theme.text:theme.text_secondary,fmt);
            };
            const auto cells=PaneLayoutCells(c.preview_preset,area,8*scale_);
            for(size_t i=0;i<cells.size();++i) {
                const auto cell=cells[i];
                painter_.FillRoundedRect(cell,8*scale_,WithAlpha(theme.accent,vm.dark?.10f:.07f));
                MakeBrush(dc,mk.accent_fg,brAccentText_);
                const float inset=.75f*scale_;
                dc->DrawRoundedRectangle(D2D1::RoundedRect({cell.left+inset,cell.top+inset,cell.right-inset,cell.bottom-inset},8*scale_,8*scale_),
                    brAccentText_.get(),1.5f*scale_,dashStroke_.get());
            }
            for(size_t i=0;i<cells.size()&&i<c.preview_names.size();++i) {
                const auto cell=cells[i];
                auto name=c.preview_names[i];
                if(static_cast<int>(i)==c.preview_target_index) name+=L" · "+l10n::Get(l10n::StringId::PaneTargetBadge);
                capsule(name,(cell.left+cell.right)*.5f,(cell.top+cell.bottom)*.5f,cell.right-cell.left-24*scale_,true);
            }
            const l10n::StringId names[]={l10n::StringId::LayoutSingle,l10n::StringId::LayoutVertical,l10n::StringId::LayoutHorizontal,l10n::StringId::LayoutThree,l10n::StringId::LayoutFour};
            std::wstring note=l10n::Get(names[c.preview_preset]);
            const unsigned hidden=static_cast<unsigned>(c.pane_names.size()>cells.size()?c.pane_names.size()-cells.size():0);
            if(hidden){wchar_t caption[128]{};swprintf_s(caption,l10n::Get(l10n::StringId::LayoutPreviewHiddenFormat).c_str(),hidden);note+=L" · "+std::wstring(caption);}
            if(c.target_index>=0&&c.preview_target_index<0)note+=L" · "+l10n::Get(l10n::StringId::PaneTargetBadge)+L" · "+l10n::Get(l10n::StringId::Clear);
            if(c.compare&&cells.size()!=2)note+=L" · "+l10n::Get(l10n::StringId::LayoutPreviewCompareOff);
            capsule(note,(area.left+area.right)*.5f,area.bottom-29*scale_,(area.right-area.left)*.92f,false);
        }
        if(c.peek_index>=0&&static_cast<size_t>(c.peek_index)<c.pane_names.size()) {
            // .peek: 304 DIP flyout, radius 10, padding 10.
            const auto pop=PaneLayoutPeekBounds(vm,rect,scale_,compositor_->DwriteFactory(),small_fmt);const size_t i=static_cast<size_t>(c.peek_index);
            const bool dragging=!vm.drag_badge.empty();
            DrawFlyoutShadow(dc,pop,10*scale_,scale_,vm.dark);
            painter_.FillRoundedRect(pop,10*scale_,theme.surface_flyout);
            painter_.StrokeRoundedRect(pop,10*scale_,theme.stroke_card);
            const bool target=static_cast<int>(i)==c.target_index;
            const float badge_w=target?MeasureTextWidth(compositor_->DwriteFactory(),small_fmt,l10n::Get(l10n::StringId::PaneTargetBadge))+14*scale_:0.0f;
            glyph(command_icons::Icon::Folder,{pop.left+10*scale_,pop.top+8*scale_,pop.left+26*scale_,pop.top+32*scale_},theme.icon_folder,16.0f);
            text(c.pane_names[i],{pop.left+32*scale_,pop.top+8*scale_,pop.right-16*scale_-badge_w,pop.top+32*scale_},theme.text,compositor_->TextFormat());
            if(target) {
                // .badge: 18 DIP, fully rounded, accent-soft.
                const auto badge=D2D1::RectF(pop.right-10*scale_-badge_w,pop.top+11*scale_,pop.right-10*scale_,pop.top+29*scale_);
                painter_.FillRoundedRect(badge,9*scale_,mk.accent_soft);
                text(l10n::Get(l10n::StringId::PaneTargetBadge),{badge.left+7*scale_,badge.top,badge.right-6*scale_,badge.bottom},mk.accent_fg);
            }
            if(i<c.pane_paths.size()) {
                text(c.pane_paths[i],{pop.left+32*scale_,pop.top+30*scale_,pop.right-78*scale_,pop.top+50*scale_},theme.text_secondary);
                if(i<c.pane_counts.size()) {
                    wchar_t count[80]{};swprintf_s(count,l10n::Get(l10n::StringId::ItemsCountFormat).c_str(),static_cast<unsigned>(c.pane_counts[i]));
                    const float count_w=std::min(70*scale_,MeasureTextWidth(compositor_->DwriteFactory(),small_fmt,count));
                    text(count,{pop.right-10*scale_-count_w,pop.top+30*scale_,pop.right-10*scale_,pop.top+50*scale_},mk.text3);
                }
            }
            MakeBrush(dc,theme.stroke_divider,brStrokeDivider_);
            const float rule=std::round(pop.top+55*scale_)+.5f;
            dc->DrawLine({pop.left+10*scale_,rule},{pop.right-10*scale_,rule},brStrokeDivider_.get(),scale_);
            if(i<c.pane_previews.size())for(size_t j=0;j<std::min<size_t>(6,c.pane_previews[i].size());++j) {
                const float top=pop.top+(58+22*static_cast<float>(j))*scale_;
                const bool directory=i<c.pane_preview_dirs.size()&&j<c.pane_preview_dirs[i].size()&&c.pane_preview_dirs[i][j];
                glyph(directory?command_icons::Icon::Folder:command_icons::Icon::File,{pop.left+12*scale_,top,pop.left+28*scale_,top+22*scale_},
                    directory?theme.icon_folder:theme.text_secondary);
                const bool has_size=i<c.pane_preview_sizes.size()&&j<c.pane_preview_sizes[i].size()&&!c.pane_preview_sizes[i][j].empty();
                const float size_w=has_size?std::min(72*scale_,MeasureTextWidth(compositor_->DwriteFactory(),small_fmt,c.pane_preview_sizes[i][j])):0.0f;
                text(c.pane_previews[i][j],{pop.left+34*scale_,top,pop.right-16*scale_-size_w,top+22*scale_},theme.text);
                if(has_size)text(c.pane_preview_sizes[i][j],{pop.right-12*scale_-size_w,top,pop.right-12*scale_+1,top+22*scale_},mk.text3);
            }
            // .pk-drop: 1.5 DIP dashed, radius 7; accent while files are dragged over it.
            const auto drop_box=D2D1::RectF(pop.left+10*scale_,pop.bottom-54*scale_,pop.right-10*scale_,pop.bottom-24*scale_);
            if(dragging)painter_.FillRoundedRect(drop_box,7*scale_,mk.accent_soft);
            MakeBrush(dc,dragging?mk.accent_fg:theme.stroke_card,brAccentText_);
            const float inset=.75f*scale_;
            dc->DrawRoundedRectangle(D2D1::RoundedRect({drop_box.left+inset,drop_box.top+inset,drop_box.right-inset,drop_box.bottom-inset},7*scale_,7*scale_),
                brAccentText_.get(),1.5f*scale_,dashStroke_.get());
            wchar_t drop[256]{};swprintf_s(drop,l10n::Get(l10n::StringId::PeekDropFormat).c_str(),c.pane_names[i].c_str());
            text(drop,{drop_box.left+10*scale_,drop_box.top,drop_box.right-8*scale_,drop_box.bottom},dragging?mk.accent_fg:theme.text_secondary);
            text(l10n::Get(l10n::StringId::LayoutPeekHint),{pop.left+12*scale_,pop.bottom-24*scale_,pop.right-12*scale_,pop.bottom-4*scale_},mk.text3);
        }
        MakeBrush(dc,theme.accent_text,brAccentText_);
    }
    if (vm.status.history_open) {
        // drag_undo_mockup.html .hist: 440 DIP flyout, radius 10, the "现在"
        // line in accent, 28 DIP state circles, footer with the shortcuts.
        const MockupInk mk = MakeMockupInk(theme, vm.dark);
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, theme.text, brText_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        MakeBrush(dc, theme.text_disabled, brTextDisabled_);
        MakeBrush(dc, mk.accent_fg, brAccent_);
        MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
        IDWriteFactory2* const factory = compositor_->DwriteFactory();
        IDWriteTextFormat* const small_fmt = compositor_->SmallFormat();
        IDWriteTextFormat* const title_fmt = compositor_->FileNameFormat() ? compositor_->FileNameFormat() : small_fmt;
        const auto card = UndoHistoryBounds(vm, rect, scale_);
        DrawFlyoutShadow(dc, card, 10 * scale_, scale_, vm.dark);
        painter_.FillRoundedRect(card, 10 * scale_, theme.surface_flyout);
        painter_.StrokeRoundedRect(card, 10 * scale_, theme.stroke_card);
        const auto text = [&](const std::wstring& value, float x, float y, float width, ID2D1SolidColorBrush* brush,
                              IDWriteTextFormat* fmt = nullptr) {
            DrawTextEndEllipsis(compositor_, fmt ? fmt : small_fmt, brush, value, x, y, std::max(0.0f, width), 20 * scale_);
        };
        // .hh: the title on the left, the counts on the right, one line.
        const unsigned undo_count = static_cast<unsigned>(std::count_if(vm.status.history.begin(), vm.status.history.end(), [](const auto& row) { return row.supported && !row.undone; }));
        const unsigned redo_count = static_cast<unsigned>(std::count_if(vm.status.history.begin(), vm.status.history.end(), [](const auto& row) { return row.undone; }));
        wchar_t counts[128]{};
        swprintf_s(counts, l10n::Get(l10n::StringId::HistoryCounts).c_str(), undo_count, redo_count);
        const float inner = card.right - card.left - 28 * scale_;
        const float counts_w = std::min(inner * 0.6f, MeasureTextWidth(factory, small_fmt, counts));
        text(counts, card.right - 14 * scale_ - counts_w, card.top + 10 * scale_, counts_w + 1, brTextSecondary_.get());
        IDWriteTextFormat* const header_fmt = compositor_->HeaderFormat() ? compositor_->HeaderFormat() : small_fmt;
        text(l10n::Get(l10n::StringId::OpHistory), card.left + 14 * scale_, card.top + 10 * scale_, inner - counts_w - 12 * scale_, brText_.get(), header_fmt);
        dc->PushAxisAlignedClip(D2D1::RectF(card.left, card.top + 40 * scale_, card.right, card.bottom - 34 * scale_), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        bool now = false;
        const int hot = vm.hover_region == HitTestResult::UndoHistoryRow ? vm.hover_control_index : -1;
        for (size_t i = static_cast<size_t>(vm.status.history_offset); i < std::min<size_t>(vm.status.history_offset + 7, vm.status.history.size()); ++i) {
            const auto& row = vm.status.history[i];
            const float y = UndoHistoryRowTop(vm, card, i, scale_);
            if (i == 0 && row.undone && UndoHistoryUndoneSection(vm))
                text(l10n::Get(l10n::StringId::HistoryUndoneSection), card.left + 16 * scale_, y - 22 * scale_, inner, brTextDisabled_.get());
            if (!row.undone && !now) {
                // .hnow: an 8 DIP accent dot in a 3 DIP soft ring, the word,
                // then a 1.5 DIP accent rule at half strength.
                const float my = std::round(y - 12 * scale_);
                const auto dot = D2D1::Point2F(card.left + 18 * scale_, my);
                MakeBrush(dc, mk.accent_soft2, brFillSelected_);
                dc->FillEllipse(D2D1::Ellipse(dot, 7 * scale_, 7 * scale_), brFillSelected_.get());
                dc->FillEllipse(D2D1::Ellipse(dot, 4 * scale_, 4 * scale_), brAccent_.get());
                const std::wstring& now_label = l10n::Get(l10n::StringId::HistoryNow);
                const float now_w = MeasureTextWidth(factory, small_fmt, now_label);
                text(now_label, card.left + 30 * scale_, my - 10 * scale_, now_w + 1, brAccent_.get());
                brAccent_->SetOpacity(0.5f);
                dc->DrawLine(D2D1::Point2F(card.left + 38 * scale_ + now_w, my), D2D1::Point2F(card.right - 14 * scale_, my), brAccent_.get(), 1.5f * scale_);
                brAccent_->SetOpacity(1.0f);
                now = true;
            }
            const auto row_rect = D2D1::RectF(card.left + 8 * scale_, y, card.right - 8 * scale_, y + kUndoHistoryRowHeight * scale_);
            // .hrow.inrange: what "撤销到这里" would sweep (accent-soft), or
            // "重做到这里" (hover wash) for undone rows.
            if (hot >= 0 && static_cast<size_t>(hot) < vm.status.history.size() && row.supported && row.undone == vm.status.history[hot].undone &&
                (row.undone ? i >= static_cast<size_t>(hot) : i <= static_cast<size_t>(hot)))
                painter_.FillRoundedRect(row_rect, 6 * scale_, row.undone ? theme.fill_hover : mk.accent_soft);
            const bool row_hot = hot == static_cast<int>(i);
            if (row_hot) painter_.FillRoundedRect(row_rect, 6 * scale_, theme.fill_hover);
            const float label_left = card.left + 52 * scale_;
            // .hic: accent-soft circle; undone: hover wash, tertiary ink; locked: danger.
            const auto center = D2D1::Point2F(card.left + 28 * scale_, y + 22 * scale_);
            const auto icon_rect = D2D1::RectF(center.x - 14 * scale_, center.y - 14 * scale_, center.x + 14 * scale_, center.y + 14 * scale_);
            MakeBrush(dc, !row.supported ? mk.danger_soft : row.undone ? theme.fill_hover : mk.accent_soft, brFillSelected_);
            dc->FillEllipse(D2D1::Ellipse(center, 14 * scale_, 14 * scale_), brFillSelected_.get());
            MakeBrush(dc, !row.supported ? mk.danger : row.undone ? mk.text3 : mk.accent_fg, brAccentText_);
            command_icons::Draw(dc, brAccentText_.get(), paneHeaderStroke_.get(), !row.supported ? command_icons::Icon::Lock :
                row.undone ? command_icons::Icon::Undo : command_icons::FromGlyph(row.icon_glyph),
                command_icons::CenteredBounds(icon_rect, 13 * scale_), 1, command_icons::kStroke);
            // .hrow .tm: the time at the right, replaced by the actions on hover.
            const float time_w = !row_hot && !row.time.empty() ? MeasureTextWidth(factory, small_fmt, row.time) : 0.0f;
            if (time_w > 0.0f) text(row.time, card.right - 16 * scale_ - time_w, y + 12 * scale_, time_w + 1, brTextDisabled_.get());
            const float actions_width = row_hot ? (row.elsewhere ? 166.0f : 111.0f) * scale_ : 14 * scale_ + (time_w > 0.0f ? time_w + 12 * scale_ : 0.0f);
            const float label_width = std::max(0.0f, card.right - label_left - actions_width);
            auto* title_ink = row.undone ? brTextDisabled_.get() : !row.supported ? brTextSecondary_.get() : brText_.get();
            text(row.label, label_left, y + 3 * scale_, label_width, title_ink, title_fmt);
            if (row.undone) {
                const float strike_width = std::min(label_width, MeasureTextWidth(factory, title_fmt, row.label));
                const float sy = std::round(y + 13 * scale_) + 0.5f;
                dc->DrawLine(D2D1::Point2F(label_left, sy), D2D1::Point2F(label_left + strike_width, sy), brTextDisabled_.get(), scale_);
            }
            text(row.detail, label_left, y + 21 * scale_, label_width, brTextSecondary_.get());
            if (row_hot && vm.hover_sub_index >= 0 && vm.hover_sub_index <= 1) {
                const bool locate = vm.hover_sub_index == 1;
                painter_.FillRoundedRect(D2D1::RectF(card.right - (locate ? 160 : 105) * scale_, y + 9 * scale_,
                    card.right - (locate ? 110 : 10) * scale_, y + 35 * scale_), 4 * scale_, theme.fill_pressed);
            }
            if (row_hot) {
                auto* action_ink = row.supported ? brAccent_.get() : brTextDisabled_.get();
                const auto action_icon = [&](command_icons::Icon glyph, float x, bool mirror) {
                    D2D1_MATRIX_3X2_F original; dc->GetTransform(&original);
                    const auto box = D2D1::RectF(x, y + 15 * scale_, x + 14 * scale_, y + 29 * scale_);
                    if (mirror) dc->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F(x + 7 * scale_, y + 22 * scale_)) * original);
                    command_icons::Draw(dc, action_ink, paneHeaderStroke_.get(), glyph, box, 1, command_icons::kStroke);
                    dc->SetTransform(original);
                };
                if (row.elsewhere) {
                    action_icon(command_icons::Icon::FolderOpen, card.right - 158 * scale_, false);
                    text(l10n::Get(l10n::StringId::OpLocateShort), card.right - 140 * scale_, y + 12 * scale_, 32 * scale_, brAccent_.get());
                }
                action_icon(row.supported ? command_icons::Icon::Undo : command_icons::Icon::Lock, card.right - 103 * scale_, row.undone);
                text(l10n::Get(!row.supported ? l10n::StringId::HistoryLocked : row.undone ? l10n::StringId::HistoryRedoTo : l10n::StringId::HistoryUndoTo),
                     card.right - 85 * scale_, y + 12 * scale_, 75 * scale_, action_ink);
            }
        }
        dc->PopAxisAlignedClip();
        if (vm.status.history.empty()) text(l10n::Get(l10n::StringId::HistoryEmpty), card.left + 14 * scale_, card.top + 45 * scale_, inner, brTextSecondary_.get());
        // .hfoot: hairline, the hint on the left, the two shortcuts as keys on the right.
        const float rule = std::round(card.bottom - 32 * scale_) + 0.5f;
        dc->DrawLine(D2D1::Point2F(card.left + 14 * scale_, rule), D2D1::Point2F(card.right - 14 * scale_, rule), brStrokeDivider_.get(), scale_);
        const std::wstring keys[] = {L"Ctrl+Z", L"Ctrl+Y"};
        float key_x = card.right - 14 * scale_;
        const float key_top = card.bottom - 25 * scale_, key_bottom = card.bottom - 9 * scale_;
        for (int k = 1; k >= 0; --k) {
            const float key_w = MeasureTextWidth(factory, small_fmt, keys[k]) + 10 * scale_;
            const auto key = D2D1::RectF(key_x - key_w, key_top, key_x, key_bottom);
            painter_.FillRoundedRect(key, 4 * scale_, theme.fill_hover);
            painter_.StrokeRoundedRect(key, 4 * scale_, theme.stroke_card);
            text(keys[k], key.left + 5 * scale_, key.top - 2 * scale_, key_w - 9 * scale_, brTextSecondary_.get());
            key_x = key.left - 4 * scale_;
        }
        text(l10n::Get(l10n::StringId::HistoryFooter), card.left + 14 * scale_, card.bottom - 27 * scale_,
             key_x - card.left - 22 * scale_, brTextDisabled_.get());
        MakeBrush(dc, theme.accent, brAccent_);
        MakeBrush(dc, theme.fill_selected, brFillSelected_);
    }
    if (vm.teach.visible && vm.drag_badge.empty()) DrawTeachBubble(vm, rect, theme);
    if (!vm.status.history_open && !vm.change_popover.visible && vm.drag_badge.empty() && !vm.tooltip_text.empty() &&
        !group_wheel_.Visible() && !window_switcher_.IsOpen() && !window_overview_.IsOpen()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        // Multi-line tooltips ("\n"): one row per line, widest line sets the width.
        std::vector<std::wstring> tip_lines;
        for (size_t start = 0;;) {
            const size_t nl = vm.tooltip_text.find(L'\n', start);
            tip_lines.push_back(vm.tooltip_text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start));
            if (nl == std::wstring::npos || tip_lines.size() >= 6) break;
            start = nl + 1;
        }
        float tw = 0.0f;
        for (const auto& line : tip_lines)
            tw = std::max(tw, MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt, line));
        const float bw = std::min(tw + 20.0f * scale_, rect.right - 16.0f * scale_);
        const float line_h = 18.0f * scale_;
        const float bh = tip_lines.size() > 1
            ? 10.0f * scale_ + line_h * static_cast<float>(tip_lines.size()) : 28.0f * scale_;
        const float bx = std::clamp(vm.tooltip_x + 12.0f * scale_, 8.0f * scale_,
            std::max(8.0f * scale_, rect.right - bw - 8.0f * scale_));
        float tipY = vm.hover_region == HitTestResult::StatusBarCancelSearch
            ? rect.bottom - status_height_ - bh - 8.0f * scale_ : vm.tooltip_y + 18.0f * scale_;
        if (vm.hover_region == HitTestResult::RowFolderSize && tipY + bh > rect.bottom - 8.0f * scale_ &&
            vm.tooltip_y - bh - 18.0f * scale_ >= 8.0f * scale_)
            tipY = vm.tooltip_y - bh - 18.0f * scale_;
        const float by = std::clamp(tipY, 8.0f * scale_,
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
        if (tip_lines.size() > 1) {
            for (size_t li = 0; li < tip_lines.size(); ++li)
                DrawTextRect(dc, fmt, brText_.get(), tip_lines[li], bx + 10.0f * scale_,
                    by + 5.0f * scale_ + line_h * static_cast<float>(li), bw - 20.0f * scale_, line_h);
        } else {
            DrawTextRect(dc, fmt, brText_.get(), vm.tooltip_text,
                bx + 10.0f * scale_, by, bw - 20.0f * scale_, bh);
        }
        text_background_ = saved_tip_bg;
    }
    if (group_wheel_.Visible())
        group_wheel_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme, &painter_);
    if (window_overview_.IsOpen())
        window_overview_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme);
    if (window_switcher_.IsOpen())
        window_switcher_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme);

    frame_perf_.render_us = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - perf_start).count());

}

ID2D1Bitmap* MainRenderer::LogoBitmap() {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return nullptr;
    if (logo_bitmap_.get() && logo_dc_ == dc && std::abs(logo_scale_ - scale_) <= 0.001f) {
        return logo_bitmap_.get();
    }
    logo_bitmap_.reset();
    logo_dc_ = nullptr;
    // Decode at the on-screen size (24 DIP), scaling down from the nearest larger
    // frame: the small frames are pixel-fitted, and a 2x decode would blur them.
    const int px = std::max(16, static_cast<int>(24.0f * scale_ + 0.5f));
    HICON icon = nullptr;
    if (FAILED(LoadIconWithScaleDown(ResourceModule(), MAKEINTRESOURCEW(IDI_PULSE), px, px, &icon)) ||
        !icon) {
        return nullptr;
    }
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
    const float h = TitleControlsHeight();
    const float right = rect.right;

    const float brand_width = TitleBrandWidth(vm, right);
    // Product mark: the packaged app icon; the monogram is the fallback.
    float x = (brand_width <= 54.0f*scale_ ? 2.0f : 12.0f) * scale_;
    const float mark = 24.0f * scale_;
    const float markY = (h - mark) * 0.5f;
    const D2D1_RECT_F toggleRc = SidebarToggleRect(right);
    const bool hasToggle = toggleRc.right > toggleRc.left;
    if (hasToggle) {
        DrawButton(toggleRc, theme,
            IsHovered(vm, HitTestResult::SidebarToggle) ? theme.fill_hover : kTransparent,
            L"", L"", theme.text_secondary, true, true);
        const float cx = (toggleRc.left + toggleRc.right) * 0.5f;
        const float cy = (toggleRc.top + toggleRc.bottom) * 0.5f;
        const float stroke = 1.25f * scale_;
        const auto frame = D2D1::RectF(cx - 7.5f * scale_, cy - 6.5f * scale_,
                                     cx + 7.5f * scale_, cy + 6.5f * scale_);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        dc->DrawRoundedRectangle(D2D1::RoundedRect(frame, 2.75f * scale_, 2.75f * scale_),
                                 brTextSecondary_.get(), stroke);
        const bool expanded = !(user_sidebar_collapsed_ || SidebarAutoCollapsed(right)) || SidebarPeekVisible(right);
        const float divider = frame.left + (expanded ? 5.0f : 3.75f) * scale_;
        const float inset = expanded ? 0.0f : 3.25f * scale_;
        dc->DrawLine({divider, frame.top + inset}, {divider, frame.bottom - inset},
                     brTextSecondary_.get(), stroke);
        if (!expanded) {
            for (float edge : {frame.top + inset, frame.bottom - inset})
                dc->FillEllipse(D2D1::Ellipse({divider, edge}, stroke * 0.5f, stroke * 0.5f),
                                brTextSecondary_.get());
        }
    }
    if (hasToggle && (user_sidebar_collapsed_ || SidebarAutoCollapsed(right)) && (vertical_tabs_ || embedded_)) {
        // Rail: the toggle stands in for the brand mark.
    } else if (ID2D1Bitmap* logo = LogoBitmap()) {
        // 1:1 on whole pixels so the pixel-fitted frame stays sharp.
        const D2D1_SIZE_U ls = logo->GetPixelSize();
        const float lx = std::round(x + (mark - static_cast<float>(ls.width)) * 0.5f);
        const float ly = std::round(markY + (mark - static_cast<float>(ls.height)) * 0.5f);
        dc->DrawBitmap(logo, D2D1::RectF(lx, ly, lx + static_cast<float>(ls.width), ly + static_cast<float>(ls.height)),
                       1.0f, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, nullptr);
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
    PULSE_SCENE_MARK("scene_title_logo");
    x += mark + 8.0f * scale_;
    const float brandRight = (std::min)(brand_width - 12.0f * scale_, toggleRc.left - 4.0f * scale_);
    const float nameWidth = MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
        compositor_->HeaderFormat(), L"Pulse");
    PULSE_SCENE_MARK("scene_title_measure");
    const WindowIdentityGeometry identity = WindowIdentityLayout(vm, right);
    if (identity.hit.right > identity.hit.left && !identity.compact) {
        // Multi-window M7c: this window's dot and name instead of the product name.
        if (IsHovered(vm, HitTestResult::WindowIdentity)) {
            MakeBrush(dc, theme.fill_hover, brFillHover_);
            const float radius = theme.radius_control * scale_;
            dc->FillRoundedRectangle(D2D1::RoundedRect(identity.hit, radius, radius), brFillHover_.get());
        }
        MakeBrush(dc, HexColor(vm.window_identity_rgb), brAccent_);
        dc->FillEllipse(D2D1::Ellipse(identity.dot, identity.dot_radius, identity.dot_radius), brAccent_.get());
        const float text_x = identity.hit.left + 22.0f * scale_;
        MakeBrush(dc, theme.text, brText_);
        DrawTextEndEllipsis(compositor_, compositor_->HeaderFormat(), brText_.get(),
                            vm.window_identity_name, text_x, 0.0f, identity.hit.right - 6.0f * scale_ - text_x, h);
    } else if (title_brand_name_ && x + nameWidth <= brandRight) {
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), L"Pulse",
            x, 0.0f, nameWidth + 1.0f * scale_, h);
    }

    PULSE_SCENE_MARK("scene_title_brand");
    if (identity.compact && identity.hit.right > identity.hit.left) {
        // M7c on the rail: the window's dot on the corner of the mark (or of
        // the toggle), ringed in the title bar's color so it stands apart.
        MakeBrush(dc, theme.surface_title, brAccent_);
        dc->FillEllipse(D2D1::Ellipse(identity.dot, identity.dot_radius + 1.5f * scale_,
                                      identity.dot_radius + 1.5f * scale_), brAccent_.get());
        MakeBrush(dc, HexColor(vm.window_identity_rgb), brAccent_);
        dc->FillEllipse(D2D1::Ellipse(identity.dot, identity.dot_radius, identity.dot_radius), brAccent_.get());
    }
    const float ctrlW = 46.0f * scale_;
    const TitleChrome chrome = MakeTitleChrome(right, scale_, h);

    const TabStripMetrics strip = ComputeTabStrip(vm, rect.right);
    const float tabH = strip.h;
    const float tabY = strip.y;
    // part: 0 = whole tab, 1 = background only (the gliding active plate),
    // 2 = content only (over a plate drawn elsewhere). w_override >= 0
    // replaces the slot width (the plate between two slot widths).
    auto drawTab = [&](size_t i, float left, bool raised, int part, float w_override) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = w_override >= 0.0f ? w_override : strip.Width(vm, i);
        const bool hovered = IsHovered(vm, HitTestResult::Tab, static_cast<int>(i)) ||
                             IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i));
        if (!strip.item_rects.empty() && strip.Width(vm, i) <= 0.0f) return;
        const bool connect = (active || raised) && (strip.item_rects.empty() || strip.ItemRect(vm, i).bottom + 12.0f * scale_ >= title_bar_height_);
        ChromeTabShape shape;
        shape.top_radius = (strip.item_rects.empty() ? theme.radius_control : 8.0f) * scale_;
        // Concentric with the neighboring inactive tab: outer radius = inner + gap.
        shape.bottom_radius = shape.top_radius + control_gap_;
        shape.connect_bottom = connect;
        const float tabY = strip.ItemRect(vm, i).top;
        const float tabTop = tabY;
        // Use the same sheet edge for both opacity modes; overlap shifts the arc center.
        const float rowBottom = strip.item_rects.empty() ? h : connect ? title_bar_height_ : tabY + tabH;
        const float tabBottom = connect ? std::round(rowBottom)
                                        : (std::round(rowBottom) - (strip.item_rects.empty() ? control_gap_ : 0.0f));
        const D2D1_RECT_F tabRc = D2D1::RectF(left, tabTop, left + tabW, tabBottom);
        if (part == 2) {
            // Content over a plate drawn elsewhere.
        } else if (raised) {
            D2D1_RECT_F shadow = tabRc;
            shadow.left += 1.0f * scale_;
            shadow.top += 2.0f * scale_;
            shadow.right -= 1.0f * scale_;
            shadow.bottom += 1.0f * scale_;
            MakeBrush(dc, D2D1::ColorF(0.0f, 0.0f, 0.0f, vm.dark ? 0.09f : 0.07f), brFillPressed_);
            FillChromeTab(dc, brFillPressed_.get(), shadow, shape);
            MakeBrush(dc, WithAlpha(theme.surface_sheet, sheet_alpha_), brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else if (active) {
            if (!strip.item_rects.empty() && !connect) {
                MakeBrush(dc,WithAlpha(HexColor(0x000000),0.08f),brFillPressed_);
                const auto shadow=D2D1::RectF(tabRc.left,tabRc.top+2*scale_,tabRc.right,tabRc.bottom+2*scale_);
                FillChromeTab(dc,brFillPressed_.get(),shadow,shape);
            }
            // pane_layout_mockup .tab.on: the sheet itself in both themes, so
            // the tab runs into the content below with no seam (a white plate
            // stood out against the light sheet). A tab floating in an upper
            // row is not joined to the sheet and reads as a card instead.
            MakeBrush(dc, connect ? WithAlpha(theme.surface_sheet, sheet_alpha_) : theme.surface_card, brFillSelected_);
            FillChromeTab(dc, brFillSelected_.get(), tabRc, shape);
        } else {
            // Grouped tabs get a tinted body; ungrouped keep the stock look.
            const bool has_color = vm.tabs[i].color_rgb != 0;
            if (has_color) {
                D2D1_COLOR_F tint = HexColor(vm.tabs[i].color_rgb);
                tint.a *= hovered ? 0.16f : 0.10f;
                MakeBrush(dc, tint, brFillHover_);
            } else {
                MakeBrush(dc, hovered ? theme.fill_hover : kTransparent, brFillHover_);
            }
            FillChromeTab(dc, brFillHover_.get(), tabRc, shape);
        }
        if (active && !strip.item_rects.empty() && !connect && part!=2) {
            MakeBrush(dc,theme.stroke_card,brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(tabRc,8*scale_,8*scale_),brStrokeCard_.get(),scale_);
        }
        if ((active || raised) && connect && part != 2 && !IsHighContrast()) {
            // The sheet-coloured tab is only a shade off the title bar (dark:
            // #202020 on #1A1A1A), so a hairline traces it from shoulder to
            // shoulder: lit on dark, a soft slate line on light.
            if (vm.dark) {
                // A faint top light that is gone by the bottom, so the tab
                // lifts off the title bar and still meets the sheet with no seam.
                const D2D1_GRADIENT_STOP stops[] = {{0.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.05f)},
                                                    {1.0f, D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f)}};
                ComPtr<ID2D1GradientStopCollection> collection;
                ComPtr<ID2D1LinearGradientBrush> light;
                if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 2, &collection)) &&
                    SUCCEEDED(dc->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties({0.0f, tabRc.top}, {0.0f, tabRc.bottom - 4.0f * scale_}),
                        collection.get(), &light)))
                    FillChromeTab(dc, light.get(), tabRc, shape);
            }
            MakeBrush(dc, vm.dark ? WithAlpha(HexColor(0xFFFFFF), 0.11f) : WithAlpha(HexColor(0x303B50), 0.13f),
                      brStrokeCard_);
            StrokeChromeTabEdge(dc, brStrokeCard_.get(), tabRc, shape, (std::max)(1.0f, std::round(scale_)));
        }
        if (vm.tabs[i].flash > 0.0f && part != 2) {
            MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.34f : 0.26f) * vm.tabs[i].flash), brFillHover_);
            FillChromeTab(dc, brFillHover_.get(), tabRc, shape);
        }
        // Grouped tabs draw the same top strip as the ungrouped active tab,
        // just in their group color instead of the accent.
        const float r = shape.top_radius;
        const bool has_color = vm.tabs[i].color_rgb != 0;
        if (has_color && part != 2) {
            D2D1_COLOR_F line = HexColor(vm.tabs[i].color_rgb);
            if (!active) line.a *= 0.55f;
            MakeBrush(dc, line, brAccent_);
            FillChromeTabAccent(dc, brAccent_.get(), tabRc, shape, 2.0f * scale_);
        }
        if (active && part != 2) {
            // .tab.on::after: 18x3 DIP, 2 DIP above the bottom, under the
            // icon (centered on icon-only tabs).
            const bool icon_tab = (i < strip.icon_only.size() && strip.icon_only[i]) || (pinned && !vm.show_pinned_tab_names);
            const float bar_w = std::max(0.0f, std::min(18.0f * scale_, tabW - 12.0f * scale_));
            const float bar_x = icon_tab ? tabRc.left + (tabW - bar_w) * 0.5f
                : tabRc.left + (6.0f + (vm.tabs[i].marker_rgb != 0 ? 12.0f : 0.0f)) * scale_;
            // On dark the accent is lifted a little so the bar reads against
            // the near-black sheet as clearly as it does on light.
            const D2D1_COLOR_F bar = vm.tabs[i].color_rgb ? HexColor(vm.tabs[i].color_rgb)
                : vm.dark && !IsHighContrast() ? MixColor(theme.accent, HexColor(0xFFFFFF), 0.22f) : theme.accent;
            MakeBrush(dc, bar, brAccent_);
            if (bar_w > 0.0f)
                FillRoundedRect(dc, brAccent_.get(), std::round(bar_x), std::round(tabRc.bottom - 5.0f * scale_), bar_w, 3.0f * scale_, 1.5f * scale_);
        }
        if (part == 1) return;
        if ((i < strip.icon_only.size() && strip.icon_only[i]) || (pinned && !vm.show_pinned_tab_names)) {
            // Chrome pinned tab: centered icon, no title, no close button
            // (the tooltip names it).
            DrawIconText(left, tabY, tabW, tabH,
                vm.tabs[i].title.empty() ? kIconFolder
                    : vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                        ? kIconSettings : kIconFolder, L"[]",
                active ? theme.icon_folder : theme.text_secondary, 0.85f);
            return;
        }
        const float markerReserve = vm.tabs[i].marker_rgb != 0 ? 12.0f * scale_ : 0.0f;
        if (markerReserve > 0.0f) {
            MakeBrush(dc, HexColor(vm.tabs[i].marker_rgb), brAccent_);
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(left + 10.0f * scale_,
                tabY + tabH * 0.5f), 3.0f * scale_, 3.0f * scale_), brAccent_.get());
        }
        DrawIconText(left + 6.0f * scale_ + markerReserve, tabY, 16.0f * scale_, tabH,
            vm.tabs[i].title == pulse::l10n::Get(pulse::l10n::StringId::Settings)
                ? kIconSettings : kIconFolder, L"[]",
            active ? theme.icon_folder : theme.text_secondary, 0.85f);
        // .tab / .tab.on: inactive titles step back to the secondary text in
        // both themes and come forward on hover; the active one is full text.
        MakeBrush(dc, active || hovered ? theme.text : theme.text_secondary, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_ + markerReserve;
        DrawTabTitleFaded(compositor_, compositor_->TabFormat(), brText_.get(),
                          vm.tabs[i].title, titleLeft, tabY,
                          std::max(0.0f, tabW - 24.0f * scale_ - markerReserve - closeReserve), tabH, scale_);
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
    const int dragI = (strip.item_rects.empty() || vm.tab_layout == 0) ? vm.tab_drag_index : -1;
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
    if (!vertical_tabs_) {
    // Overflowing strip: tabs are clipped between the scroll arrows and fade
    // out toward an edge that has more tabs beyond it. An opacity mask keeps
    // the fade correct over wallpapers and translucent chrome.
    bool tabLayer = false;
    if (strip.overflow && strip.view_right > strip.view_left) {
        const float view = strip.view_right - strip.view_left;
        const float fade = std::min(24.0f * scale_, view * 0.25f) / view;
        const D2D1_GRADIENT_STOP stops[] = {
            {0.0f, D2D1::ColorF(0, 0, 0, strip.scroll > 0.5f ? 0.0f : 1.0f)},
            {fade, D2D1::ColorF(0, 0, 0, 1.0f)},
            {1.0f - fade, D2D1::ColorF(0, 0, 0, 1.0f)},
            {1.0f, D2D1::ColorF(0, 0, 0, strip.scroll < strip.max_scroll - 0.5f ? 0.0f : 1.0f)}};
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> mask;
        if (SUCCEEDED(dc->CreateGradientStopCollection(stops, 4, &collection)) && collection.get() &&
            SUCCEEDED(dc->CreateLinearGradientBrush(
                D2D1::LinearGradientBrushProperties(D2D1::Point2F(strip.view_left, 0.0f),
                                                    D2D1::Point2F(strip.view_right, 0.0f)),
                collection.get(), &mask)) && mask.get()) {
            dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(strip.view_left, 0.0f, strip.view_right, h),
                                                 nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), 1.0f, mask.get(),
                                                 D2D1_LAYER_OPTIONS1_NONE), nullptr);
            tabLayer = true;
        }
    }
    for (const auto& chip : strip.chips) {
        if (chip.width <= 0 || chip.group < 0 || chip.group >= static_cast<int>(vm.tab_groups.size())) continue;
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
        const D2D1_RECT_F rc = D2D1::RectF(chipLeft, chip.top + 4.0f * scale_,
                                           chipLeft + chip.width,
                                           chip.top + 4.0f * scale_ + ch);
        const bool chip_hovered = IsHovered(vm, HitTestResult::TabGroup, chip.group);
        const bool named = !gv.name.empty() && chip.width >= 32.0f*scale_;
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
    // ---- Tab motion bookkeeping: which tabs appeared or went away. ----
    const uint64_t tnow = motion_now_;
    const bool tabDragging = dragI >= 0;
    const bool stripMotion = TabStripMotionSupported(!strip.item_rects.empty(), vm.tab_layout);
    bool tabMoving = false;
    {
        std::vector<uintptr_t> ids;
        ids.reserve(vm.tabs.size());
        for (const auto& t : vm.tabs)
            if (!t.hidden && t.id) ids.push_back(t.id);
        std::sort(ids.begin(), ids.end());
        if (ids != tab_ids_prev_) {
            const bool primed = !tab_ids_prev_.empty() && !tabDragging;
            tab_anim_on_ = motion::SystemAnimationsEnabled();
            if (primed && tab_anim_on_) {
                for (uintptr_t id : ids)
                    if (!std::binary_search(tab_ids_prev_.begin(), tab_ids_prev_.end(), id))
                        tab_slots_[id].born = tnow;
                for (uintptr_t id : tab_ids_prev_) {
                    if (std::binary_search(ids.begin(), ids.end(), id)) continue;
                    const auto it = tab_slots_.find(id);
                    if (it == tab_slots_.end() || it->second.seen + 1 < motion_frame_) continue;
                    if (tab_ghosts_.size() < 12)
                        tab_ghosts_.push_back({it->second.title, it->second.last, tnow});
                }
            }
            for (auto it = tab_slots_.begin(); it != tab_slots_.end();) {
                if (std::binary_search(ids.begin(), ids.end(), it->first)) ++it;
                else it = tab_slots_.erase(it);
            }
            ++tab_gen_;
            tab_ids_prev_ = std::move(ids);
        }
    }
    constexpr motion::SpringSpec kTabPlateSpring{900.0f, 56.0f};
    const uint32_t popMs = motion::SpringSettleMs(motion::spring::Pop);
    // Rest slot -> drawn left: siblings spring to their new slots when the
    // set of tabs changes; resizes and scrolling follow at once.
    std::vector<float> lefts(vm.tabs.size(), 0.0f);
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].hidden || inDragRun(static_cast<int>(i))) continue;
        const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
        float left = strip.x0
            + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra;
        if (!strip.item_rects.empty()) left = strip.ItemRect(vm, i).left;
        const float w = strip.Width(vm, i);
        if (vm.tabs[i].id) {
            TabSlotMotion& m = tab_slots_[vm.tabs[i].id];
            if (!tabDragging && stripMotion) {
                m.rect.SetAnimationsEnabled(tab_anim_on_);
                const D2D1_RECT_F r = m.rect.Update(1, static_cast<int64_t>(tab_gen_),
                    D2D1::RectF(left, 0.0f, left + w, 1.0f), motion_frame_, tnow, motion::spring::Smooth);
                if (m.rect.Active(tnow)) tabMoving = true;
                left = r.left;
            }
            m.seen = motion_frame_;
            m.last = D2D1::RectF(left, tabY, left + w, tabY + tabH);
            if (m.title != vm.tabs[i].title) m.title = vm.tabs[i].title;
        }
        lefts[i] = left;
        if (vm.tabs[i].active) activeI = static_cast<int>(i);
    }
    // A new tab pops in: scale .9 -> 1, rising 6 px, fading in (spring Pop).
    auto drawSlot = [&](size_t i, int part) {
        const float left = lefts[i];
        const auto it = vm.tabs[i].id ? tab_slots_.find(vm.tabs[i].id) : tab_slots_.end();
        const uint64_t born = it != tab_slots_.end() ? it->second.born : 0;
        if (stripMotion && born && tnow - born < popMs) {
            tabMoving = true;
            const float p = motion::SpringProgress(motion::spring::Pop, static_cast<float>(tnow - born));
            const float alpha = (std::clamp)(p, 0.0f, 1.0f);
            const float sc = 0.9f + 0.1f * p;
            const float w = strip.Width(vm, i);
            const D2D1_POINT_2F c = D2D1::Point2F(left + w * 0.5f, tabY + tabH * 0.5f);
            D2D1_MATRIX_3X2_F old{};
            dc->GetTransform(&old);
            dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, c) *
                             D2D1::Matrix3x2F::Translation(0.0f, 6.0f * scale_ * (1.0f - p)) *
                             *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
            dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(left - 16.0f * scale_, 0.0f,
                                                             left + w + 16.0f * scale_, h + 4.0f * scale_),
                                                 nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                 D2D1::IdentityMatrix(), alpha, nullptr,
                                                 D2D1_LAYER_OPTIONS1_NONE), nullptr);
            drawTab(i, left, false, part, -1.0f);
            dc->PopLayer();
            dc->SetTransform(old);
            return;
        }
        if (born) it->second.born = 0;
        drawTab(i, left, false, part, -1.0f);
    };
    // The active plate glides between slots; while it moves it is drawn
    // under the other tabs, at rest on top as before.
    bool plateGliding = false;
    D2D1_RECT_F plateRc{};
    if (stripMotion && activeI >= 0 && !tabDragging && vm.tabs[static_cast<size_t>(activeI)].id) {
        const size_t i = static_cast<size_t>(activeI);
        const float w = strip.Width(vm, i);
        tab_plate_.SetAnimationsEnabled(tab_anim_on_);
        plateRc = tab_plate_.Update(2, static_cast<int64_t>(vm.tabs[i].id),
            D2D1::RectF(lefts[i], 0.0f, lefts[i] + w, 1.0f), motion_frame_, tnow, kTabPlateSpring);
        plateGliding = tab_plate_.Active(tnow) &&
                       (std::fabs(plateRc.left - lefts[i]) > 0.5f || std::fabs(plateRc.right - lefts[i] - w) > 0.5f);
        if (plateGliding) {
            tabMoving = true;
            drawTab(i, plateRc.left, false, 1, plateRc.right - plateRc.left);
        }
    }
    if (!stripMotion || tabDragging || activeI < 0) tab_plate_.Hide();
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        if (vm.tabs[i].hidden) continue;
        if (inDragRun(static_cast<int>(i))) continue;
        if (static_cast<int>(i) == activeI) continue;
        drawSlot(i, 0);
    }
    if (activeI >= 0 && !inDragRun(activeI) && !vm.tabs[static_cast<size_t>(activeI)].hidden)
        drawSlot(static_cast<size_t>(activeI), plateGliding ? 2 : 0);
    // Closed tabs fade and shrink to .88 in place (120 ms, ease-in).
    if (!stripMotion) tab_ghosts_.clear();
    for (auto it = tab_ghosts_.begin(); it != tab_ghosts_.end();) {
        const float e = static_cast<float>(tnow - it->start) / 120.0f;
        if (e >= 1.0f) { it = tab_ghosts_.erase(it); continue; }
        tabMoving = true;
        const float k = e * e;
        const D2D1_RECT_F& g = it->rc;
        const float sc = 1.0f - 0.12f * k;
        D2D1_MATRIX_3X2_F old{};
        dc->GetTransform(&old);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F((g.left + g.right) * 0.5f,
                                                                       (g.top + g.bottom) * 0.5f)) *
                         *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
        dc->PushLayer(D2D1::LayerParameters1(D2D1::RectF(g.left, g.top, g.right, g.bottom), nullptr,
                                             D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(),
                                             1.0f - k, nullptr, D2D1_LAYER_OPTIONS1_NONE), nullptr);
        const float gw = g.right - g.left;
        DrawIconText(g.left + 6.0f * scale_, tabY, 16.0f * scale_, tabH, kIconFolder, L"[]",
                     theme.text_secondary, 0.85f);
        MakeBrush(dc, theme.text, brText_);
        DrawTabTitle(compositor_, compositor_->TabFormat(), brText_.get(),
                     it->title, g.left + 24.0f * scale_, tabY,
                     (std::max)(0.0f, gw - 30.0f * scale_), tabH, scale_);
        dc->PopLayer();
        dc->SetTransform(old);
        ++it;
    }
    // The dragged run floats as one block (browser group drag); collapsed
    // members stay hidden and do not take float width.
    if (dragI >= 0 && dragI < static_cast<int>(vm.tabs.size())) {
        // Lifted: the run grows to 1.04 with spring Pop as the drag starts.
        if (!tab_lift_start_) {
            tab_lift_start_ = tnow;
            tab_anim_on_ = motion::SystemAnimationsEnabled();
        }
        float sc = 1.0f;
        if (tab_anim_on_) {
            const float age = static_cast<float>(tnow - tab_lift_start_);
            sc = 1.0f + 0.04f * motion::SpringProgress(motion::spring::Pop, age);
            if (age < static_cast<float>(popMs)) tabMoving = true;
        }
        int runN = 0;
        for (int k = 0; k < dragN && dragI + k < static_cast<int>(vm.tabs.size()); ++k)
            if (!vm.tabs[static_cast<size_t>(dragI + k)].hidden) ++runN;
        D2D1_MATRIX_3X2_F old{};
        dc->GetTransform(&old);
        if (sc != 1.0f) {
            const float runW = static_cast<float>((std::max)(0, runN - 1)) * strip.pitch +
                               strip.Width(vm, static_cast<size_t>(dragI));
            dc->SetTransform(D2D1::Matrix3x2F::Scale(sc, sc, D2D1::Point2F(vm.tab_drag_x + runW * 0.5f,
                                                                           tabY + tabH * 0.5f)) *
                             *D2D1::Matrix3x2F::ReinterpretBaseType(&old));
        }
        float floatX = vm.tab_drag_x;
        // Motion v2 "tabdetach": pulled off the strip, the tab follows the
        // cursor and a window body grows out beneath it, so the tear-off
        // reads as the tab becoming a window rather than a sudden jump.
        const float pull = tab_anim_on_ ? (std::clamp)(vm.tab_detach_pull, 0.0f, 1.0f) : 0.0f;
        if (pull > 0.02f && runN == 1) {
            const float e = pull * pull * (3.0f - 2.0f * pull);
            const float tw = strip.Width(vm, static_cast<size_t>(dragI));
            const float dy = vm.tab_detach_dy * 0.85f;
            const D2D1_RECT_F body = D2D1::RectF(
                vm.tab_drag_x - 4.0f * scale_ * e, tabY + tabH * 0.5f + dy,
                vm.tab_drag_x + tw + 150.0f * scale_ * e, tabY + tabH + dy + 84.0f * scale_ * e);
            const float r = 8.0f * scale_;
            D2D1_COLOR_F fill = vm.dark ? HexColor(0x2B2B2B) : HexColor(0xFFFFFF);
            fill.a = 0.92f * e;
            MakeBrush(dc, D2D1::ColorF(0, 0, 0, (vm.dark ? 0.30f : 0.14f) * e), brFillHover_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left, body.top + 3.0f * scale_,
                                                                   body.right, body.bottom + 4.0f * scale_), r, r),
                                     brFillHover_.get());
            MakeBrush(dc, fill, brFillSelected_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(body, r, r), brFillSelected_.get());
            MakeBrush(dc, WithAlpha(theme.stroke_card, theme.stroke_card.a * e), brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(body, r, r), brStrokeCard_.get(), 1.0f);
            // Placeholder rows hint at the folder the window will show.
            MakeBrush(dc, WithAlpha(theme.text_secondary, 0.16f * e), brFillHover_);
            for (int row = 0; row < 3; ++row) {
                const float rowY = tabY + tabH + dy + (14.0f + 18.0f * static_cast<float>(row)) * scale_ * e;
                if (rowY + 6.0f * scale_ > body.bottom - 6.0f * scale_) break;
                dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(body.left + 12.0f * scale_, rowY,
                    body.left + 12.0f * scale_ + (body.right - body.left - 24.0f * scale_) * (0.8f - 0.2f * row),
                    rowY + 6.0f * scale_), 3.0f * scale_, 3.0f * scale_), brFillHover_.get());
            }
            D2D1_MATRIX_3X2_F cur{};
            dc->GetTransform(&cur);
            dc->SetTransform(cur * D2D1::Matrix3x2F::Translation(0.0f, dy));
        }
        for (int k = 0; k < dragN && dragI + k < static_cast<int>(vm.tabs.size()); ++k) {
            if (vm.tabs[static_cast<size_t>(dragI + k)].hidden) continue;
            drawTab(static_cast<size_t>(dragI + k), floatX, true, 0, -1.0f);
            floatX += strip.Width(vm, static_cast<size_t>(dragI + k)) + 2.0f*scale_;
        }
        dc->SetTransform(old);
    } else {
        tab_lift_start_ = 0;
    }
    if (tabMoving) tab_motion_until_ = tnow + 60;

    // A tab dragged from another window lands here (multi-window M6b): a bar
    // in the gap before the visible tab at the index, or after the last tab.
    if (vm.tab_drop_index >= 0) {
        float barX = strip.x0;
        for (size_t i=0;i<vm.tabs.size();++i)
            if (!vm.tabs[i].hidden && strip.Width(vm,i)>0) barX=strip.ItemRect(vm,i).right+scale_;
        for (size_t i=static_cast<size_t>(vm.tab_drop_index);i<vm.tabs.size();++i) {
            if (vm.tabs[i].hidden || strip.Width(vm,i)<=0) continue;
            barX=strip.ItemRect(vm,i).left-scale_;
            break;
        }
        const float half = 1.5f * scale_;
        MakeBrush(dc, theme.accent, brAccent_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(barX - half, tabY + tabH * 0.18f,
                                                               barX + half, tabY + tabH * 0.82f),
                                                   half, half),
                                 brAccent_.get());
    }

    // A file drag resting on a tab (multi-window M6e): the tab lights up and
    // an underline fills while the spring timer runs; when full, it opens.
    D2D1_RECT_F fileDropTab{};
    if (vm.tab_file_drop >= 0 && TabItemRect(vm, rect.right, vm.tab_file_drop, &fileDropTab)) {
        const float inset = 3.0f * scale_;
        const float radius = 6.0f * scale_;
        MakeBrush(dc, WithAlpha(theme.accent, theme.accent.a * 0.16f), brAccent_);
        dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(fileDropTab.left + inset, fileDropTab.top + inset,
                                                               fileDropTab.right - inset, fileDropTab.bottom - inset),
                                                   radius, radius),
                                 brAccent_.get());
        const float progress = (std::clamp)(vm.tab_file_drop_progress, 0.0f, 1.0f);
        const float lineH = 2.5f * scale_;
        const float lineL = fileDropTab.left + 3.0f * inset;
        const float lineR = lineL + (fileDropTab.right - fileDropTab.left - 6.0f * inset) * progress;
        if (lineR > lineL) {
            MakeBrush(dc, theme.accent, brAccent_);
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(lineL, fileDropTab.bottom - inset - lineH,
                                                                   lineR, fileDropTab.bottom - inset),
                                                       lineH * 0.5f, lineH * 0.5f),
                                     brAccent_.get());
        }
    }

    // The tab and sheet form one surface; a straight rule here breaks the shoulders.

    if (tabLayer) dc->PopLayer();
    if (!strip.hidden_indices.empty()) {
        const auto button = strip.overflow_button;
        if (IsHovered(vm, HitTestResult::TabScroll, 0))
            painter_.FillRoundedRect(button, 6.0f*scale_, theme.fill_hover);
        const std::wstring label = L"+" + std::to_wstring(strip.hidden_indices.size());
        MakeBrush(dc,theme.text_secondary,brTextSecondary_);
        const float label_width=MeasureTextWidth(compositor_->DwriteFactory(),compositor_->SmallFormat(),label);
        if (label_width <= button.right-button.left-4.0f*scale_) {
            DrawTabTitleFaded(compositor_,compositor_->SmallFormat(),brTextSecondary_.get(),label,
                button.left+2.0f*scale_,button.top,button.right-button.left-4.0f*scale_,button.bottom-button.top,scale_);
        } else {
            const float cx=(button.left+button.right)*.5f, cy=(button.top+button.bottom)*.5f;
            const float arm=std::min(4.0f*scale_,(button.right-button.left)*.3f);
            dc->DrawLine({cx-arm,cy-arm*.5f},{cx,cy+arm*.5f},brTextSecondary_.get(),1.25f*scale_);
            dc->DrawLine({cx,cy+arm*.5f},{cx+arm,cy-arm*.5f},brTextSecondary_.get(),1.25f*scale_);
        }
    }
    if (strip.overflow) {
        for (const int direction : {-1, 1}) {
            const float ax = direction < 0 ? strip.view_left - strip.arrow_w : strip.view_right;
            const bool enabled = direction < 0 ? strip.scroll > 0.5f : strip.scroll < strip.max_scroll - 0.5f;
            if (!enabled) continue; // Keep the slot, but hide arrows at either endpoint.
            const bool hot = IsHovered(vm, HitTestResult::TabScroll, direction);
            DrawButton(D2D1::RectF(ax, tabY, ax + strip.arrow_w, tabY + tabH), theme,
                       hot ? theme.fill_hover : kTransparent,
                       direction < 0 ? kIconChevronLeft : kIconChevronRight, direction < 0 ? L"<" : L">",
                       theme.text_secondary,
                       true, true, 0.7f);
        }
    }
    if (!strip.item_rects.empty() && strip.total_rows > strip.rows) {
        const auto pager = MakeTabPagerLayout(right, kTabRowHeight * scale_, strip.rows, scale_);
        for (const int direction : {-1, 1}) {
            const bool enabled = direction < 0 ? tab_first_row_ > 0 : tab_first_row_ < strip.total_rows - strip.rows;
            const auto button = direction < 0 ? pager.up : pager.down;
            if (enabled && IsHovered(vm, HitTestResult::TabScroll, direction))
                painter_.FillRoundedRect(button, 6.0f * scale_, theme.fill_hover);
            const float cx = (button.left + button.right) * 0.5f;
            const float cy = (button.top + button.bottom) * 0.5f;
            ComPtr<ID2D1Factory> factory;
            dc->GetFactory(&factory);
            ComPtr<ID2D1PathGeometry> triangle;
            ComPtr<ID2D1GeometrySink> sink;
            if (factory.get() && SUCCEEDED(factory->CreatePathGeometry(&triangle)) && SUCCEEDED(triangle->Open(&sink))) {
                sink->BeginFigure(D2D1::Point2F(cx, cy + direction * 2.5f * scale_), D2D1_FIGURE_BEGIN_FILLED);
                sink->AddLine(D2D1::Point2F(cx - 4.0f * scale_, cy - direction * 2.5f * scale_));
                sink->AddLine(D2D1::Point2F(cx + 4.0f * scale_, cy - direction * 2.5f * scale_));
                sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                if (SUCCEEDED(sink->Close())) {
                    MakeBrush(dc, enabled ? theme.text_secondary : WithAlpha(theme.text_secondary, 0.35f), brTextSecondary_);
                    dc->FillGeometry(triangle.get(), brTextSecondary_.get());
                }
            }
        }
        painter_.DrawText(std::to_wstring(tab_first_row_ + 1) + L"/" + std::to_wstring(strip.total_rows - strip.rows + 1),
            pager.label, compositor_->SmallFormat(), theme.text_secondary, fluent::HorizontalAlignment::Center);
    }
    x = strip.new_x;
    // New tab button follows the final rest slot (not the sliding tabs); a
    // scrolling strip pins it to the right edge.
    D2D1_RECT_F newRc = D2D1::RectF(x, strip.new_y, x + 32 * scale_, strip.new_y + tabH);
    DrawButton(newRc, theme, IsHovered(vm, HitTestResult::TabNew) ? theme.fill_hover : kTransparent,
        kIconAdd, L"+", theme.text_secondary, true, true);

    } else {
        // Vertical tabs: the title bar hosts the address row; a hairline
        // separates it from the command row.
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, right, 1.0f);
    }
    if (chrome.settings_w > 0) {
    // Settings + theme: one quiet pill with a hairline between the two, so
    // they read as app actions rather than two more caption buttons.
    const float groupHalf=(std::min)(18.0f*scale_,h*0.5f-4.0f*scale_);
    const float groupTop = h * 0.5f - groupHalf;
    const float groupBottom = h * 0.5f + groupHalf;
    if (!IsHighContrast()) {
        MakeBrush(dc, WithAlpha(theme.text, vm.dark ? 0.06f : 0.045f), brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), chrome.group_left, groupTop,
                        chrome.group_right - chrome.group_left, groupBottom - groupTop, 8.0f * scale_);
    }
    MakeBrush(dc, IsHighContrast() ? theme.stroke_divider : WithAlpha(theme.text, 0.12f), brStrokeDivider_);
    FillRect(dc, brStrokeDivider_.get(), chrome.theme_left - 1.0f * scale_, h * 0.5f - 8.0f * scale_,
             1.0f * scale_, 16.0f * scale_);
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    const float buttonTop = groupTop + 2.0f * scale_;
    const float buttonBottom = groupBottom - 2.0f * scale_;
    DrawButton(D2D1::RectF(chrome.windows_left, buttonTop, chrome.windows_left + chrome.settings_w, buttonBottom),
        theme, IsHovered(vm, HitTestResult::WindowManageButton) ? theme.fill_hover : kTransparent,
        L"", L"", theme.text, true, true);
    MakeBrush(dc, theme.text, brText_);
    const float windowsCenter = chrome.windows_left + chrome.settings_w * 0.5f;
    DrawWindowActionIcon(dc, D2D1::RectF(windowsCenter - 11.0f * scale_, h * 0.5f - 11.0f * scale_,
        windowsCenter + 11.0f * scale_, h * 0.5f + 11.0f * scale_), WindowActionIcon::Manage, brText_.get());
    const D2D1_RECT_F settingsRc = D2D1::RectF(chrome.settings_left, buttonTop,
        chrome.settings_left + chrome.settings_w, buttonBottom);
    // Open settings has no tab: the button shows as selected instead, with the
    // active tab's accent underline.
    const bool settingsSelected = vm.settings_open;
    const D2D1_COLOR_F settingsFill = settingsSelected
        ? WithAlpha(theme.accent, IsHovered(vm, HitTestResult::SettingsButton) ? 0.26f : 0.18f)
        : IsHovered(vm, HitTestResult::SettingsButton) ? theme.fill_hover : kTransparent;
    DrawButton(settingsRc, theme, settingsFill, kIconSettings, L"S",
        settingsSelected && !IsHighContrast() ? theme.accent : theme.text, true, true);
    if (settingsSelected) {
        MakeBrush(dc, theme.accent, brAccent_);
        const float barW = 14.0f * scale_;
        const float midX = (settingsRc.left + settingsRc.right) * 0.5f;
        FillRoundedRect(dc, brAccent_.get(), midX - barW * 0.5f, settingsRc.bottom - 3.0f * scale_,
                        barW, 2.5f * scale_, 1.25f * scale_);
    }

    const D2D1_RECT_F themeRc = D2D1::RectF(chrome.theme_left, buttonTop,
        chrome.theme_left + chrome.theme_w, buttonBottom);
    {
        // The solid half shows the current theme: right in light, left in dark.
        D2D1_MATRIX_3X2_F previous{};
        dc->GetTransform(&previous);
        if (vm.dark) {
            dc->SetTransform(D2D1::Matrix3x2F::Rotation(180.0f, D2D1::Point2F(
                (themeRc.left + themeRc.right) * 0.5f, (themeRc.top + themeRc.bottom) * 0.5f)) * previous);
        }
        DrawButton(themeRc, theme, IsHovered(vm, HitTestResult::ThemeToggle) ? theme.fill_hover : kTransparent,
            kIconTheme, L"T", theme.text, true, true);
        dc->SetTransform(previous);
    }

    }
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

void MainRenderer::DrawTeachBubble(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                                   const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    if (!dc) return;
    const TeachBubbleLayout t = MakeTeachBubbleLayout(rect, status_height_, scale_);
    if (t.card.right - t.card.left < 200.0f * scale_) return;
    const float r = 8.0f * scale_;
    MakeBrush(dc, theme.surface_flyout, brFillHover_);
    FillRoundedRect(dc, brFillHover_.get(), t.card.left, t.card.top,
                    t.card.right - t.card.left, t.card.bottom - t.card.top, r);
    MakeBrush(dc, WithAlpha(theme.accent, 0.55f), brStrokeCard_);
    dc->DrawRoundedRectangle(D2D1::RoundedRect(t.card, r, r), brStrokeCard_.get(), 1.0f * scale_);
    const D2D1_COLOR_F saved_bg = text_background_;
    text_background_ = BlendOver(theme.surface_flyout, theme.bg);
    DrawIconText(t.icon.left, t.icon.top, t.icon.right - t.icon.left, t.icon.bottom - t.icon.top,
                 L"\xE82F", L"!", theme.accent, 0.72f);
    painter_.DrawText(vm.teach.title, t.title, compositor_->HeaderFormat(), theme.text,
                      fluent::HorizontalAlignment::Left);
    ComPtr<IDWriteTextLayout> body;
    if (!vm.teach.body.empty() && SUCCEEDED(compositor_->DwriteFactory()->CreateTextLayout(
            vm.teach.body.c_str(), static_cast<UINT32>(vm.teach.body.size()), compositor_->SmallFormat(),
            t.body.right - t.body.left, t.body.bottom - t.body.top, &body))) {
        body->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        body->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        body->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        compositor_->DrawTextLayout({t.body.left, t.body.top}, body.get(), brTextSecondary_.get(),
                           D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    fluent::ControlState closeState;
    closeState.hovered = IsHovered(vm, HitTestResult::TeachDismiss);
    painter_.DrawButton({ t.close, {}, kIconCloseSmall, fluent::ButtonKind::Transparent, closeState, true });
    fluent::ButtonSpec never;
    never.bounds = t.never;
    never.text = vm.teach.never;
    never.kind = fluent::ButtonKind::Transparent;
    never.state.hovered = IsHovered(vm, HitTestResult::TeachNever);
    painter_.DrawButton(never);
    fluent::ButtonSpec primary;
    primary.bounds = t.primary;
    primary.text = vm.teach.primary;
    primary.kind = fluent::ButtonKind::Primary;
    primary.state.hovered = IsHovered(vm, HitTestResult::TeachPrimary);
    painter_.DrawButton(primary);
    text_background_ = saved_bg;
}

void MainRenderer::DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& full, const Theme& theme) {
    task_pill_rect_ = {};
    for (auto& action : task_action_rects_) action = {};
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const StatusBarMetrics sb = MakeStatusBarMetrics(
        vm, full, scale_, status_height_, factory, small_fmt);
    if (sb.undo_chip.right > sb.undo_chip.left) {
    const uint64_t identity = vm.status.history.empty() ? 0 : vm.status.history.front().task_id * 2 + static_cast<uint64_t>(vm.status.can_redo);
    if (identity != undo_chip_identity_) { undo_chip_identity_ = identity; undo_chip_bump_at_ = motion_now_; }
    D2D1_MATRIX_3X2_F before_chip; dc->GetTransform(&before_chip);
    if (undo_chip_bump_at_ && motion_now_ - undo_chip_bump_at_ < 450 && motion::SystemAnimationsEnabled()) {
        const float progress = static_cast<float>(motion_now_ - undo_chip_bump_at_) / 450.0f;
        const float bump = 1.0f + 0.06f * (progress < 0.4f ? progress / 0.4f : (1.0f - progress) / 0.6f);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(bump, bump, D2D1::Point2F((sb.undo_chip.left+sb.undo_chip.right)*0.5f,(sb.undo_chip.top+sb.undo_chip.bottom)*0.5f)) * before_chip);
        RequestMotionFrame();
    }
    const bool history_hot = IsHovered(vm, HitTestResult::StatusUndoHistory);
    const MockupInk chip_ink = MakeMockupInk(theme, vm.dark);
    // drag_undo_mockup .undo > button:hover: the pressed wash fills the whole
    // segment, cut by the capsule's rounded ends and square at the dividers.
    const auto segment_hover = [&](D2D1_RECT_F segment) {
        MakeBrush(dc, theme.fill_pressed, brFillSelected_);
        dc->PushAxisAlignedClip(segment, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillRoundedRectangle(D2D1::RoundedRect(sb.undo_chip, 11 * scale_, 11 * scale_), brFillSelected_.get());
        dc->PopAxisAlignedClip();
    };
    MakeBrush(dc, theme.fill_hover, brFillHover_);
    MakeBrush(dc, theme.text, brText_);
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
    dc->FillRoundedRectangle(D2D1::RoundedRect(sb.undo_chip, 11 * scale_, 11 * scale_), brFillHover_.get());
    if (vm.status.history_open) {
        MakeBrush(dc, chip_ink.accent_fg, brAccent_);
        auto outline = sb.undo_chip; outline.left += 0.5f * scale_; outline.top += 0.5f * scale_;
        outline.right -= 0.5f * scale_; outline.bottom -= 0.5f * scale_;
        dc->DrawRoundedRectangle(D2D1::RoundedRect(outline, 10.5f * scale_, 10.5f * scale_), brAccent_.get(), scale_);
    }
    if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
    const auto icon = [&](const D2D1_RECT_F& button, command_icons::Icon glyph, bool enabled, bool mirror) {
        if (enabled && glyph == command_icons::Icon::Undo && IsHovered(vm, mirror ? HitTestResult::StatusRedo : HitTestResult::StatusUndo))
            segment_hover(button);
        D2D1_MATRIX_3X2_F saved; dc->GetTransform(&saved);
        if (mirror) dc->SetTransform(D2D1::Matrix3x2F::Scale(-1, 1, D2D1::Point2F((button.left+button.right)*0.5f, 0)) * saved);
        MakeBrush(dc, glyph == command_icons::Icon::ChevronUp || !enabled ? theme.text_disabled : chip_ink.accent_fg, brAccent_);
        command_icons::Draw(dc, brAccent_.get(), paneHeaderStroke_.get(), glyph,
            command_icons::CenteredBounds(button, (glyph == command_icons::Icon::ChevronUp ? 8.0f : 11.0f) * scale_), 1.0f, command_icons::kStroke);
        dc->SetTransform(saved);
    };
    icon(sb.undo_button, command_icons::Icon::Undo, vm.status.can_undo, false);
    const float label_right = vm.status.can_redo ? sb.redo_button.left : sb.undo_chip.right;
    if (history_hot) segment_hover(D2D1::RectF(sb.undo_button.right, sb.undo_chip.top, label_right, sb.undo_chip.bottom));
    const auto chevron = D2D1::RectF(label_right-22*scale_, sb.undo_chip.top, label_right-2*scale_, sb.undo_chip.bottom);
    DrawTextEndEllipsis(compositor_, small_fmt, brText_.get(), vm.status.undo_label,
        sb.undo_button.right + 7 * scale_, sb.undo_chip.top,
        std::max(0.0f, chevron.left - sb.undo_button.right - 9 * scale_), sb.undo_chip.bottom - sb.undo_chip.top);
    icon(chevron, command_icons::Icon::ChevronUp, true, false);
    const auto divider = [&](float x) { dc->DrawLine(D2D1::Point2F(x,sb.undo_chip.top+5*scale_),D2D1::Point2F(x,sb.undo_chip.bottom-5*scale_),brStrokeDivider_.get()); };
    divider(sb.undo_button.right);
    if (vm.status.can_redo) { divider(sb.redo_button.left); icon(sb.redo_button, command_icons::Icon::Undo, true, true); }
    dc->SetTransform(before_chip);
    MakeBrush(dc, theme.accent, brAccent_);
    MakeBrush(dc, theme.fill_selected, brFillSelected_);
    }
    // Everything but the AI chip ends left of it (MakeStatusBarMetrics).
    D2D1_RECT_F rect = full;
    rect.right = sb.content_right;
    if (sb.ai_chip.right > sb.ai_chip.left) {
        // The AI chip (ai_panel_view.h): "✦ AI", what the assistant or an
        // outside client is doing, and whether Pulse 助手 is open.
        AiStatusChipView chip;
        chip.label = vm.status.ai_label;
        chip.text = vm.status.ai_text;
        chip.words = sb.ai_chip_text;
        switch (vm.status.ai_state) {
        case StatusAiState::Busy: chip.state = AiChipState::Busy; break;
        case StatusAiState::Waiting: chip.state = AiChipState::Waiting; break;
        case StatusAiState::Done: chip.state = AiChipState::Done; break;
        case StatusAiState::Error: chip.state = AiChipState::Error; break;
        case StatusAiState::Warn: chip.state = AiChipState::Warn; break;
        default: chip.state = AiChipState::Idle; break;
        }
        chip.progress = vm.status.ai_progress;
        chip.open = vm.status.ai_open;
        chip.unread = vm.status.ai_unread;
        chip.hover = IsHovered(vm, HitTestResult::StatusBarAi);
        chip.pressed = chip.hover && vm.status.ai_pressed;
        chip.dark = vm.dark;
        ai_panel_.DrawStatusChip(compositor_, theme, chip, sb.ai_chip, scale_, motion_now_);
        // Pulse 助手 closed and done: the bubble over the chip (.peek).
        ai_panel_.DrawPeek(compositor_, vm.ai_panel, theme, sb.ai_chip, scale_, motion_now_);
    }
    const bool centered_progress = vm.status.query_active || vm.status.task_is_update;
    float y = sb.bar.top;
    // Sits on the shared sheet painted by Render(); no own fill or top rule.
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), path::FriendlyPathText(vm.status.status_text),
        sb.status_label.left, y, sb.status_label.right - sb.status_label.left, status_height_);
    if (!vm.status.selection_text.empty()) DrawTextEndEllipsis(compositor_, small_fmt, brTextSecondary_.get(), vm.status.selection_text,
        sb.selection_label.left, y, sb.selection_label.right - sb.selection_label.left, status_height_);

    const float rightReserved = sb.right_reserved;
    if (vm.status.query_cancellable) {
        DrawButton(sb.cancel_search, theme,
            IsHovered(vm, HitTestResult::StatusBarCancelSearch) ? theme.fill_hover : kTransparent,
            kIconCloseSmall, L"×", theme.text_secondary, true, true, 0.62f);
    }
    if (!centered_progress && !vm.status.performance_text.empty()) {
        const std::wstring& perfText = rect.right < 1100.0f * scale_
            ? vm.status.performance_compact_text : vm.status.performance_text;
        const float perfWidth = std::min(std::max(0.0f, rightReserved - sb.pad),
            MeasureTextWidth(factory, small_fmt, perfText) + 16.0f * scale_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        MakeBrush(dc, theme.text_secondary, brTextSecondary_);
        DrawTextRect(dc, small_fmt, brTextSecondary_.get(), perfText,
            rect.right - rightReserved, y, perfWidth, status_height_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    } else if (!centered_progress && !vm.status.hint_text.empty()) {
        const float cancelWidth = vm.status.query_cancellable ? sb.cancel_search.right - sb.cancel_search.left + 8.0f * scale_ : 0.0f;
        const bool hasAction = sb.hint_action.right > sb.hint_action.left;
        const float actionWidth = hasAction ? sb.hint_action.right - sb.hint_action.left + 8.0f * scale_ : 0.0f;
        const float hintWidth = std::max(0.0f, rightReserved - sb.pad - cancelWidth - actionWidth);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
        DrawTextRect(dc, small_fmt, brTextSecondary_.get(), vm.status.hint_text,
            rect.right - rightReserved, y, hintWidth, status_height_);
        small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        if (hasAction) {
            // A soft accent chip: it reads as "you can click this", not as a warning.
            const auto& a = sb.hint_action;
            auto fill = theme.accent;
            fill.a = IsHovered(vm, HitTestResult::StatusHintAction) ? 0.26f : 0.14f;
            MakeBrush(dc, fill, brFillHover_);
            FillRoundedRect(dc, brFillHover_.get(), a.left, a.top, a.right - a.left, a.bottom - a.top, 5.0f * scale_);
            MakeBrush(dc, theme.accent, brAccentText_);
            small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            DrawTextRect(dc, small_fmt, brAccentText_.get(), vm.status.hint_action_text,
                a.left, y, a.right - a.left, status_height_);
            small_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        }
    }

    // Query/update activity shares the compact status area with operation summaries.
    const float taskX = sb.task.left;
    const float taskRight = sb.task.right;
    if (centered_progress) {
        const auto& progress_text = vm.status.query_active ? vm.status.query_text : vm.status.task_text;
        const float progress_value = vm.status.query_active ? vm.status.query_progress : vm.status.task_progress / 100.0f;
        const float taskWidth=std::max(0.0f,taskRight-taskX);
        const float trackWidth=std::min(100*scale_,taskWidth*0.30f);
        const float queryGap=std::min(8*scale_,taskWidth-trackWidth);
        const float textWidth=std::min(MeasureTextWidth(factory,small_fmt,progress_text),
            std::max(0.0f,taskWidth-trackWidth-queryGap));
        const float trackX=taskRight-trackWidth;
        const float textX=std::max(taskX,trackX-queryGap-textWidth);
        MakeBrush(dc,theme.accent,brAccentText_);
        DrawTextEndEllipsis(compositor_, small_fmt,brAccentText_.get(),progress_text,
            textX,y,textWidth,status_height_);
        fluent::ProgressSpec progress;
        progress.bounds=D2D1::RectF(trackX,y,std::min(taskRight,trackX+trackWidth),y+status_height_);
        progress.value=std::clamp(progress_value,0.0f,1.0f);
        progress.indeterminate=progress_value<0.0f;
        progress.animation_progress=static_cast<float>(GetTickCount64()%1952)/1952.0f;
        painter_.DrawProgressBar(progress);
    } else if (!vm.status.task_text.empty() || vm.status.task_progress >= 0.0f ||
               vm.status.task_active) {
        DrawTaskPill(vm, D2D1::RectF(taskX, y, taskRight, y + status_height_), theme);
    } else {
        task_pill_was_active_ = false;
        task_pill_spinning_ = false;
        task_pill_done_at_ = 0;
    }

}

namespace {
// Clockwise arc starting at start_deg (0 = 12 o'clock).
void DrawRingArc(ID2D1DeviceContext* dc, ID2D1Brush* brush, D2D1_POINT_2F c, float r,
                 float start_deg, float sweep_deg, float stroke) {
    if (!dc || !brush || sweep_deg <= 0.5f) return;
    if (sweep_deg >= 359.5f) {
        dc->DrawEllipse(D2D1::Ellipse(c, r, r), brush, stroke);
        return;
    }
    ComPtr<ID2D1Factory> factory;
    dc->GetFactory(&factory);
    ComPtr<ID2D1PathGeometry> geometry;
    if (!factory.get() || FAILED(factory->CreatePathGeometry(&geometry))) return;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(geometry->Open(&sink))) return;
    auto at = [&](float deg) {
        const float a = (deg - 90.0f) * 3.14159265f / 180.0f;
        return D2D1::Point2F(c.x + r * std::cos(a), c.y + r * std::sin(a));
    };
    sink->BeginFigure(at(start_deg), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(at(start_deg + sweep_deg), D2D1::SizeF(r, r), 0.0f,
        D2D1_SWEEP_DIRECTION_CLOCKWISE,
        sweep_deg > 180.0f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    if (FAILED(sink->Close())) return;
    ID2D1StrokeStyle* style = nullptr;
    dc->DrawGeometry(geometry.get(), brush, stroke, style);
}
} // namespace

void MainRenderer::DrawTaskPill(const WindowViewModel& vm, const D2D1_RECT_F& area,
                                const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* fmt = compositor_->SmallFormat();
    const ULONGLONG now = GetTickCount64();
    if (task_pill_id_ != vm.status.task_id) {
        task_pill_id_ = vm.status.task_id;
        task_pill_done_at_ = 0;
        task_pill_pop_at_ = 0;
        task_pill_was_active_ = false;
    }
    const bool active = vm.status.task_active;
    // Edge-detect completion: running -> finished without error shows the check.
    if (active) {
        task_pill_was_active_ = true;
        task_pill_done_at_ = 0;
    } else if (task_pill_was_active_) {
        task_pill_was_active_ = false;
        task_pill_done_at_ = vm.status.task_failed ? 0 : now;
        task_pill_animate_ = motion::SystemAnimationsEnabled();
        if (task_pill_animate_ && (!vm.status.task_failed || vm.status.task_failed_items > 0))
            task_pill_pop_at_ = now;
    }
    // Partial failure: red until the next
    // operation, the click opens the list.
    const bool bad = !active && (vm.status.task_failed || vm.status.task_failed_items > 0);
    if (task_pill_pop_at_ != 0 && now - task_pill_pop_at_ >= kTaskPillPopMs) task_pill_pop_at_ = 0;
    float strength = 1.0f;
    const bool done = !active && !bad && !vm.status.task_failed;
    const bool offers = done && (vm.status.task_can_undo || vm.status.task_can_open);
    task_pill_hold_ms_ = kTaskPillActionsMs;
    for (auto& action : task_action_rects_) action = {};
    if (done && task_pill_done_at_ == 0) task_pill_done_at_ = now;
    const bool compact = done && now - task_pill_done_at_ >= task_pill_hold_ms_;
    const bool show_actions = offers && !compact;
    const std::wstring undo_label = l10n::Get(l10n::StringId::Undo);
    const std::wstring open_label = l10n::Get(l10n::StringId::OpLocateShort);
    const std::wstring resume_label = l10n::Get(l10n::StringId::OpResume);
    const float action_pad = 6.0f * scale_;
    float undo_w = show_actions && vm.status.task_can_undo
        ? MeasureTextWidth(factory, fmt, undo_label) + action_pad * 2.0f : 0.0f;
    float open_w = show_actions && vm.status.task_can_open
        ? MeasureTextWidth(factory, fmt, open_label) + action_pad * 2.0f : 0.0f;
    float resume_w = active && vm.status.task_paused
        ? MeasureTextWidth(factory, fmt, resume_label) + action_pad * 2.0f : 0.0f;
    // Drop complete buttons before they would overlap the state icon.
    const float action_budget = std::max(0.0f, area.right - area.left - 36.0f * scale_);
    if (undo_w + open_w + resume_w > action_budget) undo_w = 0.0f;
    if (open_w + resume_w > action_budget) open_w = 0.0f;
    if (resume_w > action_budget) resume_w = 0.0f;
    const float actions_w = undo_w + open_w + resume_w > 0.0f
        ? undo_w + open_w + resume_w + 4.0f * scale_ : 0.0f;
    const bool indeterminate = active && !vm.status.task_paused && vm.status.task_progress < 0.0f;
    task_pill_spinning_ = indeterminate;

    const std::wstring& text = vm.status.task_text;
    const float area_w = std::max(0.0f, area.right - area.left);
    const float cy = (area.top + area.bottom) * 0.5f;
    const float pill_h = std::min(area.bottom - area.top - 4.0f * scale_, 22.0f * scale_);
    // Completed operations retain a compact, clickable state icon.
    // file_ops_mockup .pill .ring: 14 DIP, a 2.5 DIP band.
    const float ring_d = 14.0f * scale_ * (active || bad ? 1.0f : done ? strength : 0.0f);
    const float pad_l = 9.0f * scale_ * strength;
    const float pad_r = 10.0f * scale_ * strength;
    const float gap = compact ? 0.0f : 6.0f * scale_ * (active || bad ? 1.0f : done ? strength : 0.0f);
    const float text_w = compact || text.empty() ? 0.0f : MeasureTextWidth(factory, fmt, text);
    // .pill .count: "+N" operations queued behind this one, an accent badge
    // 16 DIP tall after the words (running only).
    const std::wstring queued = active && !vm.status.task_paused && vm.status.task_queued > 0
        ? L"+" + std::to_wstring(vm.status.task_queued) : std::wstring();
    const float queued_text_w = queued.empty() ? 0.0f : MeasureTextWidth(factory, fmt, queued);
    const float queued_w = queued.empty() ? 0.0f : std::max(16.0f * scale_, queued_text_w + 8.0f * scale_);
    const float queued_room = queued_w > 0.0f ? queued_w + 7.0f * scale_ : 0.0f;
    const float pill_w = std::min(area_w, pad_l + ring_d + gap + text_w + queued_room + actions_w +
                                          (actions_w > 0.0f ? 4.0f * scale_ - pad_r : 0.0f) + pad_r + 1.0f);
    // Right after the counts and the history capsule, like the mockup's
    // #pillSlot; the hint keeps the far right.
    const D2D1_RECT_F pill = D2D1::RectF(area.left, cy - pill_h * 0.5f,
                                         area.left + pill_w, cy + pill_h * 0.5f);
    task_pill_rect_ = pill;
    const bool hovered = IsHovered(vm, HitTestResult::StatusBarTask);
    // State ink (file_ops_mockup --ok / --warn / --danger): accent running,
    // warning paused, success done, danger partial. Running sits on the
    // neutral hover wash, the others on their soft fills.
    const MockupInk mk = MakeMockupInk(theme, vm.dark);
    const bool paused = active && vm.status.task_paused;
    const D2D1_COLOR_F tone = bad ? mk.danger : done ? mk.ok : paused ? mk.warn : mk.accent_fg;
    // HTML pillIn: .85 -> 1.06 at 60% -> 1, over 400 ms.
    D2D1_MATRIX_3X2_F saved_transform{};
    // Also while running: the card shrank into the pill (PopTaskPill).
    const bool popping = task_pill_pop_at_ != 0;
    if (popping) {
        const float t = std::clamp(static_cast<float>(now - task_pill_pop_at_) / kTaskPillPopMs, 0.0f, 1.0f);
        const float k = t < 0.6f ? 0.85f + 0.21f * OperationFlightEase(t / 0.6f, true)
                                 : 1.06f - 0.06f * OperationFlightEase((t - 0.6f) / 0.4f, true);
        dc->GetTransform(&saved_transform);
        dc->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F((pill.left + pill.right) * 0.5f, cy)) *
                         saved_transform);
    }

    dc->PushAxisAlignedClip(pill, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (strength > 0.0f) {
        const D2D1_COLOR_F fill = bad ? mk.danger_soft : done ? mk.ok_soft : paused ? mk.warn_soft
            : hovered ? theme.fill_pressed : theme.fill_hover;
        painter_.FillRoundedRect(pill, pill_h * 0.5f, fill);
        // .pill:hover: a hairline ring on top of the wash.
        if (hovered) painter_.StrokeRoundedRect(pill, pill_h * 0.5f, theme.stroke_card);
        if (active && !paused && vm.status.task_progress >= 0.0f) {
            // .pill .bar: the 2 DIP progress line along the bottom, kept
            // inside the rounded ends.
            const float inset = pill_h * 0.5f;
            const float span = std::max(0.0f, pill_w - inset * 2.0f);
            const float value = std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
            if (span * value > 0.5f)
                painter_.FillRoundedRect(D2D1::RectF(pill.left + inset, pill.bottom - 2.0f * scale_,
                                                     pill.left + inset + span * value, pill.bottom), scale_, mk.accent_fg);
        }
    }
    float text_x = pill.left + pad_l;
    if (ring_d > 0.5f) {
        const D2D1_POINT_2F c = D2D1::Point2F(pill.left + pad_l + ring_d * 0.5f, cy);
        const float r = ring_d * 0.5f - 1.25f * scale_;
        const float stroke = 2.5f * scale_;
        if (bad) {
            DrawIconText(c.x - ring_d * 0.5f - 2.0f * scale_, cy - ring_d * 0.5f - 2.0f * scale_,
                         ring_d + 4.0f * scale_, ring_d + 4.0f * scale_,
                         L"\xE7BA", L"!", tone, 0.82f);
        } else if (done) {
            D2D1_COLOR_F accent = tone;
            accent.a = std::max(strength, 0.0f);
            DrawIconText(c.x - ring_d * 0.5f - 2.0f * scale_, cy - ring_d * 0.5f - 2.0f * scale_,
                         ring_d + 4.0f * scale_, ring_d + 4.0f * scale_,
                         L"\xE73E", L"\u2713", accent, 0.82f);
        } else {
            // Track: the mockup's --divider, a touch firmer so it reads on the wash.
            MakeBrush(dc, WithAlpha(theme.text, vm.dark ? 0.14f : 0.12f), brStrokeDivider_);
            dc->DrawEllipse(D2D1::Ellipse(c, r, r), brStrokeDivider_.get(), stroke);
            MakeBrush(dc, tone, brAccentHover_);
            ID2D1SolidColorBrush* const tone_brush = brAccentHover_.get();
            if (tone_brush) {
                if (indeterminate) {
                    const float spin = static_cast<float>(now % 1000) * 0.36f;
                    DrawRingArc(dc, tone_brush, c, r, spin, 90.0f, stroke);
                } else {
                    const float value = std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
                    DrawRingArc(dc, tone_brush, c, r, 0.0f, std::max(value * 360.0f, 12.0f), stroke);
                }
            }
            // Shared frame brushes go back to their theme colours.
            MakeBrush(dc, theme.stroke_divider, brStrokeDivider_);
            MakeBrush(dc, theme.accent_hover, brAccentHover_);
        }
        text_x += ring_d + gap;
    }
    if (!compact && !text.empty()) {
        // .pill: the words in the normal text colour; state lives in the
        // icon and the wash.
        D2D1_COLOR_F color = theme.text;
        if (vm.status.task_failed && !active && !bad) color = theme.text_secondary;
        MakeBrush(dc, color, brAccentText_);
        const float text_right = (actions_w > 0.0f ? pill.right - actions_w
                                                   : pill.right - pad_r + 1.0f) - queued_room;
        DrawTextEndEllipsis(compositor_, fmt, brAccentText_.get(), text, text_x, area.top,
                            std::max(0.0f, text_right - text_x), status_height_);
    }
    if (queued_w > 0.0f && pill_w > queued_room + pad_l + ring_d + pad_r) {
        const D2D1_RECT_F badge = D2D1::RectF(pill.right - pad_r - queued_w, cy - 8.0f * scale_,
                                              pill.right - pad_r, cy + 8.0f * scale_);
        painter_.FillRoundedRect(badge, 8.0f * scale_, theme.accent);
        MakeBrush(dc, theme.accent_text, brAccentText_);
        DrawTextEndEllipsis(compositor_, fmt, brAccentText_.get(), queued,
                            badge.left + (queued_w - queued_text_w) * 0.5f, area.top, queued_text_w + 1.0f, status_height_);
    }
    if (actions_w > 0.0f) {
        // .pill .act: accent text buttons at the right end, 定位 before 撤销
        // as in the mockup; the card colour on hover.
        float x = pill.right - 4.0f * scale_ - open_w - undo_w - resume_w;
        const float top = pill.top + 2.0f * scale_;
        const float bottom = pill.bottom - 2.0f * scale_;
        const std::wstring* labels[3] = { &undo_label, &open_label, &resume_label };
        const float widths[3] = { undo_w, open_w, resume_w };
        constexpr int order[3] = { 1, 0, 2 };
        for (const int i : order) {
            if (widths[i] <= 0.0f) continue;
            const D2D1_RECT_F button = D2D1::RectF(x, top, x + widths[i], bottom);
            task_action_rects_[i] = button;
            if (IsHovered(vm, HitTestResult::StatusBarTask, -1, i))
                painter_.FillRoundedRect(button, (bottom - top) * 0.5f, theme.surface_card);
            MakeBrush(dc, mk.accent_fg, brAccentText_);
            DrawTextEndEllipsis(compositor_, fmt, brAccentText_.get(), *labels[i], x + action_pad, area.top,
                                widths[i] - action_pad * 2.0f + 1.0f, status_height_);
            x += widths[i];
        }
    }
    dc->PopAxisAlignedClip();
    if (popping) dc->SetTransform(saved_transform);
}

void MainRenderer::UpdateTabLayout(const WindowViewModel& vm, float width) {
    const auto strip = ComputeTabStrip(vm, width);
    tab_rows_mode_ = vm.tab_layout;
    title_bar_height_ = (embedded_ ? embedded_top_dip_ :
        !vertical_tabs_ ? kTabRowHeight * static_cast<float>(strip.rows) : kTitleBarHeight) * scale_;
}

MainRenderer::TabStripMetrics MainRenderer::ComputeTabStrip(
    const WindowViewModel& vm, float window_w) const {
    TabStripMetrics m;

    const TitleChrome chrome = MakeTitleChrome(window_w, scale_, title_bar_height_);
    m.x0 = TitleBrandWidth(vm, window_w) + margin_;
    // However many tabs are open, a stretch of title bar right of the "+"
    // button stays empty so the window can still be grabbed there; tabs
    // compact that much earlier instead.
    constexpr float kTabDragReserveDip = 56.0f;
    const float tabsRight = chrome.windows_left - (8.0f + kTabDragReserveDip) * scale_;

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

    if (vm.tab_layout != 0 && !vertical_tabs_ && !embedded_) {
        std::vector<TabRowItem> items;
        std::vector<size_t> tab_slots;
        std::vector<size_t> chip_slots;
        size_t chip_index = 0;
        int active = -1;
        for (size_t i = 0; i < vm.tabs.size(); ++i) {
            const auto& tab = vm.tabs[i];
            const float width = tab.pinned ? (vm.show_pinned_tab_names ? kTabPinnedNamedW : 36.0f) : 52.0f +
                MeasureTextWidth(compositor_->DwriteFactory(), compositor_->SmallFormat(), tab.title) / scale_;
            if (tab.group >= 0 && (i == 0 || vm.tabs[i - 1].group != tab.group) && chip_index < m.chips.size()) {
                chip_slots.push_back(items.size());
                items.push_back({m.chips[chip_index++].width / scale_ + 4.0f, true, false});
            }
            tab_slots.push_back(items.size());
            items.push_back({width, tab.pinned, tab.hidden});
            if (tab.active) active = static_cast<int>(items.size() - 1);
        }
        items.push_back({34.0f,true,false});
        const auto layout = ComputeTabRows(items, (window_w - 32.0f * scale_) / scale_,
            m.x0 / scale_, tabsRight / scale_, vm.tab_layout == 1,
            vm.tab_max_rows, tab_first_row_, active, active != tab_scroll_active_ ||
                window_w != tab_rows_width_ || vm.tab_layout != tab_rows_mode_ || vm.tab_max_rows != tab_rows_limit_);
        tab_rows_width_ = window_w; tab_rows_mode_ = vm.tab_layout; tab_rows_limit_ = vm.tab_max_rows;
        tab_first_row_ = layout.first_row;
        tab_scroll_active_ = active;
        m.rows = layout.visible_rows;
        m.total_rows = layout.rows;
        m.y = 4.0f * scale_;
        m.h = (kTabRowHeight - 4.0f) * scale_;
        m.pitch = 120.0f * scale_;
        for (size_t i = 0; i < chip_slots.size(); ++i) {
            const auto& slot = layout.slots[chip_slots[i]];
            const int row = layout.VisibleRow(slot.row);
            m.chips[i].left = slot.x * scale_;
            m.chips[i].top = row < 0 ? -100.0f * scale_ : m.y + (static_cast<float>(row) * kTabRowHeight) * scale_;
        }
        m.item_rects.resize(vm.tabs.size());
        for (size_t i = 0; i < vm.tabs.size(); ++i) {
            const auto& slot = layout.slots[tab_slots[i]];
            const int row = layout.VisibleRow(slot.row);
            if (row < 0) continue;
            const float top = m.y + (static_cast<float>(row) * kTabRowHeight) * scale_;
            m.item_rects[i] = D2D1::RectF(slot.x * scale_, top,
                (slot.x + (std::max)(0.5f, slot.width - 2.0f)) * scale_, top + m.h);
        }
        const auto& plus=layout.slots.back();
        const int plus_row=layout.VisibleRow(plus.row);
        m.new_x=plus.x*scale_;
        m.new_y=plus_row<0 ? -100*scale_ : m.y+(plus_row*kTabRowHeight)*scale_;
        return m;
    }

    if (vm.tab_layout == 0 && !vertical_tabs_ && !embedded_) {
        const float gap = 2.0f * scale_;
        const float gross = std::max(0.0f, tabsRight - m.x0 - 38.0f * scale_);
        if (chipsTotal > std::max(0.0f,gross-72.0f*scale_) && !m.chips.empty()) {
            const float budget = std::max(0.0f,gross-72.0f*scale_);
            const float per_chip = budget/static_cast<float>(m.chips.size());
            chipsTotal = 0;
            for (auto& chip : m.chips) {
                chip.width = std::max(0.0f,per_chip-chipGap);
                if (chip.width > 0) chipsTotal += chip.width+chipGap;
            }
        }
        float available = std::max(0.0f,gross-chipsTotal);
        m.y = m.new_y = 4.0f * scale_;
        m.h = (kTabRowHeight - 4.0f) * scale_;
        m.item_rects.resize(vm.tabs.size());
        m.icon_only.resize(vm.tabs.size(), false);
        std::vector<float> widths(vm.tabs.size());
        std::vector<size_t> visible;
        float total = 0;
        for (size_t i = 0; i < vm.tabs.size(); ++i) {
            const auto& tab = vm.tabs[i];
            if (tab.hidden) continue;
            visible.push_back(i);
            if (tab.active) m.active = static_cast<int>(i);
            widths[i] = tab.pinned ? (vm.show_pinned_tab_names ? kTabPinnedNamedW : 36.0f) * scale_ :
                ((60.0f + (tab.marker_rgb ? 12.0f : 0.0f)) * scale_ +
                    MeasureTextWidth(compositor_->DwriteFactory(), compositor_->TabFormat(), tab.title));
            total += widths[i];
        }
        const float gaps = visible.empty() ? 0 : static_cast<float>(visible.size()-1)*gap;
        if (total + gaps > available && !visible.empty()) {
            // Preserve readable labels; scroll instead of collapsing into icons.
            if (!FitTabWidths(widths, visible, available - gaps, kTabMinW * scale_)) {
                for (const auto i : visible) {
                    if (!vm.tabs[i].pinned)
                        widths[i] = std::max(std::min(widths[i], kTabMinW * scale_),
                            vm.tabs[i].active ? kTabActiveMinW * scale_ : 0.0f);
                }
                m.overflow = true;
                m.arrow_w = std::min(kTabScrollArrowW * scale_, gross * .25f);
                m.view_left = m.x0 + m.arrow_w;
                m.view_right = std::max(m.view_left, tabsRight - 36.0f * scale_ - m.arrow_w);
                m.x0 = m.view_left;
            }
        }
        float x = m.x0;
        size_t chip_index = 0;
        for (size_t i = 0; i < vm.tabs.size(); ++i) {
            const int group = vm.tabs[i].group;
            if (group >= 0 && (i == 0 || vm.tabs[i-1].group != group) && chip_index < m.chips.size()) {
                auto& chip = m.chips[chip_index++];
                chip.top = m.y; chip.left = x;
                if (chip.width > 0) x += chip.width + chipGap;
            }
            if (widths[i] <= 0) continue;
            m.item_rects[i] = D2D1::RectF(x,m.y,x+widths[i],m.y+m.h);
            x += widths[i]+gap;
        }
        m.new_x = m.end_x = x;
        m.w = 104.0f*scale_; m.pitch = m.w+gap;
        m.active_w = m.active >= 0 ? widths[static_cast<size_t>(m.active)] : m.w;
        if (m.overflow) {
            m.max_scroll = std::max(0.0f, x - gap - m.view_right);
            if (m.active >= 0 && m.active != tab_scroll_active_) {
                const auto& active = m.item_rects[static_cast<size_t>(m.active)];
                if (active.left < m.view_left + tab_scroll_) tab_scroll_ = active.left - m.view_left;
                if (active.right > m.view_right + tab_scroll_) tab_scroll_ = active.right - m.view_right;
            }
            tab_scroll_ = std::clamp(tab_scroll_, 0.0f, m.max_scroll);
            m.scroll = tab_scroll_;
            for (auto& item : m.item_rects) { item.left -= m.scroll; item.right -= m.scroll; }
            for (auto& chip : m.chips) chip.left -= m.scroll;
            m.x0 -= m.scroll;
            m.end_x -= m.scroll;
            m.new_x = tabsRight - 32.0f * scale_;
        } else {
            tab_scroll_ = 0;
        }
        tab_scroll_active_ = m.active;
        return m;
    }

    const float newW = 32.0f * scale_;
    const float available = std::max(0.0f, tabsRight - m.x0 - 36.0f * scale_ - chipsTotal);
    size_t visibleCount = 0;
    size_t pinnedCount = 0; // visible pinned tabs get a fixed narrow slot
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const auto& t = vm.tabs[i];
        if (t.hidden) continue;
        if (t.pinned) ++pinnedCount; else ++visibleCount;
        if (t.active && !t.pinned) m.active = static_cast<int>(i);
    }
    const float pinnedW = (vm.show_pinned_tab_names ? kTabPinnedNamedW : kTabPinnedW) * scale_;
    m.pinned_w = pinnedW;
    const float pinnedTotal = static_cast<float>(pinnedCount) * (pinnedW + control_gap_);
    const float spare = std::max(0.0f, available - pinnedTotal);
    const float minW = kTabMinW * scale_;
    const float normalW = visibleCount == 0 ? 0.0f
        : spare / static_cast<float>(visibleCount) - control_gap_;
    if (tab_freeze_w_ > 0.0f && visibleCount > 0 && visibleCount <= tab_freeze_count_ &&
        static_cast<float>(visibleCount) * (tab_freeze_w_ + control_gap_) +
            (m.active >= 0 ? tab_freeze_active_w_ - tab_freeze_w_ : 0.0f) <= spare + 0.5f) {
        // Recently closed: keep the widths the pointer was aiming at.
        m.w = tab_freeze_w_;
        m.active_w = tab_freeze_active_w_;
    } else if (visibleCount == 0 || normalW >= minW) {
        m.w = visibleCount == 0 ? 0.0f : std::min(kTabMaxW * scale_, normalW);
        m.active_w = m.w;
    } else {
        // Crowded: tabs stop shrinking at the minimum and keep their titles;
        // the strip scrolls instead (Edge's scrollable tab strip). The active
        // tab always shows close, so it gets that room on top.
        m.w = minW;
        m.active_w = m.active >= 0 ? kTabActiveMinW * scale_ : m.w;
        m.overflow = true;
    }
    if (m.active < 0) m.active_w = m.w;
    m.y = 4.0f * scale_;
    m.new_y = m.y;
    m.h = !vertical_tabs_ && !embedded_ ? (kTabRowHeight - 4.0f) * scale_ : title_bar_height_ - 8.0f * scale_;
    m.pitch = m.w + control_gap_;
    const float stripLeft = m.x0;
    if (m.overflow) {
        m.arrow_w = (std::min)(kTabScrollArrowW * scale_,
            (std::max)(0.0f, (tabsRight - newW - 4.0f * scale_ - stripLeft - 40.0f * scale_) * 0.5f));
        m.view_left = stripLeft + m.arrow_w;
        m.view_right = std::max(m.view_left, tabsRight - newW - 4.0f * scale_ - m.arrow_w);
        m.x0 = m.view_left;
    }

    // Final pass: per-tab extra offset + definitive chip positions. Collapsed
    // members contribute zero width (chip stays visible at the fold point);
    // pinned tabs use the fixed narrow slot, the active tab its own width.
    float acc = 0.0f;
    size_t chipIdx = 0;
    for (size_t i = 0; i < vm.tabs.size(); ++i) {
        const int g = vm.tabs[i].group;
        const bool runStart = g >= 0 && (i == 0 || vm.tabs[i - 1].group != g);
        if (runStart && chipIdx < m.chips.size()) {
            TabStripMetrics::Chip& chip = m.chips[chipIdx++];
            chip.top = m.y;
            chip.left = m.x0 + static_cast<float>(i) * m.pitch + acc;
            acc += chip.width + chipGap;
        }
        m.extra[i] = acc;
        if (vm.tabs[i].hidden) acc -= m.pitch;
        else if (vm.tabs[i].pinned) acc += pinnedW - m.w;
        else if (static_cast<int>(i) == m.active) acc += m.active_w - m.w;
    }
    m.end_x = m.x0 + chipsTotal + static_cast<float>(pinnedCount) *
        (pinnedW + control_gap_) + static_cast<float>(visibleCount) * m.pitch +
        (m.active >= 0 ? m.active_w - m.w : 0.0f);
    if (m.overflow) {
        const float view = m.view_right - m.view_left;
        m.max_scroll = std::max(0.0f, m.end_x - control_gap_ - m.view_left - view);
        if (m.active >= 0 && m.active != tab_scroll_active_) {
            // A newly active tab (click, keyboard, new tab) scrolls into view.
            const size_t a = static_cast<size_t>(m.active);
            const float left = static_cast<float>(a) * m.pitch + m.extra[a];
            const float margin = std::min(m.w, view * 0.25f);
            if (left - margin < tab_scroll_) tab_scroll_ = left - margin;
            if (left + m.active_w + margin > tab_scroll_ + view) tab_scroll_ = left + m.active_w + margin - view;
        }
        tab_scroll_ = std::clamp(tab_scroll_, 0.0f, m.max_scroll);
        m.scroll = tab_scroll_;
        m.x0 -= m.scroll;
        m.end_x -= m.scroll;
        for (auto& chip : m.chips) chip.left -= m.scroll;
        m.new_x = tabsRight - newW;
    } else {
        tab_scroll_ = 0.0f;
        m.new_x = m.end_x;
    }
    tab_scroll_active_ = m.active;
    return m;
}

std::vector<int> MainRenderer::HiddenTitleTabs(const WindowViewModel& vm, float width) const {
    return ComputeTabStrip(vm, width).hidden_indices;
}

bool MainRenderer::ScrollTabStrip(const WindowViewModel& vm, float window_w, float x, float y,
                                  float steps) {
    if (vertical_tabs_ || y < 0.0f || y >= title_bar_height_) return false;
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    if (!m.item_rects.empty() && vm.tab_layout != 0) {
        tab_first_row_ = std::clamp(tab_first_row_ - static_cast<int>(steps), 0, m.total_rows - m.rows);
        return m.total_rows > m.rows;
    }
    if (!m.overflow || x < m.view_left - m.arrow_w || x >= m.view_right + m.arrow_w) return false;
    tab_scroll_ = std::clamp(tab_scroll_ - steps * 2.0f * m.pitch, 0.0f, m.max_scroll);
    return true;
}

void MainRenderer::ScrollTabStripPage(const WindowViewModel& vm, float window_w, int direction) {
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    if (!m.item_rects.empty() && vm.tab_layout != 0) {
        tab_first_row_ = std::clamp(tab_first_row_ + direction, 0, m.total_rows - m.rows);
        return;
    }
    if (!m.overflow) return;
    const float page = std::max(m.pitch, (m.view_right - m.view_left) * 0.6f);
    tab_scroll_ = std::clamp(tab_scroll_ + static_cast<float>(direction) * page, 0.0f, m.max_scroll);
}

void MainRenderer::FreezeTabWidths(const WindowViewModel& vm, float window_w) {
    tab_freeze_w_ = 0.0f;
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    // Scrolling strips already keep their geometry; tabs at full width
    // simply grow back.
    if (m.overflow || m.w <= 0.0f || m.w >= kTabMaxW * scale_ - 0.5f) return;
    size_t count = 0;
    for (const auto& t : vm.tabs) if (!t.hidden && !t.pinned) ++count;
    tab_freeze_w_ = m.w;
    tab_freeze_active_w_ = m.active_w;
    tab_freeze_count_ = count;
}

bool MainRenderer::ReleaseTabWidths() {
    if (tab_freeze_w_ <= 0.0f) return false;
    tab_freeze_w_ = 0.0f;
    return true;
}

} // namespace pulse::ui
