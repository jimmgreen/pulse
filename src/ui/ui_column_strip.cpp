// ui_column_strip.cpp — Column (Miller) view: ancestor columns left of the
// regular list and the selected folder's contents to its right.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>

namespace pulse::ui {

namespace {

constexpr float kCaptionDip = 28.0f;
constexpr float kGripDip = 4.0f;

const ColumnStripColumnView* StripColumn(const ColumnStripView& cs, int id) {
    if (id == kColumnStripChildId) {
        return cs.has_child ? &cs.child : nullptr;
    }
    if (id < 0 || static_cast<size_t>(id) >= cs.ancestors.size()) return nullptr;
    return &cs.ancestors[static_cast<size_t>(id)];
}

size_t StripRowCount(const ColumnStripColumnView& column) {
    return column.rows ? column.rows->size() : 0;
}

float StripDividerX(const ColumnStripSlot& slot) {
    return slot.id == kColumnStripChildId ? slot.rect.left : slot.rect.right;
}

std::wstring LeafName(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash + 1 >= p.size()) return p;
    return p.substr(slash + 1);
}

} // namespace

ColumnStripLayout MainRenderer::ColumnStripGeometry(const PaneViewModel& vm,
                                                    const D2D1_RECT_F& pane_bounds) const {
    const ColumnStripView& cs = vm.column_strip;
    if (!cs.Active()) {
        ColumnStripLayout out;
        out.body = pane_bounds;
        return out;
    }
    return LayoutColumnStrip(pane_bounds, pane_bounds.top + pane_header_height_,
                             cs.ancestors.size(), cs.has_child, cs.widths_dip, scale_,
                             cs.scroll_from_right_dip);
}

D2D1_RECT_F MainRenderer::PaneBodyBounds(const PaneViewModel& vm,
                                         const D2D1_RECT_F& pane_bounds) const {
    if (!vm.column_strip.Active()) return pane_bounds;
    return ColumnStripGeometry(vm, pane_bounds).body;
}

float MainRenderer::ColumnStripViewHeight(const D2D1_RECT_F& column_rect) const {
    return std::max(0.0f, column_rect.bottom - column_rect.top - kCaptionDip * scale_ - 6.0f * scale_);
}

bool MainRenderer::ColumnStripHScrollRects(const ColumnStripLayout& layout, D2D1_RECT_F& track,
                                           D2D1_RECT_F& thumb) const {
    if (!layout.active || layout.max_scroll_px <= 0.0f || layout.content_px <= 0.0f) return false;
    const D2D1_RECT_F& vp = layout.viewport;
    track = D2D1::RectF(vp.left + 4.0f * scale_, vp.bottom - 12.0f * scale_,
                        vp.right - 4.0f * scale_, vp.bottom - 2.0f * scale_);
    const float track_w = track.right - track.left;
    if (track_w <= 0.0f) return false;
    const float view_w = vp.right - vp.left;
    const float thumb_w = std::clamp(track_w * view_w / layout.content_px, 32.0f * scale_, track_w);
    const float x = track.left + (track_w - thumb_w) * (layout.scroll_px / layout.max_scroll_px);
    thumb = D2D1::RectF(x, track.top, x + thumb_w, track.bottom);
    return true;
}

float MainRenderer::ColumnStripMaxScrollPx(const ColumnStripColumnView& column, float view_h) const {
    const float content = static_cast<float>(StripRowCount(column)) * row_height_;
    return std::max(0.0f, content - std::max(0.0f, view_h));
}

float MainRenderer::ColumnStripScrollPx(const ColumnStripColumnView& column, float view_h) const {
    const float max_scroll = ColumnStripMaxScrollPx(column, view_h);
    float scroll = column.scroll_dip * scale_;
    if (column.auto_scroll) {
        scroll = column.highlight_row >= 0
            ? static_cast<float>(column.highlight_row) * row_height_ - (view_h - row_height_) * 0.5f
            : 0.0f;
    }
    return std::clamp(scroll, 0.0f, max_scroll);
}

void MainRenderer::DrawColumnStrip(const WindowViewModel& vm, const PaneViewModel& pane,
                                   const D2D1_RECT_F& bounds, int pane_index, const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc) return;
    const ColumnStripLayout strip = ColumnStripGeometry(pane, bounds);
    if (!strip.active) return;
    const ColumnStripView& cs = pane.column_strip;
    const bool has_viewport = strip.viewport.right > strip.viewport.left;
    for (const ColumnStripSlot& slot : strip.columns) {
        const bool child = slot.id == kColumnStripChildId;
        if (!child) dc->PushAxisAlignedClip(strip.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        if (const ColumnStripColumnView* column = StripColumn(cs, slot.id)) {
            DrawColumnStripColumn(vm, pane, *column, slot.id, slot.rect, pane_index, theme);
        }
        if (!child) dc->PopAxisAlignedClip();
    }
    for (const ColumnStripSlot& slot : strip.columns) {
        const float lx = StripDividerX(slot);
        if (slot.id != kColumnStripChildId && has_viewport &&
            (lx < strip.viewport.left || lx > strip.viewport.right + 0.5f)) {
            continue;
        }
        const bool hot = cs.resize_column == slot.id ||
            (vm.hover_region == static_cast<int>(HitTestResult::ColumnStripDivider) &&
             vm.hover_pane_index == pane_index && vm.hover_sub_index == slot.id);
        const float line_w = hot ? 2.0f * scale_ : std::max(1.0f, scale_);
        MakeBrush(dc, hot ? theme.accent : theme.stroke_divider, brAccent_);
        const float inset = hot ? 0.0f : 6.0f * scale_;
        dc->FillRectangle(D2D1::RectF(lx - line_w * 0.5f, slot.rect.top + inset,
                                      lx + line_w * 0.5f, slot.rect.bottom - inset),
                          brAccent_.get());
    }
    D2D1_RECT_F track{}, thumb{};
    if (ColumnStripHScrollRects(strip, track, thumb)) {
        // Edge line where the scrolled strip meets the list.
        if (strip.scroll_px < strip.max_scroll_px - 0.5f) {
            MakeBrush(dc, theme.stroke_divider, brAccent_);
            const float lx = strip.viewport.right;
            dc->FillRectangle(D2D1::RectF(lx - 0.5f * scale_, strip.viewport.top + 6.0f * scale_,
                                          lx + 0.5f * scale_, strip.viewport.bottom - 6.0f * scale_),
                              brAccent_.get());
        }
        const bool hot = cs.hscroll_pressed ||
            (vm.hover_region == static_cast<int>(HitTestResult::ColumnStripHScroll) &&
             vm.hover_pane_index == pane_index);
        const float h = (hot ? theme.scrollbar_hover : theme.scrollbar_silent + 1.0f) * scale_;
        const float cy = (track.top + track.bottom) * 0.5f;
        MakeBrush(dc, theme.scrollbar_thumb, brScrollbar_);
        FillRoundedRect(dc, brScrollbar_.get(), thumb.left, cy - h * 0.5f,
                        thumb.right - thumb.left, h, h * 0.5f);
    }
}

void MainRenderer::DrawColumnStripColumn(const WindowViewModel& vm, const PaneViewModel& /*pane*/,
                                         const ColumnStripColumnView& column, int column_id,
                                         const D2D1_RECT_F& rc, int pane_index,
                                         const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_->Dc();
    IDWriteFactory2* factory = compositor_->DwriteFactory();
    const float pad = 6.0f * scale_;
    const float caption = kCaptionDip * scale_;
    const float list_top = rc.top + caption;
    const float view_h = ColumnStripViewHeight(rc);
    dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);

    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    const std::wstring title = column.title.empty() ? LeafName(column.path) : column.title;
    DrawTextEndEllipsis(dc, factory, compositor_->SmallFormat(), brTextSecondary_.get(), title,
                        rc.left + pad + 6.0f * scale_, rc.top, rc.right - rc.left - 2.0f * pad - 12.0f * scale_,
                        caption);

    const size_t count = StripRowCount(column);
    if (count == 0) {
        std::wstring note;
        if (column.error) note = l10n::Get(l10n::StringId::CannotOpen);
        else if (!column.loading && column.snapshot) note = l10n::Get(l10n::StringId::FolderEmpty);
        if (!note.empty()) {
            DrawTextEndEllipsis(dc, factory, compositor_->SmallFormat(), brTextSecondary_.get(), note,
                                rc.left + pad + 6.0f * scale_, list_top,
                                rc.right - rc.left - 2.0f * pad - 12.0f * scale_, row_height_);
        }
        dc->PopAxisAlignedClip();
        return;
    }

    const float scroll = ColumnStripScrollPx(column, view_h);
    const float max_scroll = ColumnStripMaxScrollPx(column, view_h);
    const D2D1_RECT_F list_rc = D2D1::RectF(rc.left, list_top, rc.right, list_top + view_h);
    dc->PushAxisAlignedClip(list_rc, D2D1_ANTIALIAS_MODE_ALIASED);
    const int first = std::max(0, static_cast<int>(std::floor(scroll / row_height_)));
    const int last = std::min(static_cast<int>(count) - 1,
                              static_cast<int>(std::ceil((scroll + view_h) / row_height_)));
    const float icon = 16.0f * scale_;
    const bool hover_col = vm.hover_region == static_cast<int>(HitTestResult::ColumnStripRow) &&
                           vm.hover_pane_index == pane_index && vm.hover_sub_index == column_id;
    const auto& entries = *column.snapshot;
    for (int row = first; row <= last; ++row) {
        const int source = (*column.rows)[static_cast<size_t>(row)];
        if (source < 0 || static_cast<size_t>(source) >= entries.size()) continue;
        const fs::DirEntry& entry = entries[static_cast<size_t>(source)];
        const float top = list_top + static_cast<float>(row) * row_height_ - scroll;
        const D2D1_RECT_F cell = D2D1::RectF(rc.left + pad, top + 1.0f * scale_,
                                             rc.right - pad, top + row_height_ - 1.0f * scale_);
        const bool highlighted = row == column.highlight_row;
        const bool hovered = hover_col && vm.hover_control_index == row;
        if (highlighted || hovered) {
            // The navigation path is tinted with the accent so it reads at a
            // glance against wallpaper backgrounds.
            ComPtr<ID2D1SolidColorBrush>& br = highlighted ? brFillSelected_ : brFillHover_;
            MakeBrush(dc, highlighted ? WithAlpha(theme.accent, 0.26f) : theme.fill_hover, br);
            FillRoundedRect(dc, br.get(), cell.left, cell.top, cell.right - cell.left,
                            cell.bottom - cell.top, theme.radius_control * scale_);
            if (highlighted) {
                MakeBrush(dc, theme.accent, brAccent_);
                const float bar_h = (cell.bottom - cell.top) * 0.5f;
                FillRoundedRect(dc, brAccent_.get(), cell.left + 1.0f * scale_,
                                cell.top + (cell.bottom - cell.top - bar_h) * 0.5f,
                                3.0f * scale_, bar_h, 1.5f * scale_);
            }
            if (highlighted && hovered) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), cell.left, cell.top, cell.right - cell.left,
                                cell.bottom - cell.top, theme.radius_control * scale_);
            }
        }
        const float ix = cell.left + 8.0f * scale_;
        const float iy = std::round(top + (row_height_ - icon) * 0.5f);
        const std::wstring full = JoinDirName(column.path, entry.name);
        const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + icon, iy + icon);
        if (!icon_cache_.Draw(dc, dest, full, entry.name, entry.is_dir, entry.attrs,
                              fs::IsLinkReparse(entry.reparse_kind))) {
            if (entry.is_dir) DrawFolderIcon(ix, iy, icon, theme);
            else DrawFileIcon(ix, iy, icon, theme);
        }
        const bool dim = (entry.attrs & FILE_ATTRIBUTE_HIDDEN) != 0;
        MakeBrush(dc, dim ? WithAlpha(theme.text, 0.55f) : theme.text, brText_);
        const float tx = ix + icon + 8.0f * scale_;
        const float chevron_w = entry.is_dir ? 18.0f * scale_ : 0.0f;
        DrawTextEndEllipsis(dc, factory, compositor_->FileNameFormat(), brText_.get(), entry.name,
                            tx, top, std::max(0.0f, cell.right - tx - chevron_w - 4.0f * scale_),
                            row_height_);
        if (entry.is_dir) {
            DrawIconText(cell.right - chevron_w - 2.0f * scale_, top, chevron_w, row_height_,
                         kIconChevronRight, L">", theme.text_secondary, 0.55f);
        }
    }
    dc->PopAxisAlignedClip();
    MakeBrush(dc, theme.text, brText_);

    if (max_scroll > 0.0f && view_h > 0.0f) {
        const float content = static_cast<float>(count) * row_height_;
        const float thumb_h = std::max(24.0f * scale_, view_h * view_h / content);
        const float thumb_y = list_top + (view_h - thumb_h) * (scroll / max_scroll);
        const float bar_w = theme.scrollbar_silent * scale_;
        MakeBrush(dc, theme.scrollbar_thumb, brScrollbar_);
        FillRoundedRect(dc, brScrollbar_.get(), rc.right - bar_w - 3.0f * scale_, thumb_y,
                        bar_w, thumb_h, bar_w * 0.5f);
    }
    dc->PopAxisAlignedClip();
}

bool MainRenderer::HitTestColumnStrip(const PaneViewModel& vm, const D2D1_RECT_F& pane_bounds,
                                      float x, float y, HitTestResult& out) const {
    const ColumnStripLayout strip = ColumnStripGeometry(vm, pane_bounds);
    if (!strip.active) return false;
    const ColumnStripView& cs = vm.column_strip;
    const float grip = kGripDip * scale_;
    const bool has_viewport = strip.viewport.right > strip.viewport.left;
    auto in_viewport = [&](const ColumnStripSlot& slot, float px) {
        return slot.id == kColumnStripChildId || !has_viewport ||
               (px >= strip.viewport.left && px < strip.viewport.right + grip);
    };
    D2D1_RECT_F track{}, thumb{};
    if (ColumnStripHScrollRects(strip, track, thumb) && x >= track.left && x < track.right &&
        y >= track.top - 2.0f * scale_ && y < strip.viewport.bottom) {
        out.region = HitTestResult::ColumnStripHScroll;
        out.index = x >= thumb.left && x < thumb.right ? 1 : 0; // 1 = on the thumb
        out.sub_index = -1;
        return true;
    }
    for (const ColumnStripSlot& slot : strip.columns) {
        if (y < slot.rect.top || y >= slot.rect.bottom) continue;
        const float lx = StripDividerX(slot);
        if (!in_viewport(slot, lx) || (slot.id != kColumnStripChildId && lx > strip.viewport.right + 0.5f))
            continue;
        if (std::fabs(x - lx) <= grip) {
            out.region = HitTestResult::ColumnStripDivider;
            out.sub_index = slot.id;
            out.index = -1;
            return true;
        }
    }
    for (const ColumnStripSlot& slot : strip.columns) {
        if (x < slot.rect.left || x >= slot.rect.right ||
            y < slot.rect.top || y >= slot.rect.bottom) continue;
        if (slot.id != kColumnStripChildId && has_viewport &&
            (x < strip.viewport.left || x >= strip.viewport.right)) continue;
        out.region = HitTestResult::ColumnStripBlank;
        out.sub_index = slot.id;
        out.index = -1;
        const ColumnStripColumnView* column = StripColumn(cs, slot.id);
        if (!column) return true;
        const float list_top = slot.rect.top + kCaptionDip * scale_;
        const float view_h = ColumnStripViewHeight(slot.rect);
        if (y < list_top || y >= list_top + view_h) return true;
        const float scroll = ColumnStripScrollPx(*column, view_h);
        const int row = static_cast<int>(std::floor((y - list_top + scroll) / row_height_));
        if (row >= 0 && static_cast<size_t>(row) < StripRowCount(*column)) {
            out.region = HitTestResult::ColumnStripRow;
            out.index = row;
        }
        return true;
    }
    return false;
}

} // namespace pulse::ui
