// ui_column_strip.cpp — Column (Miller) view on one horizontally scrolling axis.
#include "ui_renderer.h"
#include "ui_renderer_internal.h"
#include "../common/localization.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <unordered_map>

namespace pulse::ui {

namespace {

constexpr float kGripDip = 4.0f;

bool IsAncestorId(int id) { return id >= 0 && id < kColumnStripChildId; }

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

// Resize handles on the right edge of each column: the ancestors, the
// current folder's list and the child column (unless it ends at the pane).
struct StripDivider {
    int id = -1;
    float x = 0.0f, top = 0.0f, bottom = 0.0f;
};

std::vector<StripDivider> StripDividers(const ColumnStripLayout& strip, const D2D1_RECT_F& pane,
                                        float top) {
    std::vector<StripDivider> out;
    const float bottom = std::max(top, pane.bottom);
    for (const ColumnStripSlot& slot : strip.columns)
        if (IsAncestorId(slot.id)) out.push_back({slot.id, slot.rect.right, top, bottom});
    if (strip.body.right > pane.left && strip.body.right < pane.right - 1.0f)
        out.push_back({kColumnStripBodyId, strip.body.right, top, bottom});
    for (const ColumnStripSlot& slot : strip.columns)
        if (slot.id == kColumnStripChildId && slot.rect.right < pane.right - 1.0f)
            out.push_back({slot.id, slot.rect.right, top, bottom});
    return out;
}

// Column view motion (animation phase 4a, pulse_motion_v2). Drawing only:
// hit-testing keeps the settled layout and scroll.
//  - the selected folder's contents cascade in: each row fades in and slides
//    8 dip from the left, 9 ms after the one above (capped at 10 rows);
//  - one hover plate glides between rows and columns (Plate spring);
//  - a column's highlight glides to the new row (Snappy spring);
//  - a column's auto scroll eases to its new place (Smooth spring).
// State per renderer and pane lives here; frames come from the renderer's
// frame pump (RequestMotionFrame), asked for only while something moves.
// Folder switches should not replay a row-by-row reveal. Column selection,
// hover plates and smooth scrolling retain their existing motion behavior.
constexpr bool kAnimateColumnCascade = false;
constexpr float kCascadeSlideDip = 8.0f, kCascadeRowMs = 160.0f, kCascadeStaggerMs = 9.0f;
constexpr int kCascadeRows = 10;
constexpr float kStripHoverFadeInMs = 90.0f, kStripHoverFadeOutMs = 120.0f;

struct StripColumnMotion {
    std::wstring path;
    uint64_t born = 0;        // rows of this path first drawn (cascade start)
    bool had_rows = false;
    bool scroll_init = false;
    float scroll_shown = 0.0f, scroll_vel = 0.0f;
    uint64_t scroll_last = 0;
    motion::SpringRectMotion highlight;
    bool seen = false;
};

struct StripPaneMotion {
    std::unordered_map<int, StripColumnMotion> columns;
    motion::SpringRectMotion hover;
    float hover_alpha = 0.0f;
    uint64_t hover_context = 0, hover_last = 0;
    int64_t hover_key = 0;
    D2D1_RECT_F hover_target{}, hover_rect{}, hover_clip{};
    bool animations = true;
    uint64_t animations_checked = 0;
    bool moving = false;
};

StripPaneMotion& StripMotionFor(const void* renderer, int pane_index) {
    static std::map<std::pair<const void*, int>, StripPaneMotion> motions;
    return motions[{renderer, pane_index}];
}

// caption: the column's title row, as tall as the list's column header so
// rows line up across every column.
D2D1_RECT_F StripCell(const D2D1_RECT_F& rc, int row, float scroll, float row_h, float scale, float caption) {
    const float top = rc.top + caption + static_cast<float>(row) * row_h - scroll;
    return D2D1::RectF(rc.left + 6.0f * scale, top + 1.0f * scale, rc.right - 6.0f * scale, top + row_h - 1.0f * scale);
}

// Eases the drawn scroll toward `target` (Smooth spring, 4 ms steps); a
// different folder or a jump of several screens just lands.
float StepStripScroll(StripColumnMotion& m, float target, float view_h, uint64_t now, bool animate, bool& moving) {
    if (!m.scroll_init || !animate || std::fabs(target - m.scroll_shown) > 3.0f * (std::max)(view_h, 1.0f)) {
        m.scroll_init = true;
        m.scroll_shown = target;
        m.scroll_vel = 0.0f;
        m.scroll_last = now;
        return target;
    }
    float dt = static_cast<float>((std::min<uint64_t>)(now - m.scroll_last, 64)) * 0.001f;
    m.scroll_last = now;
    const motion::SpringSpec s = motion::spring::Smooth;
    while (dt > 0.0f) {
        const float h = (std::min)(dt, 0.004f);
        m.scroll_vel += (s.k * (target - m.scroll_shown) - s.c * m.scroll_vel) * h;
        m.scroll_shown += m.scroll_vel * h;
        dt -= h;
    }
    if (std::fabs(target - m.scroll_shown) < 0.5f && std::fabs(m.scroll_vel) < 10.0f) {
        m.scroll_shown = target;
        m.scroll_vel = 0.0f;
    } else {
        moving = true;
    }
    return m.scroll_shown;
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
                             cs.scroll_left_dip);
}

D2D1_RECT_F MainRenderer::PaneBodyBounds(const PaneViewModel& vm,
                                         const D2D1_RECT_F& pane_bounds) const {
    if (!vm.column_strip.Active()) return pane_bounds;
    return ColumnStripGeometry(vm, pane_bounds).body;
}

float MainRenderer::ColumnStripViewHeight(const D2D1_RECT_F& column_rect) const {
    return std::max(0.0f, column_rect.bottom - column_rect.top - column_header_height_ - 6.0f * scale_);
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
    StripPaneMotion& pm = StripMotionFor(this, pane_index);
    const bool hc = IsHighContrast();
    if (motion_now_ - pm.animations_checked > 2000 || !pm.animations_checked) {
        pm.animations_checked = motion_now_ ? motion_now_ : 1;
        pm.animations = motion::SystemAnimationsEnabled();
        pm.hover.SetAnimationsEnabled(pm.animations);
    }
    const bool animate = pm.animations && !hc;
    pm.moving = false;
    // Per column: which folder it shows, when its rows came, and the scroll
    // it is drawn at. Columns that went away are forgotten.
    for (auto it = pm.columns.begin(); it != pm.columns.end();) {
        const bool present = std::any_of(strip.columns.begin(), strip.columns.end(),
                                         [&](const ColumnStripSlot& s) { return s.id == it->first; });
        it = present ? std::next(it) : pm.columns.erase(it);
    }
    const ColumnStripSlot* hover_slot = nullptr;
    int hover_row = -1;
    for (const ColumnStripSlot& slot : strip.columns) {
        const ColumnStripColumnView* column = StripColumn(cs, slot.id);
        if (!column) continue;
        StripColumnMotion& cm = pm.columns[slot.id];
        cm.highlight.SetAnimationsEnabled(animate);
        if (!cm.seen || cm.path != column->path) {
            // Another folder in this column: its rows cascade in once they
            // are there (child column), its scroll lands without easing.
            cm.seen = true;
            cm.path = column->path;
            cm.had_rows = false;
            cm.born = 0;
            cm.scroll_init = false;
        }
        const bool rows_now = StripRowCount(*column) > 0;
        if (rows_now && !cm.had_rows) {
            cm.had_rows = true;
            cm.born = (animate && kAnimateColumnCascade && slot.id == kColumnStripChildId)
                          ? motion_now_ : 0;
        }
        const float view_h = ColumnStripViewHeight(slot.rect);
        StepStripScroll(cm, ColumnStripScrollPx(*column, view_h), view_h, motion_now_, animate, pm.moving);
        if (vm.hover_region == static_cast<int>(HitTestResult::ColumnStripRow) &&
            vm.hover_pane_index == pane_index && vm.hover_sub_index == slot.id &&
            vm.hover_control_index >= 0 && static_cast<size_t>(vm.hover_control_index) < StripRowCount(*column)) {
            hover_slot = &slot;
            hover_row = vm.hover_control_index;
        }
    }
    {
        const float dt = static_cast<float>((std::min<uint64_t>)(motion_now_ - pm.hover_last, 64));
        pm.hover_last = motion_now_;
        if (hover_slot && !hc) {
            const StripColumnMotion& cm = pm.columns[hover_slot->id];
            if (pm.hover_alpha <= 0.0f) ++pm.hover_context;  // from nothing: appear in place
            pm.hover_key = static_cast<int64_t>(hover_slot->id) * 1000003 + hover_row;
            pm.hover_target = StripCell(hover_slot->rect, hover_row, cm.scroll_shown, row_height_, scale_, column_header_height_);
            pm.hover_rect = pm.hover.Update(pm.hover_context, pm.hover_key, pm.hover_target, motion_frame_,
                                            motion_now_, motion::spring::Plate);
            D2D1_RECT_F clip = hover_slot->rect;
            clip.top += column_header_height_;
            clip.bottom = clip.top + ColumnStripViewHeight(hover_slot->rect);
            if (IsAncestorId(hover_slot->id) && has_viewport) {
                clip.left = (std::max)(clip.left, strip.viewport.left);
                clip.right = (std::min)(clip.right, strip.viewport.right);
            }
            pm.hover_clip = clip;
            pm.hover_alpha = animate ? (std::min)(1.0f, pm.hover_alpha + dt / kStripHoverFadeInMs) : 1.0f;
        } else {
            pm.hover_alpha = animate ? (std::max)(0.0f, pm.hover_alpha - dt / kStripHoverFadeOutMs) : 0.0f;
            if (pm.hover_alpha > 0.0f)
                pm.hover_rect = pm.hover.Update(pm.hover_context, pm.hover_key, pm.hover_target, motion_frame_,
                                                motion_now_, motion::spring::Plate);
            else
                pm.hover.Hide();
        }
        if (pm.hover.Active(motion_now_) || (pm.hover_alpha > 0.0f && pm.hover_alpha < 1.0f)) pm.moving = true;
        if (pm.hover_alpha > 0.0f && !hc) {
            // Under the rows, in the column the pointer is over.
            D2D1_COLOR_F fill = theme.fill_hover;
            fill.a *= pm.hover_alpha;
            MakeBrush(dc, fill, brFillHover_);
            dc->PushAxisAlignedClip(pm.hover_clip, D2D1_ANTIALIAS_MODE_ALIASED);
            FillRoundedRect(dc, brFillHover_.get(), pm.hover_rect.left, pm.hover_rect.top,
                            pm.hover_rect.right - pm.hover_rect.left, pm.hover_rect.bottom - pm.hover_rect.top,
                            theme.radius_control * scale_);
            dc->PopAxisAlignedClip();
        }
    }
    for (const ColumnStripSlot& slot : strip.columns) {
        const bool ancestor = IsAncestorId(slot.id);
        if (ancestor) dc->PushAxisAlignedClip(strip.viewport, D2D1_ANTIALIAS_MODE_ALIASED);
        if (const ColumnStripColumnView* column = StripColumn(cs, slot.id)) {
            // A trailing child column keeps its full logical width for the
            // horizontal scroll model, but its rectangle can extend past the
            // pane's right edge.  Use the visible right edge for layout and
            // text measurement; otherwise ellipsis is calculated from pixels
            // that are immediately clipped by the pane.
            D2D1_RECT_F visible = slot.rect;
            visible.right = std::min(visible.right, strip.viewport.right);
            if (visible.right > visible.left)
                DrawColumnStripColumn(vm, pane, *column, slot.id, visible, pane_index, theme);
        }
        if (ancestor) dc->PopAxisAlignedClip();
    }
    for (const StripDivider& divider : StripDividers(strip, bounds, bounds.top + pane_header_height_)) {
        const float lx = divider.x;
        if (IsAncestorId(divider.id) && has_viewport &&
            (lx < strip.viewport.left || lx > strip.viewport.right + 0.5f)) {
            continue;
        }
        const bool hot = cs.resize_column == divider.id ||
            (vm.hover_region == static_cast<int>(HitTestResult::ColumnStripDivider) &&
             vm.hover_pane_index == pane_index && vm.hover_sub_index == divider.id);
        const float line_w = hot ? 2.0f * scale_ : std::max(1.0f, scale_);
        MakeBrush(dc, hot ? theme.accent : theme.stroke_divider, brAccent_);
        const float inset = hot ? 0.0f : 6.0f * scale_;
        dc->FillRectangle(D2D1::RectF(lx - line_w * 0.5f, divider.top + inset,
                                      lx + line_w * 0.5f, divider.bottom - inset),
                          brAccent_.get());
    }
    if (pm.moving) RequestMotionFrame();
}

// The regular list now shares the viewport, so draw this after the body list.
void MainRenderer::DrawColumnStripScroll(const WindowViewModel& vm, const PaneViewModel& pane,
                                         const D2D1_RECT_F& bounds, int pane_index,
                                         const Theme& theme) {
    ID2D1DeviceContext* dc = compositor_ ? compositor_->Dc() : nullptr;
    if (!dc || !pane.column_strip.Active()) return;
    const ColumnStripLayout strip = ColumnStripGeometry(pane, bounds);
    const ColumnStripView& cs = pane.column_strip;
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
    const float pad = 6.0f * scale_;
    const float caption = column_header_height_;
    const float list_top = rc.top + caption;
    const float view_h = ColumnStripViewHeight(rc);
    dc->PushAxisAlignedClip(rc, D2D1_ANTIALIAS_MODE_ALIASED);

    MakeBrush(dc, theme.text_secondary, brTextSecondary_);
    const std::wstring title = column.title.empty() ? LeafName(column.path) : column.title;
    DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(), brTextSecondary_.get(), title,
                        rc.left + pad + 6.0f * scale_, rc.top, rc.right - rc.left - 2.0f * pad - 12.0f * scale_,
                        caption);

    const size_t count = StripRowCount(column);
    if (count == 0) {
        std::wstring note;
        if (column.error) note = l10n::Get(l10n::StringId::CannotOpen);
        else if (!column.loading && column.snapshot) note = l10n::Get(l10n::StringId::FolderEmpty);
        if (!note.empty()) {
            DrawTextEndEllipsis(compositor_, compositor_->SmallFormat(), brTextSecondary_.get(), note,
                                rc.left + pad + 6.0f * scale_, list_top,
                                rc.right - rc.left - 2.0f * pad - 12.0f * scale_, row_height_);
        }
        dc->PopAxisAlignedClip();
        return;
    }

    StripPaneMotion& pm = StripMotionFor(this, pane_index);
    StripColumnMotion& cm = pm.columns[column_id];
    const bool hc = IsHighContrast();
    // Painting and hit testing share the same in-flight visual scroll offset.
    const float scroll = ColumnStripVisualScroll(ColumnStripScrollPx(column, view_h),
                                                 cm.scroll_shown, cm.scroll_init,
                                                 cm.path == column.path);
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
    // The highlight glides between rows of the same folder.
    const bool glide_highlight = !hc && column.highlight_row >= 0;
    if (glide_highlight) {
        const D2D1_RECT_F target = StripCell(rc, column.highlight_row, scroll, row_height_, scale_, column_header_height_);
        const D2D1_RECT_F plate = cm.highlight.Update(std::hash<std::wstring>{}(column.path), column.highlight_row,
                                                      target, motion_frame_, motion_now_, motion::spring::Snappy);
        if (cm.highlight.Active(motion_now_)) pm.moving = true;
        MakeBrush(dc, WithAlpha(theme.accent, 0.26f), brFillSelected_);
        FillRoundedRect(dc, brFillSelected_.get(), plate.left, plate.top, plate.right - plate.left,
                        plate.bottom - plate.top, theme.radius_control * scale_);
        MakeBrush(dc, theme.accent, brAccent_);
        const float bar_h = (plate.bottom - plate.top) * 0.5f;
        FillRoundedRect(dc, brAccent_.get(), plate.left + 1.0f * scale_,
                        plate.top + (plate.bottom - plate.top - bar_h) * 0.5f, 3.0f * scale_, bar_h, 1.5f * scale_);
    } else {
        cm.highlight.Hide();
    }
    const uint64_t cascade_age = cm.born ? motion_now_ - cm.born : ~uint64_t{0};
    if (cm.born && cascade_age < static_cast<uint64_t>(kCascadeRowMs + kCascadeStaggerMs * kCascadeRows) + 20)
        pm.moving = true;
    D2D1_MATRIX_3X2_F base_transform{};
    dc->GetTransform(&base_transform);
    for (int row = first; row <= last; ++row) {
        const int source = (*column.rows)[static_cast<size_t>(row)];
        if (source < 0 || static_cast<size_t>(source) >= entries.size()) continue;
        const fs::DirEntry& entry = entries[static_cast<size_t>(source)];
        const float top = list_top + static_cast<float>(row) * row_height_ - scroll;
        const bool directory = entry.is_dir || entry.link_target_is_dir;
        const bool empty_folder = directory && column.empty_sources.contains(source);
        const ColumnStripRowLayout row_layout = LayoutColumnStripRow(
            // Keep the filename column stable when folder-size probing later
            // discovers that a directory is empty. Every row reserves the
            // chevron slot so files and folders have the same text budget.
            rc, top, top + row_height_, scale_, true);
        const D2D1_RECT_F cell = row_layout.cell;
        const bool highlighted = row == column.highlight_row;
        // The pane's gliding plate draws the hover (high contrast: here).
        const bool hovered = hc && hover_col && vm.hover_control_index == row;
        // Cascade: fade in and slide from the left, row by row.
        float fade = 1.0f;
        if (cm.born) {
            const float t = static_cast<float>(cascade_age) -
                            kCascadeStaggerMs * static_cast<float>((std::min)(row - first, kCascadeRows));
            const float p = std::clamp(t / kCascadeRowMs, 0.0f, 1.0f);
            const float ease = 1.0f - (1.0f - p) * (1.0f - p) * (1.0f - p);
            fade = ease;
            if (fade <= 0.0f) continue;
            if (ease < 1.0f)
                dc->SetTransform(base_transform *
                                 D2D1::Matrix3x2F::Translation(-kCascadeSlideDip * scale_ * (1.0f - ease), 0.0f));
        }
        const bool own_highlight = highlighted && !glide_highlight;
        if (own_highlight || hovered) {
            // The navigation path is tinted with the accent so it reads at a
            // glance against wallpaper backgrounds.
            ComPtr<ID2D1SolidColorBrush>& br = own_highlight ? brFillSelected_ : brFillHover_;
            MakeBrush(dc, own_highlight ? WithAlpha(theme.accent, 0.26f) : theme.fill_hover, br);
            FillRoundedRect(dc, br.get(), cell.left, cell.top, cell.right - cell.left,
                            cell.bottom - cell.top, theme.radius_control * scale_);
            if (own_highlight) {
                MakeBrush(dc, theme.accent, brAccent_);
                const float bar_h = (cell.bottom - cell.top) * 0.5f;
                FillRoundedRect(dc, brAccent_.get(), cell.left + 1.0f * scale_,
                                cell.top + (cell.bottom - cell.top - bar_h) * 0.5f,
                                3.0f * scale_, bar_h, 1.5f * scale_);
            }
            if (own_highlight && hovered) {
                MakeBrush(dc, theme.fill_hover, brFillHover_);
                FillRoundedRect(dc, brFillHover_.get(), cell.left, cell.top, cell.right - cell.left,
                                cell.bottom - cell.top, theme.radius_control * scale_);
            }
        }
        const float ix = row_layout.icon.left;
        const float iy = row_layout.icon.top;
        const std::wstring full = JoinDirName(column.path, entry.name);
        const D2D1_RECT_F dest = D2D1::RectF(ix, iy, ix + icon, iy + icon);
        const float hidden_alpha = HiddenEntryAlpha(entry.attrs) * fade;
        // Nothing to open into: the empty-folder icon, and no chevron below.
        if (empty_folder) {
            DrawEmptyFolderIcon(ix, iy, icon, theme, hidden_alpha);
        } else if (!icon_cache_.Draw(dc, dest, full, entry.name, directory, entry.attrs, hidden_alpha)) {
            if (directory) DrawFolderIcon(ix, iy, icon, theme, hidden_alpha);
            else DrawFileIcon(ix, iy, icon, theme, hidden_alpha);
        }
        if (fs::ClassifyLink(entry.attrs, entry.reparse_tag) != fs::LinkKind::None ||
            (!entry.is_dir && IsShortcutName(entry.name)))
            DrawLinkOverlay(ix, iy, icon, theme, hidden_alpha);
        if (IsProtectedSystemEntry(entry.attrs)) DrawProtectedBadge(dc, dest, scale_, theme);
        const D2D1_COLOR_F name_color =
            IsHighContrast() ? theme.text : HiddenEntryNameColor(entry.attrs, theme, theme.text);
        MakeBrush(dc, WithAlpha(name_color, name_color.a * fade), brText_);
        // Column rectangles can land on different fractional pixels when the
        // same folder changes from the body column to an ancestor column.
        // Feeding that raw width to the two text backends makes a filename
        // occasionally gain or lose its last visible character.  Quantize the
        // text budget in logical pixels and keep a small, fixed safety margin;
        // the column itself remains at its exact user-selected width.
        const float logical_text_width = std::floor(
            std::max(0.0f, row_layout.text.right - row_layout.text.left) /
            std::max(0.5f, scale_)) - 1.0f;
        const float stable_text_width = std::max(0.0f, logical_text_width) *
                                        std::max(0.5f, scale_);
        DrawTextEndEllipsis(compositor_, compositor_->FileNameFormat(), brText_.get(), entry.name,
                            row_layout.text.left, top,
                            stable_text_width,
                            row_height_);
        if (directory && !empty_folder) {
            DrawIconText(row_layout.chevron.left, top,
                         row_layout.chevron.right - row_layout.chevron.left, row_height_,
                         kIconChevronRight, L">", WithAlpha(theme.text_secondary, theme.text_secondary.a * fade), 0.55f);
        }
        if (fade < 1.0f) dc->SetTransform(base_transform);
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
                                      int pane_index, float x, float y, HitTestResult& out) const {
    const ColumnStripLayout strip = ColumnStripGeometry(vm, pane_bounds);
    if (!strip.active) return false;
    const ColumnStripView& cs = vm.column_strip;
    const float grip = kGripDip * scale_;
    const bool has_viewport = strip.viewport.right > strip.viewport.left;
    auto in_viewport = [&](int id, float px) {
        return !IsAncestorId(id) || !has_viewport ||
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
    for (const StripDivider& divider : StripDividers(strip, pane_bounds, pane_bounds.top + pane_header_height_)) {
        if (y < divider.top || y >= divider.bottom) continue;
        const float lx = divider.x;
        if (!in_viewport(divider.id, lx) || (IsAncestorId(divider.id) && lx > strip.viewport.right + 0.5f))
            continue;
        if (std::fabs(x - lx) <= grip) {
            out.region = HitTestResult::ColumnStripDivider;
            out.sub_index = divider.id;
            out.index = -1;
            return true;
        }
    }
    for (const ColumnStripSlot& slot : strip.columns) {
        if (x < slot.rect.left || x >= slot.rect.right ||
            y < slot.rect.top || y >= slot.rect.bottom) continue;
        if (IsAncestorId(slot.id) && has_viewport &&
            (x < strip.viewport.left || x >= strip.viewport.right)) continue;
        out.region = HitTestResult::ColumnStripBlank;
        out.sub_index = slot.id;
        out.index = -1;
        const ColumnStripColumnView* column = StripColumn(cs, slot.id);
        if (!column) return true;
        const float list_top = slot.rect.top + column_header_height_;
        const float view_h = ColumnStripViewHeight(slot.rect);
        if (y < list_top || y >= list_top + view_h) return true;
        const float target_scroll = ColumnStripScrollPx(*column, view_h);
        const auto& motions = StripMotionFor(this, pane_index).columns;
        const auto motion = motions.find(slot.id);
        const float scroll = motion == motions.end() ? target_scroll :
            ColumnStripVisualScroll(target_scroll, motion->second.scroll_shown,
                                    motion->second.scroll_init, motion->second.path == column->path);
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
