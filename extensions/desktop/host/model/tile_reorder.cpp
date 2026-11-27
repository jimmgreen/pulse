#include "tile_reorder.h"
#include <algorithm>
#include <cmath>
#include <functional>
#include <numeric>

namespace pulse::desktop::model {
namespace {
constexpr int kStep = kTileUnitPx + kTileGapPx;
std::pair<int, int> SnapAxis(int proposed, int lo, int hi, int anchor) {
    if (hi <= lo) return {lo, -1};
    const int last = (hi - anchor) / kStep;
    int cell = std::max(0, static_cast<int>(std::llround((static_cast<double>(proposed) - anchor) / kStep)));
    cell = std::min(cell, std::max(0, last));
    int position = anchor + cell * kStep;
    if (position > hi) { position = lo; cell = -1; }
    if (std::abs(static_cast<int64_t>(proposed) - lo) < std::abs(static_cast<int64_t>(proposed) - position)) {
        position = lo; cell = -1;
    }
    if (std::abs(static_cast<int64_t>(proposed) - hi) < std::abs(static_cast<int64_t>(proposed) - position)) {
        position = hi; cell = -2;
    }
    return {position, cell};
}
bool Contains(const RectI& outer, const RectI& inner) {
    return !inner.empty() && inner.x >= outer.x && inner.y >= outer.y &&
        inner.right() <= outer.right() && inner.bottom() <= outer.bottom();
}
bool Before(const RectI& a, const RectI& b) {
    return a.y != b.y ? a.y < b.y : a.x < b.x;
}
void Place(DesktopTile& tile, const RectI& rect) {
    tile.free_placed = true;
    tile.x = rect.x; tile.y = rect.y;
    tile.w = rect.w; tile.h = rect.h;
    tile.rest_origin.reset();
}
bool Overlaps(const RectI& rect, std::span<const RectI> others) {
    return std::any_of(others.begin(), others.end(), [&](const auto& other) { return rect.intersects(other); });
}
RectI Footprint(RectI rect) {
    rect.w = ((rect.w + kTileGapPx + kStep - 1) / kStep) * kStep - kTileGapPx;
    rect.h = ((rect.h + kTileGapPx + kStep - 1) / kStep) * kStep - kTileGapPx;
    return rect;
}
RectI WorkFootprint(RectI rect, const RectI& work) {
    rect = Footprint(rect);
    rect.w = std::min(rect.w, work.right() - rect.x);
    rect.h = std::min(rect.h, work.bottom() - rect.y);
    return rect;
}
bool Conflicts(RectI rect, const RectI& other) {
    // Expand both right/bottom edges to enforce the gap symmetrically. Fixed
    // legacy cards retain their actual size; only new placements reserve cells.
    rect.w += kTileGapPx; rect.h += kTileGapPx;
    RectI padded = other;
    padded.w += kTileGapPx; padded.h += kTileGapPx;
    return rect.intersects(padded);
}
bool Conflicts(const RectI& rect, std::span<const RectI> others) {
    return std::any_of(others.begin(), others.end(), [&](const auto& other) { return Conflicts(rect, other); });
}
}

TileGridTarget ComputeTileReorderTarget(const DesktopTile& tile, PointI proposed,
                                    const RectI& work_area, const RectI& board_rect) {
    TileGridTarget result;
    const auto original = ResolveTileRect(tile, board_rect);
    const int ax = work_area.x + 24, ay = work_area.y + kTileBoardTopMargin;
    if (work_area.empty() || original.empty() || original.w > work_area.w || original.h > work_area.h) return result;
    const auto [x, col] = SnapAxis(proposed.x, work_area.x, work_area.right() - original.w, ax);
    const auto [y, row] = SnapAxis(proposed.y, work_area.y, work_area.bottom() - original.h, ay);
    result.cell = {col, row};
    result.rect = {x, y, original.w, original.h};
    result.reservation = WorkFootprint(result.rect, work_area);
    result.valid = Contains(work_area, result.rect);
    return result;
}

TileGridTarget FindNearestFreeTileCell(const std::vector<DesktopTile>& tiles, size_t moving_index,
                                       PointI proposed, const RectI& work_area, const RectI& board_rect,
                                       std::span<const RectI> groups) {
    TileGridTarget result;
    if (moving_index >= tiles.size() || work_area.empty()) return result;
    const RectI origin = ResolveTileRect(tiles[moving_index], board_rect);
    if (origin.empty() || origin.w > work_area.w || origin.h > work_area.h) return result;

    std::vector<RectI> obstacles;
    obstacles.reserve(tiles.size() + groups.size());
    for (const RectI& g : groups) {
        if (!g.empty()) obstacles.push_back(g);
    }
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (i == moving_index) continue;
        const RectI r = ResolveTileRect(tiles[i], board_rect);
        if (!r.empty()) obstacles.push_back(r);
    }

    const int max_x = work_area.right() - origin.w;
    const int max_y = work_area.bottom() - origin.h;

    int ax = work_area.x + 24;
    int ay = work_area.y + kTileBoardTopMargin;
    int step_x = kStep;
    int step_y = kStep;
    int gap_x = kTileGapPx;
    int gap_y = kTileGapPx;

    // When a right-aligned (or DPI-scaled) tile board exists, anchor the grid pitch & exact
    // ComputeTilePixelRect columns/rows to the board so every slot aligns pixel-for-pixel with
    // starter tiles (matching reference widget_demo.cpp's right-anchored grid) without 1-2px
    // integer-division rounding false conflicts.
    std::vector<int> xs;
    std::vector<int> ys;
    int clear_x = gap_x;
    int clear_y = gap_y;
    if (!board_rect.empty()) {
        constexpr int kStdW = kTileCols * kTileUnitPx + (kTileCols - 1) * kTileGapPx;
        constexpr int kStdH = kTileRows * kTileUnitPx + (kTileRows - 1) * kTileGapPx;
        gap_x = std::clamp((board_rect.w * kTileGapPx + kStdW / 2) / kStdW, 0, board_rect.w / kTileCols);
        gap_y = std::clamp((board_rect.h * kTileGapPx + kStdH / 2) / kStdH, 0, board_rect.h / kTileRows);
        const int usable_w = std::max(kTileCols, board_rect.w - (kTileCols - 1) * gap_x);
        const int usable_h = std::max(kTileRows, board_rect.h - (kTileRows - 1) * gap_y);
        step_x = std::max(16, (usable_w + kTileCols / 2) / kTileCols + gap_x);
        step_y = std::max(16, (usable_h + kTileRows / 2) / kTileRows + gap_y);
        ax = board_rect.x;
        ay = board_rect.y;
        // Allow up to 2px integer-division slack across DPI-scaled board columns so a tile
        // originating from a +1px remainder column never false-conflicts with an adjacent slot.
        clear_x = std::max(0, gap_x - 2);
        clear_y = std::max(0, gap_y - 2);

        for (int c = 0; c < kTileCols; ++c) {
            const int cx = board_rect.x + (c * usable_w) / kTileCols + c * gap_x;
            if (cx >= work_area.x && cx <= max_x) xs.push_back(cx);
        }
        for (int r = 0; r < kTileRows; ++r) {
            const int ry = board_rect.y + (r * usable_h) / kTileRows + r * gap_y;
            if (ry >= work_area.y && ry <= max_y) ys.push_back(ry);
        }
    }

    auto conflicts_board = [&](RectI rect) noexcept {
        rect.w += clear_x;
        rect.h += clear_y;
        for (const RectI& other : obstacles) {
            RectI padded = other;
            padded.w += clear_x;
            padded.h += clear_y;
            if (rect.intersects(padded)) return true;
        }
        return false;
    };

    xs.push_back(work_area.x);
    xs.push_back(max_x);
    if (origin.x >= work_area.x && origin.x <= max_x) xs.push_back(origin.x);
    if (board_rect.empty()) {
        for (int x = ax; x <= max_x; x += step_x) {
            if (x >= work_area.x) xs.push_back(x);
        }
        for (int x = ax - step_x; x >= work_area.x; x -= step_x) {
            if (x <= max_x) xs.push_back(x);
        }
    } else {
        for (int x = ax - step_x; x >= work_area.x; x -= step_x) {
            if (x <= max_x) xs.push_back(x);
        }
        const int right_anchor = board_rect.right() + gap_x;
        for (int x = right_anchor; x <= max_x; x += step_x) {
            if (x >= work_area.x) xs.push_back(x);
        }
    }

    ys.push_back(work_area.y);
    ys.push_back(max_y);
    if (origin.y >= work_area.y && origin.y <= max_y) ys.push_back(origin.y);
    if (board_rect.empty()) {
        for (int y = ay; y <= max_y; y += step_y) {
            if (y >= work_area.y) ys.push_back(y);
        }
        for (int y = ay - step_y; y >= work_area.y; y -= step_y) {
            if (y <= max_y) ys.push_back(y);
        }
    } else {
        for (int y = ay - step_y; y >= work_area.y; y -= step_y) {
            if (y <= max_y) ys.push_back(y);
        }
        const int bottom_anchor = board_rect.bottom() + gap_y;
        for (int y = bottom_anchor; y <= max_y; y += step_y) {
            if (y >= work_area.y) ys.push_back(y);
        }
    }

    // Add flush-adjacent and edge-aligned snap lines relative to every existing tile & group.
    // Without these, a tile previously docked to work_area.right() / work_area.x (off-phase from
    // board_rect by kTileBoardRightMargin) leaves an unfillable 30-60px dead gap beside it.
    for (const RectI& o : obstacles) {
        const int oxs[4] = {o.x - origin.w - gap_x, o.right() + gap_x, o.x, o.right() - origin.w};
        for (int cx : oxs) {
            if (cx >= work_area.x && cx <= max_x) xs.push_back(cx);
        }
        const int oys[4] = {o.y - origin.h - gap_y, o.bottom() + gap_y, o.y, o.bottom() - origin.h};
        for (int cy : oys) {
            if (cy >= work_area.y && cy <= max_y) ys.push_back(cy);
        }
    }

    auto dedup = [](std::vector<int>& v) {
        std::sort(v.begin(), v.end());
        v.erase(std::unique(v.begin(), v.end()), v.end());
    };
    dedup(xs);
    dedup(ys);

    bool found = false;
    int64_t best_dist_sq = 0;
    RectI best_rect{};

    for (int y : ys) {
        for (int x : xs) {
            const RectI cand{x, y, origin.w, origin.h};
            if (!Contains(work_area, cand) || conflicts_board(cand)) continue;
            const int64_t dx = static_cast<int64_t>(x) - proposed.x;
            const int64_t dy = static_cast<int64_t>(y) - proposed.y;
            const int64_t d2 = dx * dx + dy * dy;
            const bool better = !found || d2 < best_dist_sq ||
                                (d2 == best_dist_sq && (cand == origin || (best_rect != origin && Before(cand, best_rect))));
            if (better) {
                found = true;
                best_dist_sq = d2;
                best_rect = cand;
            }
        }
    }

    if (!found) {
        result.rect = origin;
        result.valid = false;
        return result;
    }

    auto axis_cell = [&](int pos, int anchor, int step, int lo) noexcept -> int {
        if (pos >= anchor && (pos - anchor) % step == 0) return (pos - anchor) / step;
        return pos == lo ? -1 : -2;
    };
    result.valid = true;
    result.rect = best_rect;
    result.cell = {axis_cell(best_rect.x, ax, step_x, work_area.x),
                   axis_cell(best_rect.y, ay, step_y, work_area.y)};
    result.reservation = WorkFootprint(best_rect, work_area);
    return result;
}

TileReorderResult PreviewTileReorder(const std::vector<DesktopTile>& start, size_t moving_index,
                              PointI proposed, const RectI& work_area, const RectI& board_rect,
                              std::span<const RectI> groups, TileReorderBudget budget) {
    TileReorderResult result;
    result.updated_tiles = start;
    if (moving_index >= start.size() || budget.max_states == 0 || budget.max_cells == 0) return result;
    if (start.size() > 256 || groups.size() > budget.max_cells) {
        result.reason = TileReorderStatus::BudgetExceeded; return result;
    }
    const auto target = ComputeTileReorderTarget(start[moving_index], proposed, work_area, board_rect);
    result.target_rect = target.rect; result.target_cell = target.cell;
    if (!target.valid) return result;
    const auto target_footprint = target.reservation;
    if (Conflicts(target_footprint, groups)) { result.reason = TileReorderStatus::BlockedTarget; return result; }
    std::vector<RectI> original;
    for (const auto& tile : start) original.push_back(ResolveTileRect(tile, board_rect));
    for (size_t i = 0; i < original.size(); ++i) {
        if (original[i].empty()) return result;
        if (i != moving_index && Overlaps(original[i], groups)) return result;
        for (size_t j = 0; j < i; ++j) if (original[i].intersects(original[j])) return result;
        // Cross-monitor/partially out-of-work-area cards are fixed obstacles,
        // even when they are excluded from the local reading-order chain.
        if (i != moving_index && !Contains(work_area, original[i]) && Conflicts(target_footprint, original[i])) {
            result.reason = TileReorderStatus::BlockedTarget; return result;
        }
    }
    std::vector<size_t> order;
    for (size_t i = 0; i < start.size(); ++i)
        if (i == moving_index || Contains(work_area, original[i])) order.push_back(i);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return Before(original[a], original[b]); });
    const size_t moving_rank = static_cast<size_t>(std::find(order.begin(), order.end(), moving_index) - order.begin());
    size_t lo = moving_rank, hi = moving_rank;
    bool collision = false;
    for (size_t rank = 0; rank < order.size(); ++rank) {
        if (order[rank] != moving_index && Conflicts(target_footprint, original[order[rank]])) {
            collision = true;
            lo = std::min(lo, rank); hi = std::max(hi, rank);
        }
    }
    if (!collision) {
        Place(result.updated_tiles[moving_index], target.rect);
        result.valid = true; result.reason = TileReorderStatus::Success;
        return result;
    }
    const bool forward = Before(original[moving_index], target.rect);
    const int ax = work_area.x + 24, ay = work_area.y + kTileBoardTopMargin;
    const int cols = (work_area.right() - ax - 1) / kStep + 1;
    const int rows = (work_area.bottom() - ay - 1) / kStep + 1;
    if (cols <= 0 || rows <= 0 || static_cast<size_t>(cols) * static_cast<size_t>(rows) > budget.max_cells) {
        result.reason = TileReorderStatus::BudgetExceeded; return result;
    }
    std::vector<PointI> cells;
    const bool edge_layout = target.cell.x < 0 || target.cell.y < 0 || std::any_of(original.begin(), original.end(), [&](const auto& rect) {
        return Contains(work_area, rect) && (rect.x == work_area.x || rect.y == work_area.y || rect.right() == work_area.right() || rect.bottom() == work_area.bottom());
    });
    std::vector<int> xs, ys;
    if (edge_layout) { xs.push_back(work_area.x); ys.push_back(work_area.y); }
    for (int col = 0; col < cols; ++col) xs.push_back(ax + col * kStep);
    for (int row = 0; row < rows; ++row) ys.push_back(ay + row * kStep);
    if (edge_layout) for (const auto& rect : original) {
        if (rect.w <= work_area.w) xs.push_back(work_area.right() - rect.w);
        if (rect.h <= work_area.h) ys.push_back(work_area.bottom() - rect.h);
    }
    const auto unique = [](auto& values) { std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end()); };
    unique(xs); unique(ys);
    if (xs.size() * ys.size() > budget.max_cells) { result.reason = TileReorderStatus::BudgetExceeded; return result; }
    for (int y : ys) for (int x : xs) cells.push_back({x, y});
    bool exhausted = false;
    for (;;) {
        std::vector<size_t> affected;
        std::vector<RectI> occupied(groups.begin(), groups.end());
        occupied.push_back(target_footprint);
        for (size_t i = 0; i < start.size(); ++i)
            if (i != moving_index && !Contains(work_area, original[i])) occupied.push_back(original[i]);
        for (size_t rank = 0; rank < order.size(); ++rank) {
            if (rank < lo || rank > hi) occupied.push_back(original[order[rank]]);
            else if (order[rank] != moving_index) affected.push_back(order[rank]);
        }
        std::vector<RectI> chosen(affected.size());
        RectI lower = original[order[lo]], upper = original[order[hi]];
        // A multi-cell origin releases its entire footprint, not just its top-left
        // anchor. Smaller displaced tiles may occupy the trailing origin cells.
        for (size_t rank = lo; rank <= hi; ++rank) {
            RectI tail = original[order[rank]];
            tail.x += (tail.w + kTileGapPx - 1) / kStep * kStep;
            tail.y += (tail.h + kTileGapPx - 1) / kStep * kStep;
            if (Before(upper, tail)) upper = tail;
        }
        if (Before(target.rect, lower)) lower = target.rect;
        if (Before(upper, target.rect)) upper = target.rect;
        std::function<bool(size_t, size_t)> search = [&](size_t at, size_t first_cell) {
            if (at == affected.size()) return true;
            for (size_t cell = first_cell; cell < cells.size(); ++cell) {
                if (result.visited_states >= budget.max_states) { exhausted = true; return false; }
                ++result.visited_states;
                RectI rect{cells[cell].x, cells[cell].y, original[affected[at]].w, original[affected[at]].h};
                const bool grid_x = rect.x == work_area.x || rect.right() == work_area.right() || (rect.x >= ax && (rect.x - ax) % kStep == 0);
                const bool grid_y = rect.y == work_area.y || rect.bottom() == work_area.bottom() || (rect.y >= ay && (rect.y - ay) % kStep == 0);
                if (!grid_x || !grid_y) continue;
                if (Before(rect, lower) || Before(upper, rect)) continue;
                if (forward ? !Before(rect, target.rect) : !Before(target.rect, rect)) continue;
                const auto reserved = WorkFootprint(rect, work_area);
                if (!Contains(work_area, rect) || Conflicts(reserved, occupied)) continue;
                chosen[at] = rect;
                occupied.push_back(reserved);
                if (search(at + 1, cell + 1)) return true;
                occupied.pop_back();
                if (exhausted) return false;
            }
            return false;
        };
        if (search(0, 0)) {
            Place(result.updated_tiles[moving_index], target.rect);
            for (size_t i = 0; i < affected.size(); ++i) Place(result.updated_tiles[affected[i]], chosen[i]);
            result.valid = true; result.reason = TileReorderStatus::Success;
            return result;
        }
        if (exhausted) { result.reason = TileReorderStatus::BudgetExceeded; return result; }
        // Expand only the adjacent end that can release space in insertion order.
        if (forward && lo > 0) --lo;
        else if (!forward && hi + 1 < order.size()) ++hi;
        else { result.reason = TileReorderStatus::NoSpace; return result; }
    }
}
}
