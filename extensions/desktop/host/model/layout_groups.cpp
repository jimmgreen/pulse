// layout_groups.cpp - Desktop group geometry, non-destructive push-away layout, and view slicing.
#include "desktop_model.h"
#include "group_tabs.h"

#include <algorithm>
#include <cctype>
#include <cwctype>
#include <limits>
#include <numeric>

namespace pulse::desktop::model {
namespace {

int64_t FloorDiv(int64_t a, int64_t b) noexcept {
    const int64_t q = a / b;
    const int64_t r = a % b;
    return (r != 0 && ((r < 0) != (b < 0))) ? (q - 1) : q;
}

int SnapDown(int value, int step) noexcept {
    if (step <= 1) {
        return value;
    }
    return static_cast<int>(FloorDiv(value, step) * step);
}

int SnapUp(int value, int step) noexcept {
    if (step <= 1) {
        return value;
    }
    return static_cast<int>(-FloorDiv(-static_cast<int64_t>(value), step) * step);
}

wchar_t FoldWideChar(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
}

int CompareWideIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const wchar_t ca = FoldWideChar(a[i]);
        const wchar_t cb = FoldWideChar(b[i]);
        if (ca < cb) {
            return -1;
        }
        if (ca > cb) {
            return 1;
        }
    }
    if (a.size() < b.size()) {
        return -1;
    }
    if (a.size() > b.size()) {
        return 1;
    }
    return 0;
}

bool EqualsWideIgnoreCase(std::wstring_view a, std::wstring_view b) noexcept {
    if (a.size() != b.size()) {
        return false;
    }
    for (size_t i = 0; i < a.size(); ++i) {
        if (FoldWideChar(a[i]) != FoldWideChar(b[i])) {
            return false;
        }
    }
    return true;
}

std::wstring_view EffectiveExtension(const DesktopItem& item) noexcept {
    if (!item.extension.empty()) {
        return item.extension;
    }
    if (item.is_folder) {
        return {};
    }
    const size_t dot = item.name.rfind(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        return {};
    }
    return std::wstring_view(item.name).substr(dot);
}

struct SnappedBounds {
    int min_x = 0;
    int max_x = 0;
    int min_y = 0;
    int max_y = 0;
};

SnappedBounds ComputeBoundsForSize(int w, int h, const RectI& work_area) noexcept {
    if (work_area.empty()) {
        return {-32768, 32768, -32768, 32768};
    }
    constexpr int kMargin = kSnapGridPx;
    const int raw_min_x = work_area.x + kMargin;
    const int raw_max_x = work_area.right() - kMargin - w;
    const int raw_min_y = work_area.y + kMargin;
    const int raw_max_y = work_area.bottom() - kMargin - h;

    SnappedBounds b{};
    if (raw_min_x <= raw_max_x) {
        b.min_x = SnapUp(raw_min_x, kSnapGridPx);
        b.max_x = SnapDown(raw_max_x, kSnapGridPx);
        if (b.min_x > b.max_x) {
            b.min_x = raw_min_x;
            b.max_x = raw_min_x;
        }
    } else {
        const int fallback_x = std::max(work_area.x, work_area.x + (work_area.w - w) / 2);
        b.min_x = fallback_x;
        b.max_x = fallback_x;
    }

    if (raw_min_y <= raw_max_y) {
        b.min_y = SnapUp(raw_min_y, kSnapGridPx);
        b.max_y = SnapDown(raw_max_y, kSnapGridPx);
        if (b.min_y > b.max_y) {
            b.min_y = raw_min_y;
            b.max_y = raw_min_y;
        }
    } else {
        const int fallback_y = std::max(work_area.y, work_area.y + (work_area.h - h) / 2);
        b.min_y = fallback_y;
        b.max_y = fallback_y;
    }
    return b;
}

int ClampSnapped(int v, int lo, int hi) noexcept {
    if (lo >= hi) {
        return lo;
    }
    return std::clamp(SnapCoord(v, kSnapGridPx), lo, hi);
}

void AppendUnique(std::vector<int>& vals, int v, int lo, int hi) {
    if (v < lo || v > hi) {
        return;
    }
    if (std::find(vals.begin(), vals.end(), v) == vals.end()) {
        vals.push_back(v);
    }
}

std::optional<PointI> FindBestGroupSpot(int want_x, int want_y, int w, int h,
                                        const RectI& work_area,
                                        const RectI& tile_board_rect,
                                        std::span<const RectI> placed,
                                        std::span<const RectI> extra_obstacles,
                                        int group_pad) {
    const SnappedBounds b = ComputeBoundsForSize(w, h, work_area);
    const int cx0 = ClampSnapped(want_x, b.min_x, b.max_x);
    const int cy0 = ClampSnapped(want_y, b.min_y, b.max_y);

    const auto fits = [&](int x, int y) noexcept -> bool {
        if (x < b.min_x || x > b.max_x || y < b.min_y || y > b.max_y) {
            return false;
        }
        const RectI r{x, y, w, h};
        if (!tile_board_rect.empty() && r.intersects(tile_board_rect, group_pad)) {
            return false;
        }
        for (const RectI& obs : extra_obstacles) {
            if (!obs.empty() && r.intersects(obs, group_pad)) {
                return false;
            }
        }
        for (const RectI& p : placed) {
            if (r.intersects(p, group_pad)) {
                return false;
            }
        }
        return true;
    };

    if (fits(cx0, cy0)) {
        return PointI{cx0, cy0};
    }

    std::vector<int> xs;
    std::vector<int> ys;
    xs.reserve(6 + (placed.size() + extra_obstacles.size() + 1) * 2);
    ys.reserve(6 + (placed.size() + extra_obstacles.size() + 1) * 2);

    AppendUnique(xs, cx0, b.min_x, b.max_x);
    AppendUnique(xs, b.min_x, b.min_x, b.max_x);
    AppendUnique(xs, b.max_x, b.min_x, b.max_x);

    AppendUnique(ys, cy0, b.min_y, b.max_y);
    AppendUnique(ys, b.min_y, b.min_y, b.max_y);
    AppendUnique(ys, b.max_y, b.min_y, b.max_y);

    const auto add_obstacle_edges = [&](const RectI& obs) {
        if (obs.empty()) {
            return;
        }
        AppendUnique(xs, SnapDown(obs.x - w - group_pad, kSnapGridPx), b.min_x, b.max_x);
        AppendUnique(xs, SnapUp(obs.right() + group_pad, kSnapGridPx), b.min_x, b.max_x);
        AppendUnique(xs, ClampSnapped(obs.x, b.min_x, b.max_x), b.min_x, b.max_x);

        AppendUnique(ys, SnapDown(obs.y - h - group_pad, kSnapGridPx), b.min_y, b.max_y);
        AppendUnique(ys, SnapUp(obs.bottom() + group_pad, kSnapGridPx), b.min_y, b.max_y);
        AppendUnique(ys, ClampSnapped(obs.y, b.min_y, b.max_y), b.min_y, b.max_y);
    };

    add_obstacle_edges(tile_board_rect);
    for (const RectI& obs : extra_obstacles) {
        add_obstacle_edges(obs);
    }
    for (const RectI& p : placed) {
        add_obstacle_edges(p);
    }

    std::optional<PointI> best;
    int64_t best_dist_sq = std::numeric_limits<int64_t>::max();

    for (const int y : ys) {
        for (const int x : xs) {
            if (!fits(x, y)) {
                continue;
            }
            const int64_t dx = static_cast<int64_t>(x) - want_x;
            const int64_t dy = static_cast<int64_t>(y) - want_y;
            const int64_t dist_sq = dx * dx + dy * dy;
            if (!best.has_value() || dist_sq < best_dist_sq ||
                (dist_sq == best_dist_sq && (y < best->y || (y == best->y && x < best->x)))) {
                best_dist_sq = dist_sq;
                best = PointI{x, y};
            }
        }
    }
    return best;
}

PointI ResolveSingleGroupSpot(int want_x, int want_y, int w, int h,
                              const RectI& work_area,
                              const RectI& tile_board_rect,
                              std::span<const RectI> placed,
                              std::span<const RectI> extra_obstacles) {
    if (auto spot = FindBestGroupSpot(want_x, want_y, w, h, work_area, tile_board_rect, placed, extra_obstacles, kSnapGridPx)) {
        return *spot;
    }
    if (auto spot = FindBestGroupSpot(want_x, want_y, w, h, work_area, tile_board_rect, placed, extra_obstacles, 0)) {
        return *spot;
    }
    if (!tile_board_rect.empty() || !extra_obstacles.empty()) {
        if (auto spot = FindBestGroupSpot(want_x, want_y, w, h, work_area, tile_board_rect, {}, extra_obstacles, 0)) {
            return *spot;
        }
        if (!extra_obstacles.empty()) {
            if (auto spot = FindBestGroupSpot(want_x, want_y, w, h, work_area, tile_board_rect, placed, {}, 0)) {
                return *spot;
            }
        }
    }
    const SnappedBounds b = ComputeBoundsForSize(w, h, work_area);
    return PointI{ClampSnapped(want_x, b.min_x, b.max_x), ClampSnapped(want_y, b.min_y, b.max_y)};
}

} // namespace

int SnapCoord(int value, int step) noexcept {
    if (step <= 1) {
        return value;
    }
    const int64_t v = value;
    const int64_t s = step;
    const int64_t q = (v >= 0) ? ((v + s / 2) / s) : ((v - (s - 1) / 2) / s);
    return static_cast<int>(q * s);
}

RectI ComputeTileBoardRect(const RectI& primary_work_area, bool tiles_enabled) noexcept {
    if (!tiles_enabled || primary_work_area.empty()) {
        return {0, 0, 0, 0};
    }

    constexpr int kStdW = kTileCols * kTileUnitPx + (kTileCols - 1) * kTileGapPx; // 532
    constexpr int kStdH = kTileRows * kTileUnitPx + (kTileRows - 1) * kTileGapPx; // 748

    if (primary_work_area.w >= 1200 && primary_work_area.h >= 820) {
        const int x = primary_work_area.right() - kStdW - kTileBoardRightMargin;
        const int y = primary_work_area.y + kTileBoardTopMargin;
        return {x, y, kStdW, kStdH};
    }

    // Narrow screen: proportionally shrink unit & gap and cap board within 44% width and (h - 40)
    // so minimum-width groups always fit cleanly on the left without overlap.
    constexpr int kMinGroupW = kGroupPaddingX * 2 + kGroupMinCols * kGroupCellW;
    const int max_w = std::min({
        kStdW,
        primary_work_area.w * 44 / 100,
        std::max(120, primary_work_area.w - kMinGroupW - 40),
    });
    const int max_h = std::min(kStdH, std::max(140, primary_work_area.h - 40));

    int chosen_w = std::max(1, max_w);
    int chosen_h = std::max(1, max_h);
    for (int unit = kTileUnitPx; unit >= 16; --unit) {
        const int gap = std::clamp((unit * kTileGapPx + kTileUnitPx / 2) / kTileUnitPx, 2, kTileGapPx);
        const int cand_w = kTileCols * unit + (kTileCols - 1) * gap;
        const int cand_h = kTileRows * unit + (kTileRows - 1) * gap;
        if (cand_w <= max_w && cand_h <= max_h) {
            chosen_w = cand_w;
            chosen_h = cand_h;
            break;
        }
    }

    const int right_margin = std::min(
        kTileBoardRightMargin,
        std::max(kSnapGridPx, primary_work_area.w - chosen_w - kMinGroupW - 2 * kSnapGridPx));
    const int top_margin =
        (chosen_h + kTileBoardTopMargin + kSnapGridPx <= primary_work_area.h)
            ? kTileBoardTopMargin
            : std::max(kSnapGridPx, (primary_work_area.h - chosen_h) / 2);

    const int x = primary_work_area.right() - chosen_w - right_margin;
    const int y = primary_work_area.y + top_margin;
    return {x, y, chosen_w, chosen_h};
}

RectI ComputeGroupFrame(const DesktopGroup& group, DesktopIconSize icon_size) noexcept {
    const PointI cell = IconCellMetrics(icon_size);
    const int cols = std::clamp(group.cols, kGroupMinCols, kGroupMaxCols);
    const int w = kGroupPaddingX * 2 + cols * cell.x;
    if (group.collapsed) {
        return {group.x, group.y, w, kGroupHeaderHeight};
    }
    int rows = std::clamp(group.visible_rows, kGroupMinRows, kGroupMaxRows);
    bool footer = true;
    if (group.content_count >= 0) {
        // As tall as its rows: one row when empty, the footer only when members overflow.
        const int needed = std::max(1, (group.content_count + cols - 1) / cols);
        footer = group.content_count > rows * cols;
        rows = std::min(rows, needed);
    }
    const int h = kGroupHeaderHeight + kGroupPaddingTop + rows * cell.y +
                  kGroupPaddingBottom + (footer ? kGroupFooterHeight : 0);
    return {group.x, group.y, w, h};
}

void UpdateGroupContentCounts(DesktopModelState& state, int64_t now_unix_ms) {
    for (auto& g : state.groups) g.content_count = 0;
    if (!state.settings.organize_enabled) return;
    for (const auto& item : state.items) {
        const std::optional<size_t> g = ClassifyItemToGroup(item, state.groups, now_unix_ms);
        if (g.has_value() && *g < state.groups.size()) ++state.groups[*g].content_count;
    }
}

std::vector<DesktopGroup> MakeDefaultGroups(const RectI& primary_work_area) {
    const RectI area = primary_work_area.empty() ? RectI{0, 0, 1920, 1032} : primary_work_area;
    const int col0_x = SnapCoord(area.x + 24);
    const int row0_y = SnapCoord(area.y + 24);

    DesktopGroup in_progress{};
    in_progress.id = "in_progress";
    in_progress.title = L"进行中";
    in_progress.icon_glyph = L'\xE8A5';
    in_progress.rule_summary = L"最近 7 天改过的文档";
    in_progress.cols = 4;
    in_progress.visible_rows = 2;
    in_progress.focus_visible = true;
    in_progress.sort_by = GroupSortBy::ModifiedTime;
    in_progress.sort_ascending = false;
    in_progress.rule.extensions = {
        L".docx", L".doc", L".xlsx", L".xls", L".csv",
        L".pptx", L".ppt", L".pdf", L".md", L".txt", L".rtf"
    };
    in_progress.rule.modified_within_days = 7;
    in_progress.rule.match_files = true;
    in_progress.rule.match_folders = false;
    in_progress.x = col0_x;
    in_progress.y = row0_y;

    const RectI frame0 = ComputeGroupFrame(in_progress, DesktopIconSize::Medium);
    const int col1_x = SnapCoord(col0_x + frame0.w + 16);
    const int row1_y = SnapCoord(row0_y + frame0.h + 16);

    DesktopGroup installers{};
    installers.id = "installers";
    installers.title = L"安装包";
    installers.icon_glyph = L'\xE7B8';
    installers.rule_summary = L".exe · .msi";
    installers.cols = 3;
    installers.visible_rows = 2;
    installers.focus_visible = false;
    installers.sort_by = GroupSortBy::ModifiedTime;
    installers.sort_ascending = false;
    installers.rule.extensions = {
        L".exe", L".msi", L".msix", L".msixbundle"
    };
    installers.rule.match_files = true;
    installers.rule.match_folders = false;
    installers.x = col1_x;
    installers.y = row0_y;

    DesktopGroup screenshots{};
    screenshots.id = "screenshots";
    screenshots.title = L"截图与图片";
    screenshots.icon_glyph = L'\xEB9F';
    screenshots.rule_summary = L"图片 · 截图";
    screenshots.cols = 4;
    screenshots.visible_rows = 2;
    screenshots.focus_visible = false;
    screenshots.sort_by = GroupSortBy::ModifiedTime;
    screenshots.sort_ascending = false;
    screenshots.rule.extensions = {
        L".png", L".jpg", L".jpeg", L".webp", L".gif",
        L".bmp", L".svg", L".avif", L".ico", L".tif", L".tiff"
    };
    screenshots.rule.match_files = true;
    screenshots.rule.match_folders = false;
    screenshots.x = col0_x;
    screenshots.y = row1_y;

    DesktopGroup uncategorized{};
    uncategorized.id = "uncategorized";
    uncategorized.title = L"未归类";
    uncategorized.icon_glyph = L'\xE8B7';
    uncategorized.rule_summary = L"其他桌面文件";
    uncategorized.cols = 4;
    uncategorized.visible_rows = 2;
    uncategorized.focus_visible = false;
    uncategorized.sort_by = GroupSortBy::ModifiedTime;
    uncategorized.sort_ascending = false;
    uncategorized.rule.fallback_catch_all = true;
    uncategorized.rule.match_files = true;
    uncategorized.rule.match_folders = true;
    uncategorized.x = col1_x;
    uncategorized.y = row1_y;

    std::vector<DesktopGroup> groups;
    groups.reserve(4);
    groups.push_back(std::move(in_progress));
    groups.push_back(std::move(installers));
    groups.push_back(std::move(screenshots));
    groups.push_back(std::move(uncategorized));

    ResolveGroupCollisions(groups, std::nullopt, area, ComputeTileBoardRect(area, true), DesktopIconSize::Medium);
    CommitGroupRestOrigins(groups);
    return groups;
}

void ResolveGroupCollisions(std::vector<DesktopGroup>& groups, std::optional<size_t> active_index,
                            const RectI& work_area, const RectI& tile_board_rect,
                            DesktopIconSize icon_size,
                            std::span<const RectI> extra_obstacles) {
    if (groups.empty()) {
        return;
    }

    if (std::any_of(groups.begin(),groups.end(),[](const auto& g){return !g.tab_stack_id.empty();})) {
        NormalizeGroupTabs(groups);
        std::vector<DesktopGroup> representatives;
        std::vector<size_t> source;
        std::optional<size_t> active;
        for(size_t i=0;i<groups.size();++i)if(groups[i].tab_active) {
            auto representative=groups[i];
            for(size_t member:TabMembers(groups,groups[i].id)) {
                representative.content_count=std::max(representative.content_count,groups[member].content_count);
                if(active_index&&member==*active_index)active=representatives.size();
            }
            representative.tab_stack_id.clear();
            representatives.push_back(std::move(representative));source.push_back(i);
        }
        ResolveGroupCollisions(representatives,active,work_area,tile_board_rect,icon_size,extra_obstacles);
        for(size_t r=0;r<source.size();++r)for(size_t member:TabMembers(groups,groups[source[r]].id)) {
            groups[member].x=representatives[r].x;groups[member].y=representatives[r].y;
            groups[member].rest_origin=representatives[r].rest_origin;
        }
        return;
    }

    const size_t n = groups.size();
    std::vector<PointI> intended(n);
    for (size_t i = 0; i < n; ++i) {
        if (active_index.has_value() && i == *active_index) {
            intended[i] = PointI{SnapCoord(groups[i].x), SnapCoord(groups[i].y)};
        } else if (groups[i].rest_origin.has_value()) {
            intended[i] = *groups[i].rest_origin;
            groups[i].x = intended[i].x;
            groups[i].y = intended[i].y;
        } else {
            intended[i] = PointI{SnapCoord(groups[i].x), SnapCoord(groups[i].y)};
        }
    }

    std::vector<RectI> placed;
    placed.reserve(n);

    if (active_index.has_value() && *active_index < n) {
        const size_t act = *active_index;
        const RectI frame = ComputeGroupFrame(groups[act], icon_size);
        const PointI spot = ResolveSingleGroupSpot(
            intended[act].x, intended[act].y, frame.w, frame.h, work_area, tile_board_rect, placed, extra_obstacles);
        groups[act].x = spot.x;
        groups[act].y = spot.y;
        groups[act].rest_origin.reset();
        placed.push_back(ComputeGroupFrame(groups[act], icon_size));
    }

    std::vector<size_t> order;
    order.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (!active_index.has_value() || i != *active_index) {
            order.push_back(i);
        }
    }
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (intended[a].y != intended[b].y) {
            return intended[a].y < intended[b].y;
        }
        return intended[a].x < intended[b].x;
    });

    for (const size_t idx : order) {
        const RectI frame = ComputeGroupFrame(groups[idx], icon_size);
        const PointI spot = ResolveSingleGroupSpot(
            intended[idx].x, intended[idx].y, frame.w, frame.h, work_area, tile_board_rect, placed, extra_obstacles);
        groups[idx].x = spot.x;
        groups[idx].y = spot.y;
        if (spot != intended[idx]) {
            groups[idx].rest_origin = intended[idx];
        } else {
            groups[idx].rest_origin.reset();
        }
        placed.push_back(ComputeGroupFrame(groups[idx], icon_size));
    }
}

void MoveGroupInteractive(std::vector<DesktopGroup>& groups, size_t group_index,
                          int proposed_x, int proposed_y,
                          const RectI& work_area, const RectI& tile_board_rect,
                          DesktopIconSize icon_size,
                          std::span<const RectI> extra_obstacles) {
    if (group_index >= groups.size()) {
        return;
    }
    NormalizeGroupTabs(groups);
    if(!groups[group_index].tab_active)
        for(size_t member:TabMembers(groups,groups[group_index].id))if(groups[member].tab_active){group_index=member;break;}
    groups[group_index].x = SnapCoord(proposed_x);
    groups[group_index].y = SnapCoord(proposed_y);
    groups[group_index].rest_origin.reset();
    ResolveGroupCollisions(groups, group_index, work_area, tile_board_rect, icon_size, extra_obstacles);
}

void CommitGroupRestOrigins(std::vector<DesktopGroup>& groups) noexcept {
    for (DesktopGroup& group : groups) {
        group.rest_origin.reset();
    }
}

void ResizeGroupColumns(std::vector<DesktopGroup>& groups, size_t group_index,
                        int proposed_cols, int proposed_rows,
                        const RectI& work_area, const RectI& tile_board_rect,
                        DesktopIconSize icon_size,
                        std::span<const RectI> extra_obstacles) {
    if (group_index >= groups.size()) {
        return;
    }

    NormalizeGroupTabs(groups);
    if(!groups[group_index].tab_active)
        for(size_t member:TabMembers(groups,groups[group_index].id))if(groups[member].tab_active){group_index=member;break;}
    DesktopGroup& group = groups[group_index];
    const PointI cell = IconCellMetrics(icon_size);
    int cols = std::clamp(proposed_cols, kGroupMinCols, kGroupMaxCols);
    int rows = std::clamp(proposed_rows, kGroupMinRows, kGroupMaxRows);

    if (!work_area.empty()) {
        const int max_avail_w = work_area.w - 2 * kSnapGridPx;
        while (cols > kGroupMinCols && (kGroupPaddingX * 2 + cols * cell.x) > max_avail_w) {
            --cols;
        }

        const int max_avail_h = work_area.h - 2 * kSnapGridPx;
        const int overhead_h = kGroupHeaderHeight + kGroupPaddingTop +
                               kGroupPaddingBottom + kGroupFooterHeight;
        while (rows > kGroupMinRows && (overhead_h + rows * cell.y) > max_avail_h) {
            --rows;
        }

        if (!tile_board_rect.empty() || !extra_obstacles.empty()) {
            while (cols > kGroupMinCols) {
                DesktopGroup probe = group;
                probe.cols = cols;
                probe.visible_rows = rows;
                const RectI probe_frame = ComputeGroupFrame(probe, icon_size);
                if (FindBestGroupSpot(group.x, group.y, probe_frame.w, probe_frame.h,
                                      work_area, tile_board_rect, {}, extra_obstacles, 0).has_value()) {
                    break;
                }
                --cols;
            }
        }
    }

    group.cols = cols;
    group.visible_rows = rows;
    group.rest_origin.reset();
    ResolveGroupCollisions(groups, group_index, work_area, tile_board_rect, icon_size, extra_obstacles);
}

std::vector<GroupComputedView> ComputeGroupViews(const DesktopModelState& state,
                                                 int64_t now_unix_ms,
                                                 const std::unordered_map<std::string, int>* group_pages,
                                                 std::string_view hover_expanded_group_id) {
    if (!state.settings.organize_enabled) {
        return {};
    }

    const size_t group_count = state.groups.size();
    std::vector<std::vector<size_t>> buckets(group_count);

    for (size_t item_idx = 0; item_idx < state.items.size(); ++item_idx) {
        const std::optional<size_t> g = ClassifyItemToGroup(state.items[item_idx], state.groups, now_unix_ms);
        if (g.has_value() && *g < group_count) {
            buckets[*g].push_back(item_idx);
        }
    }

    std::vector<GroupComputedView> views;
    views.reserve(group_count);

    for (size_t g = 0; g < group_count; ++g) {
        const DesktopGroup& group = state.groups[g];
        std::vector<size_t>& indices = buckets[g];

        switch (group.sort_by) {
        case GroupSortBy::Name: {
            std::stable_sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
                const DesktopItem& a = state.items[ia];
                const DesktopItem& b = state.items[ib];
                int cmp = CompareWideIgnoreCase(a.name, b.name);
                if (cmp == 0) {
                    cmp = a.name.compare(b.name);
                }
                if (cmp == 0) {
                    cmp = a.path.compare(b.path);
                }
                return group.sort_ascending ? (cmp < 0) : (cmp > 0);
            });
            break;
        }
        case GroupSortBy::ModifiedTime: {
            std::stable_sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
                const DesktopItem& a = state.items[ia];
                const DesktopItem& b = state.items[ib];
                if (a.modified_unix_ms != b.modified_unix_ms) {
                    return group.sort_ascending ? (a.modified_unix_ms < b.modified_unix_ms)
                                                : (a.modified_unix_ms > b.modified_unix_ms);
                }
                const int cmp = CompareWideIgnoreCase(a.name, b.name);
                if (cmp != 0) {
                    return cmp < 0;
                }
                return ia < ib;
            });
            break;
        }
        case GroupSortBy::Size: {
            std::stable_sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
                const DesktopItem& a = state.items[ia];
                const DesktopItem& b = state.items[ib];
                if (a.size_bytes != b.size_bytes) {
                    return group.sort_ascending ? (a.size_bytes < b.size_bytes)
                                                : (a.size_bytes > b.size_bytes);
                }
                const int cmp = CompareWideIgnoreCase(a.name, b.name);
                if (cmp != 0) {
                    return cmp < 0;
                }
                return ia < ib;
            });
            break;
        }
        case GroupSortBy::Type: {
            std::stable_sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
                const DesktopItem& a = state.items[ia];
                const DesktopItem& b = state.items[ib];
                if (a.is_folder != b.is_folder) {
                    return group.sort_ascending ? (a.is_folder > b.is_folder)
                                                : (a.is_folder < b.is_folder);
                }
                int cmp = CompareWideIgnoreCase(EffectiveExtension(a), EffectiveExtension(b));
                if (cmp != 0) {
                    return group.sort_ascending ? (cmp < 0) : (cmp > 0);
                }
                cmp = CompareWideIgnoreCase(a.name, b.name);
                if (cmp != 0) {
                    return group.sort_ascending ? (cmp < 0) : (cmp > 0);
                }
                return ia < ib;
            });
            break;
        }
        case GroupSortBy::Manual: {
            const auto manual_rank = [&](const DesktopItem& item) -> size_t {
                for (size_t r = 0; r < group.manual_order.size(); ++r) {
                    if (!item.path.empty() && EqualsWideIgnoreCase(item.path, group.manual_order[r])) {
                        return r;
                    }
                }
                for (size_t r = 0; r < group.manual_order.size(); ++r) {
                    if (!item.name.empty() && EqualsWideIgnoreCase(item.name, group.manual_order[r])) {
                        return r;
                    }
                }
                return group.manual_order.size();
            };
            std::stable_sort(indices.begin(), indices.end(), [&](size_t ia, size_t ib) {
                const size_t ra = manual_rank(state.items[ia]);
                const size_t rb = manual_rank(state.items[ib]);
                if (ra != rb) {
                    return ra < rb;
                }
                return ia < ib;
            });
            break;
        }
        }

        const bool persisted_collapsed = group.collapsed ||
            (state.settings.active_scene == DesktopScene::Focus && !group.focus_visible);
        const bool temporary = persisted_collapsed && group.id == hover_expanded_group_id;
        const bool effective_collapsed = persisted_collapsed && !temporary;

        DesktopGroup effective_group = group;
        effective_group.collapsed = effective_collapsed;
        effective_group.content_count = static_cast<int>(indices.size());
        const auto members=TabMembers(state.groups,group.id);
        for(size_t member:members)effective_group.content_count=std::max(effective_group.content_count,static_cast<int>(buckets[member].size()));

        const int cols = std::clamp(group.cols, kGroupMinCols, kGroupMaxCols);
        const int rows = std::clamp(group.visible_rows, kGroupMinRows, kGroupMaxRows);
        GroupComputedView view{};
        view.group_id=group.id;view.view_mode=group.view_mode;
        view.tab_active=group.tab_stack_id.empty()||group.tab_active;
        view.expanded=!effective_collapsed&&view.tab_active;
        view.temporarily_expanded=temporary&&view.tab_active;
        for(size_t member:members) {
            if(state.groups[member].tab_active)view.active_tab_index=static_cast<int>(view.tab_group_ids.size());
            view.tab_group_ids.push_back(state.groups[member].id);
        }
        view.frame=ComputeGroupFrame(effective_group,state.settings.icon_size);
        const auto metrics=IconCellMetrics(state.settings.icon_size);
        const int footer=(group.view_mode!=GroupViewMode::Icons||effective_group.content_count>cols*rows)?kGroupFooterHeight:0;
        view.content_rect={view.frame.x+kGroupPaddingX,view.frame.y+kGroupHeaderHeight+kGroupPaddingTop,
            view.frame.w-2*kGroupPaddingX,std::max(0,view.frame.h-kGroupHeaderHeight-kGroupPaddingTop-kGroupPaddingBottom-footer)};
        view.item_columns=cols;view.cell_width=metrics.x;view.cell_height=metrics.y;
        if(group.view_mode==GroupViewMode::LargeIcons) {
            view.item_columns=std::max(1,cols/2);view.cell_width=view.content_rect.w/view.item_columns;
            view.cell_height=std::min(view.content_rect.h,metrics.y+32);
        } else if(group.view_mode==GroupViewMode::Tiles) {
            view.item_columns=std::max(1,view.content_rect.w/180);view.cell_width=view.content_rect.w/view.item_columns;view.cell_height=56;
        } else if(group.view_mode==GroupViewMode::Details) {
            view.item_columns=1;view.cell_width=view.content_rect.w;view.cell_height=28;view.details_header_height=24;
        }
        size_t capacity = effective_collapsed ? 0u : static_cast<size_t>(cols * rows);
        if(group.view_mode!=GroupViewMode::Icons)capacity=effective_collapsed?0u:
            static_cast<size_t>(view.item_columns*(std::max(0,view.content_rect.h-view.details_header_height)/std::max(1,view.cell_height)));
        const size_t total = indices.size();
        const int page_count = (!effective_collapsed && capacity > 0 && total > 0)
                                   ? static_cast<int>((total + capacity - 1) / capacity)
                                   : 1;
        int req_page = 0;
        if (group_pages != nullptr) {
            if (const auto pit = group_pages->find(group.id); pit != group_pages->end()) {
                req_page = pit->second;
            }
        }
        const int cur_page = std::clamp(req_page, 0, page_count - 1);
        const size_t first = effective_collapsed ? 0u : static_cast<size_t>(cur_page) * capacity;
        const size_t visible_count =
            (!effective_collapsed && first < total) ? std::min(capacity, total - first) : 0u;

        if(!view.tab_active)view.frame={};
        view.visible_item_indices.assign(
            indices.begin() + static_cast<std::ptrdiff_t>(first),
            indices.begin() + static_cast<std::ptrdiff_t>(first + visible_count));
        view.member_item_indices = indices;
        view.total_item_count = total;
        view.overflow_count = (total > capacity) ? (total - capacity) : 0u;
        view.page_index = cur_page;
        view.page_count = page_count;
        view.page_capacity = capacity;
        if (group.mapped_folder_path.empty() && !group.rule.fallback_catch_all && indices.size() == 1) {
            const DesktopItem& sole = state.items[indices[0]];
            if (sole.is_folder) {
                view.single_subfolder_path = sole.path;
                view.single_subfolder_name = sole.name;
            }
        }
        views.push_back(std::move(view));
    }

    return views;
}

} // namespace pulse::desktop::model
