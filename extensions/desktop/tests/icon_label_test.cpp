#include "host/render/desktop_renderer.h"
#include "host/overlays/desktop_overlays.h"
#include <cstdio>
#include <filesystem>

using namespace pulse::desktop;

namespace pulse::desktop::overlays {
struct DesktopBeautifyTestPeer {
    static bool ClickStyle(DesktopOverlayController& overlay, int style) {
        const std::string id = "b-lbl:" + std::to_string(style);
        for (const auto& region : overlay.click_regions_) {
            if (region.action_id != id) continue;
            const RECT bounds = region.bounds;
            overlay.HandleLeftClick((bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2);
            return true;
        }
        return false;
    }
    static void Prepare(DesktopOverlayController& overlay, model::BeautifySettings& settings, int& calls) {
        overlay.active_kind_ = OverlayKind::BeautifyDrawer;
        overlay.card_w_ = 372;
        overlay.card_h_ = 700;
        overlay.card_screen_x_ = 0;
        overlay.card_screen_y_ = 0;
        overlay.beautify_settings_ = settings;
        overlay.callbacks_.on_beautify_changed = [&](const auto& changed) { settings = changed; ++calls; };
    }
};
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    {
        const std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(400, 700)) return 3;
        model::DesktopModelState state;
        state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
        render::LiveTileRuntimeData live;
        render::DesktopInteractionState interaction;
        int callbacks = 0;
        overlays::DesktopOverlayController overlay;
        overlays::DesktopBeautifyTestPeer::Prepare(overlay, state.settings, callbacks);
        for (int style = 0; style < 4; ++style) {
            check(renderer.RenderCompositePng((output / (L"drawer-" + std::to_wstring(style) + L".png")).wstring(),
                state, {}, {}, live, interaction, [&](auto* dc) { overlay.CompositeOnto(dc); }), "render actual beautify controls");
            check(overlays::DesktopBeautifyTestPeer::ClickStyle(overlay, style), "actual click region dispatches style action");
            check(static_cast<int>(state.settings.label_style) == style && callbacks == style + 1,
                "beautify callback updates selected label style");
            model::DesktopModelState restored;
            check(model::DeserializeModelStateJson(model::SerializeModelStateJson(state), restored) &&
                restored.settings.label_style == state.settings.label_style, "selected style survives model serialization");
        }
        overlay.Dismiss();
        for (bool dark : {false, true}) for (const char* surface : {"loose", "group", "apps"}) {
            state = {};
            state.settings.dark_mode = dark;
            state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
            state.settings.organize_enabled = true;
            state.settings.tiles_enabled = true;
            model::DesktopItem item;
            item.name = L"Label fixture.lnk";
            item.path = L"C:\\pulse-label-fixture\\item.lnk";
            item.extension = L".lnk";
            item.loose_pos = {40, 80};
            const std::string which(surface);
            if (which == "group") {
                model::DesktopGroup group;
                group.id = "fixture-group"; group.title = L"Group";
                group.x = 24; group.y = 24; group.cols = 3; group.visible_rows = 1;
                state.groups.push_back(group);
                item.pinned_group_id = group.id;
            } else if (which == "apps") {
                model::DesktopTile tile;
                tile.id = "fixture-apps"; tile.kind = model::TileKind::Apps; tile.title = L"Apps";
                tile.size = {2, 2}; tile.free_placed = true;
                tile.x = 24; tile.y = 24; tile.w = 320; tile.h = 230; tile.live_content = true;
                state.tiles.push_back(tile);
                item.in_apps_tile = true;
            }
            state.items.push_back(item);
            const auto views = model::ComputeGroupViews(state, 0);
            for (int style = 0; style < 4; ++style) {
                state.settings.label_style = static_cast<model::IconLabelStyle>(style);
                for (const char* mode : {"idle", "hover", "other"}) {
                    interaction = {};
                    if (std::string_view(mode) != "idle") interaction.hovered_path = item.path;
                    if (std::string_view(mode) == "hover") interaction.hovered_tile_id = "fixture-apps";
                    if (std::string_view(mode) == "other") interaction.hovered_tile_id = "another-apps";
                    const std::string name = std::string(dark ? "dark-" : "light-") + surface + "-" + std::to_string(style) + "-" + mode + ".png";
                    check(renderer.RenderCompositePng((output / name).wstring(), state, views, {}, live, interaction), "render real icon label surface");
                }
            }
        }
    }
    CoUninitialize();
    std::printf("RESULT: failures=%d\n", failures);
    return failures ? 1 : 0;
}
