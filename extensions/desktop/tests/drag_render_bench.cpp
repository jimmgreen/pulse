#include "host/render/desktop_renderer.h"
#include "host/model/drag_damage.h"
#include <algorithm>
#include <cstdio>

using namespace pulse::desktop;

int main() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"STATIC", L"drag fixture",
        WS_POPUP, -30000, -30000, 1920, 1080, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!hwnd) return 3;
    int failures = 0;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeForHwnd(hwnd, 1920, 1080, true, false)) return 4;
        renderer.SetSurfaceGeometry(96, {0, 0, 1920, 1080}, {-30000, -30000, 1920, 1080});
        model::DesktopModelState state;
        state.settings.organize_enabled = true;
        state.settings.tiles_enabled = true;
        render::LiveTileRuntimeData live;
        render::DesktopInteractionState interaction;
        // Solid cards isolate partial-render work even if this OS initializes a native backdrop.
        // This deliberately tests renderer cost, not the host's native-acrylic full-redraw guard.
        interaction.transparency_enabled = false;
        interaction.dragging_tile = true;
        interaction.dragging_tile_id = "tile0";
        for (int i = 0; i < 12; ++i) {
            model::DesktopTile tile;
            tile.id = "tile" + std::to_string(i);
            tile.title = L"Fixture";
            tile.kind = model::TileKind::Apps;
            tile.size = {2, 2}; tile.free_placed = true;
            tile.x = 24 + (i % 4) * 450; tile.y = 40 + (i / 4) * 320;
            tile.w = 300; tile.h = 240; tile.live_content = true;
            state.tiles.push_back(tile);
        }
        auto snapshot = [&] {
            model::DragDamageSnapshot result;
            for (const auto& tile : state.tiles) result.frames.push_back(model::ResolveTileRect(tile, {}));
            result.preview = result.frames[0];
            return result;
        };
        for (bool partial : {false, true}) {
            state.tiles[0].x = 24;
            interaction.drag_tile_preview_rect = snapshot().preview;
            renderer.InvalidateAll();
            if (!renderer.Tick(state, {}, {}, live, interaction)) ++failures;
            std::vector<double> samples;
            for (int i = 0; i < 80; ++i) {
                const auto before = snapshot();
                state.tiles[0].x = 24 + (i % 20) * 4;
                const auto after = snapshot();
                interaction.drag_tile_preview_rect = after.preview;
                if (partial) {
                    const auto damage = model::ComputeDragDamage(before, after);
                    if (damage.empty()) continue;
                    renderer.InvalidateRect(damage);
                } else renderer.InvalidateAll();
                if (!renderer.Tick(state, {}, {}, live, interaction)) ++failures;
                samples.push_back(renderer.Metrics().last_draw_ms);
            }
            std::sort(samples.begin(), samples.end());
            if (renderer.NativeAcrylicVisualCount() != 0) ++failures;
            std::printf("%s LOCAL real DComp solid 12-card drag: frames=%zu p50_ms=%.3f p95_ms=%.3f native=%d visuals=%u\n",
                partial ? "MODIFIED" : "BASELINE", samples.size(), samples[samples.size() / 2],
                samples[samples.size() * 95 / 100], renderer.NativeAcrylicActive() ? 1 : 0,
                renderer.NativeAcrylicVisualCount());
        }
        std::printf("[%s] all live render ticks succeeded; not Win10 acceptance\n", failures ? "FAIL" : "PASS");
    }
    DestroyWindow(hwnd);
    CoUninitialize();
    return failures ? 1 : 0;
}
