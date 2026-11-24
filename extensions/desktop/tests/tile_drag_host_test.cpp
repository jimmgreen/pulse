// Compile the real host dispatch with persistence redirected to an in-memory string.
#define PULSE_DESKTOP_DRAG_TEST 1
#define main DesktopPluginEntryForDragTest
#include "host/host_main.cpp"
#undef main

namespace {
struct DesktopDragHostTestPeer {
    static void Prepare(DesktopHostSession& host) {
        host.state_ = {};
        host.state_.settings.organize_enabled = true;
        host.state_.settings.tiles_enabled = true;
        for (int i = 0; i < 3; ++i) {
            model::DesktopTile t;
            t.id = std::to_string(i); t.kind = model::TileKind::Apps; t.size = {1,1};
            t.free_placed = true; t.x = 24 + i * 108; t.y = 44; t.w = t.h = 100;
            host.state_.tiles.push_back(t);
        }
        host.undo_.Clear();
        host.test_saved_model_.clear();
        host.drag_tile_undo_snapshot_ = host.state_;
        host.dragging_tile_idx_ = 0;
        host.drag_offset_ = {};
        host.tile_drag_session_.Begin(host.state_.tiles, 0, host.PrimaryWorkAreaClient(), {}, {});
        host.interaction_.dragging_tile = true;
        host.interaction_.dragging_tile_id = "0";
    }
    static int RunEdges() {
        int count=0,failed=0;
        auto check=[&](bool ok,const char* name){++count;failed+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);};
        for (int dpi : {96,120,144}) for (auto size : {model::PointI{100,100},model::PointI{216,137},model::PointI{424,208}}) {
            const model::RectI work{-1920*96/dpi,20,1920*96/dpi,1040*96/dpi};
            for (auto target : {model::PointI{work.x,work.y},model::PointI{work.right()-size.x,work.y},
                                model::PointI{work.x,work.bottom()-size.y},model::PointI{work.right()-size.x,work.bottom()-size.y}}) {
                DesktopHostSession host({},0);Prepare(host);host.state_.tiles.resize(1);
                auto& tile=host.state_.tiles[0];tile.x=work.x+300;tile.y=work.y+250;tile.w=size.x;tile.h=size.y;
                host.state_.monitors={{L"primary",{0,0,1280,800},{0,0,1280,752},96,true},
                                      {L"secondary",{work.x,0,work.w,work.h+60},work,static_cast<uint32_t>(dpi),false}};
                host.tile_drag_session_.Reset();host.dragging_tile_idx_.reset();host.interaction_.dragging_tile=false;
                host.pending_tile_idx_=0;host.drag_start_pt_={tile.x,tile.y};host.drag_offset_={};
                const auto original=model::SerializeModelStateJson(host.state_);
                auto send=[&](UINT msg,WPARAM wp,model::PointI p){bool handled=false;host.OnSurfaceMessage(nullptr,msg,wp,MAKELPARAM(p.x,p.y),handled);};
                send(WM_MOUSEMOVE,MK_LBUTTON,target);
                check(host.state_.tiles[0].x==target.x&&host.state_.tiles[0].y==target.y,"real drag preview reaches exact selected-monitor workarea edge");
                host.SaveModel();check(host.test_saved_model_==original,"edge drag intermediate save remains original snapshot");
                send(WM_LBUTTONUP,0,target);
                model::DesktopModelState loaded;
                check(model::DeserializeModelStateJson(host.test_saved_model_,loaded)&&loaded.tiles[0].x==target.x&&loaded.tiles[0].y==target.y,
                      "mouseup commits exact edge and serialized reload keeps position");
                if (!loaded.tiles.empty()) host.state_.tiles=loaded.tiles;
                host.RecomputeLayout();
                const auto rendered=render::DesktopRenderer::ComputeTileRectInline(host.tile_board_rect_,host.state_.tiles[0]);
                check(rendered.x==target.x&&rendered.y==target.y&&rendered.w==size.x&&rendered.h==size.y,
                      "renderer geometry shares persisted actual edge rectangle");
            }
        }
        std::printf("Host tile edges: %d cases, %d failures\n",count,failed);return failed?1:0;
    }

    static int Run() {
        int count = 0, failed = 0;
        auto check = [&](bool ok, const char* label) {
            ++count; failed += !ok;
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        };
        DesktopHostSession host({},0);
        auto send = [&](UINT message, WPARAM wp=0, LPARAM lp=0) {
            bool handled = false;
            host.OnSurfaceMessage(nullptr,message,wp,lp,handled);
        };
        Prepare(host);
        const auto original = model::SerializeModelStateJson(host.state_);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        check(host.state_.tiles[0].x == 240 && host.state_.tiles[1].x == 24,
              "real mouse-move dispatch uses phone reorder preview");
        host.SaveModel();
        check(host.test_saved_model_ == original, "background save serializes original, not preview");
        host.state_.settings.weather_city = L"asynchronous setting";
        send(WM_KEYDOWN,VK_ESCAPE);
        check(!host.dragging_tile_idx_ && !host.tile_drag_session_.Active() && host.state_.tiles[0].x == 24,
              "Escape restores original tile layout and clears active drag");
        check(host.state_.settings.weather_city == L"asynchronous setting", "cancel preserves unrelated current model fields");
        check(!host.undo_.CanUndo(), "cancel does not create undo entry");
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        send(WM_CAPTURECHANGED);
        check(host.state_.tiles[0].x == 24 && !host.tile_drag_session_.Active(), "capture loss cancels instead of committing preview");
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        send(WM_CANCELMODE);
        check(host.state_.tiles[0].x == 24 && !host.dragging_tile_idx_, "cancel-mode restores starting layout");
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        send(WM_LBUTTONUP,0,MAKELPARAM(240,44));
        check(!host.tile_drag_session_.Active() && host.state_.tiles[0].x == 240 && host.undo_.UndoDepth()==1,
              "release commits preview with exactly one undo entry");
        model::DesktopModelState persisted;
        check(model::DeserializeModelStateJson(host.test_saved_model_,persisted) && persisted.tiles[0].x==240,
              "release persists committed placement");
        check(host.undo_.Undo(host.state_) && host.state_.tiles[0].x==24 && host.state_.tiles[1].x==132,
              "undo restores pre-drag neighbor order");
        check(host.undo_.Redo(host.state_) && host.state_.tiles[0].x==240 && host.state_.tiles[1].x==24,
              "redo restores single committed reorder");
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        host.RecomputeLayout();
        check(!host.tile_drag_session_.Active() && host.state_.tiles[0].x==24,
              "external layout refresh cancels stale obstacle snapshot");
        Prepare(host);
        model::DesktopGroup obstacle;
        obstacle.id = "fixed"; obstacle.x = 348; obstacle.y = 44; obstacle.cols = 2;
        host.state_.groups.push_back(obstacle);
        host.group_views_ = model::ComputeGroupViews(host.state_,0);
        host.tile_drag_session_.Begin(host.state_.tiles,0,host.PrimaryWorkAreaClient(),{},host.CurrentGroupFrames());
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(348,44));
        check(!host.interaction_.drag_tile_valid, "real dispatch marks group-blocked grid slot invalid");
        send(WM_LBUTTONUP,0,MAKELPARAM(348,44));
        check(host.state_.tiles[0].x==24 && !host.undo_.CanUndo() && host.test_saved_model_.empty(),
              "invalid release restores origin without undo or persistence");
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(24,44));
        send(WM_LBUTTONUP,0,MAKELPARAM(24,44));
        check(host.state_.tiles[0].x==24 && !host.undo_.CanUndo() && host.test_saved_model_.empty(),
              "return-and-release does not create a redundant undo or save");
        Prepare(host);
        for (auto& tile : host.state_.tiles) tile.kind = model::TileKind::Clock;
        host.state_.tiles[1].rest_origin = model::PointI{700,44};
        host.drag_tile_undo_snapshot_ = host.state_;
        host.tile_drag_session_.Begin(host.state_.tiles,0,host.PrimaryWorkAreaClient(),{},{});
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(182,94));
        check(host.tile_drag_session_.HasStackCandidate(),
              "real visible target, not historical rest origin, starts merge dwell");
        check(host.state_.tiles[1].x==132 && host.state_.tiles[2].x==240,
              "merge approach preserves visible target and neighbors before dwell completes");
        send(WM_KEYDOWN,VK_ESCAPE);
        check(host.state_.tiles.size()==3 && host.state_.tiles[0].x==24 && !host.undo_.CanUndo(),
              "merge approach Escape restores all original tiles without undo");
        Prepare(host);
        for (auto& tile : host.state_.tiles) tile.kind = model::TileKind::Clock;
        host.drag_tile_undo_snapshot_ = host.state_;
        host.drag_offset_ = {50,50};
        host.tile_drag_session_.Begin(host.state_.tiles,0,host.PrimaryWorkAreaClient(),{},{});
        bool stationary = true;
        for (int x = 74; x <= 182; x += 9) {
            send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(x,94));
            stationary &= host.state_.tiles[1].x==132 && host.state_.tiles[2].x==240;
        }
        check(stationary && host.tile_drag_session_.NeedsDwellTimer(),
              "continuous center-grab approach keeps merge target reachable and starts dwell");
        check(host.state_.tiles[0].x==132 && host.state_.tiles[0].y==44,
              "only the dragged card follows the pointer while target is reserved");
        host.SaveModel();
        model::DesktopModelState waiting;
        check(model::DeserializeModelStateJson(host.test_saved_model_,waiting) && waiting.tiles[0].x==24 && waiting.tiles[1].x==132,
              "saving while waiting for merge persists original geometry only");
        Sleep(625);
        send(WM_TIMER,kTimerTileReorder);
        check(host.tile_drag_session_.StackTarget()=="1" && host.interaction_.stack_drop_target_tile_id=="1",
              "real timer arms the same stationary target after dwell");
        send(WM_LBUTTONUP,0,MAKELPARAM(182,94));
        check(host.state_.tiles.size()==2 && host.undo_.UndoDepth()==1,
              "release after dwell merges once instead of displacing target");
        model::DesktopModelState merged;
        check(model::DeserializeModelStateJson(host.test_saved_model_,merged) && merged.tiles.size()==2,
              "armed merge persists committed stack");
        check(host.undo_.Undo(host.state_) && host.state_.tiles.size()==3 && host.state_.tiles[1].x==132,
              "one undo restores the complete pre-merge layout");
        std::printf("Host drag dispatch: %d cases, %d failures\n",count,failed);
        return failed ? 1 : 0;
    }
};
}
int main(int argc, char** argv) {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const int result = argc > 1 && std::string_view(argv[1]) == "--edges" ? DesktopDragHostTestPeer::RunEdges() : DesktopDragHostTestPeer::Run();
    CoUninitialize();
    return result;
}
