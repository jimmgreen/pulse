#include "view_layout.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {

namespace {
// Icon grid density (DIP). The thumbnail sets the column width, not the name.
constexpr float kIconGridMarginDip = 8.0f;    // left and right of the rows
constexpr float kIconGridTopDip = 4.0f;       // above the first row
constexpr float kIconGridBottomDip = 8.0f;    // below the last row
constexpr float kIconGridGapXDip = 6.0f;
constexpr float kIconGridGapYDip = 2.0f;
constexpr float kIconGridInsetDip = 4.0f;     // cell edge to thumbnail (sides, top)
constexpr float kIconGridMinCellDip = 90.0f;  // two lines of a camera file name fit
constexpr float kIconGridMaxFillDip = 16.0f;  // leftover a cell may absorb
constexpr float kIconGridNameGapDip = 4.0f;   // thumbnail to name
constexpr float kIconGridNameInsetDip = 3.0f; // cell edge to name
constexpr float kIconGridGroupTopDip = 4.0f;  // group header to its first icon row
constexpr float kGridGroupGapDip = 8.0f;      // grouped grids: below a group's last row
// inset + name gap + 2 below the name; the slot and name block come on top.
constexpr float kIconGridChromeDip = kIconGridInsetDip + kIconGridNameGapDip + 2.0f;

// Extra large as a zoom stop (DIP): its 240 device pixels, but kept above
// large icons where 240 px would be smaller (200 % scaling and up).
float IconZoomXlDip(float scale) noexcept {
    return std::max(240.0f / std::max(0.5f, scale), 120.0f);
}

// Thumbnail slot width; extra large stays 240 device pixels at every scale.
// A custom size (icon_dip > 0) replaces the preset.
float IconGridSlotWidth(ViewMode mode, float scale, float icon_dip = 0.0f) noexcept {
    if (icon_dip > 0.0f) return icon_dip * scale;
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return 240.0f;
    case ViewMode::LargeIcons: return 96.0f * scale;
    default: return 48.0f * scale;
    }
}

// Presets are square, extra large 4:3. A custom size is square up to large
// icons and eases to 4:3 at the extra large stop.
float IconGridSlotHeight(ViewMode mode, float scale, float icon_dip) noexcept {
    if (icon_dip > 0.0f) {
        const float t = std::clamp((icon_dip - 96.0f) / (IconZoomXlDip(scale) - 96.0f), 0.0f, 1.0f);
        return icon_dip * scale * (1.0f - 0.25f * t);
    }
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return 180.0f;
    case ViewMode::LargeIcons: return 96.0f * scale;
    default: return 48.0f * scale;
    }
}
} // namespace

float IconGridNameWidth(ViewMode mode, float scale, float icon_dip) noexcept {
    const float cell = std::max(IconGridSlotWidth(mode, scale, icon_dip) + 2.0f * kIconGridInsetDip * scale,
                                kIconGridMinCellDip * scale) + kIconGridMaxFillDip * scale;
    return cell - 2.0f * kIconGridNameInsetDip * scale;
}

int AutoIconNameLines(std::vector<int>& needed, bool has_folders) noexcept {
    if (needed.empty()) return kIconGridNameLines;
    std::sort(needed.begin(), needed.end());
    const auto at = [&](size_t percent) {
        return needed[std::min(needed.size() - 1, needed.size() * percent / 100)];
    };
    if (at(95) <= 1 && !has_folders) return 1;
    return std::clamp(at(75), 2, kIconGridMaxNameLines);
}

const wchar_t* ViewModeName(ViewMode mode) noexcept {
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return L"extra-large-icons";
    case ViewMode::LargeIcons: return L"large-icons";
    case ViewMode::MediumIcons: return L"medium-icons";
    case ViewMode::SmallIcons: return L"small-icons";
    case ViewMode::List: return L"list";
    case ViewMode::Details: return L"details";
    case ViewMode::Tiles: return L"tiles";
    case ViewMode::Content: return L"content";
    }
    return L"details";
}

ViewMode ParseViewMode(const std::wstring& value) noexcept {
    std::wstring lower = value;
    for (auto& c : lower) c = static_cast<wchar_t>(std::towlower(c));
    for (int i = 0; i < 8; ++i) {
        const ViewMode mode = ViewModeFromIndex(i);
        if (lower == ViewModeName(mode)) return mode;
    }
    return ViewMode::Details;
}

int ViewModeIndex(ViewMode mode) noexcept {
    const int value = static_cast<int>(mode);
    return value >= 0 && value < 8 ? value : 5;
}

ViewMode ViewModeFromIndex(int index) noexcept {
    return index >= 0 && index < 8 ? static_cast<ViewMode>(index) : ViewMode::Details;
}

bool ShowsColumnHeader(ViewMode mode) noexcept { return mode == ViewMode::Details; }

bool IsZoomIconMode(ViewMode mode) noexcept {
    return mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons ||
        mode == ViewMode::ExtraLargeIcons;
}

ViewMode ZoomViewMode(ViewMode current, ViewMode last_icon, int step) noexcept {
    static constexpr ViewMode kChain[] = {ViewMode::Details, ViewMode::MediumIcons,
        ViewMode::LargeIcons, ViewMode::ExtraLargeIcons};
    constexpr int kLast = static_cast<int>(std::size(kChain)) - 1;
    auto position = [](ViewMode mode) {
        for (int i = 0; i <= kLast; ++i) if (kChain[i] == mode) return i;
        return 1;
    };
    const ViewMode icon = IsZoomIconMode(last_icon) ? last_icon : ViewMode::MediumIcons;
    if (step == 0) return current;
    if (current == ViewMode::Details) {
        if (step < 0) return ViewMode::Details;
        return kChain[std::clamp(position(icon) + step - 1, 1, kLast)];
    }
    if (!IsZoomIconMode(current)) return step > 0 ? icon : ViewMode::Details;
    return kChain[std::clamp(position(current) + step, 0, kLast)];
}

float IconPresetDip(ViewMode mode, float scale) noexcept {
    switch (mode) {
    case ViewMode::ExtraLargeIcons: return 240.0f / std::max(0.5f, scale);
    case ViewMode::LargeIcons: return 96.0f;
    default: return 48.0f;
    }
}

float IconDip(ViewMode mode, float icon_dip, float scale) noexcept {
    return icon_dip > 0.0f ? icon_dip : IconPresetDip(mode, scale);
}

ViewMode IconModeForDip(float dip, float scale) noexcept {
    if (dip < std::sqrt(48.0f * 96.0f)) return ViewMode::MediumIcons;
    if (dip < std::sqrt(96.0f * IconZoomXlDip(scale))) return ViewMode::LargeIcons;
    return ViewMode::ExtraLargeIcons;
}

float NormalizeIconDip(ViewMode mode, float dip, float scale) noexcept {
    if (!IsZoomIconMode(mode) || !(dip > 0.0f)) return 0.0f;
    if (std::fabs(dip - IconPresetDip(mode, scale)) < 0.5f) return 0.0f;
    return std::round(std::clamp(dip, kMinIconDip, std::max(kMaxIconDip, IconZoomXlDip(scale))));
}

float ZoomIconDip(float dip, int step, float scale) noexcept {
    const float top = std::max(kMaxIconDip, IconZoomXlDip(scale));
    dip = std::clamp(dip, kMinIconDip, top);
    if (step == 0) return dip;
    if (step < 0 && dip <= kMinIconDip + 0.5f) return 0.0f;
    float next = dip * std::exp2(0.25f * static_cast<float>(step));
    // Stop on the first preset the step reaches or passes (within 3 %).
    const float stops[] = {48.0f, 96.0f, IconZoomXlDip(scale)};
    if (step > 0) {
        for (const float stop : stops) {
            if (dip < stop - 0.5f && next >= stop * 0.97f) { next = stop; break; }
        }
    } else {
        for (int i = 2; i >= 0; --i) {
            if (dip > stops[i] + 0.5f && next <= stops[i] * 1.03f) { next = stops[i]; break; }
        }
    }
    return std::round(std::clamp(next, kMinIconDip, top));
}

bool ShowsFolderSize(ViewMode mode) noexcept {
    return mode == ViewMode::Details || mode == ViewMode::Tiles || mode == ViewMode::Content ||
        mode == ViewMode::MediumIcons || mode == ViewMode::LargeIcons ||
        mode == ViewMode::ExtraLargeIcons;
}

bool UsesThumbnails(ViewMode mode) noexcept {
    return mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons ||
           mode == ViewMode::MediumIcons || mode == ViewMode::Tiles ||
           mode == ViewMode::Content;
}

D2D1_RECT_F DetailsContentRect(D2D1_RECT_F bounds, float scale) noexcept {
    const float unit = std::max(0.5f, scale);
    const float width = std::max(0.0f, bounds.right - bounds.left);
    bounds.left += std::min(8.0f * unit, width * 0.25f);
    bounds.right = std::max(bounds.left, bounds.right - std::min(22.0f * unit, width * 0.5f));
    return bounds;
}

int WallRows::RowOf(int view_index) const noexcept {
    if (row_first.size() < 2 || view_index < 0 || view_index >= row_first.back()) return -1;
    const auto it = std::upper_bound(row_first.begin(), row_first.end() - 1, view_index);
    return static_cast<int>(it - row_first.begin()) - 1;
}

int WallRows::RowAtY(float y) const noexcept {
    if (row_top.empty() || y < row_top.front()) return -1;
    const auto it = std::upper_bound(row_top.begin(), row_top.end(), y);
    return static_cast<int>(it - row_top.begin()) - 1;
}

WallRows BuildWallRows(const std::vector<float>& aspects, float width, float row_px, float scale, const std::atomic<uint64_t>* generation, uint64_t expected) {
    WallRows out;
    const size_t n = aspects.size();
    out.x.resize(n);
    out.w.resize(n);
    const float pad = kIconGridMarginDip * scale;
    const float gap = kIconGridGapXDip * scale;
    const float avail = std::max(1.0f, width - 2.0f * pad);
    const float h0 = std::max(1.0f, row_px);
    float y = kIconGridTopDip * scale;
    size_t first = 0;
    float sum = 0.0f;
    auto aspect = [&](size_t i) {
        // Unknown yet: 4:3. Panoramas and slivers are capped so no row turns
        // into a thin strip or a lone column.
        return aspects[i] > 0.0f ? std::clamp(aspects[i], 0.2f, 5.0f) : 4.0f / 3.0f;
    };
    auto flush = [&](size_t end, bool last) {
        if (end <= first) return;
        const float gaps = static_cast<float>(end - first - 1) * gap;
        float h = std::max(1.0f, avail - gaps) / std::max(0.01f, sum);
        if (last && h > h0 * 1.25f) h = h0;
        float x = pad;
        for (size_t i = first; i < end; ++i) {
            out.x[i] = x;
            out.w[i] = aspect(i) * h;
            x += out.w[i] + gap;
        }
        out.row_first.push_back(static_cast<int>(first));
        out.row_top.push_back(y);
        out.row_h.push_back(h);
        y += h + gap;
        first = end;
        sum = 0.0f;
    };
    for (size_t i = 0; i < n; ++i) {
        if ((i & 255) == 0 && generation && generation->load() != expected) return {};
        sum += aspect(i);
        if (sum * h0 + static_cast<float>(i - first) * gap >= avail) flush(i + 1, false);
    }
    flush(n, true);
    out.row_first.push_back(static_cast<int>(n));
    out.content_height = out.row_top.empty() ? 0.0f : y - gap + kIconGridBottomDip * scale;
    return out;
}

ViewLayout::ViewLayout(ViewMode mode, D2D1_RECT_F viewport, size_t item_count,
                       float scroll_x, float scroll_y, float scale,
                       ViewLayoutParams params, const ListGroups* groups)
    : mode_(mode), viewport_(viewport), count_(item_count),
      scroll_x_(std::max(0.0f, scroll_x)), scroll_y_(std::max(0.0f, scroll_y)),
      scale_(std::max(0.5f, scale)) {
    const float row_height_dip = params.row_height_dip;
    const float icon_dip = IsZoomIconMode(mode_) ? params.icon_dip : 0.0f;
    if (mode_ == ViewMode::Details) viewport_ = DetailsContentRect(viewport_, scale_);
    const float width = std::max(1.0f, viewport_.right - viewport_.left);
    const float height = std::max(1.0f, viewport_.bottom - viewport_.top);
    auto grid = [&](float cell_w, float cell_h, float icon, bool fixed_pixel_tier = false,
                    float slot_h = 0.0f) {
        const float factor = fixed_pixel_tier ? 1.0f : scale_;
        metrics_.cell_width = std::min(width, cell_w * factor);
        metrics_.cell_height = cell_h * factor;
        metrics_.icon_size = icon * factor;
        metrics_.slot_height = slot_h > 0.0f ? slot_h * factor : metrics_.icon_size;
        metrics_.columns = std::max(1, static_cast<int>(width / metrics_.cell_width));
        // Every grid spreads the row's leftover over its columns: a fixed
        // pixel tier keeps its artwork size, not a blank strip on the right.
        metrics_.cell_width = width / static_cast<float>(metrics_.columns);
        const size_t rows = (count_ + static_cast<size_t>(metrics_.columns) - 1) /
                            static_cast<size_t>(metrics_.columns);
        content_width_ = width;
        content_height_ = static_cast<float>(rows) * metrics_.cell_height;
    };
    // Icon grids: a cell is max(slot + 8, 90) DIP wide, 6 DIP apart, rows 2 DIP
    // apart. A row's leftover widens each cell by up to 16 DIP; the rest centres
    // the row instead of spreading blank space between the icons.
    auto icon_grid = [&](float slot_w, float slot_h) {
        const float pad = kIconGridMarginDip * scale_;
        const float gap_x = kIconGridGapXDip * scale_;
        const float gap_y = kIconGridGapYDip * scale_;
        const float avail = std::max(1.0f, width - 2.0f * pad);
        const float cell0 = std::min(avail, std::max(slot_w + 2.0f * kIconGridInsetDip * scale_,
                                                     kIconGridMinCellDip * scale_));
        const int cols = std::max(1, static_cast<int>((avail + gap_x) / (cell0 + gap_x)));
        const float n = static_cast<float>(cols);
        const float spare = std::max(0.0f, avail - n * cell0 - (n - 1.0f) * gap_x);
        const float cell_w = cell0 + std::min(kIconGridMaxFillDip * scale_, spare / n);
        const float used = n * cell_w + (n - 1.0f) * gap_x;
        metrics_.name_lines = row_height_dip > 0.0f
            ? std::clamp(static_cast<int>(std::lround((row_height_dip - 2.0f) / kIconGridNameLineDip)),
                         1, kIconGridMaxNameLines)
            : kIconGridNameLines;
        metrics_.cell_width = cell_w;
        metrics_.cell_height = slot_h + (kIconGridChromeDip + IconGridNameBlockDip(metrics_.name_lines)) * scale_;
        metrics_.icon_size = slot_w;
        metrics_.slot_height = slot_h;
        metrics_.columns = cols;
        metrics_.pitch_x = cell_w + gap_x;
        metrics_.pitch_y = metrics_.cell_height + gap_y;
        metrics_.origin_x = pad + std::max(0.0f, (avail - used) * 0.5f);
        metrics_.origin_y = kIconGridTopDip * scale_;
        const size_t rows = (count_ + static_cast<size_t>(cols) - 1) / static_cast<size_t>(cols);
        content_width_ = width;
        content_height_ = rows == 0 ? 0.0f
            : metrics_.origin_y + static_cast<float>(rows) * metrics_.pitch_y - gap_y +
              kIconGridBottomDip * scale_;
    };
    switch (mode_) {
    // Extra large thumbnails stay 240 device pixels wide; their slot is 4:3.
    case ViewMode::ExtraLargeIcons:
    case ViewMode::LargeIcons:
    case ViewMode::MediumIcons:
        icon_grid(IconGridSlotWidth(mode_, scale_, icon_dip), IconGridSlotHeight(mode_, scale_, icon_dip));
        break;
    case ViewMode::SmallIcons: grid(144.0f, 28.0f, 16.0f); break;
    case ViewMode::Tiles: grid(280.0f, 72.0f, 48.0f); break;
    case ViewMode::List: {
        metrics_.cell_width = 220.0f * scale_;
        metrics_.cell_height = 24.0f * scale_;
        metrics_.icon_size = 16.0f * scale_;
        metrics_.column_major = true;
        metrics_.rows_per_column = std::max(1, static_cast<int>(height / metrics_.cell_height));
        const size_t cols = (count_ + static_cast<size_t>(metrics_.rows_per_column) - 1) /
                            static_cast<size_t>(metrics_.rows_per_column);
        metrics_.columns = static_cast<int>(std::max<size_t>(1, cols));
        content_width_ = static_cast<float>(cols) * metrics_.cell_width;
        content_height_ = height;
        break;
    }
    case ViewMode::Details:
        metrics_.cell_width = width;
        metrics_.cell_height = (row_height_dip > 0.0f ? row_height_dip : 28.0f) * scale_;
        metrics_.icon_size = 16.0f * scale_;
        content_width_ = width;
        content_height_ = static_cast<float>(count_) * metrics_.cell_height;
        break;
    case ViewMode::Content:
        metrics_.cell_width = width;
        metrics_.cell_height = 76.0f * scale_;
        metrics_.icon_size = 32.0f * scale_;
        content_width_ = width;
        content_height_ = static_cast<float>(count_) * metrics_.cell_height;
        break;
    }
    if (metrics_.pitch_x <= 0.0f) metrics_.pitch_x = metrics_.cell_width;
    if (metrics_.pitch_y <= 0.0f) metrics_.pitch_y = metrics_.cell_height;
    if (params.wall && IsZoomIconMode(mode_) && params.wall->row_first.size() >= 2 &&
        params.wall->row_first.back() == static_cast<int>(count_) && params.wall->x.size() == count_) {
        wall_ = params.wall;
        content_height_ = wall_->content_height;
    }
    if (groups && !groups->empty() && count_ > 0 && ShowsGroups(mode_) && !wall_) {
        // Each group: its header, then its own rows of `columns` cells (one
        // cell per row in Details / Content). Single-column views keep rows
        // flush under the header, as before icon grids could group.
        groups_ = groups;
        header_h_ = kGroupHeaderDip * scale_;
        const bool zoom_grid = mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
                               mode_ == ViewMode::MediumIcons;
        const bool single = metrics_.columns <= 1 && (mode_ == ViewMode::Details || mode_ == ViewMode::Content);
        band_top_ = single ? 0.0f : (zoom_grid ? kIconGridGroupTopDip : kGridGroupGapDip) * scale_;
        band_bottom_ = single ? 0.0f : kGridGroupGapDip * scale_;
        const int cols = std::max(1, metrics_.columns);
        group_top_.reserve(groups->size());
        float y = 0.0f;
        for (const ListGroup& g : *groups) {
            group_top_.push_back(y);
            y += header_h_;
            if (!g.collapsed && g.count > 0) {
                const int rows = (g.count + cols - 1) / cols;
                y += band_top_ + static_cast<float>(rows - 1) * metrics_.pitch_y + metrics_.cell_height +
                     band_bottom_;
                visible_rows_ += g.count;
            }
        }
        content_height_ = y + (zoom_grid ? kIconGridBottomDip * scale_ : 0.0f);
    }
}

int ViewLayout::GroupCount() const noexcept {
    return groups_ ? static_cast<int>(groups_->size()) : 0;
}

int ViewLayout::GroupOfIndex(int index) const noexcept {
    if (!groups_) return -1;
    const auto it = std::upper_bound(groups_->begin(), groups_->end(), index,
        [](int v, const ListGroup& g) { return v < g.first; });
    return it == groups_->begin() ? 0 : static_cast<int>(it - groups_->begin()) - 1;
}

int ViewLayout::GroupAtY(float local_y) const noexcept {
    if (!groups_) return -1;
    const auto it = std::upper_bound(group_top_.begin(), group_top_.end(), local_y);
    return it == group_top_.begin() ? 0 : static_cast<int>(it - group_top_.begin()) - 1;
}

int ViewLayout::GroupAtTop() const noexcept { return GroupAtY(scroll_y_); }

D2D1_RECT_F ViewLayout::HeaderRect(int group) const noexcept {
    if (!groups_ || group < 0 || group >= GroupCount()) return {};
    const float top = viewport_.top + group_top_[static_cast<size_t>(group)] - scroll_y_;
    return D2D1_RECT_F{viewport_.left, top, viewport_.right, top + header_h_};
}

int ViewLayout::StickyHeader(D2D1_RECT_F* out) const noexcept {
    if (!groups_) return -1;
    const int g = GroupAtTop();
    if (g < 0 || group_top_[static_cast<size_t>(g)] >= scroll_y_) return -1;
    float top = viewport_.top;
    if (g + 1 < GroupCount()) top = std::min(top, HeaderRect(g + 1).top - header_h_);
    if (out) *out = D2D1_RECT_F{viewport_.left, top, viewport_.right, top + header_h_};
    return g;
}

int ViewLayout::HeaderHitTest(float x, float y) const noexcept {
    if (!groups_ || x < viewport_.left || x >= viewport_.right || y < viewport_.top ||
        y >= viewport_.bottom) return -1;
    const float local_y = y - viewport_.top + scroll_y_;
    const int g = GroupAtY(local_y);
    if (g < 0) return -1;
    return local_y - group_top_[static_cast<size_t>(g)] < header_h_ ? g : -1;
}

// Position among rows that are not hidden by a collapsed group; a hidden row
// maps to the slot where its group would start.
int ViewLayout::VisibleOrdinal(int index) const noexcept {
    int hidden = 0;
    for (const ListGroup& g : *groups_) {
        if (index < g.first + g.count) {
            if (g.collapsed) return g.first - hidden;
            return index - hidden;
        }
        if (g.collapsed) hidden += g.count;
    }
    return index - hidden;
}

int ViewLayout::IndexFromOrdinal(int ordinal) const noexcept {
    int seen = 0;
    for (const ListGroup& g : *groups_) {
        if (g.collapsed) continue;
        if (ordinal < seen + g.count) return g.first + (ordinal - seen);
        seen += g.count;
    }
    return -1;
}

// First (last = false) or last item whose row band can meet content-space y.
// In a header: the group's first item, or the previous group's last one.
int ViewLayout::GroupedIndexAt(float local_y, bool last) const noexcept {
    const int g = GroupAtY(local_y);
    const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
    const float off = local_y - group_top_[static_cast<size_t>(g)] - header_h_ - band_top_;
    if (grp.collapsed) return last ? grp.first + grp.count - 1 : grp.first;
    if (off < 0.0f || metrics_.pitch_y <= 0.0f) return last ? std::max(0, grp.first - 1) : grp.first;
    const int cols = std::max(1, metrics_.columns);
    const int row = static_cast<int>(off / metrics_.pitch_y);
    const int k = last ? row * cols + cols - 1 : row * cols;
    return grp.first + std::min(grp.count - 1, k);
}

float ViewLayout::MaxScrollX() const noexcept {
    return std::max(0.0f, content_width_ - (viewport_.right - viewport_.left));
}

float ViewLayout::MaxScrollY() const noexcept {
    return std::max(0.0f, content_height_ - (viewport_.bottom - viewport_.top));
}

D2D1_RECT_F ViewLayout::ItemRect(int index) const noexcept {
    if (index < 0 || static_cast<size_t>(index) >= count_) return {};
    if (wall_) {
        const size_t row = static_cast<size_t>(std::max(0, wall_->RowOf(index)));
        const float left = viewport_.left + wall_->x[static_cast<size_t>(index)] - scroll_x_;
        const float top = viewport_.top + wall_->row_top[row] - scroll_y_;
        return D2D1_RECT_F{left, top, left + wall_->w[static_cast<size_t>(index)], top + wall_->row_h[row]};
    }
    if (groups_) {
        // Rows of a collapsed group get a zero-height rect under its header.
        const int g = GroupOfIndex(index);
        const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
        const float base = viewport_.top + group_top_[static_cast<size_t>(g)] + header_h_ - scroll_y_;
        const float left0 = viewport_.left + metrics_.origin_x - scroll_x_;
        if (grp.collapsed) return D2D1_RECT_F{left0, base, left0 + metrics_.cell_width, base};
        const int cols = std::max(1, metrics_.columns);
        const int k = index - grp.first;
        const float left = left0 + static_cast<float>(k % cols) * metrics_.pitch_x;
        const float top = base + band_top_ + static_cast<float>(k / cols) * metrics_.pitch_y;
        return D2D1_RECT_F{left, top, left + metrics_.cell_width, top + metrics_.cell_height};
    }
    int row = 0;
    int col = 0;
    if (metrics_.column_major) {
        col = index / metrics_.rows_per_column;
        row = index % metrics_.rows_per_column;
    } else {
        col = index % metrics_.columns;
        row = index / metrics_.columns;
    }
    const float left = viewport_.left + metrics_.origin_x + col * metrics_.pitch_x - scroll_x_;
    const float top = viewport_.top + metrics_.origin_y + row * metrics_.pitch_y - scroll_y_;
    return D2D1_RECT_F{left, top, left + metrics_.cell_width, top + metrics_.cell_height};
}

D2D1_RECT_F ViewLayout::IconRect(int index) const noexcept {
    const D2D1_RECT_F cell = ItemRect(index);
    if (wall_) {
        // Folders and documents on the wall: the icon in the upper part, the
        // name in a strip along the bottom.
        const float w = cell.right - cell.left;
        const float h = cell.bottom - cell.top;
        const float side = std::max(0.0f, std::min(w * 0.8f, h - h * 0.06f - 24.0f * scale_));
        const float left = cell.left + (w - side) * 0.5f;
        return {left, cell.top + h * 0.06f, left + side, cell.top + h * 0.06f + side};
    }
    const float icon = metrics_.icon_size;
    const float pad = 8.0f * scale_;
    if (mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
        mode_ == ViewMode::MediumIcons) {
        const D2D1_RECT_F slot = ThumbnailRect(index);
        const float side = std::min(slot.right - slot.left, slot.bottom - slot.top);
        const float left = cell.left + (cell.right - cell.left - side) * 0.5f;
        return {left, slot.bottom - side, left + side, slot.bottom};
    }
    const float top = cell.top + (cell.bottom - cell.top - icon) * 0.5f;
    return {cell.left + pad, top, cell.left + pad + icon, top + icon};
}

D2D1_RECT_F ViewLayout::ThumbnailRect(int index) const noexcept {
    if (wall_) return ItemRect(index);  // the picture is the whole tile
    if (mode_ != ViewMode::ExtraLargeIcons && mode_ != ViewMode::LargeIcons &&
        mode_ != ViewMode::MediumIcons)
        return IconRect(index);
    const D2D1_RECT_F cell = ItemRect(index);
    const float pad = kIconGridInsetDip * scale_;
    const float available = std::max(16.0f, cell.right - cell.left - pad * 2.0f);
    const float slot_w = std::min(metrics_.icon_size, available);
    // A narrow pane shrinks the slot, keeping its shape.
    const float slot_h = metrics_.icon_size > 0.0f
        ? metrics_.slot_height * slot_w / metrics_.icon_size : slot_w;
    const float left = cell.left + (cell.right - cell.left - slot_w) * 0.5f;
    return {left, cell.top + pad, left + slot_w, cell.top + pad + slot_h};
}

D2D1_RECT_F ViewLayout::NameRect(int index) const noexcept {
    D2D1_RECT_F cell = ItemRect(index);
    if (wall_) {
        // One line along the tile's bottom (names show there on hover).
        return {cell.left + 2.0f * scale_, std::max(cell.top, cell.bottom - 19.0f * scale_),
                cell.right - 2.0f * scale_, cell.bottom - 2.0f * scale_};
    }
    const float pad = 8.0f * scale_;
    const float icon = metrics_.icon_size;
    if (mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
        mode_ == ViewMode::MediumIcons) {
        const D2D1_RECT_F slot = ThumbnailRect(index);
        const float top = std::min(cell.bottom - 4.0f * scale_,
                                   slot.bottom + kIconGridNameGapDip * scale_);
        const float lines_h = IconGridNameBlockDip(metrics_.name_lines) * scale_;
        const float inset = kIconGridNameInsetDip * scale_;
        return {cell.left + inset, top, cell.right - inset, top + lines_h};
    }
    if (mode_ == ViewMode::Tiles || mode_ == ViewMode::Content) {
        const float left = cell.left + pad + icon + 10.0f * scale_;
        return {left, cell.top + 7.0f * scale_, cell.right - pad,
                cell.top + 31.0f * scale_};
    }
    const float left = cell.left + pad + icon + 8.0f * scale_;
    return {left, cell.top + 1.0f * scale_, cell.right - pad,
            cell.bottom - 1.0f * scale_};
}

D2D1_RECT_F ViewLayout::FolderSizeRect(int index) const noexcept {
    if (!ShowsFolderSize(mode_) || wall_) return {};
    const auto name = NameRect(index);
    // Icon grids: a folder with a size keeps its name to the lines above it and
    // the size takes the last line's slot, which ends inside the cell.
    const bool grid = mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
                      mode_ == ViewMode::MediumIcons;
    if (grid) {
        const float above = static_cast<float>(std::max(1, metrics_.name_lines - 1));
        return {name.left, name.top + above * kIconGridNameLineDip * scale_, name.right, name.bottom};
    }
    const float top = name.top + 25.0f * scale_;
    return {name.left, top, name.right, top + 22.0f * scale_};
}

int ViewLayout::HitTest(float x, float y) const noexcept {
    if (x < viewport_.left || x >= viewport_.right || y < viewport_.top || y >= viewport_.bottom)
        return -1;
    const float local_x = x - viewport_.left + scroll_x_;
    const float local_y = y - viewport_.top + scroll_y_;
    if (wall_) {
        const int row = wall_->RowAtY(local_y);
        if (row < 0 || local_y >= wall_->row_top[static_cast<size_t>(row)] + wall_->row_h[static_cast<size_t>(row)])
            return -1;
        const auto begin = wall_->x.begin() + wall_->row_first[static_cast<size_t>(row)];
        const auto end = wall_->x.begin() + wall_->row_first[static_cast<size_t>(row) + 1];
        const auto it = std::upper_bound(begin, end, local_x);
        if (it == begin) return -1;
        const int index = static_cast<int>(it - wall_->x.begin()) - 1;
        return local_x < wall_->x[static_cast<size_t>(index)] + wall_->w[static_cast<size_t>(index)] ? index : -1;
    }
    if (groups_) {
        const int g = GroupAtY(local_y);
        const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
        const float off = local_y - group_top_[static_cast<size_t>(g)] - header_h_ - band_top_;
        if (off < 0.0f || grp.collapsed || metrics_.pitch_y <= 0.0f) return -1;
        const int cols = std::max(1, metrics_.columns);
        const int row = static_cast<int>(off / metrics_.pitch_y);
        int col = 0;
        if (cols > 1) {
            const float grid_x = local_x - metrics_.origin_x;
            if (grid_x < 0.0f || metrics_.pitch_x <= 0.0f) return -1;
            col = static_cast<int>(grid_x / metrics_.pitch_x);
            if (col >= cols) return -1;
        }
        const int k = row * cols + col;
        if (k >= grp.count) return -1;
        const int index = grp.first + k;
        // Gaps between cells and below the last row belong to no item.
        const auto bounds = ItemRect(index);
        return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom ? index : -1;
    }
    const float grid_x = local_x - metrics_.origin_x;
    const float grid_y = local_y - metrics_.origin_y;
    if (grid_x < 0.0f || grid_y < 0.0f) return -1;
    const int col = static_cast<int>(grid_x / metrics_.pitch_x);
    const int row = static_cast<int>(grid_y / metrics_.pitch_y);
    if (col >= metrics_.columns || (metrics_.column_major && row >= metrics_.rows_per_column)) return -1;
    const int index = metrics_.column_major
        ? col * metrics_.rows_per_column + row
        : row * metrics_.columns + col;
    if (index < 0 || static_cast<size_t>(index) >= count_) return -1;
    const auto bounds = ItemRect(index);
    return x >= bounds.left && x < bounds.right && y >= bounds.top && y < bounds.bottom ? index : -1;
}

std::pair<int, int> ViewLayout::VisibleRange() const noexcept {
    if (count_ == 0) return {-1, -1};
    if (wall_) return RangeForSpan(viewport_.top, viewport_.bottom);
    if (groups_) {
        const float height = viewport_.bottom - viewport_.top;
        auto index_at = [&](float local_y, bool last) { return GroupedIndexAt(local_y, last); };
        const int margin = std::max(1, metrics_.columns);
        const int lo = std::max(0, index_at(scroll_y_, false) - margin);
        const int hi = std::min(static_cast<int>(count_) - 1, index_at(scroll_y_ + height, true) + margin);
        return {lo, hi};
    }
    int first = 0;
    int last = static_cast<int>(count_) - 1;
    if (metrics_.column_major) {
        const int first_col = std::max(0, static_cast<int>(scroll_x_ / metrics_.cell_width) - 1);
        const int last_col = std::min(metrics_.columns - 1,
            static_cast<int>((scroll_x_ + viewport_.right - viewport_.left) / metrics_.cell_width) + 1);
        first = first_col * metrics_.rows_per_column;
        last = std::min(last, (last_col + 1) * metrics_.rows_per_column - 1);
    } else {
        const float top = scroll_y_ - metrics_.origin_y;
        const int first_row = std::max(0, static_cast<int>(top / metrics_.pitch_y) - 1);
        const int last_row = static_cast<int>((top + viewport_.bottom - viewport_.top) /
                                               metrics_.pitch_y) + 1;
        first = first_row * metrics_.columns;
        last = std::min(last, (last_row + 1) * metrics_.columns - 1);
    }
    return {first, last};
}

std::pair<int, int> ViewLayout::RangeForSpan(float lo, float hi) const noexcept {
    if (count_ == 0 || hi < lo) return {-1, -1};
    const int last_index = static_cast<int>(count_) - 1;
    if (wall_) {
        // One row of margin each side, as the grids keep.
        const int rows = static_cast<int>(wall_->RowCount());
        const int r0 = std::max(0, wall_->RowAtY(lo - viewport_.top + scroll_y_) - 1);
        const int r1 = std::clamp(wall_->RowAtY(hi - viewport_.top + scroll_y_) + 1, 0, rows - 1);
        if (r1 < r0) return {-1, -1};
        return {wall_->row_first[static_cast<size_t>(r0)], wall_->row_first[static_cast<size_t>(r1) + 1] - 1};
    }
    if (groups_) {
        const float a = std::clamp(lo - viewport_.top + scroll_y_, 0.0f, std::max(0.0f, content_height_));
        const float b = std::clamp(hi - viewport_.top + scroll_y_, a, std::max(a, content_height_));
        auto index_at = [&](float local_y, bool last) { return GroupedIndexAt(local_y, last); };
        const int margin = std::max(1, metrics_.columns);
        return {std::max(0, index_at(a, false) - margin), std::min(last_index, index_at(b, true) + margin)};
    }
    int first = 0, last = last_index;
    if (metrics_.column_major) {
        if (metrics_.cell_width <= 0.0f || metrics_.rows_per_column <= 0) return {0, last_index};
        const float a = lo - viewport_.left + scroll_x_, b = hi - viewport_.left + scroll_x_;
        const int first_col = std::max(0, static_cast<int>(std::floor(a / metrics_.cell_width)) - 1);
        const int last_col = std::max(0, static_cast<int>(std::floor(b / metrics_.cell_width)) + 1);
        first = first_col * metrics_.rows_per_column;
        last = std::min(last, (last_col + 1) * metrics_.rows_per_column - 1);
    } else {
        if (metrics_.pitch_y <= 0.0f || metrics_.columns <= 0) return {0, last_index};
        const float a = lo - viewport_.top + scroll_y_ - metrics_.origin_y;
        const float b = hi - viewport_.top + scroll_y_ - metrics_.origin_y;
        const int first_row = std::max(0, static_cast<int>(std::floor(a / metrics_.pitch_y)) - 1);
        const int last_row = std::max(0, static_cast<int>(std::floor(b / metrics_.pitch_y)) + 1);
        first = first_row * metrics_.columns;
        last = std::min(last, (last_row + 1) * metrics_.columns - 1);
    }
    if (first > last) return {-1, -1};
    return {first, last};
}

int ViewLayout::MoveIndex(int current, int dx, int dy) const noexcept {
    if (count_ == 0) return -1;
    current = std::clamp(current, 0, static_cast<int>(count_) - 1);
    if (groups_) {
        if (visible_rows_ == 0) return current;
        const int cols = std::max(1, metrics_.columns);
        const int g0 = GroupOfIndex(current);
        if (cols == 1 || dy == 0 || (*groups_)[static_cast<size_t>(g0)].collapsed) {
            // Left / right (and single-column up / down) walk the visible order,
            // crossing into the next group.
            const int ord = std::clamp(VisibleOrdinal(current) + dy * cols + dx, 0, visible_rows_ - 1);
            const int idx = IndexFromOrdinal(ord);
            return idx >= 0 ? idx : current;
        }
        // Up / down by rows; past a group's edge into the next open group's
        // first (or last) row, keeping the column where that row has one.
        int g = g0;
        const int k0 = current - (*groups_)[static_cast<size_t>(g)].first;
        const int col = k0 % cols;
        int row = k0 / cols;
        int idx = current;
        const int groups = GroupCount();
        for (int step = 0; step < std::abs(dy); ++step) {
            const ListGroup& here = (*groups_)[static_cast<size_t>(g)];
            const int rows = (here.count + cols - 1) / cols;
            if (dy > 0) {
                if (row + 1 < rows) {
                    ++row;
                } else {
                    int next = g + 1;
                    while (next < groups && ((*groups_)[static_cast<size_t>(next)].collapsed ||
                                             (*groups_)[static_cast<size_t>(next)].count == 0)) ++next;
                    if (next >= groups) break;
                    g = next;
                    row = 0;
                }
            } else {
                if (row > 0) {
                    --row;
                } else {
                    int prev = g - 1;
                    while (prev >= 0 && ((*groups_)[static_cast<size_t>(prev)].collapsed ||
                                         (*groups_)[static_cast<size_t>(prev)].count == 0)) --prev;
                    if (prev < 0) break;
                    g = prev;
                    row = ((*groups_)[static_cast<size_t>(g)].count + cols - 1) / cols - 1;
                }
            }
            const ListGroup& at = (*groups_)[static_cast<size_t>(g)];
            idx = at.first + std::min(row * cols + col, at.count - 1);
        }
        return idx;
    }
    if (wall_) {
        if (dy == 0) return std::clamp(current + dx, 0, static_cast<int>(count_) - 1);
        // Up / down: the item in that row nearest the current one's centre.
        const int row = wall_->RowOf(current) + dy;
        if (row < 0) return 0;
        if (row >= static_cast<int>(wall_->RowCount())) return static_cast<int>(count_) - 1;
        const float cx = wall_->x[static_cast<size_t>(current)] + wall_->w[static_cast<size_t>(current)] * 0.5f;
        int best = wall_->row_first[static_cast<size_t>(row)];
        for (int i = best; i < wall_->row_first[static_cast<size_t>(row) + 1]; ++i) {
            const auto d = [&](int k) {
                return std::fabs(wall_->x[static_cast<size_t>(k)] + wall_->w[static_cast<size_t>(k)] * 0.5f - cx);
            };
            if (d(i) < d(best)) best = i;
        }
        return best;
    }
    int delta = 0;
    if (metrics_.column_major) delta = dx * metrics_.rows_per_column + dy;
    else delta = dx + dy * metrics_.columns;
    return std::clamp(current + delta, 0, static_cast<int>(count_) - 1);
}

int ViewLayout::PageDelta() const noexcept {
    if (metrics_.column_major) return std::max(1, metrics_.rows_per_column);
    if (wall_ && wall_->RowCount() > 0 && wall_->content_height > 0.0f) {
        const float rows = static_cast<float>(wall_->RowCount());
        const float pitch = wall_->content_height / rows;
        const float per_row = static_cast<float>(count_) / rows;
        const float page_rows = std::max(1.0f, std::floor((viewport_.bottom - viewport_.top) / pitch) - 1.0f);
        return std::max(1, static_cast<int>(page_rows * per_row));
    }
    const float height = viewport_.bottom - viewport_.top;
    const int rows = std::max(1, static_cast<int>(height / metrics_.pitch_y) - 1);
    return rows * metrics_.columns;
}

} // namespace pulse::ui
