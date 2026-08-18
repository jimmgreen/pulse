#pragma once

#include <d2d1.h>
#include <cstddef>
#include <string>
#include <utility>

namespace pulse::ui {

enum class ViewMode : unsigned char {
    ExtraLargeIcons,
    LargeIcons,
    MediumIcons,
    SmallIcons,
    List,
    Details,
    Tiles,
    Content,
};

const wchar_t* ViewModeName(ViewMode mode) noexcept;
ViewMode ParseViewMode(const std::wstring& value) noexcept;
int ViewModeIndex(ViewMode mode) noexcept;
ViewMode ViewModeFromIndex(int index) noexcept;
bool ShowsColumnHeader(ViewMode mode) noexcept;
bool UsesThumbnails(ViewMode mode) noexcept;

struct ViewLayoutMetrics {
    float cell_width = 0.0f;
    float cell_height = 0.0f;
    float icon_size = 0.0f;
    int columns = 1;
    int rows_per_column = 1;
    bool column_major = false;
};

// Pure, O(1) virtual geometry. Coordinates returned by ItemRect are in the
// same pixel space as viewport; no per-item state is allocated.
class ViewLayout {
public:
    ViewLayout(ViewMode mode, D2D1_RECT_F viewport, size_t item_count,
               float scroll_x, float scroll_y, float scale);

    ViewMode Mode() const noexcept { return mode_; }
    const ViewLayoutMetrics& Metrics() const noexcept { return metrics_; }
    float ContentWidth() const noexcept { return content_width_; }
    float ContentHeight() const noexcept { return content_height_; }
    float MaxScrollX() const noexcept;
    float MaxScrollY() const noexcept;
    D2D1_RECT_F ItemRect(int view_index) const noexcept;
    D2D1_RECT_F IconRect(int view_index) const noexcept;
    D2D1_RECT_F NameRect(int view_index) const noexcept;
    int HitTest(float x, float y) const noexcept;
    std::pair<int, int> VisibleRange() const noexcept;
    int MoveIndex(int current, int dx, int dy) const noexcept;
    int PageDelta() const noexcept;

private:
    ViewMode mode_ = ViewMode::Details;
    D2D1_RECT_F viewport_{};
    size_t count_ = 0;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    float scale_ = 1.0f;
    ViewLayoutMetrics metrics_{};
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
};

} // namespace pulse::ui
