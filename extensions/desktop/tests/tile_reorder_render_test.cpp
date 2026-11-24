#include "host/model/tile_drag_session.h"
#include "host/render/desktop_renderer.h"
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
            state.settings.default_material = model::CardMaterial::Solid;
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
            interaction.transparency_enabled = false;
            auto render = [&](const char* label) {
                const auto file = out / ((dark ? std::string("dark-") : "light-") + label + ".png");
                const bool ok = renderer.RenderCompositePng(file.wstring(),state,views,{}, {},interaction);
                ++frames; failures += !ok;
                std::printf("[%s] %s\n",ok ? "PASS" : "FAIL",file.filename().string().c_str());
            };
            render("before");
            drag.Update({456,44},{456,44},100);
            state.tiles = drag.Preview();
            interaction.dragging_tile = true; interaction.dragging_tile_id = "0";
            interaction.drag_tile_preview_rect = drag.Target(); interaction.drag_tile_valid = drag.Valid();
            failures += !drag.Valid();
            render("insert");
            drag.Update({672,44},{672,44},200);
            state.tiles = drag.Preview();
            interaction.drag_tile_preview_rect = drag.Target(); interaction.drag_tile_valid = drag.Valid();
            failures += drag.Valid();
            render("blocked");
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
                state.tiles[0].free_placed = true;
                state.tiles[0].x = merge.ProposalRect().x;
                state.tiles[0].y = merge.ProposalRect().y;
                interaction.dragging_tile = true;
                interaction.dragging_tile_id = "0";
                interaction.drag_tile_preview_rect = merge.Target();
                interaction.drag_tile_valid = merge.Valid();
                interaction.stack_drop_target_tile_id = merge.StackTarget();
                failures += merge.Phase()!=phase || state.tiles[1].x!=240 || state.tiles[2].x!=456;
                render(label);
            };
            merge_frame("merge-approach",{100,44},{204,148},1000,model::TileDragPhase::Approach);
            merge_frame("merge-pending",{240,44},{344,148},1100,model::TileDragPhase::StackPending);
            merge_frame("merge-armed",{240,44},{344,148},1700,model::TileDragPhase::StackArmed);
        }
    }
    CoUninitialize();
    std::printf("Reorder render: %d frames, %d failures\n",frames,failures);
    return failures ? 1 : 0;
}
