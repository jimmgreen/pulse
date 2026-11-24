// model_layout_test.cpp - Deterministic unit tests for pulse::desktop::model & ai
#include "../host/ai/desktop_ai_organizer.h"
#include "../host/model/desktop_model.h"

#include <wincrypt.h>

#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

using namespace pulse::desktop::model;

constexpr int64_t kDayMs = 86400000LL;
constexpr int64_t kNowMs = 1794000000000LL; // Fixed reference timestamp

int g_pass_count = 0;
int g_fail_count = 0;

void CheckImpl(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::printf("  [ASSERT FAIL] %s:%d: %s\n", file, line, expr);
        ++g_fail_count;
    }
}

#define CHECK(expr) CheckImpl(static_cast<bool>(expr), #expr, __FILE__, __LINE__)

template <typename Fn>
void RunCase(const char* name, Fn&& fn) {
    const int fails_before = g_fail_count;
    fn();
    if (g_fail_count == fails_before) {
        std::printf("[PASS] %s\n", name);
        ++g_pass_count;
    } else {
        std::printf("[FAIL] %s\n", name);
    }
}

bool HasNoGroupOverlaps(std::span<const DesktopGroup> groups,
                        const RectI& work_area,
                        const RectI& tile_board,
                        DesktopIconSize icon_size = DesktopIconSize::Medium) {
    for (size_t i = 0; i < groups.size(); ++i) {
        const RectI fi = ComputeGroupFrame(groups[i], icon_size);
        if (fi.x < work_area.x || fi.y < work_area.y ||
            fi.right() > work_area.right() || fi.bottom() > work_area.bottom()) {
            return false;
        }
        if (!tile_board.empty() && fi.intersects(tile_board, 0)) {
            return false;
        }
        for (size_t j = i + 1; j < groups.size(); ++j) {
            const RectI fj = ComputeGroupFrame(groups[j], icon_size);
            if (fi.intersects(fj, 0)) {
                return false;
            }
        }
    }
    return true;
}

bool HasValidTileBoard(std::span<const DesktopTile> tiles) {
    for (size_t i = 0; i < tiles.size(); ++i) {
        const DesktopTile& ti = tiles[i];
        if (!IsValidTileSize(ti.size)) {
            return false;
        }
        if (ti.col < 0 || ti.row < 0 ||
            ti.col + ti.size.cols > kTileCols ||
            ti.row + ti.size.rows > kTileRows) {
            return false;
        }
        const RectI ri{ti.col, ti.row, ti.size.cols, ti.size.rows};
        for (size_t j = i + 1; j < tiles.size(); ++j) {
            const DesktopTile& tj = tiles[j];
            const RectI rj{tj.col, tj.row, tj.size.cols, tj.size.rows};
            if (ri.intersects(rj, 0)) {
                return false;
            }
        }
    }
    return true;
}

void TestDefaultGroupsAndGeometry() {
    CHECK(kGroupHeaderHeight == 38);
    CHECK(kGroupCellW == 86);
    CHECK(kGroupCellH == 92);
    CHECK(kTileUnitPx == 100);

    CHECK(SnapCoord(0) == 0);
    CHECK(SnapCoord(3) == 0);
    CHECK(SnapCoord(4) == 8);
    CHECK(SnapCoord(23) == 24);
    CHECK(SnapCoord(-3) == 0);
    CHECK(SnapCoord(-5) == -8);

    const RectI work{0, 0, 1920, 1032};
    const RectI disabled_board = ComputeTileBoardRect(work, false);
    CHECK(disabled_board.empty());

    const RectI board = ComputeTileBoardRect(work, true);
    CHECK(board.w == 532);
    CHECK(board.h == 748);
    CHECK(board.x == 1920 - 532 - kTileBoardRightMargin);
    CHECK(board.y == kTileBoardTopMargin);

    // Narrow screen proportional tile board scaling check (900x650)
    const RectI narrow_work{0, 0, 900, 650};
    const RectI narrow_board = ComputeTileBoardRect(narrow_work, true);
    CHECK(!narrow_board.empty());
    CHECK(narrow_board.w <= narrow_work.w * 44 / 100);
    CHECK(narrow_board.h <= narrow_work.h - 40);
    const int min_group_w = kGroupPaddingX * 2 + kGroupMinCols * kGroupCellW;
    CHECK(narrow_board.x >= narrow_work.x + min_group_w + kSnapGridPx);

    const std::vector<DesktopGroup> groups = MakeDefaultGroups(work);
    CHECK(groups.size() == 4);
    CHECK(groups[0].id == "in_progress" && groups[0].title == L"进行中");
    CHECK(groups[0].icon_glyph == L'\xE8A5');
    CHECK(groups[0].rule_summary == L"最近 7 天改过的文档");
    CHECK(groups[0].focus_visible);
    CHECK(groups[0].cols == 4 && groups[0].visible_rows == 2);
    CHECK(groups[0].rule.modified_within_days == 7);

    CHECK(groups[1].id == "installers" && groups[1].title == L"安装包");
    CHECK(groups[1].icon_glyph == L'\xE7B8');
    CHECK(groups[1].rule_summary == L".exe · .msi");
    CHECK(groups[1].cols == 3 && groups[1].visible_rows == 2);

    CHECK(groups[2].id == "screenshots" && groups[2].title == L"截图与图片");
    CHECK(groups[2].icon_glyph == L'\xEB9F');
    CHECK(groups[2].rule_summary == L"图片 · 截图");
    CHECK(groups[2].cols == 4 && groups[2].visible_rows == 2);

    CHECK(groups[3].id == "uncategorized" && groups[3].title == L"未归类");
    CHECK(groups[3].icon_glyph == L'\xE8B7');
    CHECK(groups[3].rule_summary == L"其他桌面文件");
    CHECK(groups[3].cols == 4 && groups[3].visible_rows == 2);
    CHECK(groups[3].rule.fallback_catch_all);

    for (const DesktopGroup& g : groups) {
        CHECK(g.x % kSnapGridPx == 0);
        CHECK(g.y % kSnapGridPx == 0);
        CHECK(!g.rest_origin.has_value());
    }
    CHECK(HasNoGroupOverlaps(groups, work, board));

    // Default appearance: Acrylic material at 60% opacity
    const DesktopModelState def_state{};
    CHECK(def_state.settings.default_material == CardMaterial::Acrylic);
    CHECK(def_state.settings.card_opacity_percent == 60);

    // IconSize cell metrics check in ComputeGroupFrame
    const RectI small_frame = ComputeGroupFrame(groups[0], DesktopIconSize::Small);
    const RectI med_frame = ComputeGroupFrame(groups[0], DesktopIconSize::Medium);
    const RectI large_frame = ComputeGroupFrame(groups[0], DesktopIconSize::Large);
    CHECK(small_frame.w < med_frame.w && med_frame.w < large_frame.w);
    CHECK(small_frame.h < med_frame.h && med_frame.h < large_frame.h);
}

void TestGroupDragPushAwayMemoryAndResize() {
    const RectI work{0, 0, 1920, 1032};
    const RectI board = ComputeTileBoardRect(work, true);
    std::vector<DesktopGroup> groups = MakeDefaultGroups(work);

    const PointI g0_home{groups[0].x, groups[0].y};
    const PointI g1_home{groups[1].x, groups[1].y};
    const PointI g3_home{groups[3].x, groups[3].y};

    // Drag group 0 directly onto group 1: group 1 must push away and remember its rest_origin.
    MoveGroupInteractive(groups, 0, g1_home.x, g1_home.y, work, board);
    CHECK(groups[0].x == g1_home.x && groups[0].y == g1_home.y);
    CHECK(groups[1].x != g1_home.x || groups[1].y != g1_home.y);
    CHECK(groups[1].rest_origin.has_value());
    CHECK(*groups[1].rest_origin == g1_home);
    CHECK(HasNoGroupOverlaps(groups, work, board));

    // Drag group 0 back to its original home: group 1 and group 3 must spring back to their exact rest origins!
    MoveGroupInteractive(groups, 0, g0_home.x, g0_home.y, work, board);
    CHECK(groups[0].x == g0_home.x && groups[0].y == g0_home.y);
    CHECK(groups[1].x == g1_home.x && groups[1].y == g1_home.y);
    CHECK(groups[3].x == g3_home.x && groups[3].y == g3_home.y);
    CHECK(!groups[1].rest_origin.has_value());
    CHECK(HasNoGroupOverlaps(groups, work, board));

    // Drag onto group 1 again and commit on drop: displaced position persists and rest_origin clears.
    MoveGroupInteractive(groups, 0, g1_home.x, g1_home.y, work, board);
    const PointI g1_displaced{groups[1].x, groups[1].y};
    CHECK(g1_displaced != g1_home);
    CommitGroupRestOrigins(groups);
    for (const DesktopGroup& g : groups) {
        CHECK(!g.rest_origin.has_value());
    }
    CHECK(groups[1].x == g1_displaced.x && groups[1].y == g1_displaced.y);

    // Drag toward tile board: group must clamp/avoid overlapping tile_board_rect.
    MoveGroupInteractive(groups, 0, board.x + 100, board.y, work, board);
    CHECK(HasNoGroupOverlaps(groups, work, board));

    // Resize clamping: min/max cols & rows and narrow work area clamping.
    groups = MakeDefaultGroups(work);
    ResizeGroupColumns(groups, 0, 99, 99, work, board);
    CHECK(groups[0].cols == kGroupMaxCols);
    CHECK(groups[0].visible_rows == kGroupMaxRows);
    CHECK(HasNoGroupOverlaps(groups, work, board));

    ResizeGroupColumns(groups, 0, 1, 0, work, board);
    CHECK(groups[0].cols == kGroupMinCols);
    CHECK(groups[0].visible_rows == kGroupMinRows);

    const RectI narrow_work{0, 0, 900, 650};
    const RectI narrow_board = ComputeTileBoardRect(narrow_work, true);
    ResizeGroupColumns(groups, 0, 7, 6, narrow_work, narrow_board);
    CHECK(groups[0].cols >= kGroupMinCols && groups[0].cols < 7);
    CHECK(!ComputeGroupFrame(groups[0]).intersects(narrow_board, 0));
}

void TestGroupViewsCollapseOverflowSortAndFocus() {
    const RectI work{0, 0, 1920, 1032};
    DesktopModelState state{};
    state.groups = MakeDefaultGroups(work);
    state.groups[0].cols = 4;
    state.groups[0].visible_rows = 2; // capacity = 8
    state.groups[0].sort_by = GroupSortBy::Size;
    state.groups[0].sort_ascending = true;

    for (int i = 0; i < 11; ++i) {
        DesktopItem it{};
        it.path = L"C:\\Users\\Demo\\Desktop\\doc_" + std::to_wstring(i) + L".docx";
        it.name = L"doc_" + std::to_wstring(i) + L".docx";
        it.extension = L".docx";
        it.size_bytes = static_cast<uint64_t>((11 - i) * 1024);
        it.modified_unix_ms = kNowMs - 5 * kDayMs; // matches "in_progress" (within 7 days)
        state.items.push_back(std::move(it));
    }

    auto views = ComputeGroupViews(state, kNowMs);
    CHECK(views.size() == 4);
    CHECK(views[0].total_item_count == 11);
    CHECK(views[0].visible_item_indices.size() == 8);
    CHECK(views[0].overflow_count == 3);
    // Ascending size: last item (index 10, 1024 bytes) comes first.
    CHECK(views[0].visible_item_indices.front() == 10);

    // Manual sort order test
    state.groups[0].sort_by = GroupSortBy::Manual;
    state.groups[0].manual_order = {
        state.items[5].path,
        state.items[2].path,
    };
    views = ComputeGroupViews(state, kNowMs);
    CHECK(views[0].visible_item_indices[0] == 5);
    CHECK(views[0].visible_item_indices[1] == 2);

    // Collapse group 0 -> height = 38px, 0 visible, overflow = 11
    state.groups[0].collapsed = true;
    views = ComputeGroupViews(state, kNowMs);
    CHECK(views[0].frame.h == kGroupHeaderHeight);
    CHECK(views[0].visible_item_indices.empty());
    CHECK(views[0].total_item_count == 11);
    CHECK(views[0].overflow_count == 11);
    state.groups[0].collapsed = false;

    // Focus scene folds non-focus_visible groups in GroupComputedView without mutating group.collapsed
    DesktopItem old_png{};
    old_png.path = L"C:\\Users\\Demo\\Desktop\\wall.png";
    old_png.name = L"wall.png";
    old_png.extension = L".png";
    old_png.modified_unix_ms = kNowMs - 10 * kDayMs;
    state.items.push_back(old_png);

    state.settings.active_scene = DesktopScene::Focus;
    views = ComputeGroupViews(state, kNowMs);
    CHECK(views[0].visible_item_indices.size() == 8);
    CHECK(views[2].total_item_count == 1);
    CHECK(views[2].frame.h == kGroupHeaderHeight);
    CHECK(views[2].visible_item_indices.empty());
    CHECK(views[2].overflow_count == 1);
    CHECK(!state.groups[2].collapsed);

    // Disabling desktop organize returns empty group views so all items render as loose desktop icons
    state.settings.organize_enabled = false;
    views = ComputeGroupViews(state, kNowMs);
    CHECK(views.empty());
}

void TestLooseIconsSettlement() {
    const RectI work{0, 0, 1920, 1032};
    const RectI board = ComputeTileBoardRect(work, true);

    DesktopModelState state{};
    state.groups = MakeDefaultGroups(work);
    // Remove catch-all so unmatched items become naturally loose alongside force_loose items.
    state.groups.pop_back();

    // Item 0: grouped inside "in_progress"
    DesktopItem grouped_doc{};
    grouped_doc.path = L"C:\\Desktop\\active.md";
    grouped_doc.name = L"active.md";
    grouped_doc.extension = L".md";
    grouped_doc.modified_unix_ms = kNowMs - 1000;
    grouped_doc.native_pos = {24, 24};
    state.items.push_back(grouped_doc);

    // Loose items whose native positions collide with groups[0], tile_board_rect, and each other
    for (int i = 0; i < 6; ++i) {
        DesktopItem loose{};
        loose.path = L"C:\\Desktop\\loose_" + std::to_wstring(i) + L".txt";
        loose.name = L"loose_" + std::to_wstring(i) + L".txt";
        loose.extension = L".txt";
        loose.modified_unix_ms = kNowMs - 20 * kDayMs;
        loose.force_loose = (i % 2 == 0);
        loose.native_pos = (i < 4) ? PointI{40, 40} : PointI{board.x + 20, board.y + 20};
        state.items.push_back(std::move(loose));
    }

    const auto views = ComputeGroupViews(state, kNowMs);
    SettleLooseIcons(state.items, views, work, board);

    // Grouped item loose_pos remains untouched at {0, 0}
    CHECK(state.items[0].loose_pos == (PointI{0, 0}));

    // Verify every loose icon is aligned to the loose grid, inside work_area, avoids groups (+6px) & tile board, and has unique cell
    for (size_t i = 1; i < state.items.size(); ++i) {
        const PointI p = state.items[i].loose_pos;
        CHECK((p.x - (work.x + kLooseMarginX)) % kLooseCellW == 0);
        CHECK((p.y - (work.y + kLooseMarginY)) % kLooseCellH == 0);
        const RectI cell{p.x, p.y, kLooseCellW, kLooseCellH};
        CHECK(cell.x >= work.x && cell.y >= work.y);
        CHECK(cell.right() <= work.right() && cell.bottom() <= work.bottom());
        CHECK(!cell.intersects(board, 0));
        for (const GroupComputedView& gv : views) {
            CHECK(!cell.intersects(gv.frame, 6));
        }
        for (size_t j = i + 1; j < state.items.size(); ++j) {
            CHECK(state.items[j].loose_pos != p);
        }
    }

    // When organize_enabled == false and align_to_grid == false, native_pos is preserved when non-colliding!
    state.items[0].native_pos = {213, 187};
    SettleLooseIcons(state.items, {}, work, board,
                     DesktopIconSize::Medium, false, false, GroupSortBy::Name, true);
    CHECK(state.items[0].loose_pos == (PointI{213, 187}));
}

void TestTileBoardAndDisplacement() {
    CHECK(AllowedTileSizes().size() == 6);
    CHECK(IsValidTileSize({1, 1}));
    CHECK(IsValidTileSize({2, 1}));
    CHECK(IsValidTileSize({4, 1}));
    CHECK(IsValidTileSize({2, 2}));
    CHECK(IsValidTileSize({4, 2}));
    CHECK(IsValidTileSize({4, 4}));
    CHECK(IsValidTileSize({3, 2}));
    CHECK(IsValidTileSize({5, 1}));
    CHECK(IsValidTileSize({4, 6}));
    CHECK(!IsValidTileSize({13, 1}));
    CHECK(!IsValidTileSize({0, 1}));

    // 10 default tiles occupying 33/35 cells on 5x7 board, leaving (4,5) & (4,6) free
    const std::vector<DesktopTile> starter = MakeDefaultTiles();
    CHECK(starter.size() == 10);
    CHECK(HasValidTileBoard(starter));

    int total_cells = 0;
    for (const DesktopTile& t : starter) {
        total_cells += t.size.cols * t.size.rows;
    }
    CHECK(total_cells == 33);
    CHECK(starter[0].id == "photos" && starter[0].col == 0 && starter[0].row == 0 && starter[0].size == (TileSize{4, 2}));
    CHECK(starter[1].id == "recycle_bin" && starter[1].col == 4 && starter[1].row == 0 && starter[1].size == (TileSize{1, 1}));
    CHECK(starter[2].id == "this_pc" && starter[2].col == 4 && starter[2].row == 1 && starter[2].size == (TileSize{1, 1}));
    CHECK(starter[3].id == "downloads" && starter[3].col == 0 && starter[3].row == 2 && starter[3].size == (TileSize{2, 2}));
    CHECK(starter[4].id == "recent" && starter[4].col == 2 && starter[4].row == 2 && starter[4].size == (TileSize{2, 2}));
    CHECK(starter[5].id == "disk_ring" && starter[5].col == 4 && starter[5].row == 2 && starter[5].size == (TileSize{1, 1}));
    CHECK(starter[6].id == "pulse_launcher" && starter[6].col == 4 && starter[6].row == 3 && starter[6].size == (TileSize{1, 1}));
    CHECK(starter[7].id == "apps" && starter[7].kind == TileKind::Apps && starter[7].col == 0 && starter[7].row == 4 && starter[7].size == (TileSize{4, 2}));
    CHECK(starter[8].id == "clock" && starter[8].col == 4 && starter[8].row == 4 && starter[8].size == (TileSize{1, 1}) && starter[8].analog_clock);
    CHECK(starter[9].id == "staging_tray" && starter[9].col == 0 && starter[9].row == 6 && starter[9].size == (TileSize{4, 1}));

    const RectI board{1360, 44, 532, 748};
    const RectI photos_px = ComputeTilePixelRect(board, starter[0]);
    CHECK(photos_px.x == 1360 && photos_px.y == 44);
    CHECK(photos_px.w == 4 * kTileUnitPx + 3 * kTileGapPx);
    CHECK(photos_px.h == 2 * kTileUnitPx + 1 * kTileGapPx);

    // Narrow scaled board pixel geometry check: zero overflow and zero overlap
    const RectI narrow_board = ComputeTileBoardRect({0, 0, 900, 650}, true);
    for (size_t i = 0; i < starter.size(); ++i) {
        const RectI ri = ComputeTilePixelRect(narrow_board, starter[i]);
        CHECK(!ri.empty());
        CHECK(ri.x >= narrow_board.x && ri.y >= narrow_board.y);
        CHECK(ri.right() <= narrow_board.right() && ri.bottom() <= narrow_board.bottom());
        for (size_t j = i + 1; j < starter.size(); ++j) {
            const RectI rj = ComputeTilePixelRect(narrow_board, starter[j]);
            CHECK(!ri.intersects(rj, 0));
        }
    }

    // Swap Downloads (2x2 at 0,2) onto Recent (2x2 at 2,2) -> Recent relocates into (0,2)!
    const TilePlacementResult swap_res = PreviewTilePlacement(starter, "downloads", 2, 2, {2, 2});
    CHECK(swap_res.valid);
    CHECK(swap_res.updated_tiles.size() == starter.size());
    CHECK(HasValidTileBoard(swap_res.updated_tiles));
    for (const DesktopTile& t : swap_res.updated_tiles) {
        if (t.id == "downloads") {
            CHECK(t.col == 2 && t.row == 2);
        }
        if (t.id == "recent") {
            CHECK(t.col == 0 && t.row == 2);
        }
    }

    // Displace DiskRing (1x1 at 4,2) by moving RecycleBin (1x1 at 4,0) onto (4,2):
    // Vacated (4,0) has Manhattan dist 2 vs (4,5) dist 3, so DiskRing relocates to (4,0).
    const TilePlacementResult swap_1x1 = PreviewTilePlacement(starter, "recycle_bin", 4, 2, {1, 1});
    CHECK(swap_1x1.valid);
    CHECK(HasValidTileBoard(swap_1x1.updated_tiles));
    for (const DesktopTile& t : swap_1x1.updated_tiles) {
        if (t.id == "recycle_bin") {
            CHECK(t.col == 4 && t.row == 2);
        }
        if (t.id == "disk_ring") {
            CHECK(t.col == 4 && t.row == 0);
        }
    }

    // Displace Clock (1x1 at 4,4) by moving RecycleBin (1x1 at 4,0) onto (4,4):
    // Nearest free cell to (4,4) is (4,5) with Manhattan dist 1 (vs (4,0) dist 4).
    const TilePlacementResult one_res = PreviewTilePlacement(starter, "recycle_bin", 4, 4, {1, 1});
    CHECK(one_res.valid);
    CHECK(HasValidTileBoard(one_res.updated_tiles));
    for (const DesktopTile& t : one_res.updated_tiles) {
        if (t.id == "recycle_bin") {
            CHECK(t.col == 4 && t.row == 4);
        }
        if (t.id == "clock") {
            CHECK(t.col == 4 && t.row == 5);
        }
    }

    // Tie-breaking check: top-to-bottom, left-to-right on equal Manhattan distance (dist == 1)
    {
        DesktopTile a{};
        a.id = "a";
        a.col = 4;
        a.row = 6;
        a.size = {1, 1};
        DesktopTile center{};
        center.id = "center";
        center.col = 1;
        center.row = 1;
        center.size = {1, 1};
        const std::vector<DesktopTile> mini = {a, center};
        const TilePlacementResult tie = PreviewTilePlacement(mini, "a", 1, 1, {1, 1});
        CHECK(tie.valid);
        CHECK(tie.updated_tiles[1].col == 1 && tie.updated_tiles[1].row == 0);
    }

    // Impossible 4x4 resize on 33/35-occupied starter board -> MUST bounce (valid == false)!
    const TilePlacementResult bounce_resize = PreviewTilePlacement(starter, "downloads", 0, 2, {4, 4});
    CHECK(!bounce_resize.valid);
    CHECK(bounce_resize.updated_tiles.empty());

    // Out-of-bounds placement -> valid == false
    const TilePlacementResult oob_res = PreviewTilePlacement(starter, "downloads", 4, 2, {2, 2});
    CHECK(!oob_res.valid);

    // InsertNewTile: 1x1 at (4, 0) displaces RecycleBin cleanly
    DesktopTile extra{};
    extra.id = "custom_folder";
    extra.kind = TileKind::PinnedFolder;
    extra.title = L"设计稿";
    extra.col = 4;
    extra.row = 0;
    extra.size = {1, 1};
    const TilePlacementResult ins_ok = InsertNewTile(starter, extra);
    CHECK(ins_ok.valid);
    CHECK(ins_ok.updated_tiles.size() == 11);
    CHECK(HasValidTileBoard(ins_ok.updated_tiles));

    // InsertNewTile: impossible 4x4 on starter board -> rejected
    DesktopTile giant = extra;
    giant.id = "giant_4x4";
    giant.col = 0;
    giant.row = 0;
    giant.size = {4, 4};
    const TilePlacementResult ins_fail = InsertNewTile(starter, giant);
    CHECK(!ins_fail.valid);
}

void TestRulesSearchVersionAndHealthEngine() {
    // FormatRuleSummary checks
    {
        GroupRule doc_rule{};
        doc_rule.extensions = {L".docx", L".pdf"};
        doc_rule.modified_within_days = 7;
        CHECK(FormatRuleSummary(doc_rule) == L"Word · PDF · 7天内");

        GroupRule inst_rule{};
        inst_rule.extensions = {L".EXE", L".msi"};
        CHECK(FormatRuleSummary(inst_rule) == L".exe · .msi");

        GroupRule fallback_rule{};
        fallback_rule.fallback_catch_all = true;
        CHECK(FormatRuleSummary(fallback_rule) == L"其他桌面文件");

        GroupRule manual_rule{};
        CHECK(FormatRuleSummary(manual_rule) == L"手动归入");
    }

    // MatchesDesktopSearchQuery checks (case-insensitive substring + 7 semantic Chinese keywords)
    {
        DesktopItem img{};
        img.name = L"Wallpaper_4K.webp";
        img.extension = L".webp";
        CHECK(MatchesDesktopSearchQuery(img, L"wallpaper"));
        CHECK(MatchesDesktopSearchQuery(img, L"图片"));
        CHECK(!MatchesDesktopSearchQuery(img, L"截图"));
        CHECK(!MatchesDesktopSearchQuery(img, L"文档"));

        DesktopItem sheet{};
        sheet.name = L"Q4_Budget.xlsx";
        sheet.extension = L".xlsx";
        CHECK(MatchesDesktopSearchQuery(sheet, L"表格"));
        CHECK(MatchesDesktopSearchQuery(sheet, L"文档"));
        CHECK(!MatchesDesktopSearchQuery(sheet, L"演示"));

        DesktopItem slide{};
        slide.name = L"Roadmap.pptx";
        slide.extension = L".pptx";
        CHECK(MatchesDesktopSearchQuery(slide, L"演示"));
        CHECK(MatchesDesktopSearchQuery(slide, L"文档"));

        DesktopItem setup{};
        setup.name = L"7z2600-x64.msi";
        setup.extension = L".msi";
        CHECK(MatchesDesktopSearchQuery(setup, L"安装包"));

        DesktopItem archive{};
        archive.name = L"logs_backup.7z";
        archive.extension = L".7z";
        CHECK(MatchesDesktopSearchQuery(archive, L"压缩包"));

        DesktopItem dir{};
        dir.name = L"客户资料";
        dir.is_folder = true;
        CHECK(MatchesDesktopSearchQuery(dir, L"文件夹"));
        CHECK(!MatchesDesktopSearchQuery(dir, L"文档"));
        CHECK(!MatchesDesktopSearchQuery(dir, L"   "));
    }

    const RectI work{0, 0, 1920, 1032};
    std::vector<DesktopGroup> groups = MakeDefaultGroups(work);

    // Add a manual-only group (empty criteria, fallback_catch_all = false) before uncategorized:
    // It must NOT greedily swallow arbitrary files unless pinned_group_id matches.
    DesktopGroup manual_grp{};
    manual_grp.id = "manual_vip";
    manual_grp.title = L"重点项目";
    groups.insert(groups.begin() + 3, manual_grp);

    DesktopItem installer_old{};
    installer_old.name = L"7-Zip-26.00-x64.exe";
    installer_old.extension = L".EXE";
    installer_old.modified_unix_ms = kNowMs - 10 * kDayMs;
    CHECK(ClassifyItemToGroup(installer_old, groups, kNowMs) == 1u);

    DesktopItem pinned_item = installer_old;
    pinned_item.pinned_group_id = "manual_vip";
    CHECK(ClassifyItemToGroup(pinned_item, groups, kNowMs) == 3u);

    DesktopItem forced_loose = pinned_item;
    forced_loose.force_loose = true;
    CHECK(!ClassifyItemToGroup(forced_loose, groups, kNowMs).has_value());

    DesktopItem old_misc{};
    old_misc.name = L"notes.txt";
    old_misc.extension = L".txt";
    old_misc.modified_unix_ms = kNowMs - 20 * kDayMs;
    // Falls past manual_vip (index 3) into uncategorized (index 4)!
    CHECK(ClassifyItemToGroup(old_misc, groups, kNowMs) == 4u);

    // Compound GroupRule check (prefix + contains + tag)
    GroupRule custom_rule{};
    custom_rule.extensions = {L".png"};
    custom_rule.name_prefixes = {L"snipaste_"};
    custom_rule.name_contains = {L"review"};
    custom_rule.tag_id = L"urgent";
    DesktopItem tagged_shot{};
    tagged_shot.name = L"Snipaste_UI_Review_01.PNG";
    tagged_shot.extension = L".png";
    tagged_shot.tag_id = L"URGENT";
    CHECK(ItemMatchesRule(tagged_shot, custom_rule, kNowMs));
    tagged_shot.tag_id = L"archive";
    CHECK(!ItemMatchesRule(tagged_shot, custom_rule, kNowMs));

    // Version string comparator
    CHECK(CompareVersionStrings(L"26.00", L"26.0") == 0);
    CHECK(CompareVersionStrings(L"v22.11.0", L"22.11.0.0") == 0);
    CHECK(CompareVersionStrings(L"1.96.2", L"1.96.10") < 0);
    CHECK(CompareVersionStrings(L"2026.1.4", L"2025.12.31") > 0);
    CHECK(CompareVersionStrings(L"7-Zip 26.00 (x64)", L"26.0") == 0);

    // Desktop Health Check
    const std::vector<InstalledAppRecord> apps = {
        {L"7-Zip", L"26.00", L"Igor Pavlov"},
        {L"Pulse", L"1.0.60", L"Pulse Team"},
    };

    std::vector<DesktopItem> desk_items;

    // 1. Installed installer (same version -> flagged)
    DesktopItem i_7z{};
    i_7z.path = L"C:\\Desktop\\7-Zip-26.00-x64.exe";
    i_7z.name = L"7-Zip-26.00-x64.exe";
    i_7z.extension = L".exe";
    i_7z.size_bytes = 2 * 1024 * 1024;
    i_7z.modified_unix_ms = kNowMs - 5 * kDayMs;
    desk_items.push_back(i_7z);

    // 2. Newer installer than installed version -> NOT flagged!
    DesktopItem i_newer{};
    i_newer.path = L"C:\\Desktop\\PulseSetup-2.0.0.exe";
    i_newer.name = L"PulseSetup-2.0.0.exe";
    i_newer.extension = L".exe";
    i_newer.size_bytes = 40 * 1024 * 1024;
    i_newer.modified_unix_ms = kNowMs - kDayMs;
    desk_items.push_back(i_newer);

    // 3. Stale screenshot (20 days > 14 days -> flagged)
    DesktopItem i_shot{};
    i_shot.path = L"C:\\Desktop\\屏幕截图 2026-10-01.png";
    i_shot.name = L"屏幕截图 2026-10-01.png";
    i_shot.extension = L".png";
    i_shot.size_bytes = 512 * 1024;
    i_shot.modified_unix_ms = kNowMs - 20 * kDayMs;
    desk_items.push_back(i_shot);

    // 4. Broken shortcut -> flagged
    DesktopItem i_lnk{};
    i_lnk.path = L"C:\\Desktop\\OldApp.lnk";
    i_lnk.name = L"OldApp.lnk";
    i_lnk.extension = L".lnk";
    i_lnk.is_shortcut = true;
    i_lnk.link_target = L"D:\\Removed\\OldApp.exe";
    i_lnk.link_target_missing = true;
    i_lnk.size_bytes = 2048;
    desk_items.push_back(i_lnk);

    // 5. Duplicate copy "(1)" -> flagged
    DesktopItem i_dup{};
    i_dup.path = L"C:\\Desktop\\季度汇报 (1).pptx";
    i_dup.name = L"季度汇报 (1).pptx";
    i_dup.extension = L".pptx";
    i_dup.size_bytes = 1024 * 1024;
    i_dup.modified_unix_ms = kNowMs - 2 * kDayMs;
    desk_items.push_back(i_dup);

    // 6. Normal file with year "(2026)" and non-empty folder (folder_is_empty == false) -> NOT flagged!
    DesktopItem i_clean{};
    i_clean.path = L"C:\\Desktop\\Annual Report (2026).pdf";
    i_clean.name = L"Annual Report (2026).pdf";
    i_clean.extension = L".pdf";
    i_clean.size_bytes = 409600;
    i_clean.modified_unix_ms = kNowMs - 2 * kDayMs;
    desk_items.push_back(i_clean);

    DesktopItem i_folder{};
    i_folder.path = L"C:\\Desktop\\Projects";
    i_folder.name = L"Projects";
    i_folder.is_folder = true;
    i_folder.folder_is_empty = false;
    i_folder.size_bytes = 0;
    i_folder.modified_unix_ms = kNowMs - 60 * kDayMs;
    desk_items.push_back(i_folder);

    // 7. Empty folder -> flagged as EmptyFolder (selected_by_default == true)
    DesktopItem i_empty_dir{};
    i_empty_dir.path = L"C:\\Desktop\\新建文件夹";
    i_empty_dir.name = L"新建文件夹";
    i_empty_dir.is_folder = true;
    i_empty_dir.folder_is_empty = true;
    i_empty_dir.size_bytes = 0;
    i_empty_dir.modified_unix_ms = kNowMs - 3 * kDayMs;
    desk_items.push_back(i_empty_dir);

    // 8. Stale archive (>14d .zip) -> flagged as OneTimeUse (selected_by_default == false)
    DesktopItem i_old_zip{};
    i_old_zip.path = L"C:\\Desktop\\sdk_bundle.zip";
    i_old_zip.name = L"sdk_bundle.zip";
    i_old_zip.extension = L".zip";
    i_old_zip.size_bytes = 8 * 1024 * 1024;
    i_old_zip.modified_unix_ms = kNowMs - 20 * kDayMs;
    desk_items.push_back(i_old_zip);

    // 9. Versionless installer "PulseSetup-x64.exe" when Pulse 1.0.60 is installed -> flagged!
    DesktopItem i_verless{};
    i_verless.path = L"C:\\Desktop\\PulseSetup-x64.exe";
    i_verless.name = L"PulseSetup-x64.exe";
    i_verless.extension = L".exe";
    i_verless.size_bytes = 38 * 1024 * 1024;
    i_verless.modified_unix_ms = kNowMs - 4 * kDayMs;
    desk_items.push_back(i_verless);

    // 10. 0-byte file -> flagged as DuplicateOrTemp
    DesktopItem i_zero{};
    i_zero.path = L"C:\\Desktop\\scratch.txt";
    i_zero.name = L"scratch.txt";
    i_zero.extension = L".txt";
    i_zero.size_bytes = 0;
    i_zero.modified_unix_ms = kNowMs - kDayMs;
    desk_items.push_back(i_zero);

    const HealthReport report = AnalyzeDesktopHealth(desk_items, apps, kNowMs, 14);
    CHECK(report.issues.size() == 8);
    CHECK(report.issues[0].kind == HealthIssueKind::InstalledInstaller);
    CHECK(report.issues[0].reason == L"已安装 7-Zip 26.00");
    CHECK(report.issues[1].kind == HealthIssueKind::StaleScreenshot);
    CHECK(report.issues[2].kind == HealthIssueKind::BrokenShortcut);
    CHECK(report.issues[3].kind == HealthIssueKind::DuplicateOrTemp);
    CHECK(report.issues[4].kind == HealthIssueKind::EmptyFolder);
    CHECK(report.issues[4].reason == L"空文件夹" && report.issues[4].selected_by_default);
    CHECK(report.issues[5].kind == HealthIssueKind::OneTimeUse);
    CHECK(report.issues[5].reason == L"用过可能不再需要" && !report.issues[5].selected_by_default);
    CHECK(report.issues[6].kind == HealthIssueKind::InstalledInstaller);
    CHECK(report.issues[6].reason == L"已安装 Pulse 1.0.60");
    CHECK(report.issues[7].kind == HealthIssueKind::DuplicateOrTemp);

    // total_reclaimable_bytes sums default-selected issues (excluding unchecked OneTimeUse)
    const uint64_t expected_bytes =
        i_7z.size_bytes + i_shot.size_bytes + i_lnk.size_bytes + i_dup.size_bytes + i_verless.size_bytes;
    CHECK(report.total_reclaimable_bytes == expected_bytes);
    CHECK(report.score >= 20 && report.score < 100);

    // Clean desktop yields score == 100
    const std::vector<DesktopItem> clean_only = {i_clean, i_folder};
    const HealthReport clean_report = AnalyzeDesktopHealth(clean_only, apps, kNowMs, 14);
    CHECK(clean_report.issues.empty());
    CHECK(clean_report.score == 100);

    // Heavy clutter clamps score at minimum 20
    std::vector<DesktopItem> many_dups(25, i_dup);
    const HealthReport heavy_report = AnalyzeDesktopHealth(many_dups, apps, kNowMs, 14);
    CHECK(heavy_report.score == 20);

    // Read-only HKLM/HKCU Uninstall probe smoke check
    const std::vector<InstalledAppRecord> live_apps = QueryInstalledAppsFromRegistry();
    for (const InstalledAppRecord& rec : live_apps) {
        CHECK(!rec.display_name.empty());
    }
}

void TestUndoHistoryAndJsonRoundTrip() {
    UndoHistory history(32);
    DesktopModelState current{};
    current.settings.card_opacity_percent = 50;

    for (int step = 0; step < 40; ++step) {
        history.Push(current, L"步骤 " + std::to_wstring(step));
        current.settings.card_opacity_percent = static_cast<uint8_t>(51 + step);
    }
    CHECK(history.UndoDepth() == 32);
    CHECK(history.CanUndo());
    CHECK(!history.CanRedo());
    CHECK(history.NextUndoLabel() == L"步骤 39");

    int undo_steps = 0;
    while (history.Undo(current)) {
        ++undo_steps;
    }
    CHECK(undo_steps == 32);
    CHECK(!history.CanUndo());
    CHECK(history.CanRedo());
    // Oldest retained snapshot out of 0..39 with max_depth=32 is step 8 (opacity = 50 + 8 = 58)
    CHECK(current.settings.card_opacity_percent == 58);

    int redo_steps = 0;
    while (history.Redo(current)) {
        ++redo_steps;
    }
    CHECK(redo_steps == 32);
    CHECK(current.settings.card_opacity_percent == 90);

    // Pushing after Undo clears Redo branch
    CHECK(history.Undo(current));
    CHECK(history.CanRedo());
    history.Push(current, L"新分支");
    CHECK(!history.CanRedo());

    // Full JSON round-trip covering all 1:1 fields
    const RectI work{0, 0, 1920, 1032};
    DesktopModelState orig{};
    orig.settings.dark_mode = false;
    orig.settings.default_material = CardMaterial::MicaAlt;
    orig.settings.card_opacity_percent = 84;
    orig.settings.auto_wallpaper_accent = false;
    orig.settings.accent_argb = 0xFFE5484Du;
    orig.settings.wallpaper_preset = WallpaperPreset::Forest;
    orig.settings.label_style = IconLabelStyle::FrostedPill;
    orig.settings.icon_size = DesktopIconSize::Large;
    orig.settings.active_scene = DesktopScene::Focus;
    orig.settings.organize_enabled = false;
    orig.settings.tiles_enabled = false;
    orig.settings.live_tiles_enabled = false;
    orig.settings.align_to_grid = false;
    orig.settings.loose_sort_by = GroupSortBy::Size;
    orig.settings.loose_sort_ascending = true;
    orig.settings.icons_hidden = true;
    orig.settings.health_check_enabled = false;
    orig.settings.restore_on_exit = false;

    MonitorWorkArea mon{};
    mon.device_name = L"\\\\.\\DISPLAY1";
    mon.bounds = {-1920, 0, 1920, 1080};
    mon.work_area = {-1920, 0, 1920, 1032};
    mon.dpi = 144;
    mon.primary = false;
    orig.monitors.push_back(mon);

    DesktopItem item{};
    item.path = L"C:\\Users\\张三\\Desktop\\季度汇报 \"终版\"\t🗂️.pptx";
    item.name = L"季度汇报 \"终版\"\t🗂️.pptx";
    item.extension = L".pptx";
    item.link_target = L"D:\\Projects\\Target.exe";
    item.installed_app_hint = L"7-Zip 26.00";
    item.is_folder = false;
    item.folder_is_empty = true;
    item.is_shortcut = true;
    item.link_target_missing = true;
    item.size_bytes = 1099511627776ULL; // 1 TiB (> 32-bit)
    item.modified_unix_ms = kNowMs;
    item.tag_argb = 0xFF30A46Cu;
    item.tag_id = L"进行中";
    item.native_pos = {-120, 340};
    item.loose_pos = {16, 114};
    item.pinned_group_id = "in_progress";
    item.force_loose = false;
    orig.items.push_back(item);

    orig.groups = MakeDefaultGroups(work);
    orig.groups[0].mapped_folder_path = L"D:\\Synced\\InProgress";
    orig.groups[1].rest_origin = PointI{392, 24};
    orig.groups[0].manual_order = {item.path, L"C:\\Users\\张三\\Desktop\\说明\n换行.md"};

    orig.tiles = MakeDefaultTiles();
    DesktopTile pinned_file{};
    pinned_file.id = "pinned_spec";
    pinned_file.kind = TileKind::PinnedItem;
    pinned_file.title = L"架构文档";
    pinned_file.target_path = L"D:\\Docs\\Spec.md";
    pinned_file.live_content = false;
    pinned_file.col = 4;
    pinned_file.row = 5;
    pinned_file.size = {1, 1};
    orig.tiles.push_back(pinned_file);

    const std::string json = SerializeModelStateJson(orig);
    CHECK(!json.empty());

    DesktopModelState restored{};
    CHECK(DeserializeModelStateJson(json, restored));
    CHECK(restored.settings == orig.settings);
    CHECK(restored.monitors.size() == 1);
    CHECK(restored.monitors[0].device_name == orig.monitors[0].device_name);
    CHECK(restored.monitors[0].bounds == orig.monitors[0].bounds);
    CHECK(restored.monitors[0].dpi == 144);
    CHECK(!restored.monitors[0].primary);

    CHECK(restored.items.size() == 1);
    CHECK(restored.items[0].path == orig.items[0].path);
    CHECK(restored.items[0].name == orig.items[0].name);
    CHECK(restored.items[0].installed_app_hint == orig.items[0].installed_app_hint);
    CHECK(restored.items[0].folder_is_empty == orig.items[0].folder_is_empty);
    CHECK(restored.items[0].size_bytes == orig.items[0].size_bytes);
    CHECK(restored.items[0].modified_unix_ms == orig.items[0].modified_unix_ms);
    CHECK(restored.items[0].native_pos == orig.items[0].native_pos);
    CHECK(restored.items[0].loose_pos == orig.items[0].loose_pos);
    CHECK(restored.items[0].tag_id == orig.items[0].tag_id);

    CHECK(restored.groups.size() == orig.groups.size());
    CHECK(restored.groups[0].title == L"进行中");
    CHECK(restored.groups[0].icon_glyph == L'\xE8A5');
    CHECK(restored.groups[0].rule_summary == L"最近 7 天改过的文档");
    CHECK(restored.groups[0].mapped_folder_path == L"D:\\Synced\\InProgress");
    CHECK(restored.groups[0].manual_order == orig.groups[0].manual_order);
    CHECK(restored.groups[1].rest_origin == orig.groups[1].rest_origin);
    CHECK(!restored.groups[0].rest_origin.has_value());

    CHECK(restored.tiles.size() == orig.tiles.size());
    CHECK(restored.tiles.back().kind == TileKind::PinnedItem);
    CHECK(!restored.tiles.back().live_content);
    CHECK(restored.tiles.back().target_path == L"D:\\Docs\\Spec.md");

    // UTF-8 BOM + legacy minimal JSON omitting all new fields must decode cleanly with defaults
    DesktopModelState escaped_state{};
    const std::string bom_escaped =
        "\xEF\xBB\xBF{\"version\":1,\"settings\":{\"dark_mode\":true},"
        "\"groups\":[{\"id\":\"g1\",\"title\":\"\\u8FDB\\u884C\\u4E2D \\uD83D\\uDCC2\"}]}";
    CHECK(DeserializeModelStateJson(bom_escaped, escaped_state));
    CHECK(escaped_state.settings.organize_enabled);
    CHECK(escaped_state.settings.tiles_enabled);
    CHECK(escaped_state.settings.restore_on_exit);
    CHECK(escaped_state.settings.icon_size == DesktopIconSize::Medium);
    CHECK(escaped_state.groups.size() == 1);
    CHECK(escaped_state.groups[0].title == L"进行中 📂");
    CHECK(escaped_state.groups[0].icon_glyph == L'\xE8B7');

    // Malformed JSON must fail cleanly without corrupting out_state
    const size_t prev_tile_count = restored.tiles.size();
    CHECK(!DeserializeModelStateJson("{broken json", restored));
    CHECK(!DeserializeModelStateJson("{\"version\": 999}", restored));
    CHECK(restored.tiles.size() == prev_tile_count);
}

bool WriteBinaryFile(const std::filesystem::path& path, const void* data, size_t len) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    if (len > 0) {
        ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(len));
    }
    return ofs.good();
}

std::string ReadBinaryFileAsString(const std::filesystem::path& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return {};
    return std::string((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
}

bool WriteDpapiTestKey(const std::filesystem::path& key_path,
                       std::wstring_view plain_key,
                       std::string_view entropy_ascii) {
    DATA_BLOB in{
        static_cast<DWORD>(plain_key.size() * sizeof(wchar_t)),
        const_cast<BYTE*>(reinterpret_cast<const BYTE*>(plain_key.data())),
    };
    std::string ent(entropy_ascii);
    DATA_BLOB entropy{
        static_cast<DWORD>(ent.size()),
        reinterpret_cast<BYTE*>(ent.data()),
    };
    DATA_BLOB out{};
    if (!CryptProtectData(&in, L"Pulse AI Key Test", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        return false;
    }
    const bool ok = WriteBinaryFile(key_path, out.pbData, out.cbData);
    if (out.pbData) LocalFree(out.pbData);
    return ok;
}

void TestAiOrganizerDpapiParseArchiveAndUndo() {
    using namespace pulse::desktop::ai;
    namespace fs = std::filesystem;

    struct ScopedTempDir {
        fs::path root;
        ScopedTempDir() {
            const std::wstring dir_name =
                L"pulse_desktop_ai_test_" + std::to_wstring(GetCurrentProcessId()) +
                L"_" + std::to_wstring(GetTickCount64());
            root = fs::temp_directory_path() / dir_name;
            std::error_code ec;
            fs::create_directories(root, ec);
        }
        ~ScopedTempDir() {
            std::error_code ec;
            fs::remove_all(root, ec);
        }
    } temp;

    const fs::path ai_dir = temp.root / L"ai_cfg";
    const fs::path desk_dir = temp.root / L"desk";
    std::error_code ec;
    fs::create_directories(ai_dir, ec);
    fs::create_directories(desk_dir, ec);

    // 1. ProbePulseAiAssistantConnection + DPAPI decryption tests
    {
        const AiConnectionInfo empty_probe = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(!empty_probe.configured);
        CHECK(empty_probe.badge_text() == L"未连接 AI 助手");

        // DeepSeek default service + model_key.bin + "Pulse.AI.ModelKey.v1"
        const std::string ds_json =
            "{\"enabled\":true,\"assistant\":{\"service\":\"deepseek\",\"model\":\"deepseek-flash\"}}";
        CHECK(WriteBinaryFile(ai_dir / L"ai.json", ds_json.data(), ds_json.size()));
        CHECK(WriteDpapiTestKey(ai_dir / L"model_key.bin", L"sk-test-deepseek-key", "Pulse.AI.ModelKey.v1"));
        const AiConnectionInfo ds_probe = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(ds_probe.configured);
        CHECK(ds_probe.assistant_enabled);
        CHECK(ds_probe.needs_key);
        CHECK(ds_probe.service_id == "deepseek");
        CHECK(ds_probe.provider_name == L"DeepSeek");
        CHECK(ds_probe.model_id == "deepseek-flash");
        CHECK(ds_probe.base_url == L"https://api.deepseek.com");
        CHECK(ds_probe.badge_text() == L"DeepSeek · deepseek-flash");

        // Wrong entropy on Qwen must fail DPAPI decryption -> unconfigured
        const std::string qwen_json =
            "{\"enabled\":true,\"assistant\":{\"service\":\"qwen\",\"model\":\"qwen-flash\"}}";
        CHECK(WriteBinaryFile(ai_dir / L"ai.json", qwen_json.data(), qwen_json.size()));
        CHECK(WriteDpapiTestKey(ai_dir / L"model_key_qwen.bin", L"sk-qwen", "Wrong.Entropy.v1"));
        const AiConnectionInfo qwen_bad = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(!qwen_bad.configured);

        // MiMo with tp- Token Plan key -> dedicated token-plan endpoint
        const std::string mimo_json =
            "{\"enabled\":true,\"assistant\":{\"service\":\"mimo\",\"model\":\"mimo-v2-pro\"}}";
        CHECK(WriteBinaryFile(ai_dir / L"ai.json", mimo_json.data(), mimo_json.size()));
        CHECK(WriteDpapiTestKey(ai_dir / L"model_key_mimo.bin", L"tp-token-plan-secret", "Pulse.AI.ModelKey.mimo.v1"));
        const AiConnectionInfo mimo_probe = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(mimo_probe.configured);
        CHECK(mimo_probe.provider_name == L"小米 MiMo");
        CHECK(mimo_probe.base_url == L"https://token-plan-cn.xiaomimimo.com/v1");

        // Ollama local keyless service -> configured without any model_key_ollama.bin
        const std::string ollama_json =
            "{\"enabled\":true,\"assistant\":{\"service\":\"ollama\",\"model\":\"qwen3:8b\"}}";
        CHECK(WriteBinaryFile(ai_dir / L"ai.json", ollama_json.data(), ollama_json.size()));
        const AiConnectionInfo ollama_probe = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(ollama_probe.configured);
        CHECK(!ollama_probe.needs_key);
        CHECK(ollama_probe.provider_name == L"Ollama");
        CHECK(ollama_probe.base_url == L"http://localhost:11434/v1");

        // Disabled assistant -> unconfigured
        const std::string disabled_json =
            "{\"enabled\":false,\"assistant\":{\"service\":\"ollama\",\"model\":\"qwen3:8b\"}}";
        CHECK(WriteBinaryFile(ai_dir / L"ai.json", disabled_json.data(), disabled_json.size()));
        const AiConnectionInfo disabled_probe = ProbePulseAiAssistantConnection(ai_dir.wstring());
        CHECK(!disabled_probe.assistant_enabled);
        CHECK(!disabled_probe.configured);
    }

    // 2. Folder name safety validation
    {
        CHECK(IsValidArchiveFolderName(L"项目归档 2026"));
        CHECK(IsValidArchiveFolderName(L"Design_Assets"));
        CHECK(!IsValidArchiveFolderName(L""));
        CHECK(!IsValidArchiveFolderName(L"."));
        CHECK(!IsValidArchiveFolderName(L".."));
        CHECK(!IsValidArchiveFolderName(L"foo..bar"));
        CHECK(!IsValidArchiveFolderName(L"a/b"));
        CHECK(!IsValidArchiveFolderName(L"a\\b"));
        CHECK(!IsValidArchiveFolderName(L"CON"));
        CHECK(!IsValidArchiveFolderName(L"com1.txt"));
        CHECK(!IsValidArchiveFolderName(L"lpt9"));
        CHECK(!IsValidArchiveFolderName(L" trailing "));
        CHECK(!IsValidArchiveFolderName(L"dotend."));
        CHECK(!IsValidArchiveFolderName(std::wstring(81, L'a')));
    }

    // 3. Create isolated mock desktop files & folders and test CollectDesktopAiContext
    const fs::path existing_archive_dir = desk_dir / L"已有归档";
    const fs::path empty_sub_dir = desk_dir / L"空目录";
    fs::create_directories(existing_archive_dir, ec);
    fs::create_directories(empty_sub_dir, ec);

    // Preexisting file inside 已有归档 with identical name "report.md" to verify KeepBoth never overwrites!
    const std::string preexisting_archived_bytes = "ORIGINAL_ARCHIVED_CONTENT_DO_NOT_OVERWRITE";
    const std::string desktop_report_bytes =
        "# Pulse Q4 Release Notes\n\n"
        "This desktop markdown document describes the upcoming Q4 desktop organizer milestone, "
        "performance metrics, and release checklist across multiple lines of text that exceed 160 chars "
        "so we also verify snippet truncation and whitespace collapsing.";
    CHECK(WriteBinaryFile(existing_archive_dir / L"report.md",
                          preexisting_archived_bytes.data(), preexisting_archived_bytes.size()));
    CHECK(WriteBinaryFile(existing_archive_dir / L"old_spec.pdf", "PDF", 3));
    CHECK(WriteBinaryFile(desk_dir / L"report.md", desktop_report_bytes.data(), desktop_report_bytes.size()));

    // UTF-16LE BOM text file
    {
        const std::wstring utf16_text = L"会议纪要：Q4   预算与供应商合同审批清单";
        std::vector<uint8_t> raw(2 + utf16_text.size() * sizeof(wchar_t));
        raw[0] = 0xFFu;
        raw[1] = 0xFEu;
        std::memcpy(raw.data() + 2, utf16_text.data(), utf16_text.size() * sizeof(wchar_t));
        CHECK(WriteBinaryFile(desk_dir / L"notes_utf16.txt", raw.data(), raw.size()));
    }

    // Binary installer file with embedded NUL bytes
    {
        const uint8_t bin_bytes[] = {'M', 'Z', 0x90, 0x00, 0x03, 0x00, 0x00, 0x00};
        CHECK(WriteBinaryFile(desk_dir / L"setup_v2.exe", bin_bytes, sizeof(bin_bytes)));
    }

    const RectI work_area{0, 0, 1920, 1032};
    DesktopModelState model_state{};
    model_state.groups = MakeDefaultGroups(work_area);
    model_state.tiles = MakeDefaultTiles();

    auto add_item = [&](const std::wstring& name, const fs::path& full_path,
                        const std::wstring& ext, uint64_t sz, int64_t age_d,
                        bool is_dir, bool dir_empty = false, bool is_lnk = false,
                        const std::wstring& lnk_target = {}) {
        DesktopItem it{};
        it.name = name;
        it.path = full_path.wstring();
        it.extension = ext;
        it.size_bytes = sz;
        it.modified_unix_ms = kNowMs - age_d * kDayMs;
        it.is_folder = is_dir;
        it.folder_is_empty = dir_empty;
        it.is_shortcut = is_lnk;
        it.link_target = lnk_target;
        model_state.items.push_back(std::move(it));
    };

    add_item(L"已有归档", existing_archive_dir, L"", 0, 30, true, false);
    add_item(L"空目录", empty_sub_dir, L"", 0, 20, true, true);
    add_item(L"report.md", desk_dir / L"report.md", L".md", desktop_report_bytes.size(), 12, false);
    add_item(L"notes_utf16.txt", desk_dir / L"notes_utf16.txt", L".txt", 64, 15, false);
    add_item(L"setup_v2.exe", desk_dir / L"setup_v2.exe", L".exe", 8, 45, false);
    add_item(L"Chrome.lnk", desk_dir / L"Chrome.lnk", L".lnk", 1024, 90, false, false, true,
             L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe");

    // Virtual shell entry must be ignored by CollectDesktopAiContext
    {
        DesktopItem virt{};
        virt.name = L"回收站";
        virt.path = L"::{645FF040-5081-101B-9F08-00AA002F954E}";
        model_state.items.push_back(std::move(virt));
    }

    std::vector<DesktopAiCandidate> candidates;
    std::vector<ExistingDesktopFolder> existing_folders;
    CollectDesktopAiContext(desk_dir.wstring(), model_state.items, model_state.groups, kNowMs,
                            candidates, existing_folders);

    CHECK(existing_folders.size() == 1);
    CHECK(existing_folders[0].name == L"已有归档");
    CHECK(existing_folders[0].sample_children.size() == 2);
    CHECK(candidates.size() == 6); // 6 real entries, virtual shell entry skipped

    const DesktopAiCandidate* report_cand = nullptr;
    const DesktopAiCandidate* utf16_cand = nullptr;
    const DesktopAiCandidate* exe_cand = nullptr;
    const DesktopAiCandidate* empty_dir_cand = nullptr;
    for (const auto& c : candidates) {
        if (c.name == L"report.md") report_cand = &c;
        else if (c.name == L"notes_utf16.txt") utf16_cand = &c;
        else if (c.name == L"setup_v2.exe") exe_cand = &c;
        else if (c.name == L"空目录") empty_dir_cand = &c;
    }
    CHECK(report_cand != nullptr);
    CHECK(!report_cand->snippet.empty());
    CHECK(report_cand->snippet.size() <= 160);
    CHECK(report_cand->snippet.find(L"Pulse Q4 Release Notes") != std::wstring::npos);

    CHECK(utf16_cand != nullptr);
    CHECK(utf16_cand->snippet == L"会议纪要：Q4 预算与供应商合同审批清单");

    CHECK(exe_cand != nullptr);
    CHECK(exe_cand->snippet.empty());

    CHECK(empty_dir_cand != nullptr);
    CHECK(empty_dir_cand->is_folder && empty_dir_cand->folder_is_empty);

    // 4. BuildAiOrganizeChatBody thinking suppression & payload checks
    {
        const std::string body_ds = BuildAiOrganizeChatBody(
            "deepseek", "deepseek-chat", AiOrganizeStrategy::ByProjectTopic,
            L"按项目归档", candidates, existing_folders);
        CHECK(body_ds.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
        CHECK(body_ds.find("Pulse Q4 Release Notes") != std::string::npos);

        const std::string body_r1 = BuildAiOrganizeChatBody(
            "deepseek", "deepseek-r1", AiOrganizeStrategy::ArchiveClutter, {}, candidates, existing_folders);
        CHECK(body_r1.find("\"thinking\":{\"type\":\"disabled\"}") == std::string::npos);

        const std::string body_glm5 = BuildAiOrganizeChatBody(
            "zhipu", "glm-5-plus", AiOrganizeStrategy::ArchiveClutter, {}, candidates, existing_folders);
        CHECK(body_glm5.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);

        const std::string body_glm53 = BuildAiOrganizeChatBody(
            "zhipu", "glm-5.3", AiOrganizeStrategy::ArchiveClutter, {}, candidates, existing_folders);
        CHECK(body_glm53.find("\"thinking\":{\"type\":\"disabled\"}") == std::string::npos);
    }

    // 5. ParseAndValidateAiOrganizeReply strict safety & deduplication checks
    AiOrganizePlan parsed_plan{};
    std::wstring parse_err;
    {
        // Unsafe folder traversal -> rejected
        CHECK(!ParseAndValidateAiOrganizeReply(
            "{\"summary\":\"bad\",\"groups\":[{\"folder_name\":\"../escape\",\"items\":[\"report.md\"]}]}",
            desk_dir.wstring(), candidates, existing_folders, parsed_plan, parse_err));

        // Reserved folder name -> rejected
        CHECK(!ParseAndValidateAiOrganizeReply(
            "{\"summary\":\"bad\",\"groups\":[{\"folder_name\":\"CON\",\"items\":[\"report.md\"]}]}",
            desk_dir.wstring(), candidates, existing_folders, parsed_plan, parse_err));

        // Unknown / path-traversal item name -> rejected
        CHECK(!ParseAndValidateAiOrganizeReply(
            "{\"summary\":\"bad\",\"groups\":[{\"folder_name\":\"归档\",\"items\":[\"..\\\\secret.txt\"]}]}",
            desk_dir.wstring(), candidates, existing_folders, parsed_plan, parse_err));

        // Valid Markdown-fenced JSON with existing folder reuse, case-insensitive deduplication, and self-move guard
        const std::string fenced_reply =
            "```json\n"
            "{\n"
            "  \"summary\": \"已按项目文档与安装包整理桌面\",\n"
            "  \"groups\": [\n"
            "    {\n"
            "      \"folder_name\": \"已有归档\",\n"
            "      \"reason\": \"复用桌面已有归档目录存放报告与纪要\",\n"
            "      \"items\": [\"report.md\", \"REPORT.MD\", \"已有归档\", \"notes_utf16.txt\"]\n"
            "    },\n"
            "    {\n"
            "      \"folder_name\": \"安装包与空目录\",\n"
            "      \"reason\": \"收纳旧安装包与空文件夹\",\n"
            "      \"items\": [\"setup_v2.exe\", \"notes_utf16.txt\", \"空目录\"]\n"
            "    }\n"
            "  ]\n"
            "}\n"
            "```";
        CHECK(ParseAndValidateAiOrganizeReply(
            fenced_reply, desk_dir.wstring(), candidates, existing_folders, parsed_plan, parse_err));
        CHECK(parse_err.empty());
        CHECK(parsed_plan.summary == L"已按项目文档与安装包整理桌面");
        CHECK(parsed_plan.groups.size() == 2);

        // Group 0: "已有归档" (already exists, "已有归档" self-move excluded, "REPORT.MD" deduplicated)
        CHECK(parsed_plan.groups[0].folder_already_exists);
        CHECK(parsed_plan.groups[0].items.size() == 2);
        CHECK(parsed_plan.groups[0].items[0].name == L"report.md");
        CHECK(parsed_plan.groups[0].items[1].name == L"notes_utf16.txt");

        // Group 1: "安装包与空目录" (new folder, cross-group duplicate "notes_utf16.txt" excluded)
        CHECK(!parsed_plan.groups[1].folder_already_exists);
        CHECK(parsed_plan.groups[1].items.size() == 2);
        CHECK(parsed_plan.groups[1].items[0].name == L"setup_v2.exe");
        CHECK(parsed_plan.groups[1].items[1].name == L"空目录");

        CHECK(parsed_plan.SelectedItemCount() == 4);
        CHECK(parsed_plan.ActiveFolderCount() == 2);
        CHECK(parsed_plan.NewFolderCount() == 1);
    }

    // 6. Local semantic fallback & async AiOrganizeSession checks
    {
        const AiOrganizePlan local_plan = BuildSmartLocalOrganizePlan(
            desk_dir.wstring(), AiOrganizeStrategy::ArchiveClutter, {}, candidates, existing_folders);
        CHECK(!local_plan.from_ai_model);
        CHECK(local_plan.model_badge == L"本地智能规则");
        CHECK(!local_plan.groups.empty());

        // Async session with force_local_fallback = true
        {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
            AiOrganizeSession::Outcome outcome{};

            AiOrganizeSession::Request req{};
            req.desktop_dir = desk_dir.wstring();
            req.override_ai_dir = (temp.root / L"missing_ai_dir").wstring();
            req.force_local_fallback = true;
            req.items = model_state.items;
            req.groups = model_state.groups;
            req.now_unix_ms = kNowMs;

            auto session = AiOrganizeSession::Start(std::move(req), [&](AiOrganizeSession::Outcome out) {
                std::lock_guard<std::mutex> lk(mu);
                outcome = std::move(out);
                done = true;
                cv.notify_all();
            });
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; });
            CHECK(done);
            CHECK(outcome.ok);
            CHECK(!outcome.plan.groups.empty());
        }

        // Async session with unconfigured AI directory -> outcome.unconfigured == true
        {
            std::mutex mu;
            std::condition_variable cv;
            bool done = false;
            AiOrganizeSession::Outcome outcome{};

            AiOrganizeSession::Request req{};
            req.desktop_dir = desk_dir.wstring();
            req.override_ai_dir = (temp.root / L"missing_ai_dir").wstring();
            req.force_local_fallback = false;
            req.items = model_state.items;
            req.groups = model_state.groups;
            req.now_unix_ms = kNowMs;

            auto session = AiOrganizeSession::Start(std::move(req), [&](AiOrganizeSession::Outcome out) {
                std::lock_guard<std::mutex> lk(mu);
                outcome = std::move(out);
                done = true;
                cv.notify_all();
            });
            std::unique_lock<std::mutex> lk(mu);
            cv.wait_for(lk, std::chrono::seconds(5), [&] { return done; });
            CHECK(done);
            CHECK(!outcome.ok);
            CHECK(outcome.unconfigured);
        }
    }

    // 7. Non-destructive virtual groups mode (does NOT move files on disk)
    {
        DesktopModelState vg_model = model_state;
        const RectI board_rect = ComputeTileBoardRect(work_area, true);
        ApplyAiPlanAsVirtualGroups(parsed_plan, vg_model, work_area, board_rect);
        CHECK(fs::exists(desk_dir / L"report.md"));
        CHECK(fs::exists(desk_dir / L"setup_v2.exe"));
        CHECK(HasNoGroupOverlaps(vg_model.groups, work_area, board_rect));
    }

    // 8. Real-folder collision-safe ExecuteAiArchivePlan + 100% lossless UndoAiArchiveTransaction
    {
        DesktopModelState post_model{};
        const AiArchiveExecutionResult exec_res = ExecuteAiArchivePlan(
            parsed_plan, model_state, /*pin_created_folders_to_tiles=*/true, &post_model);
        CHECK(exec_res.ok);
        CHECK(exec_res.moved_count == 4);
        CHECK(exec_res.created_folder_count == 1);
        CHECK(!exec_res.undo_tx.empty());

        // Verify source files moved away from desktop root
        CHECK(!fs::exists(desk_dir / L"report.md"));
        CHECK(!fs::exists(desk_dir / L"notes_utf16.txt"));
        CHECK(!fs::exists(desk_dir / L"setup_v2.exe"));
        CHECK(!fs::exists(empty_sub_dir));

        // Verify KeepBoth collision safety: preexisting 已有归档\report.md MUST be untouched,
        // and desktop report.md MUST be archived as 已有归档\report (2).md!
        CHECK(fs::exists(existing_archive_dir / L"report.md"));
        CHECK(ReadBinaryFileAsString(existing_archive_dir / L"report.md") == preexisting_archived_bytes);
        CHECK(fs::exists(existing_archive_dir / L"report (2).md"));
        CHECK(ReadBinaryFileAsString(existing_archive_dir / L"report (2).md") == desktop_report_bytes);
        CHECK(fs::exists(existing_archive_dir / L"notes_utf16.txt"));

        // Verify newly created folder & moved items
        const fs::path new_folder_path = desk_dir / L"安装包与空目录";
        CHECK(fs::is_directory(new_folder_path));
        CHECK(fs::exists(new_folder_path / L"setup_v2.exe"));
        CHECK(fs::is_directory(new_folder_path / L"空目录"));

        // Verify post_model updated & newly created folder pinned validly to 5x7 tile board
        CHECK(HasValidTileBoard(post_model.tiles));
        bool found_pinned_tile = false;
        for (const auto& t : post_model.tiles) {
            if (t.kind == TileKind::PinnedFolder && t.target_path == new_folder_path.wstring()) {
                found_pinned_tile = true;
                break;
            }
        }
        CHECK(found_pinned_tile);

        // Now execute lossless rollback via UndoAiArchiveTransaction!
        DesktopModelState restored_model{};
        size_t restored_files = 0;
        CHECK(UndoAiArchiveTransaction(exec_res.undo_tx, restored_model, &restored_files));
        CHECK(restored_files == 4);

        // Verify all 4 items are back on desktop root with exact byte-for-byte contents,
        // preexisting 已有归档\report.md is still intact, and newly created 安装包与空目录 was removed!
        CHECK(fs::exists(desk_dir / L"report.md"));
        CHECK(ReadBinaryFileAsString(desk_dir / L"report.md") == desktop_report_bytes);
        CHECK(fs::exists(existing_archive_dir / L"report.md"));
        CHECK(ReadBinaryFileAsString(existing_archive_dir / L"report.md") == preexisting_archived_bytes);
        CHECK(!fs::exists(existing_archive_dir / L"report (2).md"));
        CHECK(fs::exists(desk_dir / L"notes_utf16.txt"));
        CHECK(fs::exists(desk_dir / L"setup_v2.exe"));
        CHECK(fs::is_directory(empty_sub_dir));
        CHECK(!fs::exists(new_folder_path));
        CHECK(fs::is_directory(existing_archive_dir));
        CHECK(restored_model.items.size() == model_state.items.size());
        CHECK(restored_model.tiles.size() == model_state.tiles.size());
    }
}

// Widgets: the default smart stack, stacking and taking out, the 应用 tile, the old 源代码 default, persistence.
void TestStackAcrossSizes() {
    // A calendar taken out of a 2x2 stack at 1x1 (no room for 2x2) goes back by dropping it on the stack.
    std::vector<DesktopTile> tiles(4);
    tiles[0].id = "weather"; tiles[0].kind = TileKind::Weather; tiles[0].size = {2, 2};
    tiles[1].id = "calendar"; tiles[1].kind = TileKind::Calendar; tiles[1].size = {1, 1}; tiles[1].col = 3;
    tiles[2].id = "disk"; tiles[2].kind = TileKind::DiskRing; tiles[2].size = {1, 1}; tiles[2].col = 4;
    tiles[3].id = "apps"; tiles[3].kind = TileKind::Apps; tiles[3].size = {4, 2}; tiles[3].row = 3;
    CHECK(CanStackTiles(tiles, "calendar", "weather"));
    CHECK(CanStackTiles(tiles, "disk", "weather"));       // non-Apps widgets can stack freely
    CHECK(!CanStackTiles(tiles, "apps", "weather"));      // the paged 应用 tile stays out
    CHECK(!CanStackTiles(tiles, "calendar", "apps"));
    CHECK(StackTiles(tiles, "calendar", "weather"));
    CHECK(tiles.size() == 3);
    for (const auto& t : tiles)
        if (t.id == "weather") {
            CHECK(t.stack.size() == 2);
            CHECK(t.size == (TileSize{2, 2}));
            CHECK(!t.stack.empty() && t.stack.back().kind == TileKind::Calendar);
        }
}

void TestWidgetsStacksAndAppsTile() {
    auto tiles = MakeDefaultTiles();
    const auto recent = std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "recent"; });
    CHECK(recent != tiles.end());
    CHECK(recent->stack.size() == 3);
    CHECK(ActiveTileView(*recent).kind == TileKind::Recent);
    CHECK(BoardHasKind(tiles, TileKind::Weather));
    CHECK(BoardHasKind(tiles, TileKind::Apps));
    CHECK(!BoardHasKind(tiles, TileKind::PinnedFolder));

    // Stack the 2x2 下载 onto the stack: four members, showing the newest; Apps still refuses stacking.
    CHECK(CanStackTiles(tiles, "recycle_bin", "recent"));
    CHECK(CanStackTiles(tiles, "clock", "recent"));
    CHECK(StackTiles(tiles, "downloads", "recent"));
    const auto st = std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "recent"; });
    CHECK(st->stack.size() == 4);
    CHECK(ActiveTileView(*st).kind == TileKind::Downloads);
    CHECK(std::none_of(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "downloads"; }));

    // Take it out again: back on the board with its id and size.
    CHECK(UnstackActiveMember(tiles, "recent"));
    const auto back = std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "downloads"; });
    CHECK(back != tiles.end() && back->kind == TileKind::Downloads);
    CHECK(std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "recent"; })->stack.size() == 3);

    // Sizes per kind (all kinds offer flexible multi-row presets plus free edge/corner sizing).
    CHECK(AllowedTileSizesFor(TileKind::Weather).size() >= 6);
    CHECK(AllowedTileSizesFor(TileKind::Todo).size() >= 7);
    CHECK(AllowedTileSizesFor(TileKind::RecycleBin).size() >= 4);

    // The 应用 tile collects shortcuts while organised; a forced-loose one stays out; off when tiles are off.
    DesktopModelState st2{};
    st2.tiles = MakeDefaultTiles();
    DesktopItem lnk{}; lnk.path = L"C:\\D\\微信.lnk"; lnk.name = L"微信"; lnk.extension = L".lnk"; lnk.is_shortcut = true;
    DesktopItem doc{}; doc.path = L"C:\\D\\a.docx"; doc.name = L"a.docx"; doc.extension = L".docx";
    DesktopItem loose = lnk; loose.path = L"C:\\D\\QQ.lnk"; loose.force_loose = true;
    DesktopItem folder_link = lnk; folder_link.path = L"C:\\D\\规范.lnk"; folder_link.link_target = L"P:\\规范、图集";
    lnk.link_target = L"C:\\Program Files\\WeChat\\WeChat.exe";
    st2.items = {lnk, doc, loose, folder_link};
    UpdateAppsTileMembership(st2);
    CHECK(st2.items[0].in_apps_tile);
    CHECK(!st2.items[1].in_apps_tile);
    CHECK(!st2.items[2].in_apps_tile);
    CHECK(!st2.items[3].in_apps_tile);  // a link to a folder is not an app
    CHECK(!ClassifyItemToGroup(st2.items[0], MakeDefaultGroups({0, 0, 1920, 1032}), 0).has_value());
    st2.settings.tiles_enabled = false;
    UpdateAppsTileMembership(st2);
    CHECK(!st2.items[0].in_apps_tile);

    // A board saved before widgets: its plain 2x2 最近使用 becomes the stack once, and only once.
    DesktopModelState old_state{};
    old_state.tiles = MakeDefaultTiles();
    for (auto& t : old_state.tiles) t.stack.clear();
    ApplyWidgetDefaults(old_state);
    const auto stacked = std::find_if(old_state.tiles.begin(), old_state.tiles.end(), [](const DesktopTile& t) { return t.id == "recent"; });
    CHECK(stacked->stack.size() == 3 && old_state.widget_defaults_applied);
    stacked->stack.clear();
    ApplyWidgetDefaults(old_state);
    CHECK(stacked->stack.empty());

    // An old saved board with the untouched 源代码 default gets the 应用 tile in its place.
    std::vector<DesktopTile> old_board(1);
    old_board[0].id = "pinned_folder"; old_board[0].kind = TileKind::PinnedFolder; old_board[0].title = L"源代码";
    MigrateDefaultTiles(old_board);
    CHECK(old_board[0].kind == TileKind::Apps && old_board[0].id == "apps");

    // Look, stack and to-dos survive a save and load.
    DesktopModelState st3{};
    st3.tiles = MakeDefaultTiles();
    st3.tiles[0].look.background = TileBackground::Color;
    st3.tiles[0].look.color_argb = 0xFF10893Eu;
    st3.tiles[0].look.title_override = L"相册";
    st3.tiles[0].look.show_title = false;
    st3.todos = MakeDefaultTodos();
    st3.settings.weather_city = L"杭州";
    DesktopModelState round{};
    CHECK(DeserializeModelStateJson(SerializeModelStateJson(st3), round));
    CHECK(round.tiles.size() == st3.tiles.size());
    CHECK(round.tiles[0].look == st3.tiles[0].look);
    const auto rs = std::find_if(round.tiles.begin(), round.tiles.end(), [](const DesktopTile& t) { return t.id == "recent"; });
    CHECK(rs != round.tiles.end() && rs->stack.size() == 3 && rs->stack[1].kind == TileKind::Calendar);
    CHECK(round.todos.size() == 4 && round.todos[2].done);
    CHECK(round.settings.weather_city == L"杭州");
}

void TestMultiPageGroupsAndMappedFolders() {
    const RectI work{0, 0, 1920, 1032};
    auto find_view = [](const std::vector<GroupComputedView>& views, const std::string& id) -> const GroupComputedView* {
        for (const auto& v : views) {
            if (v.group_id == id) return &v;
        }
        return nullptr;
    };

    // 1. Multi-page "未归类" pagination: 61 items in a 4x3 grid -> 6 pages, constant card height on last page.
    DesktopModelState st1{};
    st1.groups = MakeDefaultGroups(work);
    DesktopGroup* fb_group = nullptr;
    for (auto& g : st1.groups) {
        if (g.rule.fallback_catch_all) {
            fb_group = &g;
            break;
        }
    }
    CHECK(fb_group != nullptr);
    const std::string fb_id = fb_group->id;
    fb_group->cols = 4;
    fb_group->visible_rows = 3; // capacity = 12
    for (int i = 0; i < 61; ++i) {
        DesktopItem it{};
        it.path = L"C:\\Users\\Demo\\Desktop\\drawing_" + std::to_wstring(i) + L".dwg";
        it.name = L"drawing_" + std::to_wstring(i) + L".dwg";
        it.extension = L".dwg";
        it.modified_unix_ms = kNowMs - 30 * kDayMs;
        st1.items.push_back(std::move(it));
    }

    auto views0 = ComputeGroupViews(st1, kNowMs);
    const GroupComputedView* gv0 = find_view(views0, fb_id);
    CHECK(gv0 != nullptr);
    CHECK(gv0->total_item_count == 61);
    CHECK(gv0->page_capacity == 12);
    CHECK(gv0->page_count == 6);
    CHECK(gv0->page_index == 0);
    CHECK(gv0->visible_item_indices.size() == 12);
    CHECK(gv0->overflow_count == 49);
    const int page0_h = gv0->frame.h;

    std::unordered_map<std::string, int> pages{{fb_id, 5}};
    auto views5 = ComputeGroupViews(st1, kNowMs, &pages);
    const GroupComputedView* gv5 = find_view(views5, fb_id);
    CHECK(gv5 != nullptr);
    CHECK(gv5->page_index == 5);
    CHECK(gv5->page_count == 6);
    CHECK(gv5->visible_item_indices.size() == 1);
    CHECK(gv5->frame.h == page0_h);

    pages[fb_id] = 99;
    auto views_hi = ComputeGroupViews(st1, kNowMs, &pages);
    const GroupComputedView* gv_hi = find_view(views_hi, fb_id);
    CHECK(gv_hi != nullptr && gv_hi->page_index == 5);

    pages[fb_id] = -3;
    auto views_lo = ComputeGroupViews(st1, kNowMs, &pages);
    const GroupComputedView* gv_lo = find_view(views_lo, fb_id);
    CHECK(gv_lo != nullptr && gv_lo->page_index == 0);

    fb_group->collapsed = true;
    auto views_col = ComputeGroupViews(st1, kNowMs, &pages);
    const GroupComputedView* gv_col = find_view(views_col, fb_id);
    CHECK(gv_col != nullptr);
    CHECK(gv_col->frame.h == kGroupHeaderHeight);
    CHECK(gv_col->visible_item_indices.empty());
    CHECK(gv_col->page_count == 1);
    CHECK(gv_col->page_capacity == 0);
    fb_group->collapsed = false;

    // 2. Single-subfolder detection & folder-mapped group isolation.
    DesktopModelState st2{};
    st2.groups = MakeDefaultGroups(work);
    st2.tiles = MakeDefaultTiles();
    std::string fb2_id;
    for (const auto& g : st2.groups) {
        if (g.rule.fallback_catch_all) {
            fb2_id = g.id;
            break;
        }
    }
    CHECK(!fb2_id.empty());

    DesktopGroup g_custom{};
    g_custom.id = "grp_pic";
    g_custom.title = L"新建分组";
    g_custom.cols = 4;
    g_custom.visible_rows = 2;
    st2.groups.push_back(g_custom);

    DesktopItem it_dir{};
    it_dir.path = L"C:\\Users\\Demo\\Desktop\\截图与图片";
    it_dir.name = L"截图与图片";
    it_dir.is_folder = true;
    it_dir.folder_is_empty = true;
    it_dir.pinned_group_id = "grp_pic";
    it_dir.loose_pos = {-999, -999};

    DesktopItem it_fb_dir{};
    it_fb_dir.path = L"C:\\Users\\Demo\\Desktop\\杂项";
    it_fb_dir.name = L"杂项";
    it_fb_dir.is_folder = true;

    st2.items = {it_dir, it_fb_dir};
    auto mv1 = ComputeGroupViews(st2, kNowMs);
    const GroupComputedView* v_pic1 = find_view(mv1, "grp_pic");
    const GroupComputedView* v_fb1 = find_view(mv1, fb2_id);
    CHECK(v_pic1 != nullptr && v_pic1->single_subfolder_path == it_dir.path && v_pic1->single_subfolder_name == L"截图与图片");
    CHECK(v_fb1 != nullptr && v_fb1->single_subfolder_path.empty());

    st2.groups.back().mapped_folder_path = it_dir.path;
    st2.items[0].hidden_by_mapped_group = true;
    st2.items[0].pinned_group_id.clear();

    DesktopItem child_img{};
    child_img.path = L"C:\\Users\\Demo\\Desktop\\截图与图片\\shot.png";
    child_img.name = L"shot.png";
    child_img.extension = L".png";
    child_img.modified_unix_ms = kNowMs - 40 * kDayMs;
    child_img.mapped_owner_group_id = "grp_pic";
    child_img.loose_pos = {-999, -999};

    DesktopItem child_lnk{};
    child_lnk.path = L"C:\\Users\\Demo\\Desktop\\截图与图片\\tool.lnk";
    child_lnk.name = L"tool";
    child_lnk.extension = L".lnk";
    child_lnk.is_shortcut = true;
    child_lnk.link_target = L"C:\\App\\a.exe";
    child_lnk.link_target_missing = true;
    child_lnk.mapped_owner_group_id = "grp_pic";

    DesktopItem desk_png{};
    desk_png.path = L"C:\\Users\\Demo\\Desktop\\cover.png";
    desk_png.name = L"cover.png";
    desk_png.extension = L".png";
    desk_png.modified_unix_ms = kNowMs - kDayMs;

    st2.items.push_back(child_img);
    st2.items.push_back(child_lnk);
    st2.items.push_back(desk_png);

    UpdateAppsTileMembership(st2);
    CHECK(!st2.items[0].in_apps_tile);
    CHECK(!st2.items[3].in_apps_tile);

    const size_t pic_group_idx = st2.groups.size() - 1;
    CHECK(!ClassifyItemToGroup(st2.items[0], st2.groups, kNowMs).has_value());
    CHECK(ClassifyItemToGroup(st2.items[2], st2.groups, kNowMs) == std::optional<size_t>(pic_group_idx));
    CHECK(ClassifyItemToGroup(st2.items[3], st2.groups, kNowMs) == std::optional<size_t>(pic_group_idx));
    const auto cls_desk = ClassifyItemToGroup(st2.items[4], st2.groups, kNowMs);
    CHECK(!cls_desk.has_value() || *cls_desk != pic_group_idx);

    const auto mv2 = ComputeGroupViews(st2, kNowMs);
    SettleLooseIcons(st2.items, mv2, work, RectI{});
    CHECK(st2.items[0].loose_pos.x == -999 && st2.items[0].loose_pos.y == -999);
    CHECK(st2.items[2].loose_pos.x == -999 && st2.items[2].loose_pos.y == -999);

    const HealthReport hr = AnalyzeDesktopHealth(st2.items, {}, kNowMs, 14);
    for (const auto& issue : hr.issues) {
        CHECK(issue.path != st2.items[0].path);
        CHECK(issue.path != st2.items[2].path);
        CHECK(issue.path != st2.items[3].path);
    }
}

void TestFreeDesktopTilePlacementAndResize() {
    const RectI work{0, 0, 1920, 1032};
    const RectI board = ComputeTileBoardRect(work, true);
    std::vector<DesktopTile> tiles = MakeDefaultTiles();

    const size_t apps_idx = static_cast<size_t>(
        std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "apps"; }) - tiles.begin());
    const size_t tray_idx = static_cast<size_t>(
        std::find_if(tiles.begin(), tiles.end(), [](const DesktopTile& t) { return t.id == "staging_tray"; }) - tiles.begin());

    // 1. Move Apps tile out of top-right 5x7 board to bottom-left free coordinate (48, 420).
    MoveTileFreeInteractive(tiles, apps_idx, 48, 420, work, board, {});
    CHECK(tiles[apps_idx].free_placed);
    const RectI apps_moved = ResolveTileRect(tiles[apps_idx], board);
    CHECK(apps_moved.x <= 64 && apps_moved.y >= 400);
    CHECK(work.contains(apps_moved.x, apps_moved.y));
    CHECK(apps_moved.right() <= work.right() && apps_moved.bottom() <= work.bottom());

    // 2. Arbitrary height & width resize beyond fixed 5x7 bounds (424 x 560) pushes overlapping neighbors & restores on shrink.
    const RectI apps_home{apps_moved.x, apps_moved.y, apps_moved.w, apps_moved.h};
    MoveTileFreeInteractive(tiles, tray_idx, apps_home.x, apps_home.bottom() + kTileGapPx, work, board, {});
    CommitTileRestOrigins(tiles);
    const RectI tray_before = ResolveTileRect(tiles[tray_idx], board);

    ResizeTileFreeInteractive(tiles, apps_idx, 424, 560, work, board, {});
    const RectI apps_tall = ResolveTileRect(tiles[apps_idx], board);
    CHECK(apps_tall.w == 424 && apps_tall.h == 560);
    CHECK(tiles[apps_idx].size.cols == 4 && tiles[apps_idx].size.rows >= 5);
    const RectI tray_pushed = ResolveTileRect(tiles[tray_idx], board);
    CHECK(!apps_tall.intersects(tray_pushed, kTileGapPx));
    CHECK(tray_pushed.y != tray_before.y || tray_pushed.x != tray_before.x);

    // Shrinking back restores the displaced neighbor to rest_origin!
    ResizeTileFreeInteractive(tiles, apps_idx, apps_home.w, apps_home.h, work, board, {});
    const RectI tray_restored = ResolveTileRect(tiles[tray_idx], board);
    CHECK(tray_restored.x == tray_before.x && tray_restored.y == tray_before.y);
    CommitTileRestOrigins(tiles);

    // 3. Groups and loose icons avoid individual tile obstacle rects rather than trapping the top-right corner.
    std::vector<DesktopGroup> groups = MakeDefaultGroups(work);
    const auto tile_obs = CollectTileObstacleRects(tiles, board, true);
    ResolveGroupCollisions(groups, std::nullopt, work, RectI{}, DesktopIconSize::Medium, tile_obs);
    for (const auto& g : groups) {
        const RectI gf = ComputeGroupFrame(g, DesktopIconSize::Medium);
        for (const RectI& tobs : tile_obs) {
            CHECK(!gf.intersects(tobs, 0));
        }
    }

    // 4. JSON serialization & deserialization roundtrip preserves free coordinates and custom dimensions.
    DesktopModelState s{};
    s.tiles = tiles;
    const std::string json = SerializeModelStateJson(s);
    DesktopModelState parsed{};
    CHECK(DeserializeModelStateJson(json, parsed));
    CHECK(parsed.tiles.size() == tiles.size());
    CHECK(parsed.tiles[apps_idx].free_placed);
    CHECK(parsed.tiles[apps_idx].x == tiles[apps_idx].x && parsed.tiles[apps_idx].y == tiles[apps_idx].y);
    CHECK(parsed.tiles[apps_idx].w == tiles[apps_idx].w && parsed.tiles[apps_idx].h == tiles[apps_idx].h);
}

void TestLiquidGlassMaterialPersistenceAndDowngrade() {
    CHECK(static_cast<uint8_t>(CardMaterial::FollowHost) == 0);
    CHECK(static_cast<uint8_t>(CardMaterial::Acrylic) == 1);
    CHECK(static_cast<uint8_t>(CardMaterial::Mica) == 2);
    CHECK(static_cast<uint8_t>(CardMaterial::MicaAlt) == 3);
    CHECK(static_cast<uint8_t>(CardMaterial::Solid) == 4);
    CHECK(static_cast<uint8_t>(CardMaterial::LiquidGlass) == 5);

    const RectI work{0, 0, 1920, 1032};
    DesktopModelState s{};
    s.groups = MakeDefaultGroups(work);
    s.tiles = MakeDefaultTiles();
    s.settings.default_material = CardMaterial::LiquidGlass;
    s.settings.glass_preset = static_cast<uint8_t>(GlassPreset::Custom);
    s.settings.glass_thickness = 38;
    s.settings.glass_ior_x100 = 175;
    s.settings.glass_dispersion = 24;
    s.settings.glass_glare = 105;
    s.settings.glass_glare_angle = -110;
    s.settings.glass_frost = 18;
    s.settings.glass_live_rate = 0;
    CHECK(!s.groups.empty() && !s.tiles.empty());
    s.groups[0].material = CardMaterial::LiquidGlass;
    s.tiles[0].material = CardMaterial::LiquidGlass;

    const std::string json = SerializeModelStateJson(s);
    DesktopModelState restored{};
    CHECK(DeserializeModelStateJson(json, restored));
    CHECK(restored.settings.default_material == CardMaterial::LiquidGlass);
    CHECK(restored.settings.glass_preset == static_cast<uint8_t>(GlassPreset::Custom));
    CHECK(restored.settings.glass_thickness == 38);
    CHECK(restored.settings.glass_ior_x100 == 175);
    CHECK(restored.settings.glass_dispersion == 24);
    CHECK(restored.settings.glass_glare == 105);
    CHECK(restored.settings.glass_glare_angle == -110);
    CHECK(restored.settings.glass_frost == 18);
    CHECK(restored.settings.glass_live_rate == 0);
    CHECK(restored.groups[0].material == CardMaterial::LiquidGlass);
    CHECK(restored.tiles[0].material == CardMaterial::LiquidGlass);

    // Unknown future material values (> 5) downgrade cleanly, legacy glass_dispersion upgrades cleanly, out-of-range clamps
    const std::string future_json =
        "{\"schema_version\":3,"
        "\"settings\":{\"default_material\":77,\"glass_dispersion\":2,\"glass_live_rate\":8,\"glass_ior\":500,\"glass_angle\":-300},"
        "\"groups\":[{\"id\":\"g1\",\"title\":\"G\",\"x\":24,\"y\":24,\"cols\":4,\"visible_rows\":3,\"material\":99}],"
        "\"tiles\":[{\"id\":\"t1\",\"kind\":0,\"col\":0,\"row\":0,\"cols\":2,\"rows\":2,\"material\":88}]}";
    DesktopModelState downgraded{};
    CHECK(DeserializeModelStateJson(future_json, downgraded));
    CHECK(downgraded.settings.default_material == CardMaterial::Acrylic);
    CHECK(downgraded.settings.glass_dispersion == 20);
    CHECK(downgraded.settings.glass_live_rate == 2);
    CHECK(downgraded.settings.glass_ior_x100 == 240);
    CHECK(downgraded.settings.glass_glare_angle == -180);
    CHECK(downgraded.groups.size() == 1 && downgraded.groups[0].material == CardMaterial::FollowHost);
    CHECK(downgraded.tiles.size() == 1 && downgraded.tiles[0].material == CardMaterial::FollowHost);

    const std::string overrange_gain_json =
        "{\"schema_version\":3,\"settings\":{\"glass_disp_gain\":99}}";
    DesktopModelState clamped{};
    CHECK(DeserializeModelStateJson(overrange_gain_json, clamped));
    CHECK(clamped.settings.glass_dispersion == 35);
}

} // namespace

int main() {
    RunCase("default_groups_and_geometry", TestDefaultGroupsAndGeometry);
    RunCase("group_drag_push_away_memory_and_resize", TestGroupDragPushAwayMemoryAndResize);
    RunCase("group_views_collapse_overflow_sort_and_focus", TestGroupViewsCollapseOverflowSortAndFocus);
    RunCase("loose_icons_settlement", TestLooseIconsSettlement);
    RunCase("tile_board_5x7_and_displacement", TestTileBoardAndDisplacement);
    RunCase("rules_search_version_and_health_engine", TestRulesSearchVersionAndHealthEngine);
    RunCase("undo_history_32_and_json_roundtrip", TestUndoHistoryAndJsonRoundTrip);
    RunCase("ai_organizer_dpapi_parse_archive_and_undo", TestAiOrganizerDpapiParseArchiveAndUndo);
    RunCase("widgets_stacks_and_apps_tile", TestWidgetsStacksAndAppsTile);
    RunCase("stack_across_sizes", TestStackAcrossSizes);
    RunCase("multi_page_groups_and_mapped_folders", TestMultiPageGroupsAndMappedFolders);
    RunCase("free_desktop_tile_placement_and_resize", TestFreeDesktopTilePlacementAndResize);
    RunCase("liquid_glass_material_persistence_and_downgrade", TestLiquidGlassMaterialPersistenceAndDowngrade);

    std::printf("\nSummary: %d passed, %d failed\n", g_pass_count, g_fail_count);
    return (g_fail_count == 0) ? 0 : 1;
}
