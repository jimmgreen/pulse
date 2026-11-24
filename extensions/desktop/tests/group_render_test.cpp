#include "host/render/desktop_renderer.h"
#include "../../../src/common/localization.h"
#include <cstdio>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iterator>
using namespace pulse::desktop;
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 3;
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    int failures = 0;
    const auto check = [&](bool ok, const char* name) { std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); failures += !ok; };
    {
        render::DesktopRenderer renderer;
        if (!renderer.InitializeOffscreen(850, 600)) return 4;
        const std::filesystem::path output(argv[1]); std::filesystem::create_directories(output);
        check(!pulse::l10n::Get(pulse::l10n::StringId::ColumnName).empty(), "catalog column labels loaded");
        for (bool dark : {false, true}) for (int narrow = 0; narrow < 2; ++narrow) for (int mode = 0; mode < 4; ++mode) {
            model::DesktopModelState state;
            state.settings.organize_enabled = true; state.settings.tiles_enabled = false;
            state.settings.icon_size = narrow ? model::DesktopIconSize::Small : model::DesktopIconSize::Medium;
            state.settings.dark_mode = dark; state.settings.wallpaper_preset = model::WallpaperPreset::Bloom;
            for (int i = 0; i < 8; ++i) {
                model::DesktopGroup g;
                g.id = "group" + std::to_string(i); g.title = L"规则组长标题测试 " + std::to_wstring(i + 1);
                g.x = 28; g.y = 26; g.cols = narrow ? 2 : 6; g.visible_rows = 3;
                g.tab_stack_id = "tabs"; g.tab_order = i; g.tab_active = i == 6;
                g.view_mode = static_cast<model::GroupViewMode>(mode); g.material = model::CardMaterial::Solid;
                state.groups.push_back(g);
            }
            for (int i = 0; i < 19; ++i) {
                model::DesktopItem item; item.name = L"文件名称很长需要省略展示 - Report " + std::to_wstring(i) + L".pdf";
                item.path = L"C:\\pulse-isolated-group-render\\file" + std::to_wstring(i) + L".pdf";
                item.extension = L".pdf"; item.size_bytes = 12000 + i * 1500; item.modified_unix_ms = 1794800000000;
                item.pinned_group_id = "group6"; state.items.push_back(item);
            }
            auto views = model::ComputeGroupViews(state, 1794900000000);
            const auto& view = views[6];
            check(!view.frame.empty() && views[0].frame.empty(), "only active stack frame is rendered");
            const auto tabs = render::ComputeGroupTabLayout(view);
            check(std::any_of(tabs.tabs.begin(), tabs.tabs.end(), [](const auto& t) {return t.active && t.group_id == "group6";}), "selected overflow tab remains visible");
            int end = view.frame.x;
            for (const auto& tab : tabs.tabs) {
                check(tab.rect.w >= 56 && tab.rect.x >= end && tab.rect.right() <= view.frame.right() - 68, "tab rects do not overlap reserved actions");
                end = tab.rect.right();
                const int x = tab.rect.x + tab.rect.w / 2, y = tab.rect.y + tab.rect.h / 2;
                check(render::HitTestGroupTab(view, x, y) == tab.group_id &&
                    renderer.HitTestGroupCard(state.groups[6], view, false, 0, true, x, y) == render::GroupHeaderHitZone::Tab,
                    "drawn tab rect and header hit agree");
            }
            if (!tabs.next_group_id.empty()) check(render::HitTestGroupTab(view, tabs.next.x + 8, tabs.next.y + 8) == "group7", "next arrow reaches clipped tab");
            for (size_t i = 0; i < view.visible_item_indices.size(); ++i) {
                const auto rect = model::GroupItemRect(view, i);
                check(!rect.empty() && rect.x >= view.content_rect.x && rect.right() <= view.content_rect.right() && rect.bottom() <= view.content_rect.bottom(), "shared item rect remains inside content");
            }
            render::DesktopInteractionState interaction; interaction.hovered_group_id = "group6";
            interaction.selected_paths.insert(state.items[0].path); interaction.hovered_path = state.items[1].path;
            render::LiveTileRuntimeData live;
            const auto name = std::string(dark ? "dark-" : "light-") + (narrow ? "narrow-" : "wide-") + std::to_string(mode) + ".png";
            check(renderer.RenderCompositePng((output / name).wstring(), state, views, {}, live, interaction), "real offscreen four-mode group render");
            if (!dark && narrow && mode == 3) {
                state.groups[6].collapsed = true;
                auto collapsed = model::ComputeGroupViews(state, 1794900000000);
                auto expanded = model::ComputeGroupViews(state, 1794900000000, nullptr, "group6");
                check(!collapsed[6].expanded && expanded[6].expanded && state.groups[6].collapsed,
                    "hover expansion is temporary and retains persisted collapse");
                const auto row = model::GroupItemRect(expanded[6], 0);
                check(renderer.HitTestGroupCard(state.groups[6], expanded[6], false, 0, true,
                    expanded[6].frame.right() - 4, expanded[6].frame.bottom() - 4) == render::GroupHeaderHitZone::ResizeGrip && !row.empty(),
                    "temporarily expanded group uses content and resize hit geometry");
                check(renderer.RenderCompositePng((output / "hover-expanded.png").wstring(), state, expanded, {}, live, interaction), "real hover expanded card render");
                check(renderer.RenderCompositePng((output / "collapsed.png").wstring(), state, collapsed, {}, live, interaction), "real collapsed card render");
                model::DesktopGroup neighbor; neighbor.id = "neighbor"; neighbor.title = L"Neighbor behind hover card";
                neighbor.x = 28; neighbor.y = 110; neighbor.cols = 3; neighbor.visible_rows = 2;
                neighbor.material = model::CardMaterial::Solid; state.groups.push_back(neighbor);
                expanded = model::ComputeGroupViews(state, 1794900000000, nullptr, "group6");
                const auto first = output / "hover-overlay-first.png", last = output / "hover-overlay-last.png";
                check(renderer.RenderCompositePng(first.wstring(), state, expanded, {}, live, interaction), "hover card overlays later neighbor");
                auto moved = state.groups[6]; state.groups.erase(state.groups.begin() + 6); state.groups.push_back(moved);
                expanded = model::ComputeGroupViews(state, 1794900000000, nullptr, "group6");
                check(renderer.RenderCompositePng(last.wstring(), state, expanded, {}, live, interaction), "hover card overlays earlier neighbor");
                std::ifstream a(first, std::ios::binary), b(last, std::ios::binary);
                const std::string a_bytes((std::istreambuf_iterator<char>(a)), {}), b_bytes((std::istreambuf_iterator<char>(b)), {});
                check(!a_bytes.empty() && a_bytes == b_bytes, "hover overlay pixels are independent of group order");
            }
        }
    }
    CoUninitialize(); return failures ? 1 : 0;
}
