#include "host/model/tile_drag_session.h"
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
    check(session.Update({240,44},{240,44},100), "target change creates preview");
    check(session.Valid() && session.Preview()[0].x == 240 && session.Preview()[1].x == 24 &&
          session.Preview()[2].x == 132, "phone insertion preserves neighbor order");
    check(Json(session.Original()) == Json(source), "preview never changes committed source");
    const auto plans = session.PlanningCount();
    for (int i = 0; i < 100; ++i) session.Update({242,45},{242,45},110 + i);
    check(session.PlanningCount() == plans, "same-cell motion does not rerun solver");
    check(!session.Update({298,44},{298,44},220) && session.Target().x == 240,
          "six-DIP hysteresis prevents boundary oscillation");
    check(session.Update({301,44},{301,44},230) && session.Target().x == 348,
          "crossing hysteresis margin selects next cell");
    session.Update({25,45},{25,45},240);
    check(!session.PlacementChanged() && Json(session.Preview()) == Json(source),
          "return to origin restores all displaced neighbors");
    session.Update({240,44},{240,44},250);
    auto destination = source;
    check(session.Commit(destination) && !session.Active() && destination[0].x == 240,
          "valid drop commits one final preview");
    session.Begin(source, 0, work, {}, {});
    session.Update({240,44},{240,44},300);
    destination = session.Preview();
    session.Cancel(destination);
    check(!session.Active() && Json(destination) == Json(source), "cancel restores exact original tiles");

    const std::vector<RectI> obstacles{{348,44,100,100}};
    session.Begin(source, 0, work, {}, obstacles);
    session.Update({348,44},{348,44},350);
    check(!session.Valid() && Json(session.Preview()) == Json(source), "blocked target keeps original layout");
    destination = source;
    check(!session.Commit(destination) && Json(destination) == Json(source), "invalid release cannot commit");
    session.Cancel(destination);

    auto stack_source=source;
    for(auto& tile:stack_source)tile.kind=TileKind::Photos;
    session.Begin(stack_source, 0, work, {}, {});
    session.Update({240,44},{270,70},1000);
    check(session.StackTarget().empty(), "crossing tile center does not immediately stack");
    session.Update({240,44},{270,70},1599);
    check(session.StackTarget().empty(), "stack dwell remains unarmed before 600ms");
    session.Update({240,44},{270,70},1600);
    check(session.StackTarget() == "2" && Json(session.Preview()) == Json(stack_source),
          "stationary dwell arms stacking without persisting preview");
    session.Update({240,44},{278,70},1610);
    check(session.StackTarget().empty(), "pointer movement resets stack dwell");
    session.Update({348,200},{398,250},2300);
    check(session.StackTarget().empty() && !session.HasStackCandidate(), "leaving target clears stack state");
    check(!session.Begin(stack_source, 99, work, {}, {}) && !session.Active(), "invalid start leaves no session");
    session.Begin(stack_source,0,work,{},{});
    session.Update({190,44},{240,94},3000);
    check(session.Preview()[2].x==source[2].x&&session.Preview()[2].y==source[2].y,
          "approaching merge target keeps it visible while dwell is pending");
    session.Update({210,44},{260,94},3100);
    check(Json(session.Preview())==Json(stack_source),"pending merge takes visual priority over reorder displacement");
    session.Update({215,44},{265,94},3200);
    session.Update({215,44},{265,94},3800);
    check(session.StackTarget()=="2"&&Json(session.Preview())==Json(stack_source),
          "gradual approach followed by dwell merges at unchanged target");
    for (int grab : {0,50,95}) {
        session.Begin(stack_source,0,work,{},{});
        session.Update({145,44},{145+grab,50},4000);
        check(session.Phase()==TileDragPhase::Approach&&Json(session.Preview())==Json(stack_source)&&!session.NeedsDwellTimer(),
              "source footprint approach protects target before pointer enters core");
        session.Update({270-grab,44},{270,94},4100);
        check(session.Phase()==TileDragPhase::StackPending&&session.PendingCandidate()=="2"&&session.NeedsDwellTimer(),
              "each grab offset starts dwell only in original target core");
        session.Update({270-grab,44},{270,94},4700);
        check(session.Phase()==TileDragPhase::StackArmed&&session.StackTarget()=="2"&&
              session.CandidateRect().x==240&&session.ProposalRect().x==270-grab,
              "offset-independent dwell arms original target and retains pointer proposal");
    }
    session.Begin(stack_source,0,work,{},{});
    session.Update({240,44},{270,94},5000);
    check(session.PlacementChanged()&&Json(session.Preview())==Json(stack_source)&&session.Target().x==240,
          "pending visual freeze retains accurate quick-drop insertion target");
    destination=stack_source;
    check(session.Commit(destination)&&destination[0].x==240&&destination[1].x==24&&destination[2].x==132,
          "quick release commits saved reorder plan despite protected visuals");
    check(!session.Commit(destination),"a quick drop commits at most once");
    session.Begin(stack_source,0,work,{},{});
    session.Update({240,44},{270,94},5100);
    session.Update({348,180},{398,230},5200);
    check(session.Phase()==TileDragPhase::Reorder&&!session.HasStackCandidate()&&session.Preview()[0].x==348,
          "fully leaving protected target resumes normal empty-cell reorder");
    auto stale=stack_source;
    stale[2].rest_origin=PointI{348,44};
    session.Begin(stale,0,work,{},{});
    session.Update({348,44},{378,94},5300);
    check(!session.HasStackCandidate(),"stale rest_origin never creates a phantom merge target");
    session.Update({240,44},{270,94},5400);
    check(session.PendingCandidate()=="2"&&session.CandidateRect().x==240,
          "actual original tile rectangle remains merge candidate despite stale rest_origin");
    destination=session.Preview();
    session.Cancel(destination);
    check(Json(destination)==Json(stale)&&!session.Active(),"cancelling pending merge restores exact snapshot including metadata");
    std::printf("Drag session: %d cases, %d failures\n", count, failed);
    return failed ? 1 : 0;
}
