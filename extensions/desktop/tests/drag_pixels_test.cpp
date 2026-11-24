#include "host/render/desktop_renderer.h"
#include "host/model/drag_damage.h"
#include <cstdio>
#include <filesystem>

namespace pulse::desktop::render {
struct DesktopRendererTestAccess {
    static void Partial(DesktopRenderer& renderer, ID2D1DeviceContext* dc, const model::RectI& damage,
                        const model::DesktopModelState& state, const DesktopInteractionState& interaction) {
        // Same 96-DPI padding/grid coverage as InvalidateRect -> TickVirtual's single dirty union.
        const int left = std::max(0, damage.x - 18) / 128 * 128;
        const int top = std::max(0, damage.y - 18) / 128 * 128;
        const int right = std::min(1000, (damage.right() + 18 + 127) / 128 * 128);
        const int bottom = std::min(600, (damage.bottom() + 18 + 127) / 128 * 128);
        renderer.active_clip_dip_ = D2D1::RectF(static_cast<float>(left), static_cast<float>(top),
            static_cast<float>(right), static_cast<float>(bottom));
        dc->PushAxisAlignedClip(renderer.active_clip_dip_, D2D1_ANTIALIAS_MODE_ALIASED);
        dc->Clear(D2D1::ColorF(0, 0.0f));
        auto snap = interaction;
        snap.force_opaque_wallpaper = true;
        renderer.DrawScene(state, {}, {}, {}, snap);
        dc->PopAxisAlignedClip();
        renderer.active_clip_dip_ = D2D1::RectF(0, 0, 1e6f, 1e6f);
    }
};
}

using namespace pulse::desktop;

int wmain(int argc, wchar_t** argv) {
    if (argc != 2 || FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 2;
    std::filesystem::create_directories(argv[1]);
    int failures = 0;
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(1000, 600)) return 3;
        model::DesktopModelState old;
        old.settings.organize_enabled = true;
        old.settings.tiles_enabled = true;
        // The desktop uses the bitmap wallpaper path, not clipped procedural preset gradients.
        old.settings.wallpaper_preset = model::WallpaperPreset::System;
        for (int i = 0; i < 3; ++i) {
            model::DesktopTile t;
            t.id = "tile" + std::to_string(i); t.title = L"Fixture"; t.kind = model::TileKind::Apps;
            t.size = {2, 2}; t.free_placed = true; t.x = 30 + 320 * i; t.y = 60;
            t.w = 280; t.h = 220; t.live_content = true; old.tiles.push_back(t);
        }
        render::DesktopInteractionState initial;
        initial.dragging_tile = true; initial.dragging_tile_id = "tile0";
        initial.drag_tile_preview_rect = model::ResolveTileRect(old.tiles[0], {});
        auto snapshot = [](const auto& state, const auto& inter) {
            model::DragDamageSnapshot s;
            for (const auto& tile : state.tiles) s.frames.push_back(model::ResolveTileRect(tile, {}));
            s.preview = inter.drag_tile_preview_rect; s.stack_target = inter.stack_drop_target_tile_id;
            return s;
        };
        for (bool dark : {false, true}) {
            old.settings.dark_mode = dark;
            for (int scenario = 0; scenario < 3; ++scenario) {
                auto next = old;
                auto moved = initial;
                if (scenario == 0) {
                    next.tiles[0].x += 320;
                    next.tiles[1].y += 250;
                    moved.drag_tile_preview_rect = model::ResolveTileRect(next.tiles[0], {});
                } else if (scenario == 1) {
                    moved.stack_drop_target_tile_id = "tile2";
                    moved.drag_tile_preview_rect = model::ResolveTileRect(next.tiles[2], {});
                } else {
                    next.tiles[0].w += 140; next.tiles[0].h += 120;
                    next.tiles[1].y += 250;
                    moved.resizing_tile_id = "tile0";
                    moved.drag_tile_preview_rect = model::ResolveTileRect(next.tiles[0], {});
                }
                const auto damage = model::ComputeDragDamage(snapshot(old, initial), snapshot(next, moved));
                const auto base = std::filesystem::path(argv[1]) / ((dark ? L"dark-" : L"light-") + std::to_wstring(scenario));
                const bool partial = renderer.RenderCompositePng(base.wstring() + L"-partial.png", old, {}, {}, {}, initial,
                    [&](ID2D1DeviceContext* dc) { render::DesktopRendererTestAccess::Partial(renderer, dc, damage, next, moved); });
                const bool full = renderer.RenderCompositePng(base.wstring() + L"-full.png", next, {}, {}, {}, moved);
                std::printf("[%s] theme=%d scenario=%d partial/full capture\n", partial && full ? "PASS" : "FAIL", dark, scenario);
                failures += !(partial && full);
            }
        }
    }
    CoUninitialize();
    return failures ? 1 : 0;
}
