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

bool UsesThumbnails(ViewMode mode) noexcept {
    return mode == ViewMode::ExtraLargeIcons || mode == ViewMode::LargeIcons ||
           mode == ViewMode::MediumIcons || mode == ViewMode::Tiles ||
           mode == ViewMode::Content;
}

ViewLayout::ViewLayout(ViewMode mode, D2D1_RECT_F viewport, size_t item_count,
                       float scroll_x, float scroll_y, float scale,
                       float row_height_dip)
    : mode_(mode), viewport_(viewport), count_(item_count),
      scroll_x_(std::max(0.0f, scroll_x)), scroll_y_(std::max(0.0f, scroll_y)),
      scale_(std::max(0.5f, scale)) {
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
    case ViewMode::ExtraLargeIcons: grid(280.0f, 304.0f, 240.0f, true); break;
    case ViewMode::LargeIcons: grid(116.0f, 140.0f, 96.0f); break;
    case ViewMode::MediumIcons: grid(112.0f, 92.0f, 48.0f); break;
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
}

float ViewLayout::MaxScrollX() const noexcept {
    return std::max(0.0f, content_width_ - (viewport_.right - viewport_.left));
}

float ViewLayout::MaxScrollY() const noexcept {
    return std::max(0.0f, content_height_ - (viewport_.bottom - viewport_.top));
}

D2D1_RECT_F ViewLayout::ItemRect(int index) const noexcept {
    if (index < 0 || static_cast<size_t>(index) >= count_) return {};
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
        return {cell.left + pad, top, cell.right - pad, cell.bottom - 4.0f * scale_};
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

int ViewLayout::HitTest(float x, float y) const noexcept {
    if (x < viewport_.left || x >= viewport_.right || y < viewport_.top || y >= viewport_.bottom)
        return -1;
    const float local_x = x - viewport_.left + scroll_x_;
    const float local_y = y - viewport_.top + scroll_y_;
    const int col = std::max(0, static_cast<int>(local_x / metrics_.cell_width));
    const int row = std::max(0, static_cast<int>(local_y / metrics_.cell_height));
    const int index = metrics_.column_major
        ? col * metrics_.rows_per_column + row
        : row * metrics_.columns + col;
    return index >= 0 && static_cast<size_t>(index) < count_ ? index : -1;
}

std::pair<int, int> ViewLayout::VisibleRange() const noexcept {
    if (count_ == 0) return {-1, -1};
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

int ViewLayout::MoveIndex(int current, int dx, int dy) const noexcept {
    if (count_ == 0) return -1;
    current = std::clamp(current, 0, static_cast<int>(count_) - 1);
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
