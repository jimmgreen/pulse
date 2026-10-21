// ui_renderer.cpp — Chrome orchestration (title, toolbar, status, render).
#include "legacy_icons.h"
#include "ui_renderer.h"
#include "snack_style.h"
#include "drag_preview_layout.h"
#include "command_icons.h"
#include "toolbar_layout.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include "../common/display_path.h"
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
MainRenderer::MainRenderer()
    : owned_caches_(std::make_unique<RenderCaches>()), caches_(*owned_caches_) {}

MainRenderer::MainRenderer(RenderCaches& caches) : caches_(caches) {}

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
    empty_state_svg_dc_.reset();
    app_icon_bitmaps_.clear();
    app_icon_dc_ = nullptr;
    compositor_ = comp;
    material_.SetCompositor(comp);
    painter_.SetCompositor(comp);
    if (!comp) {
        // Closing one window must not empty the caches the others draw from.
        if (owned_caches_) ResetRenderCaches(*owned_caches_);
        preview_handler_.Reset();
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
    preview_handler_.SetNotifyWindow(hwnd);
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
    toolbar_height_ = (vertical_tabs_ || embedded_ ? 44.0f : 88.0f) * scale;
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
    embedded_top_dip_ = top_dip;
    embedded_bottom_dip_ = bottom_dip;
    SetDetailsPanelVisible(false);
    SetScale(scale_);
}

float MainRenderer::EffectiveSidebarWidth(float window_width) const {
    const float window_dip = window_width / scale_;
    const float rail = kSidebarRailWidthDip * scale_;
    if (window_dip < kSidebarRailWindowDip || collapse_value_ >= 1.0f) return rail;
    const float full = SidebarFullWidth(window_width);
    return collapse_value_ <= 0.0f ? full : full + (rail - full) * collapse_value_;
}

void MainRenderer::SetSidebarCollapsed(bool on, bool animate) {
    if (on == sidebar_collapsed_) return;
    sidebar_collapsed_ = on;
    if (on) sidebar_peek_ = false;
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
    if (!details_visible_ || window_dip < kDetailsVisibleWindowDip) return 0.0f;
    const float max_dip = MaxDetailsWidthDip(window_dip, sidebar_width_dip_,
                                             (window_dip < kSidebarRailWindowDip || sidebar_collapsed_),
                                             2.0f * margin_ / scale_);
    return (std::min)(details_width_, max_dip) * scale_ + margin_;
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
    return D2D1::RectF(left, top, w - margin_ - DetailsPanelWidth(w), bottom);
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
    if (toolbar_group_ < 0) return 0.0f;
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
    const float sort_width = ToolbarCommandWidth(painter_, format, l10n::StringId::ToolbarSort,
                                                 L"\xE8CB", true, scale_, 88.0f);
    const float filter_width = ToolbarCommandWidth(painter_, format, l10n::StringId::ToolbarFilter,
                                                   kIconFilter, false, scale_, 88.0f);
    const float selection_span = selection_count_ > 0 && !embedded_ && !pin_drop_mode_
        ? SelectionChipSpan(scale_, SelectionChipLabelWidth(w, selection_count_), selection_actions_) : 0.0f;
    ToolbarPinOptions pins;
    pins.app_from = pin_app_from_;
    pins.drop_mode = pin_drop_mode_ && !embedded_;
    pins.label_width = pins.drop_mode ? ToolbarPinLabelWidth() : 0.0f;
    if (!vertical_tabs_)
        return MakeToolbarLayout(w, scale_, title_bar_height_, margin_, create_width, left, filter_expand,
                                 search_min_dip_, group_width, sort_width, filter_width, pin_count_,
                                 selection_span, pins);
    // Command row directly under the title bar; address row centered in it,
    // stopping short of the settings button.
    ToolbarLayout out = MakeToolbarLayout(w, scale_, title_bar_height_ - 46.0f * scale_, margin_,
                                          create_width, left, filter_expand, 0.0f, group_width, sort_width,
                                          filter_width, pin_count_, selection_span, pins);
    const TitleChrome chrome = MakeTitleChrome(w, scale_, title_bar_height_);
    const float row_top = (title_bar_height_ - 36.0f * scale_) * 0.5f - 4.0f * scale_;
    const ToolbarLayout row = MakeToolbarLayout(chrome.settings_left - 4.0f * scale_, scale_, row_top,
                                                margin_, create_width, left, 0.0f, search_min_dip_);
    out.navigation = row.navigation;
    out.address = row.address;
    out.search = row.search;
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

D2D1_RECT_F MainRenderer::SidebarToggleRect(float w) const {
    if (w / scale_ < kSidebarRailWindowDip) return {};
    const float size = 32.0f * scale_;
    const float top = (title_bar_height_ - size) * 0.5f;
    if (sidebar_collapsed_) {
        const float left = (kSidebarRailWidthDip * scale_ - size) * 0.5f;
        return D2D1::RectF(left, top, left + size, top + size);
    }
    const float right = EffectiveSidebarWidth(w) - 8.0f * scale_;
    return D2D1::RectF(right - size, top, right, top + size);
}

// Dot and name sit where the product name is, after the mark: 6 dip padding,
// a 10 dip dot, 6 dip gap, the name, 6 dip padding; never under the sidebar
// toggle. Without room for that (the icon rail, a narrow sidebar) a dot sits
// on the lower right corner of the mark, or of the toggle where the toggle
// stands in for the mark.
MainRenderer::WindowIdentityGeometry MainRenderer::WindowIdentityLayout(const WindowViewModel& vm, float w) const {
    WindowIdentityGeometry g;
    if (!vm.window_identity || vm.window_identity_name.empty() || !compositor_) return g;
    const D2D1_RECT_F toggle = SidebarToggleRect(w);
    const bool has_toggle = toggle.right > toggle.left;
    if (!(has_toggle && sidebar_collapsed_)) {
        const float left = (12.0f + 24.0f + 4.0f) * scale_;
        float limit = EffectiveSidebarWidth(w) - 8.0f * scale_;
        if (has_toggle) limit = (std::min)(limit, toggle.left - 4.0f * scale_);
        const float text = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), compositor_->HeaderFormat(),
                                             vm.window_identity_name);
        const float width = (std::min)(limit - left, 28.0f * scale_ + text);
        if (width >= 40.0f * scale_) {
            const float height = 28.0f * scale_;
            const float top = (title_bar_height_ - height) * 0.5f;
            g.hit = D2D1::RectF(left, top, left + width, top + height);
            g.dot_radius = 5.0f * scale_;
            g.dot = D2D1::Point2F(left + 6.0f * scale_ + g.dot_radius, top + height * 0.5f);
            return g;
        }
    }
    g.compact = true;
    g.dot_radius = 4.0f * scale_;
    if (has_toggle && sidebar_collapsed_) {
        g.dot = D2D1::Point2F(toggle.right - 3.0f * scale_, toggle.bottom - 3.0f * scale_);
        const float reach = g.dot_radius + 3.0f * scale_;
        g.hit = D2D1::RectF(g.dot.x - reach, g.dot.y - reach, g.dot.x + reach, g.dot.y + reach);
    } else {
        const float mark = 24.0f * scale_;
        const float left = 12.0f * scale_;
        const float top = (title_bar_height_ - mark) * 0.5f;
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
    if (w-EffectiveSidebarWidth(w)<480*scale_) {
        const auto lay = ToolbarLayoutAt(w, NewButtonWidthPx(true));
        return D2D1::RectF(EffectiveSidebarWidth(w)+margin_, lay.address.top, lay.search.right, lay.address.bottom);
    }
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
    std::wstring pulse_kind, pulse_rest;
    if (app::ParsePulsePath(p, &pulse_kind, &pulse_rest)) {
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

void MainRenderer::BreadcrumbLayout(const PaneViewModel& vm, float w,
                                    std::vector<BreadcrumbPlaced>& out) const {
    out.clear();
    auto segments = SplitBreadcrumb(vm.is_query_search && vm.has_search_origin ? vm.search_origin : vm.path);
    if (vm.is_query_search && vm.has_search_origin) {
        BreadcrumbSegment search;
        search.text = vm.search_crumb.empty() ? pulse::l10n::Get(pulse::l10n::StringId::Search) : vm.search_crumb;
        search.path = vm.path;
        segments.push_back(std::move(search));
    }
    if (segments.empty()) return;
    D2D1_RECT_F addr = AddressBarRect(w);
    const float segPad = 8.0f * scale_;
    const float chevronW = 14.0f * scale_;
    const float hint = 0.0f;
    const float avail = std::max(0.0f, addr.right - addr.left - 2 * margin_ - hint);

    IDWriteFactory2* dwrite = compositor_ ? compositor_->DwriteFactory() : nullptr;
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

void MainRenderer::DrawIconText(float x, float y, float w, float h,
    const std::wstring& glyph, const std::wstring& fallback,
    const D2D1_COLOR_F& color, float size_factor) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    const auto command = command_icons::FromGlyph(glyph);
    if (command != command_icons::Icon::None) {
        if (!paneHeaderStroke_.get()) command_icons::CreateStrokeStyle(dc, &paneHeaderStroke_);
        MakeBrush(dc, color, brText_);
        const auto bounds = command_icons::CenteredBounds(D2D1::RectF(x, y, x + w, y + h),
                                                           20.0f * scale_ * size_factor);
        if (command_icons::Draw(dc, brText_.get(), paneHeaderStroke_.get(), command, bounds)) return;
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

void MainRenderer::Render(const WindowViewModel& vm, const D2D1_RECT_F& rect,
                          const Theme& theme) {
    if (!compositor_ || !compositor_->Dc()) return;
    ++motion_frame_;
    folder_thumbnail_cache_.BeginFrame();
    struct FolderFrameScope {
        FolderThumbnailCache& cache;
        ~FolderFrameScope() { cache.EndFrame(); }
    } folder_frame{folder_thumbnail_cache_};
    // One timestamp per frame: every glide and the sidebar width agree.
    motion_now_ = motion::NowMs();
    list_loading_active_ = false;
    folder_size_anim_active_ = false;
    if (collapse_anim_) {
        // Sampled once per frame so every layout query agrees on the width.
        const float t = static_cast<float>(motion_now_ - collapse_start_) / kSidebarCollapseMs;
        const float target = sidebar_collapsed_ ? 1.0f : 0.0f;
        if (t >= 1.0f) {
            collapse_value_ = target;
            collapse_anim_ = false;
        } else {
            collapse_value_ = collapse_from_ + (target - collapse_from_) * motion::EaseOutCubic(t);
        }
    }
    ID2D1DeviceContext* dc = compositor_->Dc();
    icon_cache_.SetDeviceContext(compositor_->Dc());
    open_with_icons_.SetDeviceContext(compositor_->Dc());
    UpdateBrushes(theme);
    text_background_ = theme.bg;
    painter_.BeginFrame(theme, IsHighContrast());

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
    const LayerAlphas layers = ComputeLayerAlphas(image_mode, backdrop_drawn,
        vm.backdrop_enabled, vm.dark, vm.wallpaper_look, vm.wallpaper_blur);
    if (!backdrop_drawn && !vm.backdrop_enabled) {
        // Lower pane opacity must reveal the theme canvas when no material or
        // wallpaper exists, instead of exposing an unpainted transparent base.
        MakeBrush(dc, theme.bg, brBg_);
        dc->FillRectangle(rect, brBg_.get());
    }
    sheet_alpha_ = layers.sheet;
    card_alpha_ = layers.card;
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
    if (vm.settings_open) {
        preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                              theme.bg, theme.text, false);
        DrawSettings(vm, rect, theme);
        DrawStatusBar(vm, rect, theme);
    } else {
        DrawToolbar(vm, rect, theme);
        const bool peek = SidebarPeekVisible(rect.right);
        if (!peek) DrawSidebar(vm, rect, theme);
        DrawPane(vm, rect, theme);
        if (vm.details_visible) DrawDetailsPanel(vm, rect, theme);
        else preview_handler_.Sync(notify_hwnd_, {}, L"", 0, 0, 0, 0, vm.dark,
                                   theme.bg, theme.text, false);
        if (peek) DrawSidebarPeek(vm, rect, theme);
        if (!embedded_) DrawStatusBar(vm, rect, theme);
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
    D2D1_RECT_F item_preview_rect{};
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
        float pillW = 30.0f * scale_ + titleW + (chipW > 0.0f ? 7.0f * scale_ + chipW : 0.0f) + 13.0f * scale_;
        float px = std::round((std::clamp)(ghost.x - 28.0f * scale_, rect.left, (std::max)(rect.left, rect.right - pillW)));
        float py = std::round((std::clamp)(ghost.y - 14.0f * scale_, rect.top, (std::max)(rect.top, rect.bottom - pillH)));
        if (ghost.item_preview) {
            const float badgeW = vm.drag_badge.empty() ? 0.0f :
                MeasureLayoutText(compositor_, factory, smallFmt, vm.drag_badge) + 20.0f * scale_;
            const float stackH = pillH + (vm.drag_badge.empty() ? 0.0f : 30.0f * scale_);
            item_preview_rect = ItemDragPreviewRect(rect, ghost.x, ghost.y,
                                                    (std::max)(pillW, badgeW), stackH, scale_);
            px = item_preview_rect.left;
            py = item_preview_rect.top;
            pillW = (std::min)(pillW, item_preview_rect.right - px);
            dc->PushAxisAlignedClip(item_preview_rect, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        }
        plate(px, py, pillW, pillH, pillR);
        const std::wstring& glyph = ghost.icon_glyph.empty() ? kIconFolder : ghost.icon_glyph;
        const D2D1_COLOR_F color = ghost.icon_color.a > 0.0f ? ghost.icon_color : theme.icon_folder;
        DrawIconText(px + 10.0f * scale_, py, 16.0f * scale_, pillH, glyph,
                     ghost.icon_fallback.empty() ? L"[]" : ghost.icon_fallback, color, 0.95f);
        MakeBrush(dc, pal.text, brText_);
        const float visibleTitleW = ghost.item_preview ? (std::max)(0.0f, pillW - 43.0f * scale_) : titleW;
        DrawTabTitle(dc, factory, tabFmt, brText_.get(), ghost.title, px + 30.0f * scale_, py, visibleTitleW, pillH, scale_);
        if (ghost.item_preview) dc->PopAxisAlignedClip();
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

    // Drag action badge (ui.md §7.8): tooltip-style flyout near the cursor.
    if (!vm.drag_badge.empty()) {
        IDWriteTextFormat* fmt = compositor_->SmallFormat();
        float tw = MeasureLayoutText(compositor_, compositor_->DwriteFactory(), fmt,
                                     vm.drag_badge);
        float bw = tw + 20 * scale_;
        float bh = 24 * scale_;
        float bx = std::min(vm.drag_badge_x + 14 * scale_, rect.right - bw - margin_);
        float by = std::min(vm.drag_badge_y + 16 * scale_, rect.bottom - bh - margin_);
        if (vm.drag_ghost.on && vm.drag_ghost.item_preview) {
            bw = (std::min)(bw, item_preview_rect.right - item_preview_rect.left);
            tw = (std::max)(0.0f, bw - 20.0f * scale_);
            bx = item_preview_rect.left;
            by = item_preview_rect.top + 34.0f * scale_;
        }
        D2D1_RECT_F brc = D2D1::RectF(bx, by, bx + bw, by + bh);
        MakeBrush(dc, theme.surface_flyout, brFillHover_);
        FillRoundedRect(dc, brFillHover_.get(), brc.left, brc.top, bw, bh, 4 * scale_);
        if (vm.drag_badge_move) {
            // Moving removes the source; make it unmistakable before the drop.
            const D2D1_COLOR_F amber = D2D1::ColorF(0xD48A1A);
            MakeBrush(dc, amber, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(brc, 4 * scale_, 4 * scale_), brStrokeCard_.get(), 1.5f * scale_);
            FillRoundedRect(dc, brStrokeCard_.get(), brc.left + 3 * scale_, brc.top + 5 * scale_,
                            3 * scale_, bh - 10 * scale_, 1.5f * scale_);
        } else {
            MakeBrush(dc, theme.stroke_card, brStrokeCard_);
            dc->DrawRoundedRectangle(D2D1::RoundedRect(brc, 4 * scale_, 4 * scale_), brStrokeCard_.get(), 1.0f);
        }
        MakeBrush(dc, theme.text, brText_);
        const D2D1_COLOR_F saved_badge_bg = text_background_;
        text_background_ = BlendOver(theme.surface_flyout, theme.bg);
        DrawTextRect(dc, fmt, brText_.get(), vm.drag_badge, bx + 10 * scale_, by, tw, bh);
        text_background_ = saved_badge_bg;
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
    if (vm.teach.visible && vm.drag_badge.empty()) DrawTeachBubble(vm, rect, theme);
    if (!vm.change_popover.visible && vm.drag_badge.empty() && !vm.tooltip_text.empty() &&
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
        group_wheel_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme);
    if (window_overview_.IsOpen())
        window_overview_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme);
    if (window_switcher_.IsOpen())
        window_switcher_.Draw(dc, compositor_->DwriteFactory(), compositor_->TextFormat(), theme);

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

    // Product mark: the packaged app icon; the monogram is the fallback.
    float x = 12.0f * scale_;
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
        const bool expanded = !sidebar_collapsed_ || SidebarPeekVisible(right);
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
    if (hasToggle && sidebar_collapsed_) {
        // Rail: the toggle stands in for the brand mark.
    } else if (ID2D1Bitmap* logo = LogoBitmap()) {
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
    const float brandRight = EffectiveSidebarWidth(right) - 12.0f * scale_;
    const float nameWidth = MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
        compositor_->HeaderFormat(), L"Pulse");
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
        DrawTextEndEllipsis(dc, compositor_->DwriteFactory(), compositor_->HeaderFormat(), brText_.get(),
                            vm.window_identity_name, text_x, 0.0f, identity.hit.right - 6.0f * scale_ - text_x, h);
    } else if (x + nameWidth <= brandRight) {
        MakeBrush(dc, theme.text, brText_);
        DrawTextRect(dc, compositor_->HeaderFormat(), brText_.get(), L"Pulse",
            x, 0.0f, nameWidth + 1.0f * scale_, h);
        x += nameWidth + 12.0f * scale_;
        const auto description = l10n::Get(l10n::StringId::AppDescription);
        const float descriptionWidth = MeasureLayoutText(compositor_, compositor_->DwriteFactory(),
            compositor_->SmallFormat(), description);
        if (x + descriptionWidth <= brandRight) {
            MakeBrush(dc, theme.text_secondary, brTextSecondary_);
            DrawTextRect(dc, compositor_->SmallFormat(), brTextSecondary_.get(), description,
                x, 0.0f, descriptionWidth + 1.0f * scale_, h);
        }
    }

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
    auto drawTab = [&](size_t i, float left, bool raised) {
        const bool active = vm.tabs[i].active;
        const bool pinned = vm.tabs[i].pinned;
        const float tabW = strip.Width(vm, i);
        const bool hovered = IsHovered(vm, HitTestResult::Tab, static_cast<int>(i)) ||
                             IsHovered(vm, HitTestResult::TabClose, static_cast<int>(i));
        const bool connect = active || raised;
        ChromeTabShape shape;
        shape.top_radius = theme.radius_control * scale_;
        // Concentric with the neighboring inactive tab: outer radius = inner + gap.
        shape.bottom_radius = shape.top_radius + control_gap_;
        shape.connect_bottom = connect;
        const float tabTop = tabY;
        // Use the same sheet edge for both opacity modes; overlap shifts the arc center.
        const float tabBottom = connect ? std::round(h)
                                        : (std::round(h) - control_gap_);
        const D2D1_RECT_F tabRc = D2D1::RectF(left, tabTop, left + tabW, tabBottom);
        if (raised) {
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
            // A solid light plate remains identifiable over pale chrome and wallpapers.
            MakeBrush(dc, vm.dark ? WithAlpha(theme.surface_sheet, sheet_alpha_) : HexColor(0xFFFFFF), brFillSelected_);
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
        if (vm.tabs[i].flash > 0.0f) {
            MakeBrush(dc, WithAlpha(theme.accent, (vm.dark ? 0.34f : 0.26f) * vm.tabs[i].flash), brFillHover_);
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
        }
        if (active) {
            MakeBrush(dc, vm.tabs[i].color_rgb ? HexColor(vm.tabs[i].color_rgb) : theme.accent, brAccent_);
            FillRoundedRect(dc, brAccent_.get(), tabRc.left + 15*scale_, tabRc.bottom - 3*scale_,
                std::min(28*scale_, tabW - 24*scale_), 2*scale_, scale_);
        }
        if ((pinned && !vm.show_pinned_tab_names) || (strip.compact && !pinned && !active)) {
            // Chrome pinned tab, or a crowded inactive tab: centered icon, no
            // title, no close button (the tooltip names it).
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
        MakeBrush(dc, theme.text, brText_);
        const bool show_close = TabCloseVisible(vm, static_cast<int>(i), tabW, scale_);
        const float closeSz = kTabCloseSizeDip * scale_;
        const float closePad = kTabClosePadDip * scale_;
        const float closeReserve = show_close ? (closePad + closeSz + 6.0f * scale_)
                                              : 6.0f * scale_;
        const float titleLeft = left + 24.0f * scale_ + markerReserve;
        DrawTabTitle(dc, compositor_->DwriteFactory(), compositor_->TabFormat(), brText_.get(),
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

    // A tab dragged from another window lands here (multi-window M6b): a bar
    // in the gap before the visible tab at the index, or after the last tab.
    if (vm.tab_drop_index >= 0) {
        float barX = strip.end_x;
        for (size_t i = static_cast<size_t>(vm.tab_drop_index); i < vm.tabs.size(); ++i) {
            if (vm.tabs[i].hidden) continue;
            const float extra = i < strip.extra.size() ? strip.extra[i] : 0.0f;
            barX = strip.x0 + (static_cast<float>(i) + vm.tabs[i].x_offset) * strip.pitch + extra -
                   (std::max)(0.0f, strip.pitch - strip.w) * 0.5f;
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
    if (strip.overflow) {
        for (const int direction : {-1, 1}) {
            const float ax = direction < 0 ? strip.view_left - strip.arrow_w : strip.view_right;
            const bool enabled = direction < 0 ? strip.scroll > 0.5f : strip.scroll < strip.max_scroll - 0.5f;
            const bool hot = enabled && IsHovered(vm, HitTestResult::TabScroll, direction);
            DrawButton(D2D1::RectF(ax, tabY, ax + strip.arrow_w, tabY + tabH), theme,
                       hot ? theme.fill_hover : kTransparent,
                       direction < 0 ? kIconChevronLeft : kIconChevronRight, direction < 0 ? L"<" : L">",
                       enabled ? theme.text_secondary : WithAlpha(theme.text_secondary, theme.text_secondary.a * 0.35f),
                       true, true, 0.7f);
        }
    }
    x = strip.new_x;
    // New tab button follows the final rest slot (not the sliding tabs); a
    // scrolling strip pins it to the right edge.
    D2D1_RECT_F newRc = D2D1::RectF(x, tabY, x + 32 * scale_, tabY + tabH);
    DrawButton(newRc, theme, IsHovered(vm, HitTestResult::TabNew) ? theme.fill_hover : kTransparent,
        kIconAdd, L"+", theme.text_secondary, true, true);

    } else {
        // Vertical tabs: the title bar hosts the address row; a hairline
        // separates it from the command row.
        FillRect(dc, brStrokeDivider_.get(), 0.0f, h - 1.0f, right, 1.0f);
    }
    // Settings + theme: one quiet pill with a hairline between the two, so
    // they read as app actions rather than two more caption buttons.
    const float groupTop = h * 0.5f - 18.0f * scale_;
    const float groupBottom = h * 0.5f + 18.0f * scale_;
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
    const D2D1_RECT_F settingsRc = D2D1::RectF(chrome.settings_left, buttonTop,
        chrome.settings_left + chrome.settings_w, buttonBottom);
    DrawButton(settingsRc, theme,
        IsHovered(vm, HitTestResult::SettingsButton) ? theme.fill_hover : kTransparent,
        kIconSettings, L"S", theme.text, true, true);

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
        dc->DrawTextLayout({t.body.left, t.body.top}, body.get(), brTextSecondary_.get(),
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

void MainRenderer::DrawStatusBar(const WindowViewModel& vm, const D2D1_RECT_F& rect, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    IDWriteTextFormat* small_fmt = compositor_->SmallFormat();
    const StatusBarMetrics sb = MakeStatusBarMetrics(
        vm, rect, scale_, status_height_, factory, small_fmt);
    const bool centered_progress = vm.status.query_active || vm.status.task_is_update;
    float y = sb.bar.top;
    // Sits on the shared sheet painted by Render(); no own fill or top rule.
    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    const float gap = 16.0f * scale_;
    const float statusLimit = centered_progress ? std::min(rect.right * 0.30f, sb.task.left - gap) : rect.right * 0.30f;
    const std::wstring status_text = path::FriendlyPathText(vm.status.status_text);
    const float statusWidth = std::min(std::max(0.0f, statusLimit - sb.pad),
        MeasureTextWidth(factory, small_fmt, status_text));
    DrawTextEndEllipsis(dc, factory, small_fmt, brTextSecondary_.get(), status_text,
        sb.pad, y, statusWidth, status_height_);
    const float selectionLeft = sb.pad + statusWidth + gap;
    const bool hasTask = vm.status.query_active || !vm.status.task_text.empty() || vm.status.task_progress >= 0.0f;
    const float selectionRight = hasTask ? sb.task.left - gap : rect.right - sb.right_reserved - gap;
    DrawTextEndEllipsis(dc, factory, small_fmt, brTextSecondary_.get(), vm.status.selection_text,
        selectionLeft, y, std::max(0.0f, selectionRight - selectionLeft), status_height_);

    const float rightReserved = sb.right_reserved;
    if (vm.status.query_cancellable) {
        DrawButton(sb.cancel_search, theme,
            IsHovered(vm, HitTestResult::StatusBarCancelSearch) ? theme.fill_hover : kTransparent,
            kIconCloseSmall, L"×", theme.text_secondary, true, true, 0.62f);
    }
    if (!centered_progress && !vm.status.performance_text.empty()) {
        const std::wstring& perfText = rect.right < 1100.0f * scale_
            ? vm.status.performance_compact_text : vm.status.performance_text;
        const float perfWidth = std::min(rect.right * 0.50f,
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
        DrawTextEndEllipsis(dc,factory,small_fmt,brAccentText_.get(),progress_text,
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
    const bool active = vm.status.task_active;
    // Edge-detect completion: running -> finished without error shows the check.
    if (active) {
        task_pill_was_active_ = true;
        task_pill_done_at_ = 0;
    } else if (task_pill_was_active_) {
        task_pill_was_active_ = false;
        task_pill_done_at_ = vm.status.task_failed ? 0 : now;
        task_pill_animate_ = motion::SystemAnimationsEnabled();
    }
    float strength = active ? 1.0f : 0.0f;   // capsule background opacity
    bool done = false;
    if (!active && task_pill_done_at_ != 0) {
        const ULONGLONG age = now - task_pill_done_at_;
        if (age < kTaskPillDoneMs) {
            done = true;
            strength = 1.0f;
        } else if (task_pill_animate_ && age < kTaskPillDoneMs + kTaskPillFadeMs) {
            done = true;
            strength = 1.0f - motion::EaseOutCubic(
                static_cast<float>(age - kTaskPillDoneMs) / static_cast<float>(kTaskPillFadeMs));
        } else {
            task_pill_done_at_ = 0;
        }
    }
    const bool indeterminate = active && vm.status.task_progress < 0.0f;
    task_pill_spinning_ = indeterminate;

    const std::wstring& text = vm.status.task_text;
    const float area_w = std::max(0.0f, area.right - area.left);
    const float cy = (area.top + area.bottom) * 0.5f;
    const float pill_h = std::min(area.bottom - area.top - 4.0f * scale_, 22.0f * scale_);
    // The capsule collapses with its fade, so the text slides home instead of jumping.
    const float ring_d = 12.0f * scale_ * (active ? 1.0f : done ? strength : 0.0f);
    const float pad_l = 7.0f * scale_ * strength;
    const float pad_r = 10.0f * scale_ * strength;
    const float gap = 6.0f * scale_ * (active ? 1.0f : done ? strength : 0.0f);
    const float text_w = text.empty() ? 0.0f : MeasureTextWidth(factory, fmt, text);
    const float pill_w = std::min(area_w, pad_l + ring_d + gap + text_w + pad_r + 1.0f);
    const D2D1_RECT_F pill = D2D1::RectF(area.right - pill_w, cy - pill_h * 0.5f,
                                         area.right, cy + pill_h * 0.5f);
    const bool hovered = IsHovered(vm, HitTestResult::StatusBarTask);

    if (strength > 0.0f) {
        D2D1_COLOR_F fill = done ? theme.accent : theme.text;
        fill.a = done ? (vm.dark ? 0.24f : 0.14f) * strength
                      : (hovered ? (vm.dark ? 0.12f : 0.08f) : (vm.dark ? 0.07f : 0.045f));
        ComPtr<ID2D1SolidColorBrush> fill_brush;
        dc->CreateSolidColorBrush(fill, &fill_brush);
        if (fill_brush.get())
            FillRoundedRect(dc, fill_brush.get(), pill.left, pill.top, pill_w, pill_h, pill_h * 0.5f);
    }
    float text_x = pill.left + pad_l;
    if (ring_d > 0.5f) {
        const D2D1_POINT_2F c = D2D1::Point2F(pill.left + pad_l + ring_d * 0.5f, cy);
        const float r = ring_d * 0.5f - 1.0f * scale_;
        const float stroke = 1.75f * scale_;
        if (done) {
            D2D1_COLOR_F accent = theme.accent;
            accent.a = std::max(strength, 0.0f);
            DrawIconText(c.x - ring_d * 0.5f - 2.0f * scale_, cy - ring_d * 0.5f - 2.0f * scale_,
                         ring_d + 4.0f * scale_, ring_d + 4.0f * scale_,
                         L"\xE73E", L"\u2713", accent, 0.82f);
        } else {
            D2D1_COLOR_F track = theme.text;
            track.a = vm.dark ? 0.20f : 0.16f;
            ComPtr<ID2D1SolidColorBrush> track_brush;
            dc->CreateSolidColorBrush(track, &track_brush);
            if (track_brush.get()) dc->DrawEllipse(D2D1::Ellipse(c, r, r), track_brush.get(), stroke);
            MakeBrush(dc, theme.accent, brAccent_);
            if (indeterminate) {
                const float spin = static_cast<float>(now % 1000) * 0.36f;
                DrawRingArc(dc, brAccent_.get(), c, r, spin, 90.0f, stroke);
            } else {
                const float value = std::clamp(vm.status.task_progress / 100.0f, 0.0f, 1.0f);
                DrawRingArc(dc, brAccent_.get(), c, r, 0.0f, std::max(value * 360.0f, 12.0f), stroke);
            }
        }
        text_x += ring_d + gap;
    }
    if (!text.empty()) {
        D2D1_COLOR_F color = theme.accent;
        if (vm.status.task_failed && !active) color = theme.text_secondary;
        MakeBrush(dc, color, brAccentText_);
        DrawTextEndEllipsis(dc, factory, fmt, brAccentText_.get(), text, text_x, area.top,
                            std::max(0.0f, area.right - text_x - pad_r + 1.0f), status_height_);
    }
}

MainRenderer::TabStripMetrics MainRenderer::ComputeTabStrip(
    const WindowViewModel& vm, float window_w) const {
    TabStripMetrics m;

    const TitleChrome chrome = MakeTitleChrome(window_w, scale_, title_bar_height_);
    m.x0 = EffectiveSidebarWidth(window_w) + margin_;
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
        m.compact = tab_freeze_compact_;
    } else if (visibleCount == 0 || normalW >= minW) {
        m.w = visibleCount == 0 ? 0.0f : std::min(kTabMaxW * scale_, normalW);
        m.active_w = m.w;
    } else {
        // Crowded: inactive tabs keep only their icon, the active one its title.
        const float activeW = m.active >= 0 ? kTabActiveMinW * scale_ : 0.0f;
        const size_t rest = visibleCount - (m.active >= 0 ? 1 : 0);
        float iconW = rest == 0 ? minW
            : (spare - (m.active >= 0 ? activeW + control_gap_ : 0.0f)) / static_cast<float>(rest) - control_gap_;
        iconW = std::min(iconW, minW);
        m.compact = true;
        m.overflow = iconW < kTabCompactW * scale_;
        m.w = m.overflow ? kTabCompactW * scale_ : iconW;
        m.active_w = m.active >= 0 ? activeW : m.w;
    }
    if (m.active < 0) m.active_w = m.w;
    m.y = 4.0f * scale_;
    m.h = title_bar_height_ - 8.0f * scale_;
    m.pitch = m.w + control_gap_;
    const float stripLeft = m.x0;
    if (m.overflow) {
        m.arrow_w = kTabScrollArrowW * scale_;
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

bool MainRenderer::ScrollTabStrip(const WindowViewModel& vm, float window_w, float x, float y,
                                  float steps) {
    if (vertical_tabs_ || y < 0.0f || y >= title_bar_height_) return false;
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
    if (!m.overflow || x < m.view_left - m.arrow_w || x >= m.view_right + m.arrow_w) return false;
    tab_scroll_ = std::clamp(tab_scroll_ - steps * 2.0f * m.pitch, 0.0f, m.max_scroll);
    return true;
}

void MainRenderer::ScrollTabStripPage(const WindowViewModel& vm, float window_w, int direction) {
    const TabStripMetrics m = ComputeTabStrip(vm, window_w);
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
    tab_freeze_compact_ = m.compact;
    tab_freeze_count_ = count;
}

bool MainRenderer::ReleaseTabWidths() {
    if (tab_freeze_w_ <= 0.0f) return false;
    tab_freeze_w_ = 0.0f;
    return true;
}

} // namespace pulse::ui
