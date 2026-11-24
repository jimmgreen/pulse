#pragma once
#include "desktop_model.h"

namespace pulse::desktop::model {
struct TileReorderBudget {
    size_t max_states = 20000;
    size_t max_cells = 4096;
};
struct TileGridTarget {
    bool valid = false;
    PointI cell;
    RectI rect;
    RectI reservation; // Complete in-work-area occupancy; includes rect, excludes off-monitor cells.
};
enum class TileReorderStatus { Success, InvalidInput, BlockedTarget, NoSpace, BudgetExceeded };
struct TileReorderResult {
    bool valid = false;
    TileReorderStatus reason = TileReorderStatus::InvalidInput;
    std::vector<DesktopTile> updated_tiles;
    RectI target_rect;
    PointI target_cell;
    size_t visited_states = 0;
};
TileGridTarget ComputeTileReorderTarget(const DesktopTile& tile, PointI proposed,
                                    const RectI& work_area, const RectI& board_rect);
TileReorderResult PreviewTileReorder(const std::vector<DesktopTile>& start, size_t moving_index,
                              PointI proposed, const RectI& work_area, const RectI& board_rect,
                              std::span<const RectI> groups, TileReorderBudget budget = {});
}
