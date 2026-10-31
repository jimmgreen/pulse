#pragma once

#include <d2d1.h>
#include <atomic>
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
// Views that draw "Group by" headers: every view but List, whose columns run
// sideways. (A photo wall is not grouped either; that is a tab state.)
inline bool ShowsGroups(ViewMode mode) noexcept { return mode != ViewMode::List; }

// Ctrl+wheel / touchpad pinch zoom chain: Details <-> Medium <-> Large <-> Extra
// large icons. List, small icons, tiles and content stay off the chain (menu
// and Ctrl+Shift+1..8 still reach them). step > 0 zooms in.
bool IsZoomIconMode(ViewMode mode) noexcept;
// The first step out of Details returns to last_icon (Medium if it is off the
// chain); zooming out past Medium lands on Details and stops there.
ViewMode ZoomViewMode(ViewMode current, ViewMode last_icon, int step) noexcept;

// Continuous icon size: icon grids take a thumbnail width in DIP, 0 meaning
// the mode's preset (medium 48, large 96 DIP, extra large 240 device pixels).
// A size belongs to the icon mode whose preset is nearest on a log scale.
inline constexpr float kMinIconDip = 32.0f;
inline constexpr float kMaxIconDip = 256.0f;
float IconPresetDip(ViewMode mode, float scale) noexcept;
// icon_dip, or the mode's preset when it is 0.
float IconDip(ViewMode mode, float icon_dip, float scale) noexcept;
ViewMode IconModeForDip(float dip, float scale) noexcept;
// What a tab stores: 0 for the mode's preset (and for other views), otherwise
// whole DIP within the zoom range.
float NormalizeIconDip(ViewMode mode, float dip, float scale) noexcept;
// Ctrl+wheel: 2^(1/4) per notch, stopping on each preset it reaches. Zooming
// out of the smallest size returns 0 (Details).
float ZoomIconDip(float dip, int step, float scale) noexcept;

// Reserve a marquee gutter before the name and before the scrollbar.
D2D1_RECT_F DetailsContentRect(D2D1_RECT_F bounds, float scale) noexcept;

// "Group by": contiguous view rows [first, first + count) shown under one
// header; in icon grids each group starts a new row. Groups cover every row,
// in display order.
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

// Lines an icon-grid name may wrap to before its end is elided ("a…z.ext"):
// the default, and the most a folder's names get (AutoIconNameLines).
inline constexpr int kIconGridNameLines = 2;
inline constexpr int kIconGridMaxNameLines = 4;
// Line pitch of the 13 DIP file-name font (Segoe UI / Microsoft YaHei UI).
inline constexpr float kIconGridNameLineDip = 18.0f;
// Icon grids: height of a name block of `lines` lines. ViewLayout takes it as
// row_height_dip, which only Details otherwise uses.
inline constexpr float IconGridNameBlockDip(int lines) noexcept {
    return static_cast<float>(lines) * kIconGridNameLineDip + 2.0f;
}
// Pixel width an icon-grid name wraps in at the tier's widest cell (base cell
// plus the most leftover a cell absorbs). Independent of the pane width, so a
// folder's line count does not change while the pane is resized; no row keeps
// a blank line. In a narrower row a name needing one more line is elided in
// the middle and shown whole when selected.
float IconGridNameWidth(ViewMode mode, float scale, float icon_dip = 0.0f) noexcept;
// Name lines for a whole folder from the lines each sampled name needs: one
// only when 95% fit on one line, otherwise what 75% need, kept to 2..4.
// Folders show their size on the last line, so a folder holding subfolders
// keeps at least two. Sorts `needed`.
int AutoIconNameLines(std::vector<int>& needed, bool has_folders) noexcept;

// Photo wall (P2): justified rows of one height, each item as wide as its
// aspect ratio makes it. Built once per width / size / aspect change, O(n);
// ViewLayout then finds rows by binary search. Content coordinates.
struct WallRows {
    std::vector<int> row_first;  // first view index of each row; back() == item count
    std::vector<float> row_top;  // y of each row
    std::vector<float> row_h;
    std::vector<float> x;        // per view index
    std::vector<float> w;
    float content_height = 0.0f;
    size_t RowCount() const noexcept { return row_top.size(); }
    int RowOf(int view_index) const noexcept;
    // Row whose band [top, top + h + gap) holds y; -1 above the first row.
    int RowAtY(float y) const noexcept;
};
// aspects: width / height per view index (<= 0 = not known yet: 4:3). Rows
// aim at row_px; a full row shrinks a little to fill the width exactly, the
// last one keeps row_px unless filling it stretches less than 1.25x.
WallRows BuildWallRows(const std::vector<float>& aspects, float width, float row_px, float scale, const std::atomic<uint64_t>* generation = nullptr, uint64_t expected = 0);

// Size inputs of a pane's layout; a plain row height converts implicitly.
struct ViewLayoutParams {
    ViewLayoutParams(float row_height = 0.0f, float icon = 0.0f) noexcept
        : row_height_dip(row_height), icon_dip(icon) {}
    // >0: Details row height override; icon grids: name block (IconGridNameBlockDip)
    float row_height_dip;
    float icon_dip;  // icon grids: thumbnail width in DIP (0 = the mode's preset)
    // Icon grids shown as a photo wall: its rows (for this item count), else null.
    const WallRows* wall = nullptr;
};

struct ViewLayoutMetrics {
    float cell_width = 0.0f;
    float cell_height = 0.0f;
    float icon_size = 0.0f;
    // Icon grids: height of the thumbnail slot above the name. Equal to
    // icon_size (a square) except extra large icons, whose 4:3 slot fits the
    // landscape photos most folders hold without a blank band above them.
    float slot_height = 0.0f;
    int columns = 1;
    int rows_per_column = 1;
    bool column_major = false;
    // Where cell (0, 0) starts and the step to the next cell (cell plus gap).
    // Icon grids leave gaps and centre their rows; other views step by the
    // cell size from the viewport corner.
    float origin_x = 0.0f;
    float origin_y = 0.0f;
    float pitch_x = 0.0f;
    float pitch_y = 0.0f;
    int name_lines = kIconGridNameLines;  // icon grids
};

// Pure, O(1) virtual geometry. Coordinates returned by ItemRect are in the
// same pixel space as viewport; no per-item state is allocated.
class ViewLayout {
public:
    ViewLayout(ViewMode mode, D2D1_RECT_F viewport, size_t item_count,
               float scroll_x, float scroll_y, float scale,
               ViewLayoutParams params = {},
               const ListGroups* groups = nullptr); // ShowsGroups views, not a photo wall

    ViewMode Mode() const noexcept { return mode_; }
    const ViewLayoutMetrics& Metrics() const noexcept { return metrics_; }
    float ContentWidth() const noexcept { return content_width_; }
    float ContentHeight() const noexcept { return content_height_; }
    float MaxScrollX() const noexcept;
    float MaxScrollY() const noexcept;
    D2D1_RECT_F ItemRect(int view_index) const noexcept;
    // Shell icon square. In icon grids it rests on the bottom of the slot.
    D2D1_RECT_F IconRect(int view_index) const noexcept;
    // Icon grids: the whole thumbnail slot (wider than IconRect for extra
    // large icons). Other views: IconRect.
    D2D1_RECT_F ThumbnailRect(int view_index) const noexcept;
    // Icon grids: room for Metrics().name_lines lines under the slot.
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
    // Icon grids shown as a photo wall: its rows, else null.
    const WallRows* Wall() const noexcept { return wall_; }
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
    int GroupedIndexAt(float local_y, bool last) const noexcept;
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
    // Grouped grids: gap between a header and its first row, and after its
    // last row (0 in single-column views).
    float band_top_ = 0.0f;
    float band_bottom_ = 0.0f;
    int visible_rows_ = 0;           // rows outside collapsed groups
    const WallRows* wall_ = nullptr; // photo wall rows (icon grids), not owned
};

} // namespace pulse::ui
