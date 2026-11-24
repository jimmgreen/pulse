#include "host/render/desktop_renderer.h"
#include <cstdio>
#include <filesystem>

using namespace pulse::desktop;

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    int failures = 0;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(700, 300)) return 4;
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        model::DesktopModelState state;
        state.settings.organize_enabled = true;
        state.settings.tiles_enabled = true;
        state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        model::DesktopItem item{};
        item.name = L"Hover fixture";
        item.path = L"C:\\pulse-isolated-fixture\\hover-fixture.lnk";
        item.extension = L".lnk";
        item.in_apps_tile = true;
        state.items.push_back(item);
        for (int i = 0; i < 2; ++i) {
            model::DesktopTile tile{};
            tile.id = i == 0 ? "apps_a" : "apps_b";
            tile.kind = model::TileKind::Apps;
            tile.title = L"Apps";
            tile.size = {2, 2};
            tile.free_placed = true;
            tile.x = 24 + i * 340;
            tile.y = 24;
            tile.w = 300;
            tile.h = 240;
            tile.live_content = true;
            state.tiles.push_back(tile);
        }
        render::LiveTileRuntimeData live;
        for (bool dark : {false, true}) {
            state.settings.dark_mode = dark;
            for (const char* scenario : {"idle", "hover_a", "hover_b", "loose", "clear"}) {
                render::DesktopInteractionState interaction;
                const std::string name(scenario);
                if (name == "hover_a" || name == "hover_b" || name == "loose")
                    interaction.hovered_path = item.path;
                if (name == "hover_a") interaction.hovered_tile_id = "apps_a";
                if (name == "hover_b") interaction.hovered_tile_id = "apps_b";
                const auto file = output / ((dark ? "dark-" : "light-") + name + ".png");
                const bool ok = renderer.RenderCompositePng(file.wstring(), state, {}, {}, live, interaction);
                std::printf("[%s] render %s\n", ok ? "PASS" : "FAIL", file.filename().string().c_str());
                failures += !ok;
            }
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
