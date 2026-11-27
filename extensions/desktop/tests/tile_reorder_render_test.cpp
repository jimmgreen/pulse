#include "host/model/tile_drag_session.h"
#include "host/overlays/desktop_overlays.h"
#include "host/render/desktop_renderer.h"
#include "host/render/widget_painters.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
using namespace pulse::desktop;
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    int failures = 0, frames = 0;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(960,540)) return 3;
        const std::filesystem::path out(argv[1]);
        std::filesystem::create_directories(out);
        for (bool dark : {false,true}) {
            model::DesktopModelState state;
            state.settings.organize_enabled = state.settings.tiles_enabled = true;
            state.settings.dark_mode = dark;
            state.settings.wallpaper_preset = model::WallpaperPreset::System;
            state.settings.default_material = model::CardMaterial::LiquidGlass;
            for (int i = 0; i < 3; ++i) {
                model::DesktopTile t;
                t.id = std::to_string(i); t.title = L"Tile " + std::to_wstring(i + 1);
                t.kind = model::TileKind::Apps; t.size = {2,2};
                t.free_placed = true; t.x = 24 + 216*i; t.y = 44; t.w = t.h = 208;
                state.tiles.push_back(t);
            }
            model::DesktopGroup group;
            group.id = "fixed"; group.title = L"Fixed group";
            group.x = 672; group.y = 44; group.cols = 2; group.visible_rows = 2;
            state.groups.push_back(group);
            const auto views = model::ComputeGroupViews(state,0);
            const std::vector<model::RectI> obstacles{views[0].frame};
            model::TileDragSession drag;
            drag.Begin(state.tiles,0,{0,0,960,540},{},obstacles);
            render::DesktopInteractionState interaction;
            interaction.transparency_enabled = true;
            auto render = [&](const char* label) {
                const auto file = out / ((dark ? std::string("dark-") : "light-") + label + ".png");
                const bool ok = renderer.RenderCompositePng(file.wstring(),state,views,{}, {},interaction);
                ++frames; failures += !ok;
                std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",file.filename().string().c_str());
            };
            render("before");
            drag.Update({340,180},{340,180},100);
            state.tiles = drag.Preview();
            interaction.dragging_tile = true; interaction.dragging_tile_id = "0";
            interaction.drag_tile_preview_rect = drag.Target(); interaction.drag_tile_valid = drag.Valid();
            failures += !drag.Valid() || state.tiles[1].x != 240 || state.tiles[2].x != 456;
            render("ghost-follow");
            drag.Update({672,44},{672,44},200);
            state.tiles = drag.Preview();
            interaction.drag_tile_preview_rect = drag.Target(); interaction.drag_tile_valid = drag.Valid();
            failures += !drag.Valid() || drag.Target().intersects(obstacles[0]);
            render("around-group");
            drag.Cancel(state.tiles);
            interaction.dragging_tile = false; interaction.dragging_tile_id.clear();
            interaction.drag_tile_preview_rect = {};
            render("cancel");
            for (auto& tile : state.tiles) tile.kind = model::TileKind::Clock;
            model::TileDragSession merge;
            merge.Begin(state.tiles,0,{0,0,960,540},{},obstacles);
            auto merge_frame = [&](const char* label, model::PointI proposal, model::PointI pointer,
                                   uint64_t now, model::TileDragPhase phase) {
                merge.Update(proposal,pointer,now);
                state.tiles = merge.Preview();
                interaction.dragging_tile = true;
                interaction.dragging_tile_id = "0";
                interaction.drag_tile_preview_rect = merge.Target();
                interaction.drag_tile_valid = merge.Valid();
                interaction.drag_tile_merge_armed = !merge.StackTarget().empty();
                interaction.stack_drop_target_tile_id = merge.StackTarget();
                failures += merge.Phase()!=phase || state.tiles[1].x!=240 || state.tiles[2].x!=456;
                render(label);
            };
            merge_frame("merge-approach",{100,44},{204,148},1000,model::TileDragPhase::Approach);
            merge_frame("merge-pending",{240,44},{344,148},1100,model::TileDragPhase::StackPending);
            merge_frame("merge-armed",{240,44},{344,148},1700,model::TileDragPhase::StackArmed);
        }
        // Verify reference-faithful Liquid Glass Dock bar: dynamic capsule expansion, raised-cosine
        // fisheye hover wave, magnified hit-test alignment, and 24-app auto-fit with zero dropped icons.
        {
            render::LiquidDockBarState dock;
            dock.BuildDemoItems(true);
            model::BeautifySettings ds{};
            ds.dock_enabled = true;
            ds.dock_icon_size = 48;
            ds.dock_magnification = true;
            ds.dock_max_scale_x100 = 152;
            const model::RectI wa{0, 0, 1280, 720};
            dock.ComputeGeometry(wa, ds, -1.0f, 0.0f, 0.0);
            const float rest_w = dock.pill_rect.right - dock.pill_rect.left;
            const float focus_x = dock.items[2].rest_cx;
            dock.ComputeGeometry(wa, ds, focus_x, 1.0f, 0.0);
            const float mag_w = dock.pill_rect.right - dock.pill_rect.left;
            const int hit = dock.HitTestItem(dock.items[2].draw_cx, dock.items[2].draw_cy);
            const bool dock_mag_ok = (mag_w > rest_w + 10.0f) &&
                                     (dock.items[2].scale > 1.48f) &&
                                     (dock.items[1].scale > 1.10f) &&
                                     (dock.items[2].draw_cy < dock.items[0].draw_cy) &&
                                     (hit == 2);
            std::printf("[%s] dock fisheye widens pill & aligns magnified hit-test\n", dock_mag_ok ? "PASS" : "FAIL");
            failures += !dock_mag_ok;

            // Pinning 24 apps must keep all 24 items inside work_area without dropping any icon.
            render::LiquidDockBarState many;
            for (int i = 0; i < 24; ++i) {
                render::DockBarItem it{};
                it.kind = render::DockBarItemKind::App;
                it.key = L"pin:" + std::to_wstring(i);
                it.title = L"App " + std::to_wstring(i);
                it.pinned = true;
                many.items.push_back(std::move(it));
            }
            many.ComputeGeometry(wa, ds, -1.0f, 0.0f, 0.0);
            const bool fit_ok = (many.items.size() == 24) &&
                                (many.pill_rect.left >= static_cast<float>(wa.x)) &&
                                (many.pill_rect.right <= static_cast<float>(wa.right())) &&
                                (many.effective_base_icon < 48.0f);
            std::printf("[%s] dock auto-fits 24 pinned icons without dropping any item\n", fit_ok ? "PASS" : "FAIL");
            failures += !fit_ok;
        }
        // Render 2x2, 4x2, and 4x4 System Monitor cards with realistic 48 GB / 2.9 GB VRAM / 14C20T telemetry.
        {
            model::DesktopModelState sys_state;
            sys_state.settings.organize_enabled = sys_state.settings.tiles_enabled = true;
            sys_state.settings.dark_mode = true;
            sys_state.settings.wallpaper_preset = model::WallpaperPreset::System;
            sys_state.settings.default_material = model::CardMaterial::LiquidGlass;
            auto make_sys_tile = [](const char* id, int cols, int rows, int x, int y, int w, int h) {
                model::DesktopTile t;
                t.id = id;
                t.title = L"系统";
                t.kind = model::TileKind::System;
                t.size = {cols, rows};
                t.free_placed = true;
                t.x = x; t.y = y; t.w = w; t.h = h;
                return t;
            };
            sys_state.tiles.push_back(make_sys_tile("sys-2x2", 2, 2, 20, 20, 204, 240));
            sys_state.tiles.push_back(make_sys_tile("sys-4x2", 4, 2, 20, 276, 420, 232));
            sys_state.tiles.push_back(make_sys_tile("sys-4x4", 4, 4, 464, 20, 472, 488));

            render::LiveTileRuntimeData live{};
            auto& sy = live.system;
            sy.valid = true;
            sy.cpu_pct = 4;
            sy.cpu_pct_exact = 4.2f;
            sy.physical_cores = 14;
            sy.logical_cores = 20;
            sy.mem_total = static_cast<uint64_t>(47.835 * 1024.0 * 1024.0 * 1024.0);
            sy.mem_used = static_cast<uint64_t>(21.91 * 1024.0 * 1024.0 * 1024.0);
            sy.mem_pct_exact = 45.8f;
            sy.has_vram = true;
            sy.vram_used_gb = 2.30f;
            sy.vram_total_gb = 2.90f;
            sy.vram_pct = 79.6f;
            sy.gpu_name = L"NVIDIA GeForce GTX 1060 3GB";
            sy.disk_used_pct = 93;
            sy.disk_free = 32ull << 30;
            sy.net_down_bps = 21 * 1024;
            sy.net_up_bps = 3 * 1024;
            for (int i = 0; i < 48; ++i) {
                sy.cpu_history.push_back(3.5f + static_cast<float>((i * 7) % 15));
                const float wave = std::sin(static_cast<float>(i) * 0.24f) * 0.08f
                                 + (i >= 28 ? 0.14f : 0.0f);
                sy.mem_history.push_back(45.75f + wave);
                sy.net_history.push_back(12.0f + static_cast<float>((i * 11) % 60));
            }
            {
                const auto r48 = render::ComputeMemoryZoomRange(sy.mem_history, 47.8f);
                const float span48 = r48.max_val - r48.min_val;
                if (!(r48.min_val >= 45.0f && r48.max_val <= 46.5f && span48 >= 0.24f && span48 <= 0.60f)) {
                    std::fprintf(stderr, "[FAIL] mem zoom 48GB unexpected range=[%.3f, %.3f]\n", r48.min_val, r48.max_val);
                    ++failures;
                }
                const float lo_edge[] = {0.02f, 0.05f, 0.03f};
                const auto rlo = render::ComputeMemoryZoomRange(lo_edge, 48.0f);
                if (!(rlo.min_val == 0.0f && rlo.max_val > 0.20f)) {
                    std::fprintf(stderr, "[FAIL] mem zoom low clamp unexpected range=[%.3f, %.3f]\n", rlo.min_val, rlo.max_val);
                    ++failures;
                }
                const float hi_edge[] = {99.95f, 99.98f, 99.99f};
                const auto rhi = render::ComputeMemoryZoomRange(hi_edge, 16.0f);
                if (!(rhi.max_val == 100.0f && rhi.min_val < 99.5f)) {
                    std::fprintf(stderr, "[FAIL] mem zoom high clamp unexpected range=[%.3f, %.3f]\n", rhi.min_val, rhi.max_val);
                    ++failures;
                }
                const auto lbl = render::FormatMemoryZoomLabel(r48);
                if (lbl.find(L'–') == std::wstring::npos || lbl.back() != L'%') {
                    std::fprintf(stderr, "[FAIL] mem zoom label format unexpected\n");
                    ++failures;
                }
            }
            render::DesktopInteractionState inter;
            inter.transparency_enabled = true;
            const auto file = out / "dark-system-cards.png";
            const bool ok = renderer.RenderCompositePng(file.wstring(), sys_state, {}, {}, live, inter);
            ++frames;
            failures += !ok;
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
        }
        // Regression 1: Double-clicking desktop sets icons_hidden=true -> CollectOccupiedRects MUST still
        // cover the entire Dock (left, center, right) so DComp VirtualSurface Trim never slices the Dock.
        {
            model::DesktopModelState hidden_state;
            hidden_state.settings.organize_enabled = true;
            hidden_state.settings.tiles_enabled = false;
            hidden_state.settings.icons_hidden = true;
            hidden_state.settings.dock_enabled = true;
            hidden_state.settings.dark_mode = true;
            hidden_state.settings.default_material = model::CardMaterial::LiquidGlass;
            hidden_state.monitors.push_back(model::MonitorWorkArea{L"Primary", {0, 0, 960, 540}, {0, 0, 960, 540}, 96, true});

            render::LiveTileRuntimeData live{};
            live.dock.BuildDemoItems(true);
            live.dock.ComputeGeometry({0, 0, 960, 540}, hidden_state.settings, -1.0f, 0.0f, 0.0);

            render::DesktopInteractionState inter;
            inter.transparency_enabled = true;
            inter.dock_bar_reveal = 1.0f;

            const auto occupied = renderer.CollectOccupiedRects(hidden_state, {}, {}, live, inter);
            auto covered = [&](float x_dip, float y_dip) {
                const LONG px = static_cast<LONG>(std::lround(x_dip));
                const LONG py = static_cast<LONG>(std::lround(y_dip));
                for (const RECT& r : occupied) {
                    if (px >= r.left && px < r.right && py >= r.top && py < r.bottom) return true;
                }
                return false;
            };
            const float cy = (live.dock.pill_rect.top + live.dock.pill_rect.bottom) * 0.5f;
            const bool dock_covered = live.dock.pill_rect.right > live.dock.pill_rect.left &&
                                      covered(live.dock.pill_rect.left + 8.0f, cy) &&
                                      covered((live.dock.pill_rect.left + live.dock.pill_rect.right) * 0.5f, cy) &&
                                      covered(live.dock.pill_rect.right - 8.0f, cy);
            std::printf("[%s] icons_hidden full-frame occupied rects cover entire Dock (left/mid/right)\n",
                        dock_covered ? "PASS" : "FAIL");
            failures += !dock_covered;

            const auto file = out / "icons-hidden-dock.png";
            const bool ok = renderer.RenderCompositePng(file.wstring(), hidden_state, {}, {}, live, inter);
            ++frames;
            failures += !ok;
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
            const model::RectI btn = renderer.LastBannerActionRect();
            const bool banner_clear_of_dock = !btn.empty() &&
                                              static_cast<float>(btn.bottom()) <= live.dock.pill_rect.top;
            std::printf("[%s] icons_hidden banner lifts cleanly above Dock without overlapping icons\n",
                        banner_clear_of_dock ? "PASS" : "FAIL");
            failures += !banner_clear_of_dock;
        }
        // Regression 2: Edit-mode peak jiggle (edit_phase=0.25 -> max +/-0.45 deg) with LiquidGlass tiles
        // on right half of screen in light theme must keep glass card, rim, shadow, and content locked together
        // with zero vertical detachment, and edit toolbar readable above top-placed tiles.
        {
            model::DesktopModelState edit_state;
            edit_state.settings.organize_enabled = edit_state.settings.tiles_enabled = true;
            edit_state.settings.dark_mode = false;
            edit_state.settings.wallpaper_preset = model::WallpaperPreset::System;
            edit_state.settings.default_material = model::CardMaterial::LiquidGlass;
            edit_state.monitors.push_back(model::MonitorWorkArea{L"Primary", {0, 0, 960, 540}, {0, 0, 960, 540}, 96, true});

            auto mk = [](const char* id, model::TileKind k, const wchar_t* title, int cols, int rows, int x, int y, int w, int h) {
                model::DesktopTile t;
                t.id = id; t.kind = k; t.title = title; t.size = {cols, rows};
                t.free_placed = true; t.x = x; t.y = y; t.w = w; t.h = h;
                return t;
            };
            edit_state.tiles.push_back(mk("e1", model::TileKind::Clock, L"时钟", 1, 1, 836, 20, 100, 100));
            edit_state.tiles.push_back(mk("e2", model::TileKind::DiskRing, L"C:", 1, 1, 836, 132, 100, 100));
            edit_state.tiles.push_back(mk("e3", model::TileKind::Todo, L"待办", 2, 2, 616, 20, 208, 212));
            edit_state.tiles.push_back(mk("e4", model::TileKind::System, L"系统", 4, 2, 224, 250, 420, 228));
            edit_state.tiles.push_back(mk("e5", model::TileKind::Calendar, L"日历", 2, 2, 656, 250, 208, 228));

            render::LiveTileRuntimeData live{};
            live.system.valid = true;
            live.system.cpu_pct = 9;
            live.system.physical_cores = 14;
            live.system.logical_cores = 20;
            live.system.mem_total = static_cast<uint64_t>(47.835 * 1024.0 * 1024.0 * 1024.0);
            live.system.mem_used = static_cast<uint64_t>(20.1 * 1024.0 * 1024.0 * 1024.0);
            live.system.mem_pct_exact = 42.1f;
            live.system.has_vram = true;
            live.system.vram_used_gb = 2.2f;
            live.system.vram_total_gb = 2.9f;
            live.system.vram_pct = 75.9f;
            for (int i = 0; i < 48; ++i) {
                live.system.cpu_history.push_back(6.0f + static_cast<float>((i * 5) % 12));
                live.system.mem_history.push_back(42.1f);
            }
            const model::RectI board_rc = render::DesktopRenderer::ComputeBoardRectFallback({0, 0, 960, 540}, true);
            render::DesktopInteractionState inter;
            inter.transparency_enabled = true;
            inter.tile_edit_mode = true;
            inter.edit_phase = 0.25f;  // peak +/-0.45 deg rotation
            inter.hovered_tile_id = "e5";

            const auto file = out / "edit-jiggle-tiles.png";
            const bool ok = renderer.RenderCompositePng(file.wstring(), edit_state, {}, board_rc, live, inter);
            ++frames;
            failures += !ok;
            std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
        }
        // Regression 3: Left & Right vertical Dock bars with vertical fisheye magnification, horizontal divider,
        // outward running dots, and side tooltips + Widget Craft Showcase (Weather sky wash, Apple Calendar 1x1,
        // Apple Notes ribbon+rules, Reminders Todo ring+pill, World Clock day/night dials, DiskRing, 1x1 aura tiles).
        {
            for (int pos : {1, 2}) {
                model::DesktopModelState dst;
                dst.settings.organize_enabled = dst.settings.tiles_enabled = true;
                dst.settings.dock_enabled = true;
                dst.settings.dock_position = static_cast<uint8_t>(pos);
                dst.settings.dock_icon_size = 44;
                dst.settings.dock_magnification = true;
                dst.settings.dock_max_scale_x100 = 152;
                dst.settings.dark_mode = true;
                dst.settings.default_material = model::CardMaterial::LiquidGlass;
                dst.monitors.push_back(model::MonitorWorkArea{L"Primary", {0, 0, 960, 540}, {0, 0, 960, 540}, 96, true});
                dst.todos = {
                    {L"核对季度预算表", true},
                    {L"更新桌面小组件设计稿", false},
                    {L"发送测试版本给团队", false},
                };

                auto mk = [](const char* id, model::TileKind k, const wchar_t* title, int cols, int rows, int x, int y, int w, int h) {
                    model::DesktopTile t;
                    t.id = id; t.kind = k; t.title = title; t.size = {cols, rows};
                    t.free_placed = true; t.x = x; t.y = y; t.w = w; t.h = h;
                    return t;
                };
                if (pos == 1) {
                    // Left Dock + Full Widget Craft Showcase on right side!
                    dst.tiles.push_back(mk("w1", model::TileKind::Weather, L"天气", 4, 2, 112, 18, 412, 206));
                    dst.tiles.push_back(mk("w2", model::TileKind::StickyNote, L"便笺", 2, 2, 536, 18, 198, 206));
                    dst.tiles.back().note_body = L"周五 16:00 评审\n· 核对预算表\n· 发送 1.0.34 包";
                    dst.tiles.push_back(mk("w3", model::TileKind::Todo, L"待办", 2, 2, 746, 18, 198, 206));
                    dst.tiles.push_back(mk("w4", model::TileKind::Clock, L"时钟", 4, 1, 112, 236, 412, 98));
                    dst.tiles.push_back(mk("w5", model::TileKind::StagingTray, L"暂存区", 4, 1, 112, 346, 412, 98));
                    dst.tiles.push_back(mk("w6", model::TileKind::Calendar, L"日历", 1, 1, 536, 236, 94, 98));
                    dst.tiles.push_back(mk("w7", model::TileKind::DiskRing, L"C:", 1, 1, 640, 236, 94, 98));
                    dst.tiles.push_back(mk("w8", model::TileKind::PulseLauncher, L"Pulse", 1, 1, 536, 346, 94, 98));
                    dst.tiles.push_back(mk("w9", model::TileKind::RecycleBin, L"回收站", 1, 1, 640, 346, 94, 98));
                    dst.tiles.push_back(mk("w10", model::TileKind::Weather, L"天气", 2, 2, 746, 236, 198, 208));
                }

                render::LiveTileRuntimeData live{};
                live.recycle_bin.item_count = 18;
                live.dock.BuildDemoItems(true);
                live.dock.ComputeGeometry({0, 0, 960, 540}, dst.settings, -1.0f, 0.0f, 0.0);
                const float focus_y = live.dock.items[2].rest_cy;
                live.dock.ComputeGeometry({0, 0, 960, 540}, dst.settings, focus_y, 1.0f, 0.0);
                const int hit_idx = live.dock.HitTestItem(live.dock.items[2].draw_cx, live.dock.items[2].draw_cy);
                const int div_hit = live.dock.HitTestDivider(live.dock.items[5].draw_cx, live.dock.items[5].draw_cy);
                const bool vert_ok = live.dock.IsVertical() &&
                                     live.dock.items[2].scale > 1.45f &&
                                     hit_idx == 2 && div_hit == 5 &&
                                     (pos == 1 ? (live.dock.items[2].draw_cx > live.dock.items[0].draw_cx)
                                               : (live.dock.items[2].draw_cx < live.dock.items[0].draw_cx));
                std::printf("[%s] dock pos=%d vertical fisheye, inward pop, item & divider hit-test\n",
                            vert_ok ? "PASS" : "FAIL", pos);
                failures += !vert_ok;

                render::DesktopInteractionState inter;
                inter.transparency_enabled = true;
                inter.dock_bar_reveal = 1.0f;
                inter.dock_bar_hovered = true;
                inter.dock_bar_tooltip_index = 2;
                inter.dock_bar_tooltip_alpha = 1.0f;

                const auto file = out / (pos == 1 ? "dock-left-widgets-showcase.png" : "dock-right-hover.png");
                const bool ok = renderer.RenderCompositePng(file.wstring(), dst, {}, {}, live, inter);
                ++frames;
                failures += !ok;
                std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
            }
        }
        // Regression 4: New Interactive Widgets (Pomodoro, MediaPlayer, Countdown, Battery) + Curated Glass Modes.
        {
            const bool mode_map_ok =
                (model::GlassModeIndexToPreset(0) == 5 && model::GlassPresetToModeIndex(5) == 0) &&
                (model::GlassModeIndexToPreset(1) == 1 && model::GlassPresetToModeIndex(1) == 1) &&
                (model::GlassModeIndexToPreset(2) == 3 && model::GlassPresetToModeIndex(3) == 2) &&
                (model::GlassModeIndexToPreset(3) == 2 && model::GlassPresetToModeIndex(2) == 3);
            std::printf("[%s] curated 4-mode liquid glass preset mapping\n", mode_map_ok ? "PASS" : "FAIL");
            failures += !mode_map_ok;

            int py = 0, pm = 0, pd = 0;
            const bool iso_ok = live::ParseIsoDate(L"2027-01-01", py, pm, pd) && py == 2027 && pm == 1 && pd == 1 &&
                                !live::ParseIsoDate(L"2027-13-40", py, pm, pd);
            SYSTEMTIME fake_now{};
            fake_now.wYear = 2026; fake_now.wMonth = 11; fake_now.wDay = 26; fake_now.wDayOfWeek = 4;
            fake_now.wHour = 14; fake_now.wMinute = 30;
            const auto cd = live::ComputeCountdown(L"产品发版", L"2027-01-01", fake_now);
            const bool cd_ok = iso_ok && cd.days_delta == 36 && !cd.is_today && !cd.is_past &&
                               cd.day_pct > 55.0f && cd.day_pct < 65.0f && cd.year_pct > 88.0f && cd.year_pct < 92.0f;
            std::printf("[%s] countdown date parsing, delta=36 days, and year/day progress math\n", cd_ok ? "PASS" : "FAIL");
            failures += !cd_ok;

            live::PomodoroState pomo{};
            pomo.SetPreset(live::PomodoroPhase::Focus, 25);
            pomo.remaining_sec = 2;
            pomo.Toggle();
            bool fin = false;
            const bool t1 = pomo.TickOneSecond(fin);
            const bool t2 = pomo.TickOneSecond(fin);
            const bool pomo_ok = !t1 && t2 && fin && pomo.completed_today == 1 &&
                                 pomo.phase == live::PomodoroPhase::ShortBreak && !pomo.running &&
                                 pomo.remaining_sec == 5 * 60;
            std::printf("[%s] pomodoro timer ticks and auto-transitions focus->short break\n", pomo_ok ? "PASS" : "FAIL");
            failures += !pomo_ok;

            const auto bat_live = live::SampleBatteryPower(false);
            const bool bat_ok = bat_live.valid && bat_live.battery_pct >= 0 && bat_live.battery_pct <= 100 &&
                                !bat_live.power_plan.empty();
            std::printf("[%s] live battery & power plan sampling\n", bat_ok ? "PASS" : "FAIL");
            failures += !bat_ok;

            for (bool dark : {true, false}) {
                model::DesktopModelState nst;
                nst.settings.organize_enabled = nst.settings.tiles_enabled = true;
                nst.settings.dark_mode = dark;
                nst.settings.wallpaper_preset = dark ? model::WallpaperPreset::Dusk : model::WallpaperPreset::Bloom;
                renderer.ReloadWallpaper(nst.settings.wallpaper_preset, dark);
                nst.settings.default_material = model::CardMaterial::LiquidGlass;
                nst.settings.glass_preset = dark ? 5 : 1;
                nst.monitors.push_back(model::MonitorWorkArea{L"Primary", {0, 0, 960, 540}, {0, 0, 960, 540}, 96, true});

                auto mk = [](const char* id, model::TileKind k, const wchar_t* title, int cols, int rows, int x, int y, int w, int h) {
                    model::DesktopTile t;
                    t.id = id; t.kind = k; t.title = title; t.size = {cols, rows};
                    t.free_placed = true; t.x = x; t.y = y; t.w = w; t.h = h;
                    return t;
                };
                nst.tiles.push_back(mk("nw-pomo-4x2", model::TileKind::Pomodoro, L"番茄专注", 4, 2, 20, 18, 436, 212));
                nst.tiles.push_back(mk("nw-media-4x2", model::TileKind::MediaPlayer, L"正在播放", 4, 2, 472, 18, 468, 212));
                nst.tiles.push_back(mk("nw-cd-4x2", model::TileKind::Countdown, L"2027 元旦", 4, 2, 20, 246, 436, 212));
                nst.tiles.back().countdown_date = L"2027-01-01";
                nst.tiles.push_back(mk("nw-bat-2x2", model::TileKind::Battery, L"电池设备", 2, 2, 472, 246, 224, 212));
                nst.tiles.push_back(mk("nw-pomo-1x1", model::TileKind::Pomodoro, L"番茄专注", 1, 1, 712, 246, 108, 100));
                nst.tiles.push_back(mk("nw-cd-1x1", model::TileKind::Countdown, L"春节", 1, 1, 832, 246, 108, 100));
                nst.tiles.back().countdown_date = L"2027-02-06";
                nst.tiles.push_back(mk("nw-bat-2x1", model::TileKind::Battery, L"电池设备", 2, 1, 712, 358, 228, 100));

                // Verify inline interactive button hit-testing aligns with 4x2 Pomodoro & MediaPlayer layouts.
                const D2D1_RECT_F pomo_rc = D2D1::RectF(20.0f, 18.0f, 456.0f, 230.0f);
                const D2D1_RECT_F media_rc = D2D1::RectF(472.0f, 18.0f, 940.0f, 230.0f);
                // The play controls of the redesigned faces (widget_faces.cpp): the pomodoro's 开始 pill at the
                // bottom left, the media play button in the row under the title, right of the 180-DIP cover.
                const int p_hit = render::DesktopRenderer::HitTestPomodoroButton(nst.tiles[0], pomo_rc, 70.0f, 200.0f);
                const int m_hit = render::DesktopRenderer::HitTestMediaButton(nst.tiles[1], media_rc, 740.0f, 198.0f);
                if (dark) {
                    // 待办: open items first, done ones after; rows, the × and 添加待办 stay inside the card, and a
                    // 4-wide card splits the list into two columns.
                    model::DesktopTile todo2{}; todo2.kind = model::TileKind::Todo; todo2.size = {2, 2};
                    model::DesktopTile todo4 = todo2; todo4.size = {4, 2};
                    const std::vector<model::TodoItem> items{{L"a", true}, {L"b", false}, {L"c", false}, {L"d", true}, {L"e", false}};
                    const auto order = render::DesktopRenderer::TodoDisplayOrder(items);
                    const D2D1_RECT_F c2 = D2D1::RectF(0, 0, 208, 208), c4 = D2D1::RectF(0, 0, 424, 208);
                    const size_t cap2 = render::DesktopRenderer::TodoRowCapacity(todo2, c2);
                    const D2D1_RECT_F last = render::DesktopRenderer::TodoRowRect(todo2, c2, cap2 - 1);
                    const D2D1_RECT_F add = render::DesktopRenderer::TodoAddRect(todo2, c2, std::min(cap2, items.size()));
                    const D2D1_RECT_F x0 = render::DesktopRenderer::TodoDeleteRect(todo2, c2, 0);
                    const D2D1_RECT_F col2 = render::DesktopRenderer::TodoRowRect(todo4, c4, render::DesktopRenderer::TodoRowCapacity(todo4, c4) / 2);
                    const bool todo_ok = order == std::vector<size_t>{1, 2, 4, 0, 3} && cap2 >= 4 && last.bottom <= c2.bottom &&
                                         add.bottom > add.top && add.bottom <= c2.bottom && add.top >= last.top &&
                                         x0.right <= c2.right && x0.left > c2.left + 100 && col2.left > c4.left + 200 &&
                                         render::DesktopRenderer::TodoRowRect(todo2, c2, cap2).right == 0.0f;
                    std::printf("[%s] todo rows, delete and add areas follow the drawn list (cap=%zu)\n", todo_ok ? "PASS" : "FAIL", cap2);
                    failures += !todo_ok;
                    const bool hit_ok = (p_hit == 0) && (m_hit == 1);
                    std::printf("[%s] pomodoro & media inline button hit-test (p=%d m=%d)\n", hit_ok ? "PASS" : "FAIL", p_hit, m_hit);
                    failures += !hit_ok;
                }

                render::LiveTileRuntimeData live{};
                live.pomodoro.running = true;
                live.pomodoro.focus_minutes = 25;
                live.pomodoro.total_sec = 25 * 60;
                live.pomodoro.remaining_sec = 18 * 60 + 24;
                live.pomodoro.completed_today = 3;
                live.media.active = true;
                live.media.playing = true;
                live.media.title = L"夜曲 · Nocturne";
                live.media.artist = L"周杰伦";
                live.media.album = L"十一月的萧邦";
                live.media.source_app = L"网易云音乐";
                live.battery.valid = true;
                live.battery.has_battery = true;
                live.battery.battery_pct = 86;
                live.battery.charging = true;
                live.battery.ac_online = true;
                live.battery.uptime_sec = 4ull * 3600ull + 22ull * 60ull;
                live.battery.power_plan = L"平衡";
                live.battery.bt_devices = {
                    {L"AirPods", 74, 1},
                    {L"MX Master", 52, 2},
                    {L"机械键盘", 91, 3},
                };

                render::DesktopInteractionState inter{};
                inter.transparency_enabled = true;
                const auto file = out / (dark ? "dark-new-widgets-showcase.png" : "light-new-widgets-showcase.png");
                const bool ok = renderer.RenderCompositePng(file.wstring(), nst, {}, {}, live, inter);
                ++frames;
                failures += !ok;
                std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
            }
        }
        // Regression 5: Dock .dk-sys system status & tray area (Bottom + Left/Right vertical + Desktop PC no-battery hide)
        // plus frosted glass Dock System Panel snapshots (TrayOverflow, Volume, Wifi, Battery, Clock) in Dark & Light.
        {
            for (bool dark : {true, false}) {
                model::DesktopModelState dst;
                dst.settings.organize_enabled = dst.settings.tiles_enabled = true;
                dst.settings.dock_enabled = true;
                dst.settings.dock_system_area = true;
                dst.settings.dock_position = 0; // Bottom
                dst.settings.dock_icon_size = 48;
                dst.settings.dock_magnification = true;
                dst.settings.dark_mode = dark;
                dst.settings.wallpaper_preset = dark ? model::WallpaperPreset::Dusk : model::WallpaperPreset::Bloom;
                renderer.ReloadWallpaper(dst.settings.wallpaper_preset, dark);
                dst.settings.default_material = model::CardMaterial::LiquidGlass;
                dst.settings.glass_preset = dark ? 5 : 1;
                dst.monitors.push_back(model::MonitorWorkArea{L"Primary", {0, 0, 960, 540}, {0, 0, 960, 540}, 96, true});

                render::LiveTileRuntimeData live{};
                live.dock.BuildDemoItems(true);
                live.dock.BuildDemoSystemSnapshot(/*has_battery=*/true);
                live.dock.ComputeGeometry({0, 0, 960, 540}, dst.settings, -1.0f, 0.0f, 0.0);

                // Verify all .dk-sys slots laid out inside pill_rect and hit-test cleanly without colliding with app icons
                const auto* vol_slot = live.dock.FindSysSlot(render::DockSysSlotKind::Volume);
                const auto* bat_slot = live.dock.FindSysSlot(render::DockSysSlotKind::Battery);
                const auto* clk_slot = live.dock.FindSysSlot(render::DockSysSlotKind::Clock);
                const auto* tray_slot = live.dock.FindSysSlot(render::DockSysSlotKind::PinnedTrayApp, 0);
                const bool slots_ok = vol_slot && bat_slot && clk_slot && tray_slot &&
                                      live.dock.HitTestSysSlot((vol_slot->rect.left + vol_slot->rect.right) * 0.5f,
                                                               (vol_slot->rect.top + vol_slot->rect.bottom) * 0.5f) == vol_slot &&
                                      live.dock.HitTestItem((vol_slot->rect.left + vol_slot->rect.right) * 0.5f,
                                                            (vol_slot->rect.top + vol_slot->rect.bottom) * 0.5f) == -1 &&
                                      live.dock.sys_capsule_rect.right < live.dock.pill_rect.right;
                std::printf("[%s] dock .dk-sys bottom layout & hit-test (dark=%d)\n", slots_ok ? "PASS" : "FAIL", dark ? 1 : 0);
                failures += !slots_ok;

                if (dark) {
                    // Desktop PC (has_battery=false) must hide the Battery slot completely (design spec).
                    render::LiquidDockBarState desktop_dock;
                    desktop_dock.BuildDemoItems(false);
                    desktop_dock.BuildDemoSystemSnapshot(/*has_battery=*/false);
                    desktop_dock.ComputeGeometry({0, 0, 960, 540}, dst.settings, -1.0f, 0.0f, 0.0);
                    const bool no_bat_ok = (desktop_dock.FindSysSlot(render::DockSysSlotKind::Battery) == nullptr) &&
                                           (desktop_dock.FindSysSlot(render::DockSysSlotKind::Volume) != nullptr) &&
                                           (desktop_dock.FindSysSlot(render::DockSysSlotKind::Clock) != nullptr);
                    std::printf("[%s] dock .dk-sys hides battery slot on desktop PCs without battery\n",
                                no_bat_ok ? "PASS" : "FAIL");
                    failures += !no_bat_ok;
                }

                render::DesktopInteractionState inter{};
                inter.transparency_enabled = true;
                inter.dock_bar_reveal = 1.0f;
                inter.dock_sys_hovered_kind = render::DockSysSlotKind::Volume;
                const auto file = out / (dark ? "dock-sys-bottom-dark.png" : "dock-sys-bottom-light.png");
                const bool ok = renderer.RenderCompositePng(file.wstring(), dst, {}, {}, live, inter);
                ++frames;
                failures += !ok;
                std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());

                // Export frosted glass Dock System Panel snapshots (TrayOverflow, Volume, Battery, Clock)
                overlays::DesktopOverlayController ov;
                if (ov.Initialize({})) {
                    const std::pair<overlays::DockPanelKind, const char*> panels[] = {
                        {overlays::DockPanelKind::TrayOverflow, "tray"},
                        {overlays::DockPanelKind::Volume, "volume"},
                        {overlays::DockPanelKind::Battery, "battery"},
                        {overlays::DockPanelKind::Clock, "clock"},
                        {overlays::DockPanelKind::Wifi, "wifi"},
                    };
                    for (const auto& [pk, name] : panels) {
                        const auto pfile = out / (std::string("dock-panel-") + name + (dark ? "-dark.png" : "-light.png"));
                        const bool pok = ov.SaveDockPanelSnapshot(pfile.wstring().c_str(), pk, live.dock.sys_snap, dark, 1, 20);
                        ++frames;
                        failures += !pok;
                        std::printf("[%s] %s\n", pok ? "PASS" : "FAIL", pfile.filename().string().c_str());
                    }
                    ov.Shutdown();
                }
            }
        }

        // Wired Ethernet desktop PC (.dk-sys hides Battery & BT, draws Monitor+RJ45 Ethernet glyph,
        // loads real Windows .exe icons, and opens authentic Liquid Glass popups over Dusk wallpaper!)
        {
            const std::wstring clean_title = live::SanitizeTrayTitle(L"\u200E \u200F微信\r\n新消息");
            const bool sanitize_ok = (clean_title == L"微信");
            const std::wstring expanded = live::ExpandNotifyIconExecutablePath(
                L"{6D809377-6AF0-444B-8957-A3773F02200E}\\Tencent\\Weixin\\Weixin.exe");
            const bool expand_ok = (expanded.find(L"Program Files") != std::wstring::npos &&
                                    expanded.find(L"Weixin.exe") != std::wstring::npos);
            std::printf("[%s] tray title sanitization & Win11 KnownFolder path expansion\n",
                        (sanitize_ok && expand_ok) ? "PASS" : "FAIL");
            failures += !(sanitize_ok && expand_ok);

            const model::RectI wa{0, 0, 960, 540};
            model::DesktopModelState st{};
            st.settings.dark_mode = false; // Light Windows theme + dark purple wallpaper -> adaptive white Liquid Glass ink!
            st.settings.wallpaper_preset = model::WallpaperPreset::Dusk;
            renderer.ReloadWallpaper(st.settings.wallpaper_preset, true);
            st.settings.default_material = model::CardMaterial::LiquidGlass;
            st.settings.glass_preset = 5;
            st.settings.dock_enabled = true;
            st.settings.dock_position = 0;
            st.settings.dock_icon_size = 48;
            st.settings.dock_system_area = true;
            st.settings.tiles_enabled = false;
            st.settings.icons_hidden = true;
            st.monitors.push_back(model::MonitorWorkArea{L"Primary", wa, wa, 96, true});

            render::LiveTileRuntimeData live{};
            live.dock.BuildDemoItems(true);
            live.dock.BuildDemoSystemSnapshot(/*has_battery=*/false, /*wired_ethernet=*/true);
            // Attach real Windows system executables so we verify real Shell HICON -> D2D rendering in BOTH Dock & TrayOverflow!
            live.dock.sys_snap.tray_apps[0].exe_path = L"C:\\Windows\\explorer.exe";
            live.dock.sys_snap.tray_apps[1].exe_path = L"C:\\Windows\\System32\\notepad.exe";
            live.dock.sys_snap.tray_apps[2].exe_path = L"C:\\Windows\\System32\\calc.exe";
            live.dock.sys_snap.tray_apps[3].exe_path = L"C:\\Windows\\System32\\cmd.exe";
            live.dock.ComputeGeometry(wa, st.settings, -1.0f, 0.0f, 0.0);

            const bool wired_ok =
                live.dock.sys_snap.network_kind == live::DockNetworkKind::Ethernet &&
                live.dock.FindSysSlot(render::DockSysSlotKind::Wifi) != nullptr &&
                live.dock.FindSysSlot(render::DockSysSlotKind::Battery) == nullptr &&
                live.dock.FindSysSlot(render::DockSysSlotKind::Bluetooth) == nullptr;
            std::printf("[%s] wired desktop PC .dk-sys shows Ethernet & hides Battery/BT cleanly\n",
                        wired_ok ? "PASS" : "FAIL");
            failures += !wired_ok;

            render::DesktopInteractionState di{};
            di.transparency_enabled = true;
            di.banner_suppressed = true;
            di.dock_bar_reveal = 1.0f;
            di.dock_sys_hovered_kind = render::DockSysSlotKind::Wifi;

            const auto eth_file = out / L"dock-sys-ethernet-desktop.png";
            const bool eth_ok = renderer.RenderCompositePng(eth_file.wstring(), st, {}, {}, live, di);
            ++frames;
            failures += !eth_ok;
            std::printf("[%s] %s\n", eth_ok ? "PASS" : "FAIL", eth_file.filename().string().c_str());

            // Render real Liquid Glass TrayOverflow & Ethernet Network popups directly over the Dusk Dock scene!
            overlays::DesktopOverlayController ov;
            if (ov.Initialize({})) {
                ov.SetBackdropSource(&renderer.Backdrop(), renderer.DeviceContext());
                ov.SetAppearance(st.settings.dark_mode, 0xFF4CC2FFu);
                ov.SetBeautifySettings(st.settings);
                ov.SetDockPanelLightInk(true);

                const RECT wa_rc{0, 0, wa.w, wa.h};
                const auto* ovf_slot = live.dock.FindSysSlot(render::DockSysSlotKind::TrayOverflow);
                const auto* net_slot = live.dock.FindSysSlot(render::DockSysSlotKind::Wifi);
                if (ovf_slot) {
                    const RECT anchor{static_cast<LONG>(ovf_slot->rect.left), static_cast<LONG>(ovf_slot->rect.top),
                                      static_cast<LONG>(ovf_slot->rect.right), static_cast<LONG>(ovf_slot->rect.bottom)};
                    ov.OpenDockSystemPanel(wa_rc, anchor, 0, overlays::DockPanelKind::TrayOverflow,
                                           live.dock.sys_snap, -1, 1, 20, {});
                    di.dock_sys_hovered_kind = render::DockSysSlotKind::None;
                    di.dock_sys_open_kind = render::DockSysSlotKind::TrayOverflow;
                    const auto f_tray = out / L"dock-panel-tray-glass.png";
                    const bool ok_t = renderer.RenderCompositePng(
                        f_tray.wstring(), st, {}, {}, live, di,
                        [&](ID2D1DeviceContext* dc) { ov.CompositeOnto(dc, 0, 0); });
                    ++frames;
                    failures += !ok_t;
                    std::printf("[%s] %s\n", ok_t ? "PASS" : "FAIL", f_tray.filename().string().c_str());
                }
                if (net_slot) {
                    const RECT anchor{static_cast<LONG>(net_slot->rect.left), static_cast<LONG>(net_slot->rect.top),
                                      static_cast<LONG>(net_slot->rect.right), static_cast<LONG>(net_slot->rect.bottom)};
                    ov.OpenDockSystemPanel(wa_rc, anchor, 0, overlays::DockPanelKind::Wifi,
                                           live.dock.sys_snap, -1, 1, 20, {});
                    di.dock_sys_open_kind = render::DockSysSlotKind::Wifi;
                    const auto f_net = out / L"dock-panel-ethernet-glass.png";
                    const bool ok_n = renderer.RenderCompositePng(
                        f_net.wstring(), st, {}, {}, live, di,
                        [&](ID2D1DeviceContext* dc) { ov.CompositeOnto(dc, 0, 0); });
                    ++frames;
                    failures += !ok_n;
                    std::printf("[%s] %s\n", ok_n ? "PASS" : "FAIL", f_net.filename().string().c_str());
                }
                ov.Dismiss();
                ov.Shutdown();
            }
        }
    }
    CoUninitialize();
    std::printf("Reorder render: %d frames, %d failures\n",frames,failures);
    return failures ? 1 : 0;
}
