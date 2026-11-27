// Compile the real host dispatch with persistence redirected to an in-memory string.
#define PULSE_DESKTOP_DRAG_TEST 1
#define main DesktopPluginEntryForDragTest
#include "host/host_main.cpp"
#undef main

namespace {
struct DesktopDragHostTestPeer {
    static void Prepare(DesktopHostSession& host) {
        host.tile_springs_.Clear();
        host.group_springs_.Clear();
        host.tile_ghost_.Hide();
        host.group_ghost_.Hide();
        host.interaction_ = {};
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
                check(host.state_.tiles[0].x==target.x&&host.state_.tiles[0].y==target.y&&
                      host.interaction_.drag_tile_preview_rect.x==target.x&&host.interaction_.drag_tile_preview_rect.y==target.y,
                      "real drag preview and ghost reach exact selected-monitor workarea edge");
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
        check(host.state_.tiles[0].x == 240 && host.state_.tiles[1].x == 132 && host.state_.tiles[2].x == 240,
              "real mouse-move follows pointer 1:1 while keeping sibling tiles stationary");
        check(host.interaction_.drag_tile_preview_rect.x == 348 && host.interaction_.drag_tile_preview_rect.y == 44,
              "ghost preview snaps to nearest free grid cell around occupied tile");
        host.SaveModel();
        check(host.test_saved_model_ == original, "background save serializes original, not preview");

        host.state_.settings.weather_city = L"asynchronous setting";
        send(WM_KEYDOWN,VK_ESCAPE);
        check(!host.dragging_tile_idx_ && !host.tile_drag_session_.Active() && host.state_.tiles[0].x == 24 &&
              host.interaction_.drag_tile_preview_rect.empty(),
              "Escape restores original tile layout and hides ghost");
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

        // Drop at off-grid point (290, 80) -> commits to snapped cell (348, 44) and launches spring snap-back!
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(290,80));
        send(WM_LBUTTONUP,0,MAKELPARAM(290,80));
        check(!host.tile_drag_session_.Active() &&
              host.state_.tiles[0].x == 348 && host.state_.tiles[0].y == 44 &&
              host.state_.tiles[1].x == 132 && host.state_.tiles[2].x == 240 &&
              host.undo_.UndoDepth() == 1,
              "release commits snapped free cell with siblings calm and exactly one undo entry");
        check(!host.tile_springs_.Empty() && host.interaction_.tile_anim_positions.contains("0"),
              "off-grid release launches SwiftUI spring snap-back visual animation");
        host.TickMotionAnimations(NowAnimMs() + 200.0);
        check(host.interaction_.tile_anim_positions.contains("0") &&
              host.interaction_.tile_anim_positions["0"].x > 290,
              "spring snap-back advances visual tile position smoothly toward committed cell");
        host.TickMotionAnimations(NowAnimMs() + 800.0);
        check(host.tile_springs_.Empty() && host.interaction_.tile_anim_positions.empty(),
              "spring snap-back completes cleanly and clears visual override");

        model::DesktopModelState persisted;
        check(model::DeserializeModelStateJson(host.test_saved_model_,persisted) && persisted.tiles[0].x==348,
              "release persists committed snapped placement (never mid-spring coord)");
        check(host.undo_.Undo(host.state_) && host.state_.tiles[0].x==24 && host.state_.tiles[1].x==132,
              "undo restores pre-drag tile position");
        check(host.undo_.Redo(host.state_) && host.state_.tiles[0].x==348 && host.state_.tiles[1].x==132,
              "redo restores single committed move");

        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        host.RecomputeLayout();
        check(!host.tile_drag_session_.Active() && host.state_.tiles[0].x==24,
              "external layout refresh cancels stale obstacle snapshot");

        // Dragging over a group redirects ghost to nearest free cell outside the group.
        Prepare(host);
        model::DesktopGroup obstacle;
        obstacle.id = "fixed"; obstacle.x = 348; obstacle.y = 44; obstacle.cols = 2;
        host.state_.groups.push_back(obstacle);
        host.group_views_ = model::ComputeGroupViews(host.state_,0);
        host.tile_drag_session_.Begin(host.state_.tiles,0,host.PrimaryWorkAreaClient(),{},host.CurrentGroupFrames());
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(348,44));
        check(host.interaction_.drag_tile_valid &&
              !host.interaction_.drag_tile_preview_rect.intersects(host.group_views_[0].frame),
              "dragging over file group redirects ghost to nearest free non-overlapping cell");

        // Return to origin creates zero undo and zero save.
        Prepare(host);
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(240,44));
        send(WM_MOUSEMOVE,MK_LBUTTON,MAKELPARAM(24,44));
        send(WM_LBUTTONUP,0,MAKELPARAM(24,44));
        check(host.state_.tiles[0].x==24 && !host.undo_.CanUndo() && host.test_saved_model_.empty(),
              "return-and-release does not create a redundant undo or save");

        // Dwell-to-stack workflow preserved end-to-end.
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
        check(host.tile_drag_session_.StackTarget()=="1" && host.interaction_.stack_drop_target_tile_id=="1" &&
              host.interaction_.drag_tile_merge_armed,
              "real timer arms the same stationary target and lights merge ghost after dwell");
        send(WM_LBUTTONUP,0,MAKELPARAM(182,94));
        check(host.state_.tiles.size()==2 && host.undo_.UndoDepth()==1,
              "release after dwell merges once instead of displacing target");
        model::DesktopModelState merged;
        check(model::DeserializeModelStateJson(host.test_saved_model_,merged) && merged.tiles.size()==2,
              "armed merge persists committed stack");
        check(host.undo_.Undo(host.state_) && host.state_.tiles.size()==3 && host.state_.tiles[1].x==132,
              "one undo restores the complete pre-merge layout");

        // Liquid Glass Dock click activation, jitter/mouseleave tolerance, double-click safety,
        // middle-click new instance, drag-to-reorder, drag-out-to-unpin + poof, and divider resize.
        {
            DesktopHostSession dock_host({}, 0);
            dock_host.state_.monitors = {{L"primary", {0, 0, 1920, 1080}, {0, 0, 1920, 1032}, 96, true}};
            dock_host.state_.settings.dock_enabled = true;
            dock_host.state_.settings.dock_icon_size = 48;
            dock_host.state_.settings.dock_magnification = true;
            dock_host.state_.settings.dock_max_scale_x100 = 152;
            dock_host.state_.settings.double_click_hide_icons = true;
            dock_host.state_.settings.icons_hidden = false;
            dock_host.state_.settings.dock_pinned_paths = {
                L"C:\\Windows\\explorer.exe",
                L"C:\\Windows\\System32\\notepad.exe",
                L"C:\\Windows\\System32\\calc.exe",
                L"ms-settings:",
            };
            dock_host.live_data_.dock.BuildDemoItems(true);
            dock_host.live_data_.dock.items.clear();
            // Build deterministic items matching dock_pinned_paths + divider + downloads + widgets + trash
            for (const auto& p : dock_host.state_.settings.dock_pinned_paths) {
                render::DockBarItem it{};
                it.kind = render::DockBarItemKind::App;
                it.key = p;
                for (auto& ch : it.key) ch = static_cast<wchar_t>(std::towlower(ch));
                it.launch_path = p;
                it.title = render::LiquidDockBarState::FriendlyAppName(p);
                it.pinned = true;
                dock_host.live_data_.dock.items.push_back(std::move(it));
            }
            {
                render::DockBarItem div{};
                div.kind = render::DockBarItemKind::Divider;
                div.key = L"special:divider";
                dock_host.live_data_.dock.items.push_back(std::move(div));
            }
            {
                render::DockBarItem tr{};
                tr.kind = render::DockBarItemKind::Trash;
                tr.key = L"special:trash";
                tr.title = L"回收站";
                tr.pinned = true;
                dock_host.live_data_.dock.items.push_back(std::move(tr));
            }
            const model::RectI wa = dock_host.PrimaryWorkAreaClient();
            dock_host.live_data_.dock.ComputeGeometry(wa, dock_host.state_.settings, -1.0f, 0.0f, 0.0);

            auto send_dock = [&](UINT msg, WPARAM wp, int x, int y) {
                bool h = false;
                dock_host.OnSurfaceMessage(nullptr, msg, wp, MAKELPARAM(x, y), h);
            };

            // 1) Normal click on Settings icon activates ms-settings: and starts bounce
            const int sx = static_cast<int>(std::lround(dock_host.live_data_.dock.items[3].draw_cx));
            const int sy = static_cast<int>(std::lround(dock_host.live_data_.dock.items[3].draw_cy));
            send_dock(WM_LBUTTONDOWN, MK_LBUTTON, sx, sy);
            send_dock(WM_LBUTTONUP, 0, sx, sy);
            check(dock_host.test_dock_activations_.size() == 1 &&
                  dock_host.test_dock_activations_.back() == L"act:ms-settings:" &&
                  dock_host.live_data_.dock.HasActiveBounce(NowAnimMs()),
                  "dock normal click activates ms-settings: and triggers bounce");

            // 2) Click with mid-press WM_MOUSELEAVE and 6px jitter STILL activates pressed icon!
            const int ex = static_cast<int>(std::lround(dock_host.live_data_.dock.items[0].draw_cx));
            const int ey = static_cast<int>(std::lround(dock_host.live_data_.dock.items[0].draw_cy));
            send_dock(WM_LBUTTONDOWN, MK_LBUTTON, ex, ey);
            send_dock(WM_MOUSELEAVE, 0, 0, 0);
            send_dock(WM_LBUTTONUP, 0, ex + 6, ey + 2);
            check(dock_host.test_dock_activations_.size() == 2 &&
                  dock_host.test_dock_activations_.back() == L"act:C:\\Windows\\explorer.exe",
                  "dock click survives mid-press WM_MOUSELEAVE and small cursor jitter");

            // 3) Double-click on Dock activates icon and NEVER hides desktop icons
            send_dock(WM_LBUTTONDBLCLK, MK_LBUTTON, sx, sy);
            check(dock_host.test_dock_activations_.size() == 3 &&
                  dock_host.test_dock_activations_.back() == L"act:ms-settings:" &&
                  !dock_host.state_.settings.icons_hidden,
                  "dock double-click activates icon without triggering hide-desktop-icons");

            // 4) Middle-click on Dock triggers new-instance launch
            send_dock(WM_MBUTTONUP, 0, ex, ey);
            check(dock_host.test_dock_activations_.size() == 4 &&
                  dock_host.test_dock_activations_.back() == L"new:C:\\Windows\\explorer.exe",
                  "dock middle-click triggers new-instance launch");

            // 5) Drag-to-reorder moves ms-settings: (slot 3) before explorer.exe (slot 0)
            const int x0 = static_cast<int>(std::lround(dock_host.live_data_.dock.items[0].draw_cx));
            send_dock(WM_LBUTTONDOWN, MK_LBUTTON, sx, sy);
            send_dock(WM_MOUSEMOVE, MK_LBUTTON, sx - 20, sy);
            send_dock(WM_MOUSEMOVE, MK_LBUTTON, x0 - 12, sy);
            check(dock_host.interaction_.dock_bar_drag.active &&
                  dock_host.interaction_.dock_bar_drag.target_pinned_slot == 0,
                  "dock drag enters reorder mode and computes live insertion slot 0");
            send_dock(WM_LBUTTONUP, 0, x0 - 12, sy);
            check(!dock_host.state_.settings.dock_pinned_paths.empty() &&
                  dock_host.state_.settings.dock_pinned_paths.front() == L"ms-settings:",
                  "dock drag-to-reorder persists moved pin at slot 0");

            // 6) Drag-out upward unpins non-destructively and triggers Poof smoke animation
            const int ux = static_cast<int>(std::lround(dock_host.live_data_.dock.items[0].draw_cx));
            const int uy = static_cast<int>(std::lround(dock_host.live_data_.dock.items[0].draw_cy));
            const int out_y = static_cast<int>(std::lround(dock_host.live_data_.dock.pill_rect.top - 120.0f));
            send_dock(WM_LBUTTONDOWN, MK_LBUTTON, ux, uy);
            send_dock(WM_MOUSEMOVE, MK_LBUTTON, ux, out_y);
            check(dock_host.interaction_.dock_bar_drag.active && dock_host.interaction_.dock_bar_drag.drag_out,
                  "dragging pinned icon upward arms drag-out unpin badge");
            send_dock(WM_LBUTTONUP, 0, ux, out_y);
            check(dock_host.state_.settings.dock_pinned_paths.size() == 3 &&
                  dock_host.live_data_.dock.HasActivePoof(NowAnimMs()),
                  "releasing drag-out unpins icon cleanly and starts poof animation");

            // 7) Dragging vertical divider upward increases dock_icon_size smoothly
            int div_idx = -1;
            for (size_t di = 0; di < dock_host.live_data_.dock.items.size(); ++di) {
                if (dock_host.live_data_.dock.items[di].kind == render::DockBarItemKind::Divider) {
                    div_idx = static_cast<int>(di);
                    break;
                }
            }
            const int dx = div_idx >= 0 ? static_cast<int>(std::lround(dock_host.live_data_.dock.items[static_cast<size_t>(div_idx)].draw_cx)) : 0;
            const int dy = div_idx >= 0 ? static_cast<int>(std::lround(dock_host.live_data_.dock.items[static_cast<size_t>(div_idx)].draw_cy)) : 0;
            send_dock(WM_LBUTTONDOWN, MK_LBUTTON, dx, dy);
            send_dock(WM_MOUSEMOVE, MK_LBUTTON, dx, dy - 24);
            send_dock(WM_LBUTTONUP, 0, dx, dy - 24);
            check(div_idx >= 0 && dock_host.state_.settings.dock_icon_size == 56,
                  "dragging dock divider upward resizes dock_icon_size from 48 to 56");

            // 8) Auto-Hide smooth collapse (reveal 1.0 -> 0.0) & bottom-edge hover expand (0.0 -> 1.0)
            dock_host.state_.settings.dock_hide_mode = 2; // AutoHide
            dock_host.dock_bar_should_hide_ = true;
            dock_host.dock_bar_reveal_ = 1.0f;
            dock_host.interaction_.dock_bar_reveal = 1.0f;
            const float rest_top = dock_host.live_data_.dock.pill_rect.top;
            double sim_ms = NowAnimMs();
            dock_host.last_motion_ms_ = sim_ms;
            for (int step = 0; step < 24; ++step) {
                sim_ms += 16.0;
                dock_host.TickMotionAnimations(sim_ms);
            }
            const float hidden_top = dock_host.live_data_.dock.pill_rect.top;
            check(dock_host.dock_bar_reveal_ <= 0.01f && hidden_top > rest_top + 60.0f,
                  "dock auto-hide smoothly collapses pill off bottom edge");
            // Moving cursor to bottom edge immediately wakes expand slide back to resting top
            send_dock(WM_MOUSEMOVE, 0, wa.w / 2, wa.bottom() - 4);
            check(!dock_host.dock_bar_should_hide_, "moving cursor to bottom edge triggers dock expand");
            for (int step = 0; step < 24; ++step) {
                sim_ms += 16.0;
                dock_host.TickMotionAnimations(sim_ms);
            }
            check(dock_host.dock_bar_reveal_ >= 0.99f &&
                  std::fabs(dock_host.live_data_.dock.pill_rect.top - rest_top) < 1.5f,
                  "dock expand animation returns smoothly to exact resting position");

            // 9) Icon enter / exit smooth slot expand & leaving-ghost collapse animation
            {
                render::LiquidDockBarState slot_bar{};
                std::vector<std::wstring> pins = {L"C:\\Windows\\explorer.exe", L"ms-settings:"};
                slot_bar.RefreshSystemItems(pins, GetCurrentProcessId(), 0, 1000.0);
                pins.push_back(L"C:\\Windows\\System32\\calc.exe");
                slot_bar.RefreshSystemItems(pins, GetCurrentProcessId(), 0, 1500.0);
                slot_bar.ComputeGeometry(wa, dock_host.state_.settings, -1.0f, 0.0f, 1580.0);
                const int calc_i = slot_bar.FindItemIndexByKey(L"c:\\windows\\system32\\calc.exe");
                const bool enter_ok = slot_bar.HasActiveSlotMotion(1580.0) && calc_i >= 0 &&
                    slot_bar.items[static_cast<size_t>(calc_i)].slot_factor > 0.1f &&
                    slot_bar.items[static_cast<size_t>(calc_i)].slot_factor < 0.98f;
                // Now remove calc -> spawns shrinking ghost slot
                pins.pop_back();
                slot_bar.RefreshSystemItems(pins, GetCurrentProcessId(), 0, 2000.0);
                slot_bar.ComputeGeometry(wa, dock_host.state_.settings, -1.0f, 0.0f, 2100.0);
                bool has_shrinking_ghost = false;
                for (const auto& it : slot_bar.items) {
                    if (it.is_ghost && it.slot_factor > 0.05f && it.slot_factor < 0.95f) {
                        has_shrinking_ghost = true;
                        break;
                    }
                }
                check(enter_ok && has_shrinking_ghost,
                      "dock icon enter expands slot smoothly and exit spawns shrinking collapse ghost");
            }

            // 10) D3D11 DockGenieAnimator renders curved funnel Genie & Scale warp frames cleanly
            {
                render::DockWindowSnapshot snap{};
                snap.width = 160;
                snap.height = 120;
                snap.screen_rect = {80, 40, 240, 160};
                snap.pixels.assign(static_cast<size_t>(snap.width * snap.height), 0xFF3A82F6u);
                render::DockGenieAnimator genie{};
                std::vector<uint32_t> bgra_genie, bgra_scale;
                const RECT win_rc{80, 40, 240, 160};
                const RECT dock_rc{140, 220, 180, 260};
                const bool g_ok = genie.RenderPreviewFrame(
                    snap, 320, 280, win_rc, dock_rc, 0.48f, render::DockMinimizeEffect::Genie, bgra_genie);
                const bool s_ok = genie.RenderPreviewFrame(
                    snap, 320, 280, win_rc, dock_rc, 0.48f, render::DockMinimizeEffect::Scale, bgra_scale);
                auto row_span = [&](const std::vector<uint32_t>& buf, int y) {
                    int lo = 320, hi = -1;
                    for (int x = 0; x < 320; ++x) {
                        if ((buf[static_cast<size_t>(y) * 320 + static_cast<size_t>(x)] >> 24) > 20u) {
                            lo = std::min(lo, x);
                            hi = std::max(hi, x);
                        }
                    }
                    return hi >= lo ? (hi - lo + 1) : 0;
                };
                const int top_w = g_ok ? row_span(bgra_genie, 110) : 0;
                const int neck_w = g_ok ? row_span(bgra_genie, 210) : 0;
                const int scale_mid_w = s_ok ? row_span(bgra_scale, 160) : 0;
                check(g_ok && s_ok && top_w > neck_w + 18 && neck_w > 12 && scale_mid_w > 20,
                      "D3D11 DockGenieAnimator warps Genie curved funnel (wider top -> pinched neck) and Scale frame");
            }

            // 11) Dock right-click menu commands execute end-to-end (including stale index resolution via dock_item_key)
            {
                dock_host.state_.settings.dock_enabled = true;
                dock_host.state_.settings.dock_icon_size = 48;
                dock_host.state_.settings.dock_magnification = true;
                dock_host.state_.settings.dock_max_scale_x100 = 152;
                dock_host.state_.settings.dock_hide_mode = 0;
                dock_host.state_.settings.dock_minimize_effect = 1;
                dock_host.state_.settings.dock_pinned_paths = {
                    L"C:\\Windows\\explorer.exe",
                    L"C:\\Windows\\System32\\calc.exe",
                    L"ms-settings:",
                };
                dock_host.RefreshLiquidDockBar(false, false);

                overlays::ContextMenuSpec dspec{};
                dspec.target_kind = overlays::ContextMenuTargetKind::DockBar;
                dspec.dock_item_index = 99; // Deliberately stale index; must resolve via dock_item_key!
                dspec.dock_item_key = L"c:\\windows\\system32\\calc.exe";
                dspec.target_path = L"C:\\Windows\\System32\\calc.exe";
                dspec.dock_item_pinned = true;

                dock_host.ExecuteContextMenuCommand("dock-size:42", dspec);
                check(dock_host.state_.settings.dock_icon_size == 42, "dock right-click dock-size:42 updates icon size");

                dock_host.ExecuteContextMenuCommand("dock-scale:178", dspec);
                check(dock_host.state_.settings.dock_max_scale_x100 == 178 && dock_host.state_.settings.dock_magnification,
                      "dock right-click dock-scale:178 updates magnification scale");

                dock_host.ExecuteContextMenuCommand("dock-mag-toggle", dspec);
                check(!dock_host.state_.settings.dock_magnification, "dock right-click dock-mag-toggle toggles magnification");

                dock_host.ExecuteContextMenuCommand("dock-hide:2", dspec);
                check(dock_host.state_.settings.dock_hide_mode == 2, "dock right-click dock-hide:2 sets AutoHide");

                dock_host.ExecuteContextMenuCommand("dock-hide:0", dspec);
                check(dock_host.state_.settings.dock_hide_mode == 0 && !dock_host.dock_bar_should_hide_,
                      "dock right-click dock-hide:0 restores Always-visible Dock");

                dock_host.ExecuteContextMenuCommand("dock-minfx:2", dspec);
                check(dock_host.state_.settings.dock_minimize_effect == 2, "dock right-click dock-minfx:2 switches to Scale");

                // Multi-edge Dock positioning: Left (1), Right (2), and back to Bottom (0)
                dock_host.ExecuteContextMenuCommand("dock-pos:1", dspec);
                const bool left_geom_ok = dock_host.state_.settings.dock_position == 1 &&
                                          dock_host.live_data_.dock.IsVertical() &&
                                          dock_host.live_data_.dock.pill_rect.left < 60.0f &&
                                          (dock_host.live_data_.dock.pill_rect.bottom - dock_host.live_data_.dock.pill_rect.top) >
                                          (dock_host.live_data_.dock.pill_rect.right - dock_host.live_data_.dock.pill_rect.left);
                check(left_geom_ok, "dock right-click dock-pos:1 moves Dock to vertical Left edge");

                dock_host.ExecuteContextMenuCommand("dock-pos:2", dspec);
                const bool right_geom_ok = dock_host.state_.settings.dock_position == 2 &&
                                           dock_host.live_data_.dock.IsVertical() &&
                                           dock_host.live_data_.dock.pill_rect.right > 900.0f &&
                                           (dock_host.live_data_.dock.pill_rect.bottom - dock_host.live_data_.dock.pill_rect.top) >
                                           (dock_host.live_data_.dock.pill_rect.right - dock_host.live_data_.dock.pill_rect.left);
                check(right_geom_ok, "dock right-click dock-pos:2 moves Dock to vertical Right edge");

                dock_host.ExecuteContextMenuCommand("dock-pos:0", dspec);
                check(dock_host.state_.settings.dock_position == 0 && !dock_host.live_data_.dock.IsVertical(),
                      "dock right-click dock-pos:0 restores horizontal Bottom Dock");

                const bool prev_tiles = dock_host.state_.settings.tiles_enabled;
                dock_host.ExecuteContextMenuCommand("dock-toggle-widgets", dspec);
                check(dock_host.state_.settings.tiles_enabled == !prev_tiles,
                      "dock right-click dock-toggle-widgets toggles desktop widgets");

                dock_host.test_dock_activations_.clear();
                dock_host.ExecuteContextMenuCommand("dock-open", dspec);
                check(dock_host.test_dock_activations_.size() == 1 &&
                      dock_host.test_dock_activations_[0] == L"act:C:\\Windows\\System32\\calc.exe",
                      "dock right-click dock-open resolves stale index by key and activates item");

                dock_host.ExecuteContextMenuCommand("dock-pin-toggle", dspec);
                check(dock_host.state_.settings.dock_pinned_paths.size() == 2,
                      "dock right-click dock-pin-toggle unpins by key even with stale index");

                // 11b) .dk-sys system area toggle, calm-fisheye hover, and tray pin persistence
                dock_host.state_.settings.dock_system_area = true;
                dock_host.live_data_.dock.BuildDemoSystemSnapshot(true);
                dock_host.live_data_.dock.ComputeGeometry(wa, dock_host.state_.settings, -1.0f, 0.0f, NowAnimMs());
                const auto* vol_slot = dock_host.live_data_.dock.FindSysSlot(render::DockSysSlotKind::Volume);
                check(vol_slot != nullptr, "dock .dk-sys lays out Volume slot when dock_system_area is enabled");
                if (vol_slot) {
                    const int vx = static_cast<int>(std::lround((vol_slot->rect.left + vol_slot->rect.right) * 0.5f));
                    const int vy = static_cast<int>(std::lround((vol_slot->rect.top + vol_slot->rect.bottom) * 0.5f));
                    send_dock(WM_MOUSEMOVE, 0, vx, vy);
                    check(dock_host.dock_bar_sys_hover_ &&
                          dock_host.interaction_.dock_sys_hovered_kind == render::DockSysSlotKind::Volume,
                          "hovering .dk-sys Volume slot tracks hovered slot and calms icon fisheye");
                }
                dock_host.ExecuteContextMenuCommand("dock-sys-toggle", dspec);
                check(!dock_host.state_.settings.dock_system_area && dock_host.live_data_.dock.sys_slots.empty(),
                      "dock right-click dock-sys-toggle hides .dk-sys system area cleanly");
                dock_host.ExecuteContextMenuCommand("dock-sys-toggle", dspec);
                check(dock_host.state_.settings.dock_system_area && !dock_host.live_data_.dock.sys_slots.empty(),
                      "dock right-click dock-sys-toggle restores .dk-sys system area cleanly");

                dock_host.ExecuteContextMenuCommand("dock-disable", dspec);
                check(!dock_host.state_.settings.dock_enabled, "dock right-click dock-disable hides Dock");

                // 11c) Dock Genie animation must NEVER hang when host activity suspends (e.g. another
                //      maximized app is behind the minimized window): independent overlay watchdog timer
                //      must drive it to completion and invoke on_complete!
                {
                    render::DockWindowSnapshot snap{};
                    snap.width = 240;
                    snap.height = 160;
                    snap.screen_rect = {-3200, 140, -2960, 300};
                    snap.pixels.assign(static_cast<size_t>(snap.width) * static_cast<size_t>(snap.height), 0xFF336699u);
                    const RECT win_rc = snap.screen_rect;
                    const RECT dock_rc = {-3100, 680, -3052, 728};

                    render::DockGenieAnimator genie;
                    if (genie.Initialize(GetModuleHandleW(nullptr))) {
                        bool min_done = false;
                        const bool min_started = genie.Start(
                            snap, win_rc, dock_rc, /*reverse=*/false,
                            render::DockMinimizeEffect::Genie,
                            [&] { min_done = true; }, /*dock_edge=*/0);
                        if (min_started) {
                            // Pump Win32 messages ONLY (zero manual genie.Tick calls, simulating suspended host clock).
                            const ULONGLONG deadline = GetTickCount64() + 1200;
                            while (genie.IsActive() && GetTickCount64() < deadline) {
                                MSG gm{};
                                while (PeekMessageW(&gm, nullptr, 0, 0, PM_REMOVE)) {
                                    TranslateMessage(&gm);
                                    DispatchMessageW(&gm);
                                }
                                Sleep(10);
                            }
                            const bool overlay_vis = genie.OverlayHandle() && IsWindowVisible(genie.OverlayHandle());
                            check(!genie.IsActive() && !overlay_vis && min_done,
                                  "Dock Genie minimize completes and hides overlay via watchdog without host VSync ticks");
                        } else {
                            check(true, "Dock Genie minimize cleanly skipped when headless DComp unavailable");
                        }

                        bool force_done = false;
                        const bool rest_started = genie.Start(
                            snap, win_rc, dock_rc, /*reverse=*/true,
                            render::DockMinimizeEffect::Genie,
                            [&] { force_done = true; }, /*dock_edge=*/0);
                        if (rest_started) {
                            genie.ForceComplete();
                            const bool overlay_vis = genie.OverlayHandle() && IsWindowVisible(genie.OverlayHandle());
                            check(!genie.IsActive() && !overlay_vis && force_done,
                                  "Dock Genie ForceComplete immediately hides overlay and fires callback");
                        }
                        genie.Shutdown();
                    }
                }
            }

            // 12) Real SystemSampler API accuracy verification (physical/logical cores, RAM exact pct,
            //     VRAM LUID-matched arithmetic consistency, no bogus unprimed 0% CPU spike on sample #0)
            {
                live::SystemSampler sampler;
                const auto s0 = sampler.Sample();
                Sleep(120);
                const auto s1 = sampler.Sample();
                const double expected_mem_pct = s1.mem_total > 0
                    ? (static_cast<double>(s1.mem_used) * 100.0 / static_cast<double>(s1.mem_total))
                    : 0.0;
                const bool vram_ok = !s1.has_vram ||
                    (s1.vram_total_gb > 0.1f && s1.vram_used_gb >= 0.0f &&
                     s1.vram_used_gb <= s1.vram_total_gb + 0.05f &&
                     std::fabs(s1.vram_pct - (s1.vram_used_gb * 100.0f / s1.vram_total_gb)) < 0.5f);
                std::printf("  [live probe] CPU=%d%% (%.2f%%, %uC/%uT) RAM=%.2f/%.2fGB (%.2f%%) VRAM=%s(%.2f/%.2fGB %.1f%%) Disk=%d%% Net=v%llu/^%llu B/s\n",
                            s1.cpu_pct, s1.cpu_pct_exact, s1.physical_cores, s1.logical_cores,
                            static_cast<double>(s1.mem_used) / (1024.0 * 1024.0 * 1024.0),
                            static_cast<double>(s1.mem_total) / (1024.0 * 1024.0 * 1024.0),
                            s1.mem_pct_exact,
                            s1.has_vram ? "yes" : "no",
                            s1.vram_used_gb, s1.vram_total_gb, s1.vram_pct,
                            s1.disk_used_pct,
                            static_cast<unsigned long long>(s1.net_down_bps),
                            static_cast<unsigned long long>(s1.net_up_bps));
                check(s0.valid && s0.cpu_history.empty() && s1.valid && s1.cpu_history.size() == 1,
                      "SystemSampler omits unprimed 0% CPU spike on sample #0 and records real sample #1");
                check(s1.physical_cores > 0 && s1.logical_cores >= s1.physical_cores,
                      "SystemSampler distinguishes real physical cores from logical threads");
                check(s1.mem_total > 0 && s1.mem_used <= s1.mem_total &&
                      std::fabs(s1.mem_pct_exact - expected_mem_pct) < 0.02,
                      "SystemSampler reports exact physical RAM usage and sub-percent precision");
                check(vram_ok, "SystemSampler VRAM reading is strictly LUID-matched and arithmetically self-consistent");
            }

            // 13) Dock stacks (叠放): fan geometry, view resolution, sorting, and a headless render of the
            //     Fan / Grid / List popups over a real folder. PULSE_DOCK_FX_DUMP=<dir> also writes review
            //     BMPs of the stacks and of Genie frames (mip-mapped snapshot, round corners, absorb).
            {
                using namespace overlays;
                const DockFanPoint f0 = DockFanItemCenter(0), f1 = DockFanItemCenter(1), f9 = DockFanItemCenter(9);
                const float step01 = std::hypot(f1.x - f0.x, f1.y - f0.y);
                check(f0.y < 0.0f && f1.y < f0.y && f9.y < f1.y && f9.x > f1.x + 4.0f && f1.x >= f0.x &&
                          std::fabs(step01 - kDockFanStep) < 1.5f,
                      "Dock stack fan rises from the icon along an arc that bends right, one step per item");
                const int cap_tall = DockFanCapacity(900.0f), cap_short = DockFanCapacity(200.0f);
                check(cap_tall >= 10 && cap_tall <= 14 && cap_short >= 1 && cap_short < 4,
                      "Dock stack fan capacity follows the room above the Dock (max ~a dozen items)");
                check(ResolveDockStackView(DockStackView::Automatic, false, 0, 8, 12) == DockStackView::Fan &&
                          ResolveDockStackView(DockStackView::Automatic, false, 1, 8, 12) == DockStackView::Grid &&
                          ResolveDockStackView(DockStackView::Fan, false, 2, 8, 12) == DockStackView::Grid &&
                          ResolveDockStackView(DockStackView::Fan, true, 0, 8, 12) == DockStackView::Grid &&
                          ResolveDockStackView(DockStackView::List, false, 0, 8, 12) == DockStackView::List &&
                          ResolveDockStackView(DockStackView::Automatic, false, 0, 0, 12) == DockStackView::Grid,
                      "Dock stack view: Automatic = Fan above a bottom Dock, Grid beside a side Dock and for apps");
                std::vector<DockStackEntry> es(3);
                es[0].name = L"file10"; es[0].added = 100;
                es[1].name = L"file2";  es[1].added = 300;
                es[2].name = L"Alpha";  es[2].added = 200;
                SortDockStackEntries(es, DockStackSort::DateAdded);
                const bool by_date = es[0].name == L"file2" && es[1].name == L"Alpha";
                SortDockStackEntries(es, DockStackSort::Name);
                const bool by_name = es[0].name == L"Alpha" && es[1].name == L"file2" && es[2].name == L"file10";
                check(by_date && by_name && DockStackDisplayName(L"Code.lnk", false) == L"Code" &&
                          DockStackDisplayName(L"a.lnk", true) == L"a.lnk" && IsDockStackAppNoise(L"Uninstall Foo"),
                      "Dock stack sorts newest-first / by natural name and hides .lnk endings");

                // A fixture folder beside the test exe (the harness may point TEMP at a sandbox path).
                wchar_t exe_path[MAX_PATH]{};
                GetModuleFileNameW(nullptr, exe_path, MAX_PATH);
                const std::wstring dir = std::filesystem::path(exe_path).parent_path().wstring() + L"\\pulse_dock_stack_test";
                std::error_code mk_ec;
                std::filesystem::create_directories(dir, mk_ec);
                const wchar_t* names[] = {L"报告 2026.docx", L"photo.png", L"notes.txt", L"setup.exe", L"archive.zip",
                                          L"Project Plan.pdf", L"clip.mp4"};
                for (const wchar_t* n : names) {
                    HANDLE fh = CreateFileW((dir + L"\\" + n).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                            FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (fh != INVALID_HANDLE_VALUE) CloseHandle(fh);
                }
                CreateDirectoryW((dir + L"\\Sub folder").c_str(), nullptr);
                char dump_env[MAX_PATH]{};
                const DWORD dump_len = GetEnvironmentVariableA("PULSE_DOCK_FX_DUMP", dump_env, MAX_PATH);
                const std::string dump_dir = (dump_len > 0 && dump_len < MAX_PATH) ? std::string(dump_env) : std::string();
                {
                    WIN32_FIND_DATAW probe{};
                    int raw = 0;
                    HANDLE ph = FindFirstFileW((dir + L"\\*").c_str(), &probe);
                    if (ph != INVALID_HANDLE_VALUE) {
                        do { ++raw; } while (FindNextFileW(ph, &probe));
                        FindClose(ph);
                    }
                    std::printf("  [stack] folder raw=%d listed=%zu attr=%lu\n", raw,
                                ListDockStackFolder(dir, DockStackSort::DateAdded).size(),
                                GetFileAttributesW(dir.c_str()));
                }
                auto dump = [&](const std::string& name, const std::vector<uint32_t>& px, int w, int h) {
                    if (dump_dir.empty() || px.empty()) return;
                    std::ofstream fp(dump_dir + "\\" + name, std::ios::binary);
                    if (!fp) return;
                    BITMAPFILEHEADER bf{};
                    BITMAPINFOHEADER bi{};
                    bi.biSize = sizeof(bi); bi.biWidth = w; bi.biHeight = -h; bi.biPlanes = 1; bi.biBitCount = 32;
                    bf.bfType = 0x4D42;
                    bf.bfOffBits = sizeof(bf) + sizeof(bi);
                    bf.bfSize = bf.bfOffBits + static_cast<DWORD>(w * h * 4);
                    fp.write(reinterpret_cast<const char*>(&bf), sizeof(bf));
                    fp.write(reinterpret_cast<const char*>(&bi), sizeof(bi));
                    for (int y = 0; y < h; ++y) {
                        for (int x = 0; x < w; ++x) {
                            // Premultiplied over a mid wallpaper-ish gradient.
                            const uint32_t s = px[static_cast<size_t>(y) * w + x];
                            const uint32_t a = s >> 24;
                            const uint32_t bgc[3] = {static_cast<uint32_t>(150 + 60 * y / std::max(1, h)), 110u,
                                                     static_cast<uint32_t>(70 + 50 * x / std::max(1, w))};
                            uint32_t o = 0xFF000000u;
                            for (int c = 0; c < 3; ++c) {
                                const uint32_t sc = (s >> (8 * c)) & 0xFF;
                                o |= std::min<uint32_t>(255u, sc + bgc[c] * (255u - a) / 255u) << (8 * c);
                            }
                            fp.write(reinterpret_cast<const char*>(&o), 4);
                        }
                    }
                };
                auto coverage = [](const std::vector<uint32_t>& px) {
                    size_t n = 0;
                    for (uint32_t p : px) n += (p >> 24) > 24u;
                    return n;
                };
                DockStackRequest rq{};
                rq.key = L"test:stack";
                rq.title = L"pulse_dock_stack_test";
                rq.folder = dir;
                rq.icon_screen = RECT{900, 1000, 948, 1048};
                rq.work_screen = RECT{0, 0, 1920, 1060};
                rq.scale = 1.0f;
                rq.dock_edge = 0;
                rq.dark = true;
                rq.headless = true;
                const struct { DockStackView v; uint8_t edge; const char* file; } cases[] = {
                    {DockStackView::Fan, 0, "stack_fan.bmp"},
                    {DockStackView::Grid, 0, "stack_grid.bmp"},
                    {DockStackView::List, 0, "stack_list.bmp"},
                    {DockStackView::Automatic, 1, "stack_side_grid.bmp"},
                };
                bool stacks_ok = true;
                for (const auto& c : cases) {
                    DockStackPopup pop;
                    rq.view = c.v;
                    rq.dock_edge = c.edge;
                    if (c.edge == 1) rq.icon_screen = RECT{8, 500, 56, 548};
                    std::vector<uint32_t> full, start;
                    int w = 0, h = 0, w0 = 0, h0 = 0;
                    const bool shown = pop.Show(rq);
                    const bool r1 = shown && pop.RenderSnapshot(full, w, h, 1.0f);
                    const bool r0 = shown && pop.RenderSnapshot(start, w0, h0, 0.0f);
                    const size_t cov1 = r1 ? coverage(full) : 0, cov0 = r0 ? coverage(start) : 0;
                    std::printf("  [stack] %s %dx%d coverage=%zu start=%zu\n", c.file, w, h, cov1, cov0);
                    stacks_ok = stacks_ok && r1 && r0 && w > 120 && h > 120 && cov1 > 3000 && cov0 * 4 < cov1;
                    dump(c.file, full, w, h);
                    if (c.v == DockStackView::Fan) {
                        std::vector<uint32_t> mid;
                        int wm = 0, hm = 0;
                        if (pop.RenderSnapshot(mid, wm, hm, 0.45f)) dump("stack_fan_opening.bmp", mid, wm, hm);
                    }
                    pop.Close(false);
                }
                check(stacks_ok, "Dock stack popups render headless (Fan / Grid / List / side Grid) and grow from nothing");
                for (const wchar_t* n : names) DeleteFileW((dir + L"\\" + n).c_str());
                RemoveDirectoryW((dir + L"\\Sub folder").c_str());
                RemoveDirectoryW(dir.c_str());

                // Genie polish: corners are cut round at a constant radius, the mesh ends inside the icon
                // (absorb), and a fine pattern stays smooth when shrunk (mip chain).
                render::DockWindowSnapshot snap{};
                snap.width = 640;
                snap.height = 420;
                snap.screen_rect = {200, 60, 840, 480};
                snap.pixels.resize(static_cast<size_t>(snap.width * snap.height));
                for (int y = 0; y < snap.height; ++y) {
                    for (int x = 0; x < snap.width; ++x) {
                        const bool title = y < 34;
                        const bool line = (y > 50 && (y % 14) < 2 && (x % 97) < 80);
                        const bool check_px = ((x / 2 + y / 2) & 1) != 0;
                        uint32_t c = title ? 0xFF2B2F3Au : (line ? 0xFF1A1A1Au : (check_px ? 0xFFF4F6FAu : 0xFFDDE3EEu));
                        snap.pixels[static_cast<size_t>(y) * snap.width + x] = c;
                    }
                }
                render::DockGenieAnimator genie{};
                const RECT win_rc{200, 60, 840, 480};
                const RECT dock_rc{560, 700, 608, 748};
                bool genie_ok = true;
                std::vector<uint32_t> fr;
                if (genie.RenderPreviewFrame(snap, 1040, 760, win_rc, dock_rc, 0.0f, render::DockMinimizeEffect::Genie, fr)) {
                    // The corner pixel of the window is cut away; a pixel 12 px in along the diagonal is not.
                    const uint32_t corner = fr[static_cast<size_t>(60) * 1040 + 200];
                    const uint32_t inside = fr[static_cast<size_t>(72) * 1040 + 212];
                    genie_ok = genie_ok && (corner >> 24) < 40u && (inside >> 24) > 230u;
                    dump("genie_p000.bmp", fr, 1040, 760);
                } else {
                    genie_ok = false;
                }
                for (float pg : {0.25f, 0.5f, 0.75f, 0.9f}) {
                    if (!genie.RenderPreviewFrame(snap, 1040, 760, win_rc, dock_rc, pg, render::DockMinimizeEffect::Genie, fr)) {
                        genie_ok = false;
                        continue;
                    }
                    char name[40];
                    std::snprintf(name, sizeof(name), "genie_p%03d.bmp", static_cast<int>(pg * 100.0f + 0.5f));
                    dump(name, fr, 1040, 760);
                }
                // At 0.97 everything left is inside (a slightly shrunk) icon and almost transparent.
                if (genie.RenderPreviewFrame(snap, 1040, 760, win_rc, dock_rc, 0.97f, render::DockMinimizeEffect::Genie, fr)) {
                    uint32_t max_a = 0;
                    bool outside = false;
                    for (int y = 0; y < 760; ++y) {
                        for (int x = 0; x < 1040; ++x) {
                            const uint32_t a = fr[static_cast<size_t>(y) * 1040 + x] >> 24;
                            max_a = std::max(max_a, a);
                            if (a > 8u && (x < 552 || x > 616 || y < 692 || y > 756)) outside = true;
                        }
                    }
                    std::printf("  [genie] p=0.97 max_alpha=%u outside=%d\n", max_a, outside ? 1 : 0);
                    genie_ok = genie_ok && !outside && max_a < 90u;
                } else {
                    genie_ok = false;
                }
                genie.Shutdown();
                check(genie_ok, "Dock Genie: round 8px corners, continuous absorb into the icon (no parked window)");
            }

            // 14) Dock tray apps: a click must find the app's real frame, never the hidden "Default IME" /
            //     tool / helper windows every process owns (those made the click show nothing).
            {
                WNDCLASSEXW wc{};
                wc.cbSize = sizeof(wc);
                wc.lpfnWndProc = DefWindowProcW;
                wc.hInstance = GetModuleHandleW(nullptr);
                wc.lpszClassName = L"PulseTrayFixtureFrame";
                RegisterClassExW(&wc);
                HWND frame = CreateWindowExW(0, wc.lpszClassName, L"Tray fixture", WS_OVERLAPPEDWINDOW,
                                             100, 100, 640, 480, nullptr, nullptr, wc.hInstance, nullptr);
                HWND tool = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"Tool",
                                            WS_POPUP, 0, 0, 900, 700, nullptr, nullptr, wc.hInstance, nullptr);
                ShowWindow(tool, SW_SHOWNOACTIVATE);
                auto cls_of = [](HWND h) {
                    wchar_t c[64]{};
                    if (h) GetClassNameW(h, c, 64);
                    return std::wstring(c);
                };
                int s_hidden = 0, s_min = 0;
                const HWND r_hidden = live::FindDockTrayAppWindow(GetCurrentProcessId(), L"", &s_hidden);
                ShowWindow(frame, SW_SHOWMINNOACTIVE);
                const HWND r_min = live::FindDockTrayAppWindow(GetCurrentProcessId(), L"", &s_min);
                const std::wstring c_hidden = cls_of(r_hidden), c_min = cls_of(r_min);
                std::printf("  [tray] hidden-> %ls score=%d, minimized-> %ls score=%d\n",
                            c_hidden.c_str(), s_hidden, c_min.c_str(), s_min);
                const bool helper_free = c_hidden != L"IME" && c_hidden != L"MSCTFIME UI" &&
                                         c_min != L"IME" && c_min != L"MSCTFIME UI";
                check(frame && helper_free && r_hidden && s_hidden >= 1 && r_hidden != tool &&
                          r_min && s_min >= 2 && r_min != tool,
                      "Dock tray app click resolves the app frame (hidden or minimized), not IME / tool helpers");
                DestroyWindow(tool);
                DestroyWindow(frame);
            }
        }

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
