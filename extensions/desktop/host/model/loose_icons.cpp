// loose_icons.cpp - Grid and freeform settlement engine for bare desktop icons.
#include "desktop_model.h"

#include <algorithm>
#include <cwctype>
#include <limits>
#include <utility>
#include <vector>

namespace pulse::desktop::model {
namespace {

wchar_t FoldWide(wchar_t ch) noexcept {
    if (ch >= L'A' && ch <= L'Z') {
        return static_cast<wchar_t>(ch - L'A' + L'a');
    }
    return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(ch)));
}

int CompareWideNoCase(std::wstring_view a, std::wstring_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const wchar_t ca = FoldWide(a[i]);
        const wchar_t cb = FoldWide(b[i]);
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

std::wstring_view ItemExt(const DesktopItem& item) noexcept {
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

bool CompareLooseItems(const DesktopItem& a, size_t ia,
                       const DesktopItem& b, size_t ib,
                       GroupSortBy sort_by, bool ascending) noexcept {
    switch (sort_by) {
    case GroupSortBy::Name: {
        int cmp = CompareWideNoCase(a.name, b.name);
        if (cmp == 0) {
            cmp = a.name.compare(b.name);
        }
        if (cmp != 0) {
            return ascending ? (cmp < 0) : (cmp > 0);
        }
        return ia < ib;
    }
    case GroupSortBy::ModifiedTime: {
        if (a.modified_unix_ms != b.modified_unix_ms) {
            return ascending ? (a.modified_unix_ms < b.modified_unix_ms)
                             : (a.modified_unix_ms > b.modified_unix_ms);
        }
        const int cmp = CompareWideNoCase(a.name, b.name);
        if (cmp != 0) {
            return cmp < 0;
        }
        return ia < ib;
    }
    case GroupSortBy::Size: {
        if (a.size_bytes != b.size_bytes) {
            return ascending ? (a.size_bytes < b.size_bytes)
                             : (a.size_bytes > b.size_bytes);
        }
        const int cmp = CompareWideNoCase(a.name, b.name);
        if (cmp != 0) {
            return cmp < 0;
        }
        return ia < ib;
    }
    case GroupSortBy::Type: {
        if (a.is_folder != b.is_folder) {
            return ascending ? (a.is_folder > b.is_folder)
                             : (a.is_folder < b.is_folder);
        }
        int cmp = CompareWideNoCase(ItemExt(a), ItemExt(b));
        if (cmp != 0) {
            return ascending ? (cmp < 0) : (cmp > 0);
        }
        cmp = CompareWideNoCase(a.name, b.name);
        if (cmp != 0) {
            return ascending ? (cmp < 0) : (cmp > 0);
        }
        return ia < ib;
    }
    case GroupSortBy::Manual:
    default:
        return ia < ib;
    }
}

PointI PreferredLoosePos(const DesktopItem& item, bool organize_enabled,
                         int origin_x, int origin_y) noexcept {
    if (!organize_enabled) {
        if (item.native_pos.x != 0 || item.native_pos.y != 0) {
            return item.native_pos;
        }
        if (item.loose_pos.x != 0 || item.loose_pos.y != 0) {
            return item.loose_pos;
        }
        return {origin_x, origin_y};
    }

    if (item.loose_pos.x != 0 || item.loose_pos.y != 0) {
        return item.loose_pos;
    }
    if (item.native_pos.x != 0 || item.native_pos.y != 0) {
        return item.native_pos;
    }
    return {origin_x, origin_y};
}

} // namespace

void SettleLooseIcons(std::vector<DesktopItem>& items,
                      std::span<const GroupComputedView> group_views,
                      const RectI& work_area,
                      const RectI& tile_board_rect,
                      DesktopIconSize icon_size,
                      bool organize_enabled,
                      bool align_to_grid,
                      GroupSortBy loose_sort_by,
                      bool loose_sort_ascending,
                      std::span<const RectI> extra_obstacles) {
    if (items.empty()) {
        return;
    }

    std::vector<uint8_t> grouped(items.size(), 0);
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].hidden_by_mapped_group || !items[i].mapped_owner_group_id.empty()) {
            grouped[i] = 1;
        }
    }
    if (organize_enabled) {
        for (const GroupComputedView& gv : group_views) {
            for (const size_t idx : gv.member_item_indices) {
                if (idx < items.size() && !items[idx].force_loose) {
                    grouped[idx] = 1;
                }
            }
        }

        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].in_apps_tile) grouped[i] = 1;  // shown in the 应用 tile
            if (grouped[i] || items[i].force_loose || items[i].pinned_group_id.empty()) {
                continue;
            }
            for (const GroupComputedView& gv : group_views) {
                if (gv.group_id == items[i].pinned_group_id) {
                    grouped[i] = 1;
                    break;
                }
            }
        }
    }

    std::vector<size_t> loose_indices;
    loose_indices.reserve(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        if (!grouped[i]) {
            loose_indices.push_back(i);
        }
    }
    if (loose_indices.empty()) {
        return;
    }

    std::stable_sort(loose_indices.begin(), loose_indices.end(), [&](size_t a, size_t b) {
        if (organize_enabled && items[a].force_loose != items[b].force_loose) {
            const bool a_has_loose = (items[a].loose_pos.x != 0 || items[a].loose_pos.y != 0);
            const bool b_has_loose = (items[b].loose_pos.x != 0 || items[b].loose_pos.y != 0);
            if (a_has_loose || b_has_loose || loose_sort_by == GroupSortBy::Manual) {
                return items[a].force_loose > items[b].force_loose;
            }
        }
        return CompareLooseItems(items[a], a, items[b], b, loose_sort_by, loose_sort_ascending);
    });

    const RectI area = work_area.empty() ? RectI{0, 0, 1920, 1032} : work_area;
    const PointI base_metrics = IconCellMetrics(icon_size);
    const int cell_w = base_metrics.x + 6; // Medium: 86 + 6 = 92 (kLooseCellW)
    const int cell_h = base_metrics.y + 6; // Medium: 92 + 6 = 98 (kLooseCellH)
    const int origin_x = area.x + kLooseMarginX;
    const int origin_y = area.y + kLooseMarginY;

    if (!align_to_grid) {
        const int min_x = area.x + kSnapGridPx;
        const int min_y = area.y + kSnapGridPx;
        const int max_x = std::max(min_x, area.right() - kSnapGridPx - cell_w);
        const int max_y = std::max(min_y, area.bottom() - kSnapGridPx - cell_h);

        std::vector<RectI> placed_rects;
        placed_rects.reserve(loose_indices.size());

        const auto fits_freeform = [&](int px, int py, bool respect_obstacles) noexcept -> bool {
            if (px < min_x || px > max_x || py < min_y || py > max_y) {
                return false;
            }
            const RectI cand{px, py, cell_w, cell_h};
            if (respect_obstacles) {
                if (!tile_board_rect.empty() && cand.intersects(tile_board_rect, 0)) {
                    return false;
                }
                for (const RectI& obs : extra_obstacles) {
                    if (!obs.empty() && cand.intersects(obs, 4)) {
                        return false;
                    }
                }
                if (organize_enabled) {
                    for (const GroupComputedView& gv : group_views) {
                        if (!gv.frame.empty() && cand.intersects(gv.frame, 6)) {
                            return false;
                        }
                    }
                }
            }
            for (const RectI& prev : placed_rects) {
                if (cand.intersects(prev, 0)) {
                    return false;
                }
            }
            return true;
        };

        for (const size_t item_idx : loose_indices) {
            DesktopItem& item = items[item_idx];
            const PointI pref = PreferredLoosePos(item, organize_enabled, origin_x, origin_y);
            const int cx = std::clamp(pref.x, min_x, max_x);
            const int cy = std::clamp(pref.y, min_y, max_y);

            if (fits_freeform(cx, cy, true)) {
                item.loose_pos = {cx, cy};
                placed_rects.push_back({cx, cy, cell_w, cell_h});
                continue;
            }

            const auto search_step = [&](bool respect_obstacles) -> std::optional<PointI> {
                std::optional<PointI> best;
                int64_t best_dist_sq = std::numeric_limits<int64_t>::max();
                for (int px = min_x; px <= max_x; px += kSnapGridPx) {
                    const int64_t dx = static_cast<int64_t>(px) - pref.x;
                    const int64_t dx_sq = dx * dx;
                    for (int py = min_y; py <= max_y; py += kSnapGridPx) {
                        if (!fits_freeform(px, py, respect_obstacles)) {
                            continue;
                        }
                        const int64_t dy = static_cast<int64_t>(py) - pref.y;
                        const int64_t dist_sq = dx_sq + dy * dy;
                        if (!best.has_value() || dist_sq < best_dist_sq) {
                            best_dist_sq = dist_sq;
                            best = PointI{px, py};
                        }
                    }
                }
                return best;
            };

            std::optional<PointI> chosen = search_step(true);
            if (!chosen.has_value()) {
                chosen = search_step(false);
            }
            const PointI final_pt = chosen.value_or(PointI{cx, cy});
            item.loose_pos = final_pt;
            placed_rects.push_back({final_pt.x, final_pt.y, cell_w, cell_h});
        }
        return;
    }

    const int grid_cols = std::max(1, (area.w - kLooseMarginX) / cell_w);
    const int grid_rows = std::max(1, (area.h - kLooseMarginY) / cell_h);

    const size_t cell_count = static_cast<size_t>(grid_cols) * static_cast<size_t>(grid_rows);
    std::vector<uint8_t> blocked(cell_count, 0);
    std::vector<uint8_t> occupied(cell_count, 0);

    for (int c = 0; c < grid_cols; ++c) {
        for (int r = 0; r < grid_rows; ++r) {
            const RectI cell_rect{
                origin_x + c * cell_w,
                origin_y + r * cell_h,
                cell_w,
                cell_h
            };
            bool hit = false;
            if (!tile_board_rect.empty() && cell_rect.intersects(tile_board_rect, 0)) {
                hit = true;
            }
            if (!hit) {
                for (const RectI& obs : extra_obstacles) {
                    if (!obs.empty() && cell_rect.intersects(obs, 4)) {
                        hit = true;
                        break;
                    }
                }
            }
            if (!hit && organize_enabled) {
                for (const GroupComputedView& gv : group_views) {
                    if (!gv.frame.empty() && cell_rect.intersects(gv.frame, 6)) {
                        hit = true;
                        break;
                    }
                }
            }
            if (hit) {
                blocked[static_cast<size_t>(c) * static_cast<size_t>(grid_rows) + static_cast<size_t>(r)] = 1;
            }
        }
    }

    int overflow_slot = 0;
    for (const size_t item_idx : loose_indices) {
        DesktopItem& item = items[item_idx];
        const PointI pref = PreferredLoosePos(item, organize_enabled, origin_x, origin_y);

        const auto find_best = [&](bool respect_obstacles) -> std::optional<std::pair<int, int>> {
            std::optional<std::pair<int, int>> best;
            int64_t best_dist_sq = std::numeric_limits<int64_t>::max();
            for (int c = 0; c < grid_cols; ++c) {
                const int cx = origin_x + c * cell_w;
                const int64_t dx = static_cast<int64_t>(cx) - pref.x;
                const int64_t dx_sq = dx * dx;
                for (int r = 0; r < grid_rows; ++r) {
                    const size_t flat = static_cast<size_t>(c) * static_cast<size_t>(grid_rows) +
                                        static_cast<size_t>(r);
                    if (occupied[flat]) {
                        continue;
                    }
                    if (respect_obstacles && blocked[flat]) {
                        continue;
                    }
                    const int cy = origin_y + r * cell_h;
                    const int64_t dy = static_cast<int64_t>(cy) - pref.y;
                    const int64_t dist_sq = dx_sq + dy * dy;
                    if (!best.has_value() || dist_sq < best_dist_sq) {
                        best_dist_sq = dist_sq;
                        best = std::pair<int, int>{c, r};
                    }
                }
            }
            return best;
        };

        std::optional<std::pair<int, int>> chosen = find_best(true);
        if (!chosen.has_value()) {
            chosen = find_best(false);
        }

        if (chosen.has_value()) {
            const int c = chosen->first;
            const int r = chosen->second;
            occupied[static_cast<size_t>(c) * static_cast<size_t>(grid_rows) + static_cast<size_t>(r)] = 1;
            item.loose_pos = PointI{origin_x + c * cell_w, origin_y + r * cell_h};
        } else {
            const int extra_col = grid_cols + (overflow_slot / grid_rows);
            const int extra_row = overflow_slot % grid_rows;
            ++overflow_slot;
            item.loose_pos = PointI{origin_x + extra_col * cell_w, origin_y + extra_row * cell_h};
        }
    }
}

} // namespace pulse::desktop::model
