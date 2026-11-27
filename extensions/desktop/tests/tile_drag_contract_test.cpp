#define PULSE_DESKTOP_DRAG_TEST 1
#define main DesktopEntryForContractTest
#ifdef BASELINE_HOST
#include "baseline_host.cpp"
#else
#include "host/host_main.cpp"
#endif
#undef main

namespace {
struct DesktopDragHostTestPeer {
    static int Run() {
        DesktopHostSession host({},0);
        host.state_.settings.organize_enabled = host.state_.settings.tiles_enabled = true;
        for (int i=0;i<3;++i) {
            model::DesktopTile t;
            t.id=std::to_string(i); t.kind=model::TileKind::Apps; t.size={1,1};
            t.free_placed=true; t.x=24+108*i; t.y=44; t.w=t.h=100;
            host.state_.tiles.push_back(t);
        }
        const auto original=model::SerializeModelStateJson(host.state_);
        host.drag_tile_undo_snapshot_=host.state_;
        host.dragging_tile_idx_=0;
        host.drag_offset_={};
        host.interaction_.dragging_tile=true;
        host.interaction_.dragging_tile_id="0";
#ifndef BASELINE_HOST
        host.tile_drag_session_.Begin(host.state_.tiles,0,host.PrimaryWorkAreaClient(),{},{});
#endif
        bool handled=false;
        host.OnSurfaceMessage(nullptr,WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44),handled);
        int failed=0;
        auto check=[&](bool ok,const char* label) {
            failed+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);
        };
        check(host.state_.tiles[0].x==240 && host.state_.tiles[1].x==132 && host.state_.tiles[2].x==240 &&
              host.interaction_.drag_tile_preview_rect.x==348 && host.interaction_.drag_tile_preview_rect.y==44,
              "host follows pointer 1:1 with stationary siblings and nearest-free-cell ghost preview");
        host.SaveModel();
        check(host.test_saved_model_==original,"host persists original tiles during preview");
        host.OnSurfaceMessage(nullptr,WM_CAPTURECHANGED,0,0,handled);
        check(host.state_.tiles[0].x==24 && host.state_.tiles[1].x==132 && host.state_.tiles[2].x==240 &&
              host.interaction_.drag_tile_preview_rect.empty(),
              "capture loss restores starting tile placement and clears ghost");
        std::printf("Drag contract: 3 cases, %d failures\n",failed);
        return failed?1:0;
    }
};
}
int main() {
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    const int result=DesktopDragHostTestPeer::Run();
    CoUninitialize();
    return result;
}
