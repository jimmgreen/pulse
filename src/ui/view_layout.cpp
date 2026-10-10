#include "view_layout.h"

#include <algorithm>
#include <cmath>
#include <cwctype>

namespace pulse::ui {

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

ViewLayout::ViewLayout(ViewMode mode, D2D1_RECT_F viewport, size_t item_count,
                       float scroll_x, float scroll_y, float scale,
                       float row_height_dip, const ListGroups* groups)
    : mode_(mode), viewport_(viewport), count_(item_count),
      scroll_x_(std::max(0.0f, scroll_x)), scroll_y_(std::max(0.0f, scroll_y)),
      scale_(std::max(0.5f, scale)) {
    if (mode_ == ViewMode::Details) viewport_ = DetailsContentRect(viewport_, scale_);
    const float width = std::max(1.0f, viewport_.right - viewport_.left);
    const float height = std::max(1.0f, viewport_.bottom - viewport_.top);
    auto grid = [&](float cell_w, float cell_h, float icon, bool fixed_pixel_tier = false) {
        const float factor = fixed_pixel_tier ? 1.0f : scale_;
        metrics_.cell_width = std::min(width, cell_w * factor);
        metrics_.cell_height = cell_h * factor;
        metrics_.icon_size = icon * factor;
        metrics_.columns = std::max(1, static_cast<int>(width / metrics_.cell_width));
        if (!fixed_pixel_tier)
            metrics_.cell_width = width / static_cast<float>(metrics_.columns);
        const size_t rows = (count_ + static_cast<size_t>(metrics_.columns) - 1) /
                            static_cast<size_t>(metrics_.columns);
        content_width_ = width;
        content_height_ = static_cast<float>(rows) * metrics_.cell_height;
    };
    switch (mode_) {
    case ViewMode::ExtraLargeIcons: grid(280.0f, 240.0f + 64.0f * scale_, 240.0f, true); break;
    case ViewMode::LargeIcons: grid(116.0f, 160.0f, 96.0f); break;
    case ViewMode::MediumIcons: grid(112.0f, 112.0f, 48.0f); break;
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
    if (groups && !groups->empty() && count_ > 0 &&
        (mode_ == ViewMode::Details || mode_ == ViewMode::Content)) {
        groups_ = groups;
        header_h_ = kGroupHeaderDip * scale_;
        group_top_.reserve(groups->size());
        float y = 0.0f;
        for (const ListGroup& g : *groups) {
            group_top_.push_back(y);
            y += header_h_;
            if (!g.collapsed) {
                y += static_cast<float>(g.count) * metrics_.cell_height;
                visible_rows_ += g.count;
            }
        }
        content_height_ = y;
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

float ViewLayout::MaxScrollX() const noexcept {
    return std::max(0.0f, content_width_ - (viewport_.right - viewport_.left));
}

float ViewLayout::MaxScrollY() const noexcept {
    return std::max(0.0f, content_height_ - (viewport_.bottom - viewport_.top));
}

D2D1_RECT_F ViewLayout::ItemRect(int index) const noexcept {
    if (index < 0 || static_cast<size_t>(index) >= count_) return {};
    if (groups_) {
        // Rows of a collapsed group get a zero-height rect under its header.
        const int g = GroupOfIndex(index);
        const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
        const float base = viewport_.top + group_top_[static_cast<size_t>(g)] + header_h_ - scroll_y_;
        const float left = viewport_.left - scroll_x_;
        if (grp.collapsed) return D2D1_RECT_F{left, base, left + metrics_.cell_width, base};
        const float top = base + static_cast<float>(index - grp.first) * metrics_.cell_height;
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
    const float left = viewport_.left + col * metrics_.cell_width - scroll_x_;
    const float top = viewport_.top + row * metrics_.cell_height - scroll_y_;
    return D2D1_RECT_F{left, top, left + metrics_.cell_width, top + metrics_.cell_height};
}

D2D1_RECT_F ViewLayout::IconRect(int index) const noexcept {
    const D2D1_RECT_F cell = ItemRect(index);
    const float icon = metrics_.icon_size;
    const float pad = 8.0f * scale_;
    if (mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
        mode_ == ViewMode::MediumIcons) {
        const float available = std::max(16.0f, cell.right - cell.left - pad * 2.0f);
        const float fittedIcon = std::min(icon, available);
        const float left = cell.left + (cell.right - cell.left - fittedIcon) * 0.5f;
        return {left, cell.top + pad, left + fittedIcon, cell.top + pad + fittedIcon};
    }
    const float top = cell.top + (cell.bottom - cell.top - icon) * 0.5f;
    return {cell.left + pad, top, cell.left + pad + icon, top + icon};
}

D2D1_RECT_F ViewLayout::NameRect(int index) const noexcept {
    D2D1_RECT_F cell = ItemRect(index);
    const float pad = 8.0f * scale_;
    const float icon = metrics_.icon_size;
    if (mode_ == ViewMode::ExtraLargeIcons || mode_ == ViewMode::LargeIcons ||
        mode_ == ViewMode::MediumIcons) {
        const D2D1_RECT_F icon_rect = IconRect(index);
        const float top = std::min(cell.bottom - 4.0f * scale_,
                                   icon_rect.bottom + 4.0f * scale_);
        return {cell.left + pad, top, cell.right - pad, top + 24.0f * scale_};
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
    if (!ShowsFolderSize(mode_)) return {};
    const auto name = NameRect(index);
    const float top = name.top + 25.0f * scale_;
    return {name.left, top, name.right, top + 22.0f * scale_};
}

int ViewLayout::HitTest(float x, float y) const noexcept {
    if (x < viewport_.left || x >= viewport_.right || y < viewport_.top || y >= viewport_.bottom)
        return -1;
    const float local_x = x - viewport_.left + scroll_x_;
    const float local_y = y - viewport_.top + scroll_y_;
    if (groups_) {
        const int g = GroupAtY(local_y);
        const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
        const float off = local_y - group_top_[static_cast<size_t>(g)] - header_h_;
        if (off < 0.0f || grp.collapsed) return -1;
        const int row = static_cast<int>(off / metrics_.cell_height);
        return row < grp.count ? grp.first + row : -1;
    }
    const int col = std::max(0, static_cast<int>(local_x / metrics_.cell_width));
    const int row = std::max(0, static_cast<int>(local_y / metrics_.cell_height));
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
    if (groups_) {
        const float height = viewport_.bottom - viewport_.top;
        auto index_at = [&](float local_y, bool last) {
            const int g = GroupAtY(local_y);
            const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
            const float off = local_y - group_top_[static_cast<size_t>(g)] - header_h_;
            if (grp.collapsed || off < 0.0f) return last ? grp.first + grp.count - 1 : grp.first;
            return grp.first + std::min(grp.count - 1, static_cast<int>(off / metrics_.cell_height));
        };
        const int lo = std::max(0, index_at(scroll_y_, false) - 1);
        const int hi = std::min(static_cast<int>(count_) - 1, index_at(scroll_y_ + height, true) + 1);
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
        const int first_row = std::max(0, static_cast<int>(scroll_y_ / metrics_.cell_height) - 1);
        const int last_row = static_cast<int>((scroll_y_ + viewport_.bottom - viewport_.top) /
                                               metrics_.cell_height) + 1;
        first = first_row * metrics_.columns;
        last = std::min(last, (last_row + 1) * metrics_.columns - 1);
    }
    return {first, last};
}

std::pair<int, int> ViewLayout::RangeForSpan(float lo, float hi) const noexcept {
    if (count_ == 0 || hi < lo) return {-1, -1};
    const int last_index = static_cast<int>(count_) - 1;
    if (groups_) {
        const float a = std::clamp(lo - viewport_.top + scroll_y_, 0.0f, std::max(0.0f, content_height_));
        const float b = std::clamp(hi - viewport_.top + scroll_y_, a, std::max(a, content_height_));
        auto index_at = [&](float local_y, bool last) {
            const int g = GroupAtY(local_y);
            const ListGroup& grp = (*groups_)[static_cast<size_t>(g)];
            const float off = local_y - group_top_[static_cast<size_t>(g)] - header_h_;
            if (grp.collapsed || off < 0.0f) return last ? grp.first + grp.count - 1 : grp.first;
            return grp.first + std::min(grp.count - 1, static_cast<int>(off / metrics_.cell_height));
        };
        return {std::max(0, index_at(a, false) - 1), std::min(last_index, index_at(b, true) + 1)};
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
        if (metrics_.cell_height <= 0.0f || metrics_.columns <= 0) return {0, last_index};
        const float a = lo - viewport_.top + scroll_y_, b = hi - viewport_.top + scroll_y_;
        const int first_row = std::max(0, static_cast<int>(std::floor(a / metrics_.cell_height)) - 1);
        const int last_row = std::max(0, static_cast<int>(std::floor(b / metrics_.cell_height)) + 1);
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
        const int ord = std::clamp(VisibleOrdinal(current) + dy + dx, 0, visible_rows_ - 1);
        const int idx = IndexFromOrdinal(ord);
        return idx >= 0 ? idx : current;
    }
    int delta = 0;
    if (metrics_.column_major) delta = dx * metrics_.rows_per_column + dy;
    else delta = dx + dy * metrics_.columns;
    return std::clamp(current + delta, 0, static_cast<int>(count_) - 1);
}

int ViewLayout::PageDelta() const noexcept {
    if (metrics_.column_major) return std::max(1, metrics_.rows_per_column);
    const float height = viewport_.bottom - viewport_.top;
    const int rows = std::max(1, static_cast<int>(height / metrics_.cell_height) - 1);
    return rows * metrics_.columns;
}

} // namespace pulse::ui
