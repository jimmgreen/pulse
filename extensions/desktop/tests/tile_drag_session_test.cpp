#include "host/model/tile_drag_session.h"
#include "host/model/drag_motion.h"
#include <cmath>
#include <cstdio>
using namespace pulse::desktop::model;

std::vector<DesktopTile> Tiles() {
    std::vector<DesktopTile> tiles(3);
    for (size_t i = 0; i < tiles.size(); ++i) {
        auto& t = tiles[i];
        t.id = std::to_string(i);
        t.free_placed = true;
        t.x = 24 + static_cast<int>(i) * 108;
        t.y = 44;
        t.w = t.h = 100;
        t.size = {1, 1};
        t.kind = TileKind::Apps;
    }
    return tiles;
}
std::string Json(const std::vector<DesktopTile>& tiles) {
    DesktopModelState state;
    state.tiles = tiles;
    return SerializeModelStateJson(state);
}
int main() {
    int count = 0, failed = 0;
    auto check = [&](bool ok, const char* label) {
        ++count; failed += !ok;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };
    const auto source = Tiles();
    const RectI work{0, 0, 464, 300};
    TileDragSession session;
    check(session.Begin(source, 0, work, {}, {}), "begin captures original layout");

    // Reference-aligned: dragged tile follows cursor 1:1 while siblings remain stationary;
    // ghost target snaps to nearest free grid cell.
    check(session.Update({240, 44}, {240, 44}, 100), "drag update creates smooth preview and ghost target");
    check(session.Valid() && session.Preview()[0].x == 240 && session.Preview()[0].y == 44 &&
          session.Preview()[1].x == 132 && session.Preview()[2].x == 240,
          "dragged tile follows pointer 1:1 while non-dragged tiles stay stationary");
    check(session.Target().x == 348 && session.Target().y == 44,
          "occupied cell resolves ghost target to nearest free grid cell");
    check(Json(session.Original()) == Json(source), "preview never changes committed source");

    const auto plans = session.PlanningCount();
    for (int i = 0; i < 20; ++i) session.Update({240, 44}, {240, 44}, 110 + i);
    check(session.PlanningCount() == plans, "identical proposal reuses cached free-cell plan");

    // Moving near an empty cell snaps ghost cleanly to that empty cell.
    check(session.Update({342, 148}, {342, 148}, 220) && session.Target().x == 348 && session.Target().y == 152,
          "empty region snaps ghost to nearest free grid cell");

    // Returning within 6 DIP of origin snaps back to origin with PlacementChanged() == false.
    session.Update({25, 45}, {25, 45}, 240);
    check(!session.PlacementChanged() && session.Target().x == 24 && session.Target().y == 44,
          "return near origin snaps back to origin without redundant placement change");

    session.Update({342, 148}, {342, 148}, 250);
    auto destination = source;
    check(session.Commit(destination) && !session.Active() &&
          destination[0].x == 348 && destination[0].y == 152 &&
          destination[1].x == 132 && destination[2].x == 240,
          "valid drop commits dragged tile to snapped free cell keeping siblings calm");

    session.Begin(source, 0, work, {}, {});
    session.Update({342, 148}, {342, 148}, 300);
    destination = session.Preview();
    session.Cancel(destination);
    check(!session.Active() && Json(destination) == Json(source), "cancel restores exact original tiles");

    // Group obstacle redirects ghost to nearest free cell outside group.
    const std::vector<RectI> obstacles{{348, 44, 100, 100}};
    session.Begin(source, 0, work, {}, obstacles);
    session.Update({348, 44}, {348, 44}, 350);
    check(session.Valid() && !session.Target().intersects(obstacles[0]),
          "group obstacle redirects ghost target to nearest free non-overlapping cell");
    session.Cancel(destination);

    // Fully packed work area with no free cell anywhere marks target invalid and blocks commit.
    const RectI tight_work{0, 0, 352, 150};
    const std::vector<RectI> blocking_all{{0, 0, 128, 150}};
    session.Begin(source, 0, tight_work, {}, blocking_all);
    session.Update({240, 44}, {240, 44}, 360);
    check(!session.Valid(), "work area with zero free cells marks drag invalid");
    destination = source;
    check(!session.Commit(destination) && Json(destination) == Json(source), "invalid release cannot commit");
    session.Cancel(destination);

    // 600ms dwell-to-stack on stackable tiles remains intact.
    auto stack_source = source;
    for (auto& tile : stack_source) tile.kind = TileKind::Photos;
    session.Begin(stack_source, 0, work, {}, {});
    session.Update({240, 44}, {270, 70}, 1000);
    check(session.StackTarget().empty(), "crossing tile center does not immediately stack");
    session.Update({240, 44}, {270, 70}, 1599);
    check(session.StackTarget().empty(), "stack dwell remains unarmed before 600ms");
    session.Update({240, 44}, {270, 70}, 1600);
    check(session.StackTarget() == "2" && session.Target().x == 240 && session.Target().y == 44,
          "stationary dwell arms stacking and locks ghost onto target tile");
    session.Update({240, 44}, {278, 70}, 1610);
    check(session.StackTarget().empty(), "pointer movement resets stack dwell");
    session.Update({348, 152}, {398, 202}, 2300);
    check(session.StackTarget().empty() && !session.HasStackCandidate(), "leaving target clears stack state");
    check(!session.Begin(stack_source, 99, work, {}, {}) && !session.Active(), "invalid start leaves no session");

    for (int grab : {0, 50, 95}) {
        session.Begin(stack_source, 0, work, {}, {});
        session.Update({145, 44}, {145 + grab, 50}, 4000);
        check(session.Phase() == TileDragPhase::Approach && !session.NeedsDwellTimer(),
              "source footprint approach prepares candidate before pointer enters core");
        session.Update({270 - grab, 44}, {270, 94}, 4100);
        check(session.Phase() == TileDragPhase::StackPending && session.PendingCandidate() == "2" && session.NeedsDwellTimer(),
              "each grab offset starts dwell only in original target core");
        session.Update({270 - grab, 44}, {270, 94}, 4700);
        check(session.Phase() == TileDragPhase::StackArmed && session.StackTarget() == "2" &&
              session.CandidateRect().x == 240 && session.ProposalRect().x == 270 - grab,
              "offset-independent dwell arms original target and retains 1:1 pointer proposal");
    }

    auto stale = stack_source;
    stale[2].rest_origin = PointI{348, 44};
    session.Begin(stale, 0, work, {}, {});
    session.Update({348, 44}, {378, 94}, 5300);
    check(!session.HasStackCandidate(), "stale rest_origin never creates a phantom merge target");
    session.Update({240, 44}, {270, 94}, 5400);
    check(session.PendingCandidate() == "2" && session.CandidateRect().x == 240,
          "actual original tile rectangle remains merge candidate despite stale rest_origin");
    destination = session.Preview();
    session.Cancel(destination);
    check(Json(destination) == Json(stale) && !session.Active(), "cancelling pending merge restores exact snapshot");

    // Reference SwiftUI spring curve & smooth ghost interpolation checks.
    const float sp0 = EvaluateSpringCurve(0.0f);
    const float sp_mid = EvaluateSpringCurve(0.20f);
    const float sp_end = EvaluateSpringCurve(kSpringDurationSec);
    check(sp0 == 0.0f && sp_mid > 0.5f && std::fabs(sp_end - 1.0f) < 0.02f,
          "SwiftUI spring curve starts at 0, accelerates past mid, and settles at 1");

    SmoothGhostState ghost;
    ghost.StartAt({24, 44, 100, 100}, 16.0f);
    ghost.SetTarget({348, 152, 100, 100}, true, false);
    check(ghost.Step(16.0, true, false) && ghost.CurrentRect().x > 24 && ghost.CurrentRect().x < 348,
          "smooth ghost slides smoothly toward target on each VSync tick");
    for (int i = 0; i < 40; ++i) ghost.Step(16.0, false, false);
    check(!ghost.visible && ghost.CurrentRect().empty(),
          "ghost hides automatically once settled after drag and spring finish");

    SpringMoveList springs;
    springs.AddOrReplace("0", {300, 120}, {348, 152}, 1000.0);
    std::unordered_map<std::string, PointI> pos;
    check(springs.Step(1200.0, pos) && pos.contains("0") && pos["0"].x > 300 && pos["0"].y > 120,
          "spring snap-back smoothly interpolates released tile toward committed slot");
    check(!springs.Step(1700.0, pos) && pos.empty(),
          "spring snap-back completes cleanly after 0.65s duration");

    // Right-aligned & DPI-scaled tile board snapping: dragging left out of starter board must align
    // pixel-for-pixel to board columns (board.x - step_x) with no dead-zone offset.
    {
        const RectI work_150{0, 0, 1280, 688};
        const RectI board_150 = ComputeTileBoardRect(work_150, true);
        auto starter = MakeDefaultTiles();
        TileDragSession board_sess;
        // Find a 2x2 starter tile (Downloads) inside the right-aligned board.
        size_t dl_idx = 0;
        for (size_t i = 0; i < starter.size(); ++i) {
            if (starter[i].kind == TileKind::Downloads) { dl_idx = i; break; }
        }
        const RectI orig_dl = ResolveTileRect(starter[dl_idx], board_150);
        check(board_sess.Begin(starter, dl_idx, work_150, board_150, {}),
              "begin drag on right-aligned DPI-scaled tile board");
        // Drag one column left of the starter tile's column.
        constexpr int kStdW = kTileCols * kTileUnitPx + (kTileCols - 1) * kTileGapPx;
        const int gap_x = std::clamp((board_150.w * kTileGapPx + kStdW / 2) / kStdW, 0, board_150.w / kTileCols);
        const int usable_w = std::max(kTileCols, board_150.w - (kTileCols - 1) * gap_x);
        const int step_x = std::max(16, (usable_w + kTileCols / 2) / kTileCols + gap_x);
        const int want_x = orig_dl.x - step_x;
        board_sess.Update({want_x, orig_dl.y}, {want_x + 40, orig_dl.y + 40}, 100);
        check(board_sess.Valid() && board_sess.Target().x == want_x && board_sess.Target().y == orig_dl.y,
              "right-aligned scaled board snaps ghost cleanly 1 column left without grid offset");

        // Integer-division +1px remainder columns on scaled boards (e.g. col 3..4 moved into
        // col 0..1 beside an obstacle at col 2) must never false-conflict by 1px.
        const RectI odd_board{700, 44, 533, 600}; // usable_w = 501 => col 4 is +1px wider
        std::vector<DesktopTile> odd_tiles(2);
        odd_tiles[0].id = "move"; odd_tiles[0].kind = TileKind::Downloads; odd_tiles[0].col = 3; odd_tiles[0].row = 0; odd_tiles[0].size = {2, 2};
        odd_tiles[1].id = "mid";  odd_tiles[1].kind = TileKind::Recent;    odd_tiles[1].col = 2; odd_tiles[1].row = 0; odd_tiles[1].size = {1, 2};
        DesktopTile col0_probe = odd_tiles[0]; col0_probe.col = 0;
        const RectI col0_rect = ResolveTileRect(col0_probe, odd_board);
        TileDragSession odd_sess;
        check(odd_sess.Begin(odd_tiles, 0, work_150, odd_board, {}), "begin drag on odd-width scaled board");
        odd_sess.Update({col0_rect.x, col0_rect.y}, {col0_rect.x + 30, col0_rect.y + 30}, 120);
        check(odd_sess.Valid() && odd_sess.Target().x == col0_rect.x && odd_sess.Target().y == col0_rect.y,
              "1px DPI rounding remainder column never false-conflicts with adjacent board slot");

        // Edge-docked sibling tiles (off-phase from board_rect by kTileBoardRightMargin):
        // dragging a 2x2 tile beside a 1x1 tile docked at work_area.right() must snap flush
        // with exact standard gap instead of leaving an unfillable 30-50px dead gap.
        const RectI work_1080{0, 0, 1920, 1032};
        const RectI board_1080 = ComputeTileBoardRect(work_1080, true);
        std::vector<DesktopTile> edge_gap_tiles(3);
        // 0: 2x2 Downloads being dragged
        edge_gap_tiles[0].id = "dl"; edge_gap_tiles[0].kind = TileKind::Downloads; edge_gap_tiles[0].size = {2, 2};
        edge_gap_tiles[0].free_placed = true; edge_gap_tiles[0].x = 1200; edge_gap_tiles[0].y = 400; edge_gap_tiles[0].w = 208; edge_gap_tiles[0].h = 208;
        // 1: 4x2 Photos on starter board top
        edge_gap_tiles[1].id = "ph"; edge_gap_tiles[1].kind = TileKind::Photos; edge_gap_tiles[1].col = 0; edge_gap_tiles[1].row = 0; edge_gap_tiles[1].size = {4, 2};
        // 2: 1x1 DiskRing docked flush to work_area.right() (off-phase from board_1080.right() by kTileBoardRightMargin)
        edge_gap_tiles[2].id = "dk"; edge_gap_tiles[2].kind = TileKind::DiskRing; edge_gap_tiles[2].size = {1, 1};
        edge_gap_tiles[2].free_placed = true; edge_gap_tiles[2].x = work_1080.right() - 100; edge_gap_tiles[2].y = 300; edge_gap_tiles[2].w = 100; edge_gap_tiles[2].h = 100;
        const RectI photo_rc = ResolveTileRect(edge_gap_tiles[1], board_1080);
        const int flush_x = edge_gap_tiles[2].x - edge_gap_tiles[0].w - kTileGapPx;
        const int below_photo_y = photo_rc.bottom() + kTileGapPx;
        TileDragSession gap_sess;
        check(gap_sess.Begin(edge_gap_tiles, 0, work_1080, board_1080, {}),
              "begin drag beside edge-docked sibling tile");
        gap_sess.Update({flush_x, below_photo_y}, {flush_x + 40, below_photo_y + 40}, 100);
        check(gap_sess.Valid() && gap_sess.Target().x == flush_x && gap_sess.Target().y == below_photo_y,
              "dragging beside edge-docked tile snaps flush with standard gap (no dead gap)");
    }

    // Liquid Glass Dock settings JSON round-trip (pinned paths, icon size, magnification, scale).
    {
        DesktopModelState dock_src{};
        dock_src.settings.dock_enabled = true;
        dock_src.settings.dock_icon_size = 56;
        dock_src.settings.dock_magnification = true;
        dock_src.settings.dock_max_scale_x100 = 178;
        dock_src.settings.dock_hide_mode = 2;
        dock_src.settings.dock_minimize_effect = 2;
        dock_src.settings.dock_pinned_paths = {
            L"C:\\Windows\\explorer.exe",
            L"C:\\Users\\测试\\微信.lnk",
            L"ms-settings:",
        };
        const std::string encoded = SerializeModelStateJson(dock_src);
        DesktopModelState dock_dst{};
        check(DeserializeModelStateJson(encoded, dock_dst) &&
              dock_dst.settings.dock_enabled &&
              dock_dst.settings.dock_icon_size == 56 &&
              dock_dst.settings.dock_magnification &&
              dock_dst.settings.dock_max_scale_x100 == 178 &&
              dock_dst.settings.dock_hide_mode == 2 &&
              dock_dst.settings.dock_minimize_effect == 2 &&
              dock_dst.settings.dock_pinned_paths.size() == 3 &&
              dock_dst.settings.dock_pinned_paths[0] == L"C:\\Windows\\explorer.exe" &&
              dock_dst.settings.dock_pinned_paths[1] == L"C:\\Users\\测试\\微信.lnk" &&
              dock_dst.settings.dock_pinned_paths[2] == L"ms-settings:",
              "Liquid Glass Dock settings & CJK pinned paths survive JSON round-trip intact");
    }

    std::printf("Drag session: %d cases, %d failures\n", count, failed);
    return failed ? 1 : 0;
}
