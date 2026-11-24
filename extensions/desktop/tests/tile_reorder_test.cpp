#include "host/model/tile_reorder.h"
#include <chrono>
#include <cstdio>
#include <random>

using namespace pulse::desktop::model;
namespace {
DesktopTile Tile(int id, int col, int row, int w = 100, int h = 100) {
    DesktopTile t;
    t.id = std::to_string(id); t.free_placed = true;
    t.x = 24 + col * 108; t.y = 44 + row * 108; t.w = w; t.h = h; t.size = {1, 1};
    return t;
}
bool Same(const std::vector<DesktopTile>& a, const std::vector<DesktopTile>& b) {
    return SerializeModelStateJson(DesktopModelState{.tiles=a}) == SerializeModelStateJson(DesktopModelState{.tiles=b});
}
bool Clear(const TileReorderResult& r, const RectI& work, std::span<const RectI> obstacles = {}) {
    if (!r.valid) return false;
    for (size_t i = 0; i < r.updated_tiles.size(); ++i) {
        const auto a = ResolveTileRect(r.updated_tiles[i], {});
        if (a.x < work.x || a.y < work.y || a.right() > work.right() || a.bottom() > work.bottom()) return false;
        for (size_t j = 0; j < i; ++j) if (a.intersects(ResolveTileRect(r.updated_tiles[j], {}))) return false;
        for (auto b : obstacles) if (a.intersects(b)) return false;
    }
    return true;
}
}
int main(int argc, char**) {
    int failures = 0;
    auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    const RectI work{0,0,24+4*108,44+3*108};
    std::vector<DesktopTile> start{Tile(0,0,0),Tile(1,1,0),Tile(2,2,0),Tile(3,3,0),Tile(4,0,1)};
    const auto pristine = start;
    if (argc > 1) {
        auto legacy = start;
        MoveTileFreeInteractive(legacy, 0, 240, 44, work, {}, {});
        check(legacy[1].x==24 && legacy[2].x==132 && legacy[0].x==240 &&
            legacy[0].y==44 && legacy[1].y==44 && legacy[2].y==44,
            "legacy mover satisfies stable phone-grid insertion");
        std::printf("BASELINE positions: active=(%d,%d) next=(%d,%d) target=(%d,%d)\n",
            legacy[0].x,legacy[0].y,legacy[1].x,legacy[1].y,legacy[2].x,legacy[2].y);
        return failures?1:0;
    }
    auto forward = PreviewTileReorder(start,0,{240,44},work,{},{});
    check(Clear(forward,work) && forward.updated_tiles[1].x==24 && forward.updated_tiles[2].x==132 && forward.updated_tiles[0].x==240,
        "forward insertion rotates neighbors into origin hole");
    auto backward = PreviewTileReorder(start,2,{24,44},work,{},{});
    check(Clear(backward,work) && backward.updated_tiles[0].x==132 && backward.updated_tiles[1].x==240,
        "backward insertion preserves displaced order");
    auto cross = PreviewTileReorder(start,0,{24,152},work,{},{});
    check(Clear(cross,work) && cross.updated_tiles[4].x==348 && cross.updated_tiles[4].y==44,
        "insertion chain wraps to next row without losing cards");
    auto empty = PreviewTileReorder(start,0,{348,260},work,{},{});
    check(Clear(empty,work) && empty.updated_tiles[1].x==start[1].x && empty.updated_tiles[4].y==start[4].y,
        "empty destination changes active tile only");
    check(Same(start,pristine),"preview never mutates input");
    auto again = PreviewTileReorder(start,0,{240,44},work,{},{});
    check(Same(forward.updated_tiles,again.updated_tiles) && forward.visited_states==again.visited_states,"same start and target deterministic");
    auto home = PreviewTileReorder(start,0,{24,44},work,{},{});
    check(home.valid && Same(home.updated_tiles,start),"returning to origin restores original snapshot");
    auto detour = PreviewTileReorder(start,0,{348,260},work,{},{});
    (void)detour;
    check(Same(PreviewTileReorder(start,0,{240,44},work,{},{}).updated_tiles,forward.updated_tiles),"preview path independent after detour");
    std::vector<DesktopTile> full;
    for(int i=0;i<12;++i)full.push_back(Tile(i,i%4,i/4));
    auto full_move=PreviewTileReorder(full,11,{24,44},work,{},{});
    check(Clear(full_move,work)&&full_move.updated_tiles.size()==full.size(),"full grid insertion uses moving origin vacancy");
    std::vector<DesktopTile> mixed{Tile(0,0,0,208,100),Tile(1,2,0),Tile(2,3,0),Tile(3,0,1,208,100)};
    auto mix=PreviewTileReorder(mixed,3,{24,44},work,{},{});
    check(Clear(mix,work)&&mix.updated_tiles[0].w==208&&mix.updated_tiles[3].w==208&&
        mix.updated_tiles[0].y<=mix.updated_tiles[1].y,"mixed width insertion retains actual dimensions and order");
    const std::vector<RectI> obstacles{{240,44,100,100}};
    auto blocked=PreviewTileReorder(start,0,{240,44},work,{},obstacles);
    check(!blocked.valid&&blocked.reason==TileReorderStatus::BlockedTarget&&Same(blocked.updated_tiles,start),"fixed group target rejected without mutation");
    std::vector<DesktopTile> obstacle_start{Tile(0,0,0),Tile(1,2,0),Tile(2,3,0)};
    const std::vector<RectI> middle_group{{132,44,100,100}};
    auto around=PreviewTileReorder(obstacle_start,2,{24,44},work,{},middle_group);
    check(Clear(around,work,middle_group)&&around.updated_tiles[0].x==240&&around.updated_tiles[1].x==348,
        "insertion skips fixed group without moving it");
    std::vector<DesktopTile> tall{Tile(0,0,0,100,208),Tile(1,1,0),Tile(2,2,0),Tile(3,0,2,100,208)};
    const RectI tall_work{0,0,456,600};
    auto taller=PreviewTileReorder(tall,3,{24,44},tall_work,{},{});
    check(Clear(taller,tall_work)&&taller.updated_tiles[0].h==208&&taller.updated_tiles[3].h==208,
        "multirow tiles preserve actual heights and avoid overlap");
    auto legacy=start;
    legacy.back().x=391;legacy.back().y=277;legacy.back().w=105;legacy.back().h=73;
    const RectI legacy_work{0,0,650,500};
    auto legacy_result=PreviewTileReorder(legacy,2,{24,44},legacy_work,{},{});
    check(legacy_result.valid&&legacy_result.updated_tiles.back().x==391&&legacy_result.updated_tiles.back().y==277&&
        legacy_result.updated_tiles.back().w==105,"unrelated off-grid legacy tile remains unchanged");
    std::vector<DesktopTile> impossible{Tile(0,0,0,208,100),Tile(1,2,0),Tile(2,3,0)};
    const RectI one_row{0,0,456,144};
    auto no_space=PreviewTileReorder(impossible,2,{132,44},one_row,{},{});
    check(!no_space.valid&&Same(no_space.updated_tiles,impossible)&&no_space.target_rect.x==132,
        "full mixed grid without ordered solution returns original and valid target");
    std::vector<DesktopTile> negative=start;
    for(auto& t:negative){t.x-=1000;t.y-=500;}
    auto neg=PreviewTileReorder(negative,2,{-976,-456},{-1000,-500,work.w,work.h},{},{});
    check(Clear(neg,{-1000,-500,work.w,work.h})&&neg.target_cell==PointI{0,0},"negative monitor coordinates use local grid anchor");
    auto budget=PreviewTileReorder(full,11,{24,44},work,{}, {}, {1,4096});
    check(!budget.valid&&budget.reason==TileReorderStatus::BudgetExceeded&&Same(budget.updated_tiles,full),"hard search budget returns original intact layout");
    auto huge=ComputeTileReorderTarget(Tile(9,0,0,1000,1000),{24,44},work,{});
    check(!huge.valid,"oversized tile rejected rather than resized");
    auto clamped=ComputeTileReorderTarget(start[0],{-9999,9999},work,{});
    check(clamped.valid&&clamped.rect.x==work.x&&clamped.rect.bottom()==work.bottom(),"pointer outside monitor clamps target exactly to work area edge");
    const RectI boundary_work{0,0,24+8*108,44+5*108};
    const auto reserved = [](RectI r) {
        r.w=((r.w+8+107)/108)*108-8;
        r.h=((r.h+8+107)/108)*108-8;
        return r;
    };
    const auto gap_clear = [&](const TileReorderResult& r) {
        if(!r.valid)return false;
        for(size_t i=0;i<r.updated_tiles.size();++i)for(size_t j=0;j<i;++j) {
            auto a=reserved(ResolveTileRect(r.updated_tiles[i],{}));
            auto b=reserved(ResolveTileRect(r.updated_tiles[j],{}));
            a.w+=8;a.h+=8;b.w+=8;b.h+=8;
            if(a.intersects(b))return false;
        }
        return true;
    };
    std::vector<DesktopTile> width108{Tile(0,0,0,108,100),Tile(1,3,0)};
    auto w108=PreviewTileReorder(width108,0,{240,44},boundary_work,{},{});
    check(Clear(w108,boundary_work)&&gap_clear(w108)&&w108.updated_tiles[0].w==108,
        "108-wide active reserves two cells and never touches neighbor");
    std::vector<DesktopTile> width216{Tile(0,0,0,216,100),Tile(1,3,0),Tile(2,4,0)};
    auto w216=PreviewTileReorder(width216,2,{24,44},boundary_work,{},{});
    check(Clear(w216,boundary_work)&&gap_clear(w216)&&w216.updated_tiles[0].w==216,
        "216-wide displaced tile reserves three cells preserving width");
    std::vector<DesktopTile> height108{Tile(0,0,0,100,108),Tile(1,0,3)};
    auto h108=PreviewTileReorder(height108,0,{24,152},boundary_work,{},{});
    check(Clear(h108,boundary_work)&&gap_clear(h108)&&h108.updated_tiles[0].h==108,
        "108-high tile reserves two rows with gap");
    std::vector<DesktopTile> height216{Tile(0,0,0,100,216),Tile(1,1,0),Tile(2,2,0)};
    auto h216=PreviewTileReorder(height216,2,{24,44},boundary_work,{},{});
    check(Clear(h216,boundary_work)&&gap_clear(h216)&&h216.updated_tiles[0].h==216,
        "216-high displaced tile reserves three rows");
    const std::vector<RectI> footprint_group{{180,44,30,100}};
    auto group_reserved=PreviewTileReorder(width108,0,{24,44},boundary_work,{},footprint_group);
    check(!group_reserved.valid&&group_reserved.reason==TileReorderStatus::BlockedTarget,
        "group in reserved trailing footprint blocks otherwise physically clear target");
    std::vector<DesktopTile> tight_legacy{Tile(0,0,0,108,100),Tile(1,1,0),Tile(2,4,0,109,101)};
    auto keep_tight=PreviewTileReorder(tight_legacy,2,{672,260},boundary_work,{},{});
    check(keep_tight.valid&&keep_tight.updated_tiles[0].x==24&&keep_tight.updated_tiles[1].x==132&&
        keep_tight.updated_tiles[2].w==109&&keep_tight.updated_tiles[2].h==101,
        "untouched zero-gap legacy pair does not globally invalidate empty move");
    std::vector<DesktopTile> offgrid{Tile(0,0,0,108,100),Tile(1,3,0)};
    offgrid[1].x=350;
    auto near_offgrid=PreviewTileReorder(offgrid,0,{240,44},boundary_work,{},{});
    check(near_offgrid.valid&&gap_clear(near_offgrid),"new footprint cannot collide with fixed off-grid neighbor");
    std::vector<DesktopTile> crossing{Tile(0,0,0),Tile(1,0,0)};
    crossing[1].x=400;crossing[1].y=152;crossing[1].w=100;
    const RectI local_work{0,0,456,368};
    auto cross_blocked=PreviewTileReorder(crossing,0,{348,152},local_work,{},{});
    check(!cross_blocked.valid&&cross_blocked.reason==TileReorderStatus::BlockedTarget&&
        Same(cross_blocked.updated_tiles,crossing),"partly outside right-edge card blocks empty-target fast path");
    crossing[1].x=-50;crossing[1].y=152;
    cross_blocked=PreviewTileReorder(crossing,0,{24,152},local_work,{},{});
    check(!cross_blocked.valid&&Same(cross_blocked.updated_tiles,crossing),
        "neighbor-monitor card extending across left edge remains fixed and blocks target");
    auto cross_clear=PreviewTileReorder(crossing,0,{240,152},local_work,{},{});
    check(cross_clear.valid&&cross_clear.updated_tiles[1].x==-50&&
        !ResolveTileRect(cross_clear.updated_tiles[0],{}).intersects(ResolveTileRect(cross_clear.updated_tiles[1],{})),
        "nonconflicting cross-monitor card stays fixed during empty move");
    const auto edge=ComputeTileReorderTarget(Tile(5,0,0,108,100),{400,44},local_work,{});
    check(edge.valid&&edge.rect.right()<=local_work.right()&&edge.reservation.right()<=local_work.right()&&
        edge.reservation.bottom()<=local_work.bottom()&&edge.reservation.x==edge.rect.x&&edge.reservation.y==edge.rect.y&&
        edge.reservation.w>=edge.rect.w&&edge.reservation.h>=edge.rect.h,
        "target clamp keeps complete reservation within work area");
    constexpr unsigned seed=0x51A7E123u;
    std::mt19937 rng(seed);
    bool properties=true;
    int valid_trials=0;
    for(int trial=0;trial<300;++trial) {
        const RectI area{-700,-300,24+8*108,44+6*108};
        std::vector<DesktopTile> tiles;
        auto external=Tile(99,0,0);
        external.x=area.right()-30;external.y=area.y+44+static_cast<int>(rng()%6)*108;
        tiles.push_back(external);
        for(int attempt=0;attempt<80&&tiles.size()<9;++attempt) {
            const int col=static_cast<int>(rng()%7),row=static_cast<int>(rng()%5);
            auto tile=Tile(static_cast<int>(tiles.size()),col,row,
                (rng()%3==0)?108:100,(rng()%4==0)?108:100);
            tile.x+=area.x;tile.y+=area.y;
            auto occupancy=reserved(ResolveTileRect(tile,{}));
            occupancy.w+=8;occupancy.h+=8;
            bool free=true;
            for(const auto& other:tiles) {
                auto other_rect=ResolveTileRect(other,{});
                if(other.id!="99")other_rect=reserved(other_rect);
                other_rect.w+=8;other_rect.h+=8;
                if(occupancy.intersects(other_rect))free=false;
            }
            if(free)tiles.push_back(tile);
        }
        if(tiles.size()<2){properties=false;break;}
        const auto saved=tiles;
        const size_t active=1+rng()%(tiles.size()-1);
        const PointI proposed{area.x-30+static_cast<int>(rng()%1000),area.y-30+static_cast<int>(rng()%800)};
        const auto one=PreviewTileReorder(tiles,active,proposed,area,{},{});
        const auto two=PreviewTileReorder(tiles,active,proposed,area,{},{});
        bool ok=Same(tiles,saved)&&one.valid==two.valid&&one.reason==two.reason&&
            one.visited_states==two.visited_states&&Same(one.updated_tiles,two.updated_tiles)&&
            one.updated_tiles.size()==tiles.size();
        if(one.valid) {
            ++valid_trials;
            for(size_t i=0;i<tiles.size();++i) {
                const auto& next=one.updated_tiles[i];
                const auto rect=ResolveTileRect(next,{});
                ok=ok&&next.id==tiles[i].id&&next.w==tiles[i].w&&next.h==tiles[i].h;
                if(i==0)ok=ok&&rect==ResolveTileRect(tiles[i],{});
                else ok=ok&&rect.x>=area.x&&rect.y>=area.y&&rect.right()<=area.right()&&rect.bottom()<=area.bottom();
                for(size_t j=0;j<i;++j)ok=ok&&!rect.intersects(ResolveTileRect(one.updated_tiles[j],{}));
            }
        } else ok=ok&&Same(one.updated_tiles,tiles);
        if(!ok) {
            std::printf("PROPERTY FAILURE seed=%u trial=%d active=%zu proposed=(%d,%d) valid=%d reason=%d\n",
                seed,trial,active,proposed.x,proposed.y,one.valid,static_cast<int>(one.reason));
            for(const auto& tile:tiles)std::printf("  id=%s rect=(%d,%d,%d,%d)\n",tile.id.c_str(),tile.x,tile.y,tile.w,tile.h);
            properties=false;break;
        }
    }
    check(properties&&valid_trials>0,"300 fixed-seed mixed-layout properties: bounds IDs dimensions deterministic immutable and external fixed");
    std::printf("PROPERTY seed=%u trials=300 valid=%d\n",seed,valid_trials);
    for(int count:{9,30,100}) {
        const int cols=10;
        const int rows=(count+cols-1)/cols;
        const RectI area{0,0,24+cols*108,44+rows*108};
        std::vector<DesktopTile> tiles;
        for(int i=0;i<count;++i)tiles.push_back(Tile(i,i%cols,i/cols));
        auto t0=std::chrono::steady_clock::now();
        auto result=PreviewTileReorder(tiles,tiles.size()-1,{24,44},area,{},{});
        double ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        check(Clear(result,area),"bounded dense-grid insertion succeeds");
        std::printf("BENCH cards=%d success_ms=%.3f states=%zu\n",count,ms,result.visited_states);
        t0=std::chrono::steady_clock::now();
        result=PreviewTileReorder(tiles,tiles.size()-1,{24,44},area,{}, {}, {20,4096});
        ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        std::printf("BENCH cards=%d budget_failure_ms=%.3f states=%zu valid=%d\n",count,ms,result.visited_states,result.valid);
        check(result.visited_states<=20,"bounded search candidate counter");
        std::vector<DesktopTile> fail_tiles{Tile(0,0,0,208,100)};
        for(int i=1;i<count;++i)fail_tiles.push_back(Tile(i,i+1,0));
        const RectI fail_area{0,0,24+(count+1)*108,144};
        t0=std::chrono::steady_clock::now();
        result=PreviewTileReorder(fail_tiles,fail_tiles.size()-1,{132,44},fail_area,{},{});
        ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t0).count();
        std::printf("BENCH cards=%d full_mixed_failure_ms=%.3f states=%zu reason=%d\n",count,ms,result.visited_states,static_cast<int>(result.reason));
        check(!result.valid&&Same(result.updated_tiles,fail_tiles)&&result.visited_states<=20000,
            "default-budget impossible full layout terminates intact");
    }
    return failures?1:0;
}
