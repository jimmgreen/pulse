// tile_board.cpp - 5x7 Live Tile Board factory, pixel geometry, and displacement solver.
#include "desktop_model.h"

#include <algorithm>
#include <cwctype>
#include <array>
#include <cstdlib>
#include <limits>

namespace pulse::desktop::model {
namespace {

constexpr std::array<TileSize, 6> kAllowedTileSizeTable{{
    {1, 1},
    {2, 1},
    {4, 1},
    {2, 2},
    {4, 2},
    {4, 4},
}};

using OccupancyGrid = std::array<std::array<bool, kTileCols>, kTileRows>;

bool FitsWithinBoard(int col, int row, TileSize sz) noexcept {
    return sz.cols > 0 && sz.rows > 0 &&
           col >= 0 && row >= 0 &&
           col + sz.cols <= kTileCols &&
           row + sz.rows <= kTileRows;
}

int ClampSnappedTileCoord(int v, int lo, int hi) noexcept {
    if (lo >= hi) return lo;
    return std::clamp(SnapCoord(v, kSnapGridPx), lo, hi);
}

RectI ComputeTileWorkBounds(int w, int h, const RectI& work_area) noexcept {
    if (work_area.empty()) {
        return {-32768, -32768, 65536, 65536};
    }
    const int min_x = work_area.x + kSnapGridPx;
    const int max_x = std::max(min_x, work_area.right() - kSnapGridPx - w);
    const int min_y = work_area.y + kSnapGridPx;
    const int max_y = std::max(min_y, work_area.bottom() - kSnapGridPx - h);
    return {min_x, min_y, max_x - min_x, max_y - min_y};
}

void AppendUniqueInt(std::vector<int>& vals, int v, int lo, int hi) {
    if (v < lo || v > hi) return;
    if (std::find(vals.begin(), vals.end(), v) == vals.end()) vals.push_back(v);
}

std::optional<PointI> FindBestTileSpotOnDesktop(
    int want_x, int want_y, int w, int h,
    const RectI& work_area,
    std::span<const RectI> tile_obstacles,
    std::span<const RectI> extra_obstacles,
    int pad) {
    const RectI b = ComputeTileWorkBounds(w, h, work_area);
    const int min_x = b.x;
    const int max_x = b.x + b.w;
    const int min_y = b.y;
    const int max_y = b.y + b.h;
    const int cx0 = ClampSnappedTileCoord(want_x, min_x, max_x);
    const int cy0 = ClampSnappedTileCoord(want_y, min_y, max_y);

    auto fits = [&](int x, int y) noexcept -> bool {
        if (x < min_x || x > max_x || y < min_y || y > max_y) return false;
        const RectI cand{x, y, w, h};
        for (const RectI& o : tile_obstacles) {
            if (!o.empty() && cand.intersects(o, pad)) return false;
        }
        for (const RectI& o : extra_obstacles) {
            if (!o.empty() && cand.intersects(o, pad)) return false;
        }
        return true;
    };

    if (fits(cx0, cy0)) return PointI{cx0, cy0};

    std::vector<int> xs;
    std::vector<int> ys;
    xs.reserve(16 + (tile_obstacles.size() + extra_obstacles.size()) * 4);
    ys.reserve(16 + (tile_obstacles.size() + extra_obstacles.size()) * 4);

    AppendUniqueInt(xs, cx0, min_x, max_x);
    AppendUniqueInt(xs, min_x, min_x, max_x);
    AppendUniqueInt(xs, max_x, min_x, max_x);
    AppendUniqueInt(ys, cy0, min_y, max_y);
    AppendUniqueInt(ys, min_y, min_y, max_y);
    AppendUniqueInt(ys, max_y, min_y, max_y);

    auto add_obs_edges = [&](const RectI& o) {
        if (o.empty()) return;
        const int left_of = o.x - w - pad;
        const int right_of = o.right() + pad;
        const int align_right = o.right() - w;
        AppendUniqueInt(xs, SnapCoord(left_of, kSnapGridPx), min_x, max_x);
        AppendUniqueInt(xs, SnapCoord(right_of, kSnapGridPx), min_x, max_x);
        AppendUniqueInt(xs, SnapCoord(o.x, kSnapGridPx), min_x, max_x);
        AppendUniqueInt(xs, SnapCoord(align_right, kSnapGridPx), min_x, max_x);

        const int above = o.y - h - pad;
        const int below = o.bottom() + pad;
        const int align_bot = o.bottom() - h;
        AppendUniqueInt(ys, SnapCoord(above, kSnapGridPx), min_y, max_y);
        AppendUniqueInt(ys, SnapCoord(below, kSnapGridPx), min_y, max_y);
        AppendUniqueInt(ys, SnapCoord(o.y, kSnapGridPx), min_y, max_y);
        AppendUniqueInt(ys, SnapCoord(align_bot, kSnapGridPx), min_y, max_y);
    };
    for (const RectI& o : tile_obstacles) add_obs_edges(o);
    for (const RectI& o : extra_obstacles) add_obs_edges(o);

    std::optional<PointI> best;
    int64_t best_dist = std::numeric_limits<int64_t>::max();
    for (const int y : ys) {
        for (const int x : xs) {
            if (!fits(x, y)) continue;
            const int64_t dx = static_cast<int64_t>(x) - want_x;
            const int64_t dy = static_cast<int64_t>(y) - want_y;
            const int64_t d2 = dx * dx + dy * dy;
            if (!best.has_value() || d2 < best_dist ||
                (d2 == best_dist && (y < best->y || (y == best->y && x < best->x)))) {
                best_dist = d2;
                best = PointI{x, y};
            }
        }
    }
    if (best.has_value()) return best;

    // Coarse sweep over work area when tight obstacle edges are exhausted.
    const int step = kTileGapPx * 4;
    for (int y = min_y; y <= max_y; y += step) {
        for (int x = min_x; x <= max_x; x += step) {
            if (!fits(x, y)) continue;
            const int64_t dx = static_cast<int64_t>(x) - want_x;
            const int64_t dy = static_cast<int64_t>(y) - want_y;
            const int64_t d2 = dx * dx + dy * dy;
            if (!best.has_value() || d2 < best_dist ||
                (d2 == best_dist && (y < best->y || (y == best->y && x < best->x)))) {
                best_dist = d2;
                best = PointI{x, y};
            }
        }
    }
    return best;
}

void RelocateDisplacedFreeTiles(std::vector<DesktopTile>& tiles, size_t active_idx,
                                const RectI& work_area, std::span<const RectI> group_frames) {
    const size_t n = tiles.size();
    std::vector<PointI> intended(n);
    for (size_t i = 0; i < n; ++i) {
        if (i == active_idx) {
            intended[i] = {tiles[i].x, tiles[i].y};
        } else if (tiles[i].rest_origin.has_value()) {
            intended[i] = *tiles[i].rest_origin;
        } else {
            intended[i] = {tiles[i].x, tiles[i].y};
        }
    }

    const RectI active_rect{tiles[active_idx].x, tiles[active_idx].y, tiles[active_idx].w, tiles[active_idx].h};
    std::vector<RectI> placed;
    placed.reserve(n);
    placed.push_back(active_rect);

    std::vector<size_t> displaced;
    displaced.reserve(n);

    // Keep non-overlapping tiles anchored at their intended coordinates first so distant tiles never jitter.
    for (size_t i = 0; i < n; ++i) {
        if (i == active_idx) continue;
        const RectI cand{intended[i].x, intended[i].y, tiles[i].w, tiles[i].h};
        bool overlaps = false;
        for (const RectI& p : placed) {
            if (cand.intersects(p, kTileGapPx)) {
                overlaps = true;
                break;
            }
        }
        if (!overlaps) {
            tiles[i].x = intended[i].x;
            tiles[i].y = intended[i].y;
            tiles[i].rest_origin.reset();
            placed.push_back(cand);
        } else {
            displaced.push_back(i);
        }
    }

    std::stable_sort(displaced.begin(), displaced.end(), [&](size_t a, size_t b) {
        if (intended[a].y != intended[b].y) return intended[a].y < intended[b].y;
        return intended[a].x < intended[b].x;
    });

    for (const size_t idx : displaced) {
        DesktopTile& t = tiles[idx];
        // Prefer spaces that avoid both tiles and groups; fall back to avoiding tiles only.
        auto spot = FindBestTileSpotOnDesktop(intended[idx].x, intended[idx].y, t.w, t.h,
                                              work_area, placed, group_frames, kTileGapPx);
        if (!spot.has_value() && !group_frames.empty()) {
            spot = FindBestTileSpotOnDesktop(intended[idx].x, intended[idx].y, t.w, t.h,
                                             work_area, placed, {}, kTileGapPx);
        }
        if (!spot.has_value()) {
            spot = FindBestTileSpotOnDesktop(intended[idx].x, intended[idx].y, t.w, t.h,
                                             work_area, placed, {}, 0);
        }
        if (spot.has_value()) {
            t.x = spot->x;
            t.y = spot->y;
        }
        if (PointI{t.x, t.y} != intended[idx]) {
            t.rest_origin = intended[idx];
        } else {
            t.rest_origin.reset();
        }
        placed.push_back({t.x, t.y, t.w, t.h});
    }
}

bool CanPlaceInGrid(const OccupancyGrid& occ, int col, int row, TileSize sz) noexcept {
    if (!FitsWithinBoard(col, row, sz)) {
        return false;
    }
    for (int r = row; r < row + sz.rows; ++r) {
        for (int c = col; c < col + sz.cols; ++c) {
            if (occ[static_cast<size_t>(r)][static_cast<size_t>(c)]) {
                return false;
            }
        }
    }
    return true;
}

void MarkGrid(OccupancyGrid& occ, int col, int row, TileSize sz) noexcept {
    for (int r = row; r < row + sz.rows; ++r) {
        for (int c = col; c < col + sz.cols; ++c) {
            occ[static_cast<size_t>(r)][static_cast<size_t>(c)] = true;
        }
    }
}

bool TilesOverlap(const DesktopTile& a, const DesktopTile& b) noexcept {
    const RectI ra{a.col, a.row, a.size.cols, a.size.rows};
    const RectI rb{b.col, b.row, b.size.cols, b.size.rows};
    return ra.intersects(rb, 0);
}

// Preserve unaffected tiles and backtrack when nearest placement fragments space.
bool TryRelocateDisplaced(std::vector<DesktopTile>& working,
                          const OccupancyGrid& occ,
                          std::span<const size_t> order,
                          size_t depth, size_t& remaining_nodes) {
    if (depth == order.size()) return true;
    if (remaining_nodes == 0) return false;
    --remaining_nodes;
    DesktopTile& tile = working[order[depth]];
    const int old_col = tile.col;
    const int old_row = tile.row;
    struct Candidate { int col; int row; int distance; };
    std::vector<Candidate> candidates;
    for (int r = 0; r <= kTileRows - tile.size.rows; ++r) {
        for (int c = 0; c <= kTileCols - tile.size.cols; ++c) {
            if (CanPlaceInGrid(occ, c, r, tile.size)) {
                candidates.push_back({c, r, std::abs(c - old_col) + std::abs(r - old_row)});
            }
        }
    }
    std::stable_sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) {
        return a.distance < b.distance;
    });
    for (const auto& candidate : candidates) {
        OccupancyGrid next = occ;
        MarkGrid(next, candidate.col, candidate.row, tile.size);
        tile.col = candidate.col;
        tile.row = candidate.row;
        if (TryRelocateDisplaced(working, next, order, depth + 1, remaining_nodes)) return true;
    }
    tile.col = old_col;
    tile.row = old_row;
    return false;
}

TilePlacementResult SolvePlacementWithActiveIndex(std::vector<DesktopTile> working, size_t active_idx) {
    const DesktopTile& active = working[active_idx];
    if (!IsValidTileSize(active.size) || !FitsWithinBoard(active.col, active.row, active.size)) {
        return {false, {}};
    }

    OccupancyGrid base_occ{};
    MarkGrid(base_occ, active.col, active.row, active.size);

    std::vector<size_t> displaced;
    displaced.reserve(working.size());

    for (size_t i = 0; i < working.size(); ++i) {
        if (i == active_idx) {
            continue;
        }
        const DesktopTile& t = working[i];
        if (!IsValidTileSize(t.size) || !FitsWithinBoard(t.col, t.row, t.size)) {
            return {false, {}};
        }
        if (TilesOverlap(active, t)) {
            displaced.push_back(i);
        } else {
            if (!CanPlaceInGrid(base_occ, t.col, t.row, t.size)) {
                return {false, {}};
            }
            MarkGrid(base_occ, t.col, t.row, t.size);
        }
    }

    std::stable_sort(displaced.begin(), displaced.end(), [&](size_t a, size_t b) {
        return working[a].size.cols * working[a].size.rows >
               working[b].size.cols * working[b].size.rows;
    });
    // Pointer-move previews must remain bounded for pathological imported boards.
    size_t remaining_nodes = 20000;
    if (TryRelocateDisplaced(working, base_occ, displaced, 0, remaining_nodes)) {
        return {true, std::move(working)};
    }

    return {false, {}};
}

} // namespace

bool IsValidTileSize(TileSize sz) noexcept {
    return sz.cols >= 1 && sz.cols <= kTileMaxCols &&
           sz.rows >= 1 && sz.rows <= kTileMaxRows;
}

std::span<const TileSize> AllowedTileSizes() noexcept {
    return kAllowedTileSizeTable;
}

int DefaultTileSpanPx(int units) noexcept {
    const int u = std::clamp(units, 1, std::max(kTileMaxCols, kTileMaxRows));
    return u * kTileUnitPx + (u - 1) * kTileGapPx;
}

TileSize EffectiveTileUnits(int w, int h) noexcept {
    const int step = kTileUnitPx + kTileGapPx;
    const int cols = std::clamp((std::max(w, kTileMinW) + kTileGapPx + kTileUnitPx / 2) / step, 1, kTileMaxCols);
    const int rows = std::clamp((std::max(h, kTileMinH) + kTileGapPx + kTileUnitPx / 2) / step, 1, kTileMaxRows);
    return {cols, rows};
}

std::vector<DesktopTile> MakeDefaultTiles() {
    std::vector<DesktopTile> tiles;
    tiles.reserve(10);

    DesktopTile photos{};
    photos.id = "photos";
    photos.kind = TileKind::Photos;
    photos.title = L"照片";
    photos.col = 0;
    photos.row = 0;
    photos.size = {4, 2};
    tiles.push_back(std::move(photos));

    DesktopTile recycle_bin{};
    recycle_bin.id = "recycle_bin";
    recycle_bin.kind = TileKind::RecycleBin;
    recycle_bin.title = L"回收站";
    recycle_bin.col = 4;
    recycle_bin.row = 0;
    recycle_bin.size = {1, 1};
    tiles.push_back(std::move(recycle_bin));

    DesktopTile this_pc{};
    this_pc.id = "this_pc";
    this_pc.kind = TileKind::ThisPC;
    this_pc.title = L"此电脑";
    this_pc.col = 4;
    this_pc.row = 1;
    this_pc.size = {1, 1};
    tiles.push_back(std::move(this_pc));

    DesktopTile downloads{};
    downloads.id = "downloads";
    downloads.kind = TileKind::Downloads;
    downloads.title = L"下载";
    downloads.col = 0;
    downloads.row = 2;
    downloads.size = {2, 2};
    tiles.push_back(std::move(downloads));

    // A smart stack, as on a phone: 最近使用, 日历 and 天气 share one medium slot.
    DesktopTile recent{};
    recent.id = "recent";
    recent.kind = TileKind::Recent;
    recent.title = L"最近使用";
    recent.col = 2;
    recent.row = 2;
    recent.size = {2, 2};
    recent.stack = {{TileKind::Recent, L"最近使用", L"", "recent"},
                    {TileKind::Calendar, L"日历", L"", "calendar"},
                    {TileKind::Weather, L"天气", L"", "weather"}};
    tiles.push_back(std::move(recent));

    DesktopTile disk{};
    disk.id = "disk_ring";
    disk.kind = TileKind::DiskRing;
    disk.title = L"本地磁盘";
    disk.col = 4;
    disk.row = 2;
    disk.size = {1, 1};
    tiles.push_back(std::move(disk));

    DesktopTile launcher{};
    launcher.id = "pulse_launcher";
    launcher.kind = TileKind::PulseLauncher;
    launcher.title = L"Pulse";
    launcher.col = 4;
    launcher.row = 3;
    launcher.size = {1, 1};
    tiles.push_back(std::move(launcher));

    // The desktop's app shortcuts in one place (most people have more apps than source folders).
    DesktopTile apps{};
    apps.id = "apps";
    apps.kind = TileKind::Apps;
    apps.title = L"应用";
    apps.col = 0;
    apps.row = 4;
    apps.size = {4, 2};
    tiles.push_back(std::move(apps));

    DesktopTile clock{};
    clock.id = "clock";
    clock.kind = TileKind::Clock;
    clock.title = L"时钟";
    clock.analog_clock = true;
    clock.col = 4;
    clock.row = 4;
    clock.size = {1, 1};
    tiles.push_back(std::move(clock));

    DesktopTile staging{};
    staging.id = "staging_tray";
    staging.kind = TileKind::StagingTray;
    staging.title = L"暂存区";
    staging.col = 0;
    staging.row = 6;
    staging.size = {4, 1};
    tiles.push_back(std::move(staging));

    return tiles;
}

std::span<const TileSize> AllowedTileSizesFor(TileKind kind) noexcept {
    // Rich presets per kind; users can additionally drag edges/corners to any free width/height.
    static constexpr TileSize kDefault[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {3, 2}, {4, 3}, {4, 4}, {4, 6}};
    static constexpr TileSize kWidget[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 3}, {4, 4}};
    static constexpr TileSize kTodo[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 3}, {4, 4}, {4, 6}};
    static constexpr TileSize kApps[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 3}, {4, 4}, {4, 6}};
    static constexpr TileSize kClock[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 4}};
    static constexpr TileSize kNote[] = {
        {1, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 3}, {4, 4}, {4, 6}};
    static constexpr TileSize kTray[] = {
        {1, 1}, {2, 1}, {4, 1}, {2, 2}, {4, 2}, {2, 3}, {4, 3}, {4, 4}};
    static constexpr TileSize kCompact[] = {{1, 1}, {2, 1}, {2, 2}, {4, 2}};
    switch (kind) {
    case TileKind::Weather: case TileKind::Calendar: case TileKind::System:
    case TileKind::Pomodoro: case TileKind::Countdown: case TileKind::Battery:
        return kWidget;
    case TileKind::MediaPlayer: return kTray;
    case TileKind::Todo: return kTodo;
    case TileKind::Apps: return kApps;
    case TileKind::Clock: return kClock;
    case TileKind::StickyNote: return kNote;
    case TileKind::StagingTray: return kTray;
    case TileKind::RecycleBin: case TileKind::ThisPC: case TileKind::DiskRing: case TileKind::PulseLauncher:
        return kCompact;
    default: return kDefault;
    }
}

DesktopTile ActiveTileView(const DesktopTile& tile) {
    if (tile.stack.empty()) return tile;
    DesktopTile view = tile;
    const auto& m = tile.stack[static_cast<size_t>(std::clamp(tile.stack_index, 0, static_cast<int>(tile.stack.size()) - 1))];
    view.kind = m.kind;
    view.title = m.title;
    view.target_path = m.target_path;
    view.analog_clock = m.analog_clock;
    return view;
}

namespace {
std::vector<TileStackEntry> MembersOf(const DesktopTile& t) {
    if (!t.stack.empty()) return t.stack;
    return {{t.kind, t.title, t.target_path, t.id, t.analog_clock}};
}
} // namespace

bool CanStackTiles(const std::vector<DesktopTile>& tiles, std::string_view moving_id, std::string_view target_id) {
    if (moving_id == target_id) return false;
    const auto mv = std::find_if(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == moving_id; });
    const auto tg = std::find_if(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == target_id; });
    if (mv == tiles.end() || tg == tiles.end()) return false;
    const auto members = MembersOf(*tg);
    const auto incoming = MembersOf(*mv);
    if (members.size() + incoming.size() > 6) return false;  // a stack of more than six is a drawer, not a glance
    const auto fits = [&](const TileStackEntry& m) {
        return m.kind != TileKind::Apps;  // its pages and wheel would fight the stack's
    };
    return std::all_of(members.begin(), members.end(), fits) && std::all_of(incoming.begin(), incoming.end(), fits);
}

bool StackTiles(std::vector<DesktopTile>& tiles, std::string_view moving_id, std::string_view target_id) {
    if (!CanStackTiles(tiles, moving_id, target_id)) return false;
    auto mv = std::find_if(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == moving_id; });
    auto tg = std::find_if(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == target_id; });
    auto members = MembersOf(*tg);
    auto incoming = MembersOf(*mv);
    members.insert(members.end(), incoming.begin(), incoming.end());
    tg->stack = std::move(members);
    tg->stack_index = static_cast<int>(tg->stack.size()) - 1;
    tiles.erase(mv);
    return true;
}

bool UnstackActiveMember(std::vector<DesktopTile>& tiles, std::string_view id) {
    auto it = std::find_if(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == id; });
    if (it == tiles.end() || it->stack.size() < 2) return false;
    const size_t at = static_cast<size_t>(std::clamp(it->stack_index, 0, static_cast<int>(it->stack.size()) - 1));
    const TileStackEntry m = it->stack[at];
    DesktopTile out{};
    out.id = m.origin_id.empty() ? ("tile_" + std::to_string(tiles.size() + 1)) : m.origin_id;
    while (std::any_of(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return t.id == out.id; })) out.id += "_";
    out.kind = m.kind;
    out.title = m.title;
    out.target_path = m.target_path;
    out.analog_clock = m.analog_clock;
    out.size = it->size;
    const auto sizes = AllowedTileSizesFor(m.kind);
    if (std::find(sizes.begin(), sizes.end(), out.size) == sizes.end()) out.size = sizes.front();
    const bool any_free = std::any_of(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.free_placed; });
    std::vector<DesktopTile> trial = tiles;
    auto& src = *std::find_if(trial.begin(), trial.end(), [&](const DesktopTile& t) { return t.id == id; });
    src.stack.erase(src.stack.begin() + static_cast<std::ptrdiff_t>(at));
    src.stack_index = 0;
    if (src.stack.size() == 1) {
        const TileStackEntry only = src.stack.front();
        src.kind = only.kind; src.title = only.title; src.target_path = only.target_path; src.analog_clock = only.analog_clock;
        src.stack.clear();
    }
    if (!any_free) {
        for (const TileSize sz : {out.size, TileSize{1, 1}}) {
            out.size = sz;
            const auto cell = FindFreeTileCell(trial, sz);
            if (!cell) continue;
            out.col = cell->x;
            out.row = cell->y;
            auto placed = InsertNewTile(trial, out);
            if (placed.valid) { tiles = std::move(placed.updated_tiles); return true; }
        }
    }
    const RectI fallback_wa{0, 0, 1920, 1032};
    const RectI fallback_br = ComputeTileBoardRect(fallback_wa, true);
    out.w = DefaultTileSpanPx(out.size.cols);
    out.h = DefaultTileSpanPx(out.size.rows);
    const PointI spot = FindFreeDesktopSpotForTile(trial, {}, fallback_wa, fallback_br, out.w, out.h);
    out.free_placed = true;
    out.x = spot.x;
    out.y = spot.y;
    trial.push_back(std::move(out));
    tiles = std::move(trial);
    return true;
}

std::optional<PointI> FindFreeTileCell(std::span<const DesktopTile> tiles, TileSize size) {
    for (int row = 0; row + size.rows <= kTileRows; ++row) {
        for (int col = 0; col + size.cols <= kTileCols; ++col) {
            DesktopTile probe{};
            probe.col = col; probe.row = row; probe.size = size;
            if (std::none_of(tiles.begin(), tiles.end(), [&](const DesktopTile& t) { return TilesOverlap(probe, t); }))
                return PointI{col, row};
        }
    }
    return std::nullopt;
}

bool BoardHasKind(std::span<const DesktopTile> tiles, TileKind kind) noexcept {
    for (const auto& t : tiles) {
        if (t.stack.empty() ? t.kind == kind
                            : std::any_of(t.stack.begin(), t.stack.end(), [&](const TileStackEntry& m) { return m.kind == kind; }))
            return true;
    }
    return false;
}

bool IsAppShortcut(const DesktopItem& item) noexcept {
    if (!item.is_shortcut || item.is_folder || item.extension == L".url") return false;  // .url: a web link
    // Store and advertised shortcuts have no file target; anything else must point at a program.
    if (item.link_target.empty()) return true;
    const auto dot = item.link_target.find_last_of(L'.');
    const auto slash = item.link_target.find_last_of(L"\\/");
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return false;  // a folder
    std::wstring ext = item.link_target.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(std::towlower(c));
    return ext == L".exe" || ext == L".com" || ext == L".bat" || ext == L".cmd" || ext == L".msc";
}

void ApplyWidgetDefaults(DesktopModelState& state) {
    if (state.widget_defaults_applied) return;
    state.widget_defaults_applied = true;
    if (BoardHasKind(state.tiles, TileKind::Weather) || BoardHasKind(state.tiles, TileKind::Calendar)) return;
    for (auto& t : state.tiles) {
        if (t.kind != TileKind::Recent || !t.stack.empty() || !(t.size == TileSize{2, 2})) continue;
        t.stack = {{TileKind::Recent, t.title.empty() ? L"最近使用" : t.title, t.target_path, t.id},
                   {TileKind::Calendar, L"日历", L"", "calendar"},
                   {TileKind::Weather, L"天气", L"", "weather"}};
        t.stack_index = 0;
        t.smart_rotate = true;
        return;
    }
}

void UpdateAppsTileMembership(DesktopModelState& state) {
    const bool on = state.settings.organize_enabled && state.settings.tiles_enabled &&
                    BoardHasKind(state.tiles, TileKind::Apps);
    for (auto& item : state.items) {
        if (item.hidden_by_mapped_group || !item.mapped_owner_group_id.empty()) {
            item.in_apps_tile = false;
            continue;
        }
        const bool pinned_here = item.pinned_group_id == kAppsTileGroupId;
        const bool rule = IsAppShortcut(item) && !item.force_loose && item.pinned_group_id.empty();
        item.in_apps_tile = on && (pinned_here || rule);
    }
}

void MigrateDefaultTiles(std::vector<DesktopTile>& tiles) {
    if (BoardHasKind(tiles, TileKind::Apps)) return;
    for (auto& t : tiles) {
        if (t.id == "pinned_folder" && t.kind == TileKind::PinnedFolder && t.title == L"源代码" && t.target_path.empty()) {
            t.id = "apps"; t.kind = TileKind::Apps; t.title = L"应用";
            return;
        }
    }
}

std::vector<TodoItem> MakeDefaultTodos() {
    return {{L"核对预算表", false}, {L"合同扫描件发法务", false}, {L"订周五的会议室", true}, {L"给设计评审写纪要", true}};
}

RectI ComputeTilePixelRect(const RectI& board_rect, const DesktopTile& tile) noexcept {
    if (board_rect.empty() || tile.size.cols <= 0 || tile.size.rows <= 0) {
        return {0, 0, 0, 0};
    }

    constexpr int kStdW = kTileCols * kTileUnitPx + (kTileCols - 1) * kTileGapPx; // 532
    constexpr int kStdH = kTileRows * kTileUnitPx + (kTileRows - 1) * kTileGapPx; // 748

    const int gap_x = std::clamp(
        (board_rect.w * kTileGapPx + kStdW / 2) / kStdW,
        0, board_rect.w / kTileCols);
    const int gap_y = std::clamp(
        (board_rect.h * kTileGapPx + kStdH / 2) / kStdH,
        0, board_rect.h / kTileRows);

    const int usable_w = std::max(0, board_rect.w - (kTileCols - 1) * gap_x);
    const int usable_h = std::max(0, board_rect.h - (kTileRows - 1) * gap_y);

    const int col_end = tile.col + tile.size.cols;
    const int row_end = tile.row + tile.size.rows;

    const int x0 = (tile.col * usable_w) / kTileCols + tile.col * gap_x;
    const int x1 = (col_end * usable_w) / kTileCols + (col_end - 1) * gap_x;
    const int y0 = (tile.row * usable_h) / kTileRows + tile.row * gap_y;
    const int y1 = (row_end * usable_h) / kTileRows + (row_end - 1) * gap_y;

    return {
        board_rect.x + x0,
        board_rect.y + y0,
        std::max(0, x1 - x0),
        std::max(0, y1 - y0),
    };
}

RectI ResolveTileRect(const DesktopTile& tile, const RectI& default_board_rect) noexcept {
    if (tile.free_placed) {
        const int rw = tile.w > 0 ? std::max(tile.w, kTileMinW) : DefaultTileSpanPx(tile.size.cols);
        const int rh = tile.h > 0 ? std::max(tile.h, kTileMinH) : DefaultTileSpanPx(tile.size.rows);
        return {tile.x, tile.y, rw, rh};
    }
    RectI r = ComputeTilePixelRect(default_board_rect, tile);
    if (r.empty()) {
        return {tile.x, tile.y,
                tile.w > 0 ? std::max(tile.w, kTileMinW) : DefaultTileSpanPx(tile.size.cols),
                tile.h > 0 ? std::max(tile.h, kTileMinH) : DefaultTileSpanPx(tile.size.rows)};
    }
    if (tile.w > 0) r.w = std::max(tile.w, kTileMinW);
    if (tile.h > 0) r.h = std::max(tile.h, kTileMinH);
    return r;
}

std::vector<RectI> CollectTileObstacleRects(std::span<const DesktopTile> tiles,
                                            const RectI& default_board_rect,
                                            bool tiles_visible) noexcept {
    std::vector<RectI> rects;
    if (!tiles_visible || tiles.empty()) return rects;
    rects.reserve(tiles.size());
    for (const DesktopTile& t : tiles) {
        const RectI r = ResolveTileRect(t, default_board_rect);
        if (!r.empty()) rects.push_back(r);
    }
    return rects;
}

void MaterializeFreeTiles(std::vector<DesktopTile>& tiles, const RectI& default_board_rect) noexcept {
    for (DesktopTile& t : tiles) {
        if (t.free_placed) {
            if (t.w <= 0) t.w = DefaultTileSpanPx(t.size.cols);
            if (t.h <= 0) t.h = DefaultTileSpanPx(t.size.rows);
            t.w = std::max(t.w, kTileMinW);
            t.h = std::max(t.h, kTileMinH);
            continue;
        }
        const RectI r = ResolveTileRect(t, default_board_rect);
        t.free_placed = true;
        t.x = r.x;
        t.y = r.y;
        t.w = std::max(r.w, kTileMinW);
        t.h = std::max(r.h, kTileMinH);
    }
}

void MoveTileFreeInteractive(std::vector<DesktopTile>& tiles, size_t tile_index,
                             int proposed_x, int proposed_y,
                             const RectI& work_area, const RectI& default_board_rect,
                             std::span<const RectI> group_frames) {
    if (tile_index >= tiles.size()) return;
    MaterializeFreeTiles(tiles, default_board_rect);

    DesktopTile& active = tiles[tile_index];
    const int aw = active.w;
    const int ah = active.h;

    // Magnetic edge snap (within 10px) against non-active tile origins/rest_origins and group edges.
    constexpr int kMagnetThresholdPx = 10;
    int snapped_x = SnapCoord(proposed_x, kSnapGridPx);
    int snapped_y = SnapCoord(proposed_y, kSnapGridPx);
    int best_dx = kMagnetThresholdPx + 1;
    int best_dy = kMagnetThresholdPx + 1;

    auto try_snap_x = [&](int cand_x) {
        const int d = std::abs(cand_x - proposed_x);
        if (d < best_dx) {
            best_dx = d;
            snapped_x = cand_x;
        }
    };
    auto try_snap_y = [&](int cand_y) {
        const int d = std::abs(cand_y - proposed_y);
        if (d < best_dy) {
            best_dy = d;
            snapped_y = cand_y;
        }
    };

    if (!work_area.empty()) {
        try_snap_x(work_area.x + kLooseMarginX);
        try_snap_x(work_area.right() - kTileBoardRightMargin - aw);
        try_snap_y(work_area.y + kTileBoardTopMargin);
        try_snap_y(work_area.bottom() - kLooseMarginY - ah);
    }
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (i == tile_index) continue;
        const PointI pt = tiles[i].rest_origin ? *tiles[i].rest_origin : PointI{tiles[i].x, tiles[i].y};
        const RectI r{pt.x, pt.y, tiles[i].w, tiles[i].h};
        try_snap_x(r.x);
        try_snap_x(r.right() - aw);
        try_snap_x(r.right() + kTileGapPx);
        try_snap_x(r.x - aw - kTileGapPx);
        try_snap_y(r.y);
        try_snap_y(r.bottom() - ah);
        try_snap_y(r.bottom() + kTileGapPx);
        try_snap_y(r.y - ah - kTileGapPx);
    }
    for (const RectI& gf : group_frames) {
        if (gf.empty()) continue;
        try_snap_x(gf.x);
        try_snap_x(gf.right() - aw);
        try_snap_x(gf.right() + kSnapGridPx);
        try_snap_x(gf.x - aw - kSnapGridPx);
        try_snap_y(gf.y);
        try_snap_y(gf.bottom() - ah);
        try_snap_y(gf.bottom() + kSnapGridPx);
        try_snap_y(gf.y - ah - kSnapGridPx);
    }

    const RectI b = ComputeTileWorkBounds(aw, ah, work_area);
    active.x = std::clamp(snapped_x, b.x, b.x + b.w);
    active.y = std::clamp(snapped_y, b.y, b.y + b.h);
    active.rest_origin.reset();

    RelocateDisplacedFreeTiles(tiles, tile_index, work_area, group_frames);
}

void ResizeTileFreeInteractive(std::vector<DesktopTile>& tiles, size_t tile_index,
                               int proposed_w, int proposed_h,
                               const RectI& work_area, const RectI& default_board_rect,
                               std::span<const RectI> group_frames) {
    if (tile_index >= tiles.size()) return;
    MaterializeFreeTiles(tiles, default_board_rect);

    DesktopTile& active = tiles[tile_index];
    const int max_w = work_area.empty()
        ? 3840
        : std::max(kTileMinW, work_area.right() - kSnapGridPx - active.x);
    const int max_h = work_area.empty()
        ? 2160
        : std::max(kTileMinH, work_area.bottom() - kSnapGridPx - active.y);

    auto snap_span = [](int raw, int min_v, int max_v) noexcept -> int {
        int v = SnapCoord(raw, kSnapGridPx);
        for (int u = 1; u <= std::max(kTileMaxCols, kTileMaxRows); ++u) {
            const int std_span = DefaultTileSpanPx(u);
            if (std::abs(raw - std_span) <= 6) {
                v = std_span;
                break;
            }
        }
        return std::clamp(v, min_v, max_v);
    };

    active.w = snap_span(proposed_w, kTileMinW, max_w);
    active.h = snap_span(proposed_h, kTileMinH, max_h);
    active.size = EffectiveTileUnits(active.w, active.h);
    active.rest_origin.reset();

    RelocateDisplacedFreeTiles(tiles, tile_index, work_area, group_frames);
}

void CommitTileRestOrigins(std::vector<DesktopTile>& tiles) noexcept {
    for (DesktopTile& t : tiles) {
        t.rest_origin.reset();
    }
}

PointI FindFreeDesktopSpotForTile(std::span<const DesktopTile> tiles,
                                  std::span<const RectI> group_frames,
                                  const RectI& work_area,
                                  const RectI& default_board_rect,
                                  int w, int h) noexcept {
    const int rw = std::max(w, kTileMinW);
    const int rh = std::max(h, kTileMinH);
    std::vector<RectI> tile_obs;
    tile_obs.reserve(tiles.size());
    for (const DesktopTile& t : tiles) {
        const RectI r = ResolveTileRect(t, default_board_rect);
        if (!r.empty()) tile_obs.push_back(r);
    }

    const int want_x = !default_board_rect.empty() ? default_board_rect.x : (work_area.right() - rw - kTileBoardRightMargin);
    const int want_y = !default_board_rect.empty() ? default_board_rect.y : (work_area.y + kTileBoardTopMargin);

    if (auto spot = FindBestTileSpotOnDesktop(want_x, want_y, rw, rh, work_area, tile_obs, group_frames, kTileGapPx)) {
        return *spot;
    }
    if (auto spot = FindBestTileSpotOnDesktop(want_x, want_y, rw, rh, work_area, tile_obs, {}, kTileGapPx)) {
        return *spot;
    }
    if (auto spot = FindBestTileSpotOnDesktop(want_x, want_y, rw, rh, work_area, tile_obs, {}, 0)) {
        return *spot;
    }
    const RectI b = ComputeTileWorkBounds(rw, rh, work_area);
    return {std::clamp(want_x, b.x, b.x + b.w), std::clamp(want_y, b.y, b.y + b.h)};
}

PointI ComputeTileDropCell(const RectI& board_rect, TileSize size, PointI top_left) noexcept {
    PointI best{};
    int64_t best_distance = std::numeric_limits<int64_t>::max();
    DesktopTile probe{};
    probe.size = size;
    for (int row = 0; row <= kTileRows - size.rows; ++row) {
        for (int col = 0; col <= kTileCols - size.cols; ++col) {
            probe.col = col;
            probe.row = row;
            const RectI rect = ComputeTilePixelRect(board_rect, probe);
            const int64_t dx = static_cast<int64_t>(top_left.x) - rect.x;
            const int64_t dy = static_cast<int64_t>(top_left.y) - rect.y;
            const int64_t distance = dx * dx + dy * dy;
            if (distance < best_distance) {
                best = {col, row};
                best_distance = distance;
            }
        }
    }
    return best;
}

TilePlacementResult PreviewTilePlacement(std::span<const DesktopTile> tiles,
                                         std::string_view moving_id,
                                         int target_col, int target_row, TileSize target_size) {
    if (moving_id.empty() || !IsValidTileSize(target_size) ||
        !FitsWithinBoard(target_col, target_row, target_size)) {
        return {false, {}};
    }

    std::optional<size_t> moving_idx;
    for (size_t i = 0; i < tiles.size(); ++i) {
        if (tiles[i].id == moving_id) {
            moving_idx = i;
            break;
        }
    }
    if (!moving_idx.has_value()) {
        return {false, {}};
    }

    std::vector<DesktopTile> working(tiles.begin(), tiles.end());
    working[*moving_idx].col = target_col;
    working[*moving_idx].row = target_row;
    working[*moving_idx].size = target_size;
    return SolvePlacementWithActiveIndex(std::move(working), *moving_idx);
}

TilePlacementResult InsertNewTile(std::span<const DesktopTile> tiles, const DesktopTile& new_tile) {
    if (!IsValidTileSize(new_tile.size) || !FitsWithinBoard(new_tile.col, new_tile.row, new_tile.size)) {
        return {false, {}};
    }
    if (!new_tile.id.empty()) {
        for (const DesktopTile& existing : tiles) {
            if (existing.id == new_tile.id) {
                return {false, {}};
            }
        }
    }

    std::vector<DesktopTile> working(tiles.begin(), tiles.end());
    working.push_back(new_tile);
    const size_t active_idx = working.size() - 1;
    return SolvePlacementWithActiveIndex(std::move(working), active_idx);
}

} // namespace pulse::desktop::model
