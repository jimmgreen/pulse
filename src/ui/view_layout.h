#pragma once

#include <d2d1.h>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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
bool ShowsFolderSize(ViewMode mode) noexcept;

// Reserve a marquee gutter before the name and before the scrollbar.
D2D1_RECT_F DetailsContentRect(D2D1_RECT_F bounds, float scale) noexcept;

// "Group by" (details view): contiguous view rows [first, first + count)
// shown under one header. Groups cover every row, in display order.
struct ListGroup {
    int first = 0;
    int count = 0;
    int rank = 0;
    int sample = -1;        // source index of the group's first entry (labels)
    uint64_t bytes = 0;     // file bytes in the group
    std::wstring key;       // stable identity for collapse state
    bool collapsed = false;
    std::wstring label;     // tag groups: tag name (empty = untagged)
    uint32_t color_rgb = 0; // tag groups: tag color
};
using ListGroups = std::vector<ListGroup>;

constexpr float kGroupHeaderDip = 34.0f;

// The select action is only offered when the header has room for it.
inline bool GroupSelectVisible(const D2D1_RECT_F& content, float scale) noexcept {
    return content.right - content.left >= 320.0f * scale;
}

// Hover action "select this group", right-aligned inside a header.
inline D2D1_RECT_F GroupSelectRect(const D2D1_RECT_F& header, float scale) noexcept {
    const float right = header.right - 10.0f * scale;
    return D2D1_RECT_F{right - 84.0f * scale, header.top + 5.0f * scale,
                       right, header.bottom - 5.0f * scale};
}

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
               float scroll_x, float scroll_y, float scale,
               float row_height_dip = 0.0f, // >0: Details-mode row height override
               const ListGroups* groups = nullptr); // single-column views only

    ViewMode Mode() const noexcept { return mode_; }
    const ViewLayoutMetrics& Metrics() const noexcept { return metrics_; }
    float ContentWidth() const noexcept { return content_width_; }
    float ContentHeight() const noexcept { return content_height_; }
    float MaxScrollX() const noexcept;
    float MaxScrollY() const noexcept;
    D2D1_RECT_F ItemRect(int view_index) const noexcept;
    D2D1_RECT_F IconRect(int view_index) const noexcept;
    D2D1_RECT_F NameRect(int view_index) const noexcept;
    D2D1_RECT_F FolderSizeRect(int view_index) const noexcept;
    int HitTest(float x, float y) const noexcept;
    std::pair<int, int> VisibleRange() const noexcept;
    // Indices whose band can meet [lo, hi] along the scroll axis (viewport
    // pixels: x for column-major views, y otherwise), including off screen.
    std::pair<int, int> RangeForSpan(float lo, float hi) const noexcept;
    int MoveIndex(int current, int dx, int dy) const noexcept;
    int PageDelta() const noexcept;
    // Group headers (empty unless grouped): header g in viewport pixels.
    bool Grouped() const noexcept { return groups_ != nullptr; }
    int GroupCount() const noexcept;
    D2D1_RECT_F HeaderRect(int group) const noexcept;
    int HeaderHitTest(float x, float y) const noexcept;
    int GroupOfIndex(int view_index) const noexcept;
    // Group whose band covers the top edge of the viewport (sticky header).
    int GroupAtTop() const noexcept;
    float HeaderHeight() const noexcept { return header_h_; }
    // Group whose header is pinned at the viewport top because its own header
    // scrolled away; the next header pushes it up. -1 when nothing is pinned.
    int StickyHeader(D2D1_RECT_F* out) const noexcept;

private:
    int GroupAtY(float local_y) const noexcept;
    int VisibleOrdinal(int index) const noexcept;
    int IndexFromOrdinal(int ordinal) const noexcept;
    ViewMode mode_ = ViewMode::Details;
    D2D1_RECT_F viewport_{};
    size_t count_ = 0;
    float scroll_x_ = 0.0f;
    float scroll_y_ = 0.0f;
    float scale_ = 1.0f;
    ViewLayoutMetrics metrics_{};
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
    const ListGroups* groups_ = nullptr;
    std::vector<float> group_top_;   // content-space y of each header
    float header_h_ = 0.0f;
    int visible_rows_ = 0;           // rows outside collapsed groups
};

} // namespace pulse::ui
