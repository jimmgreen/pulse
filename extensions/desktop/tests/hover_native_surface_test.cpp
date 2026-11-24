#include "host/render/desktop_renderer.h"
#include <cstdio>

using namespace pulse::desktop;

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"hover fixture",
        WS_POPUP, -30000, -30000, 1400, 480, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 3;
    int failures = 0;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeForHwnd(hwnd, 1400, 480, true, false)) return 4;
        renderer.SetSurfaceGeometry(96, {0, 0, 1400, 480}, {-30000, -30000, 1400, 480});
        if (!renderer.NativeAcrylicActive()) { std::puts("[SKIP] native acrylic unavailable"); return 5; }
        model::DesktopModelState state;
        state.settings.organize_enabled = true;
        state.settings.tiles_enabled = true;
        state.settings.default_material = model::CardMaterial::Acrylic;
        render::DesktopInteractionState interaction;
        interaction.transparency_enabled = true;
        interaction.host_material = model::CardMaterial::Acrylic;
        for (int i = 0; i < 3; ++i) {
            model::DesktopTile tile;
            tile.id = "tile" + std::to_string(i); tile.title = L"Fixture";
            tile.kind = model::TileKind::Apps; tile.material = model::CardMaterial::Acrylic;
            tile.size = {2, 2}; tile.free_placed = true;
            tile.x = 30 + i * 460; tile.y = 60; tile.w = 300; tile.h = 240; tile.live_content = true;
            state.tiles.push_back(tile);
        }
        const auto tick = [&](const char* stage, uint32_t expected = 3u) {
            const bool rendered = renderer.Tick(state, {}, {}, {}, interaction);
            const auto count = renderer.NativeAcrylicVisualCount();
            const bool ok = rendered && count == expected;
            std::printf("[%s] %s: rendered=%d native_cards=%u expected=%u updates=%llu\n", ok ? "PASS" : "FAIL",
                stage, rendered, count, expected, static_cast<unsigned long long>(renderer.NativeAcrylicUpdateCount()));
            failures += !ok;
        };
        tick("initial full frame");
        const auto updates = renderer.NativeAcrylicUpdateCount();
        interaction.hovered_tile_id = "tile0";
        renderer.InvalidateRect({10, 40, 340, 280});
        tick("hover first tile only");
        interaction.hovered_tile_id = "tile2";
        renderer.InvalidateRect({10, 40, 340, 280});
        renderer.InvalidateRect({930, 40, 340, 280});
        tick("switch first to third tile");
        interaction.hovered_tile_id.clear();
        renderer.InvalidateRect({930, 40, 340, 280});
        tick("leave third tile");
        const bool stable = renderer.NativeAcrylicUpdateCount() == updates;
        std::printf("[%s] hover does not rebuild unrelated native card geometry\n", stable ? "PASS" : "FAIL");
        failures += !stable;
        state.settings.dark_mode = !state.settings.dark_mode;
        renderer.InvalidateAll();
        tick("opposite theme full frame");
        const auto opposite_updates = renderer.NativeAcrylicUpdateCount();
        interaction.hovered_tile_id = "tile1";
        renderer.InvalidateRect({470, 40, 340, 280});
        tick("opposite theme hover middle tile");
        interaction.hovered_tile_id.clear();
        renderer.InvalidateRect({470, 40, 340, 280});
        tick("opposite theme leave middle tile");
        const bool opposite_stable = renderer.NativeAcrylicUpdateCount() == opposite_updates;
        std::printf("[%s] opposite theme hover retains native geometry\n", opposite_stable ? "PASS" : "FAIL");
        failures += !opposite_stable;
        interaction.transparency_enabled = false;
        renderer.InvalidateAll();
        tick("transparency disabled full frame", 0);
        interaction.hovered_tile_id = "tile0";
        renderer.InvalidateRect({10, 40, 340, 280});
        tick("solid hover remains without native visuals", 0);
        interaction.transparency_enabled = true;
        renderer.InvalidateRect({10, 40, 340, 280});
        tick("transparency reenabled restores all native cards");
    }
    DestroyWindow(hwnd);
    CoUninitialize();
    return failures ? 1 : 0;
}
