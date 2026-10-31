#include "app_internal.h"
#include "app_input.h"
#include "app_hosted_edit.h"
#include "workspace_sidebar.h"
#include "quick_access.h"
#include "pane_close.h"
#include "../ui/ui_renderer_internal.h"
#include <filesystem>
#include <fstream>

int RunWorkspaceUiTest(pulse::AppState& s, const wchar_t* output) {
    using namespace pulse;
    using H = ui::HitTestResult;
    std::ofstream log{std::filesystem::path(output)};
    int failures = 0;
    auto check = [&](bool ok, const char* what) { log << (ok ? "[PASS] " : "[FAIL] ") << what << '\n'; log.flush(); if (!ok) ++failures; };
    if (!s.isolatedTest || s.places.persist) return 2;
    const auto base = std::filesystem::path(output).parent_path();
    const auto old_language = l10n::preference();
    const auto original_scale = s.scale;
    const int old_width = s.compositor.Width(), old_height = s.compositor.Height();
    for (const auto* name : {L"src", L"build", L"design", L"export"}) {
        std::filesystem::create_directory(base / name);
        std::ofstream(base / name / L"readme.txt") << "isolated workspace fixture";
    }
    s.places.workspaces.clear();
    s.places.active_workspace = -1;
    for (int i = 0; i < 4; ++i) {
        app::Workspace w;
        w.name = i == 0 ? L"Pulse 开发" : i == 1 ? L"素材整理" : i == 2 ? L"安装包发布" : L"文档分拣";
        w.root = (base / (i == 0 ? L"src" : i == 1 ? L"design" : i == 2 ? L"export" : L"build")).wstring();
        w.layout = i + 1;
        w.color_rgb = i == 0 ? 0x4CC2FF : i == 1 ? 0xA78BFA : i == 2 ? 0xFBBF24 : 0x34D399;
        const int count = i < 2 ? 2 : i + 1;
        for (int j = 0; j < count; ++j) {
            w.pane_paths.push_back((base / (j == 0 ? L"src" : j == 1 ? L"build" : j == 2 ? L"design" : L"export")).wstring());
            w.pane_views.push_back(ui::ViewMode::Details);
        }
        s.places.workspaces.push_back(std::move(w));
    }
    s.sidebarCollapsedMask = 0;
    s.sidebarHiddenMask = 0;
    s.renderer.SetSidebarCollapsed(false, false);
    OpenWorkspace(s, 0);
    check(CollectPanePaths(s).size() == 2, "workspace restores both panes");
    const auto before_tabs = s.window_tabs.items.size();
    OpenWorkspaceNewTab(s, 0);
    check(s.window_tabs.items.size() == before_tabs + 1 && CollectPanePaths(s).size() == 2, "new tab preserves full workspace");
    MoveWorkspace(s, 0, 2);
    check(s.places.active_workspace == 2 && s.places.workspaces[2].name == L"Pulse 开发", "reorder keeps active workspace identity");
    MoveWorkspace(s, 2, 0);
    RemoveWorkspaceWithUndo(s, 1);
    UndoWorkspaceRemoval(s);
    check(s.places.workspaces.size() == 4 && s.places.workspaces[1].name == L"素材整理", "remove undo restores original position");
    auto panes = LiveLayout(s).VisiblePanes();
    auto* closed = panes.back();
    check(CloseWorkspacePane(s, closed) && LiveLayout(s).VisiblePanes().size() == 1, "close merges sibling and preserves remaining pane");
    check(!CloseWorkspacePane(s, s.pane), "last pane cannot close");
    auto vm = BuildVm(s, false);
    bool dirty = false;
    for (const auto& group : vm.sidebar) for (const auto& row : group.items) if (row.workspace_active) dirty = row.workspace_dirty;
    check(dirty, "closed pane marks saved workspace changed");
    check(ReopenWorkspacePane(s) && LiveLayout(s).VisiblePanes().size() == 2 && LiveLayout(s).VisiblePanes().back() == closed, "undo restores original pane objects and split");
    UpdateWorkspaceFromCurrent(s, 0);
    vm = BuildVm(s, false); dirty = false;
    for (const auto& group : vm.sidebar) for (const auto& row : group.items) if (row.workspace_active) dirty = row.workspace_dirty;
    check(!dirty, "update clears change indication");

    const auto load_until = GetTickCount64() + 3000;
    bool loading = true;
    while (loading && GetTickCount64() < load_until) {
        MSG message{};
        while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        loading = false;
        for (auto* pane : LiveLayout(s).VisiblePanes()) if (pane->ActiveTab()) loading |= pane->ActiveTab()->loading;
        if (loading) Sleep(10);
    }
    check(!loading, "fixture folders finish loading before visual capture");
    bool geometry = true, hits = true;
    for (const auto* locale : {L"zh-CN", L"zh-TW", L"en-US"}) for (float scale : {1.0f, 1.5f, 2.0f}) {
        l10n::SetLanguage(locale);
        s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        for (float sidebar : {190.0f, 240.0f, 300.0f}) {
            s.renderer.SetSidebarWidthDip(sidebar);
            vm = BuildVm(s, false);
            vm.settings_open = false;
            const float width = 1280 * scale, height = 960 * scale;
            for (auto& group : vm.sidebar) for (auto& row : group.items) if (row.workspace_index == 0) row.workspace_dirty = true;
            const int section = static_cast<int>(app::SidebarSectionId::Workspaces);
            for (const auto& group : vm.sidebar) {
                if (group.id != section) continue;
                for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
                    const auto& row = group.items[static_cast<size_t>(i)];
                    if (row.workspace_index < 0) continue;
                    D2D1_RECT_F r{};
                    geometry &= s.renderer.SidebarRowRect(vm, width, height, section, i, &r);
                    for (int action = 0; action < (row.workspace_dirty ? 4 : 2); ++action) {
                        const auto a = ui::WorkspaceActionRect(r, scale, action);
                        geometry &= a.left >= r.left && a.right <= r.right && a.bottom <= r.bottom;
                        const auto hit = s.renderer.HitTest(vm, D2D1::RectF(0,0,width,height),(a.left+a.right)/2,(a.top+a.bottom)/2);
                        const bool correct = hit.region == H::SidebarWorkspaceAction && hit.sub_index == action && hit.path == row.path;
                        if (!correct) log << "hit mismatch scale=" << scale << " sidebar=" << sidebar << " row=" << i << " action=" << action << " region=" << static_cast<int>(hit.region) << " sub=" << hit.sub_index << " rect=" << r.left << "," << r.top << "," << r.right << "," << r.bottom << '\n';
                        hits &= correct;
                    }
                }
            }
        }
    }
    check(geometry, "workspace control bounds at three DPI scales and sidebar widths");
    check(hits, "workspace actions hit correct row and action in three languages");
    s.renderer.SetSidebarWidthDip(240);
    for (bool dark : {false, true}) for (int state = 0; state < 5; ++state) {
        const float scale = 1.0f;
        l10n::SetLanguage(L"zh-CN"); s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        s.compositor.Resize(1280, 900);
        s.places.workspaces[0].expanded = state == 1;
        vm = BuildVm(s, false); vm.dark = dark; vm.settings_open = false;
        vm.window_effect = ui::WindowEffect::None; vm.background_image.clear(); vm.backdrop_enabled = false;
        vm.wallpaper_look = 0; vm.wallpaper_blur = 0;
        if (state == 4) for (auto& group : vm.sidebar) if (group.id == static_cast<int>(app::SidebarSectionId::Workspaces)) {
            group.items.clear(); group.badge_count = 0;
            ui::SidebarItem empty; empty.workspace_empty = true;
            empty.label = l10n::Get(l10n::StringId::WorkspaceEmptyHint);
            empty.detail = l10n::Get(l10n::StringId::WorkspaceSaveCurrent);
            group.items.push_back(std::move(empty));
        }
        if (state == 3) {
            D2D1_RECT_F r{};
            s.renderer.SidebarRowRect(vm,1280,900,static_cast<int>(app::SidebarSectionId::Workspaces),0,&r);
            const auto h = s.renderer.HitTest(vm,D2D1::RectF(0,0,1280,900),r.left+60,(r.top+r.bottom)/2);
            vm.hover_region = H::SidebarItem; vm.hover_control_index = h.index;
        }
        for (auto& group : vm.sidebar) for (auto& row : group.items) if (row.workspace_index == 0) row.workspace_dirty = state == 2;
        auto* dc = s.compositor.Dc(); dc->BeginDraw();
        s.renderer.Render(vm, D2D1::RectF(0,0,1280,900), ui::MakeTheme(dark,s.accentColor));
        check(SUCCEEDED(dc->EndDraw()), "workspace fixture renders");
        if (state == 3) {
            Sleep(550); dc->BeginDraw(); s.renderer.Render(vm,D2D1::RectF(0,0,1280,900),ui::MakeTheme(dark,s.accentColor));
            check(SUCCEEDED(dc->EndDraw()), "workspace hover preview renders after delay");
        }
        const auto filename = std::wstring(dark ? L"workspace-dark-" : L"workspace-light-") + std::to_wstring(state) + L".png";
        check(s.compositor.SaveSnapshot((base / filename).c_str()), "workspace screenshot captured");
    }
    s.scale = 1.0f;
    RenameWorkspace(s, 1);
    check(!s.workspaceRenameRoot.empty() && (GetWindowLongPtrW(s.hwndWorkspaceRenameEdit, GWL_STYLE) & WS_VISIBLE) != 0, "inactive workspace opens inline editor");
    if (s.hwndWorkspaceRenameEdit) {
        SetWindowTextW(s.hwndWorkspaceRenameEdit,L"Renamed workspace");
        SendMessageW(s.hwndWorkspaceRenameEdit,WM_KEYDOWN,VK_RETURN,0);
        check(s.places.workspaces[1].name == L"Renamed workspace" && s.workspaceRenameRoot.empty(), "Enter commits inline name");
        RenameWorkspace(s,1); SetWindowTextW(s.hwndWorkspaceRenameEdit,L"Cancelled name");
        SendMessageW(s.hwndWorkspaceRenameEdit,WM_KEYDOWN,VK_ESCAPE,0);
        check(s.places.workspaces[1].name == L"Renamed workspace", "Escape cancels inline name");
    }
    const auto old_windows = s.core.windows.Count();
    OpenWorkspaceNewWindow(s,0);
    AppState* extra = nullptr;
    s.core.windows.ForEach([&](HWND, AppState& window) { if (&window != &s) extra = &window; });
    check(s.core.windows.Count() == old_windows + 1 && extra && extra->isolatedTest && CollectPanePaths(*extra).size() == 2,
          "new window restores full workspace in isolated shared catalog");
    if (extra) DestroyWindow(extra->hwnd);
    vm = BuildVm(s,false);
    D2D1_RECT_F drop_row{};
    s.renderer.SidebarRowRect(vm,static_cast<float>(s.compositor.Width()),static_cast<float>(s.compositor.Height()),
        static_cast<int>(app::SidebarSectionId::Workspaces),0,&drop_row);
    const auto drop_button = ui::WorkspaceActionRect(drop_row,1.0f,1);
    const auto effect = ResolveDropTarget(s,{(base/L"export"/L"readme.txt").wstring()},
        POINT{static_cast<LONG>((drop_button.left+drop_button.right)/2),static_cast<LONG>((drop_button.top+drop_button.bottom)/2)},
        MK_CONTROL,DROPEFFECT_COPY,DROPEFFECT_COPY);
    check(effect == DROPEFFECT_COPY && _wcsicmp(fs::NormalizePath(s.dropDestDir).c_str(),fs::NormalizePath(s.places.workspaces[0].root).c_str()) == 0,
        "file drag over workspace action area resolves saved root without copying");
    ClearWorkspaceActions(s);
    l10n::SetLanguage(l10n::LanguageId(old_language));
    s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
    s.compositor.Resize(old_width, old_height);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
