#include "app_internal.h"
#include "app_input.h"
#include "app_hover.h"
#include "app_hosted_edit.h"
#include "workspace_sidebar.h"
#include "quick_access.h"
#include "pane_close.h"
#include "change_time.h"
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
    const auto starred_file = (base / L"src" / L"readme.txt").wstring();
    s.places.ToggleStarred(starred_file, app::PlaceItemKind::File);
    s.places.ToggleStarred((base / L"export").wstring(), app::PlaceItemKind::Folder);
    s.starredExpanded = true;
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
    if (GetEnvironmentVariableW(L"PULSE_TEST_WORKSPACE_STATUS_ONLY", nullptr, 0)) {
        OpenWorkspace(s, 0);
        UpdateWorkspaceFromCurrent(s, 0);
        const auto dirty = [&] {
            const auto vm = BuildVm(s, false);
            for (const auto& group : vm.sidebar) for (const auto& row : group.items)
                if (row.workspace_active) return row.workspace_dirty;
            return true;
        };
        check(!dirty(), "restored workspace layout is clean");
        auto* tab = LiveLayout(s).VisiblePanes().front()->ActiveTab();
        tab->current_path = (base/L"design").wstring();
        check(!dirty(), "browsing another folder does not mark pane layout changed");
        tab->view_mode = ui::ViewMode::LargeIcons;
        check(!dirty(), "view mode does not mark pane layout changed");
        ApplyLayoutPreset(s, app::LayoutPreset::TwoHorizontal);
        check(dirty(), "changing split orientation marks layout changed");
        ApplyLayoutPreset(s, app::LayoutPreset::Single);
        check(dirty(), "changing pane count still marks layout changed");
        OpenWorkspace(s, 0);
        check(!dirty(), "restoring saved split clears layout change");
        s.appPrefs.change_tracking_enabled = true;
        s.appPrefs.change_tracking_days = 7;
        s.changes.enabled = true; s.changes.days = 7;
        s.changes.summary_request = 901;
        const auto path = LiveLayout(s).VisiblePanes().front()->ActiveTab()->current_path;
        s.changes.requested_paths = {path};
        index::ChangeSummary summary;
        summary.path = path; summary.count = 1; summary.counts[1] = 1;
        summary.last_change = app::ChangeNow(); summary.state = index::ChangeState::Available;
        index::ChangeResponse response; response.summaries.push_back(summary);
        ApplyChangeSummaries(s, 901, response);
        ui::HitTestResult hit; hit.region = H::ChangeBadge; hit.pane_index = 0; hit.index = -1;
        UpdatePointerHover(s, hit, POINT{500,250});
        const auto deadline = s.changes.hover_ready_at;
        check(deadline > GetTickCount64() && !s.changes.popover.visible, "change badge retains delayed entry");
        bool stable = true;
        for (int i = 0; i < 14; ++i) {
            Sleep(50);
            UpdatePointerHover(s, hit, POINT{500+i%3,250+i%2});
            stable &= s.changes.hover_ready_at == deadline;
        }
        TickChangeTracking(s);
        check(stable && s.changes.popover.visible, "real dwell opens despite pointer jitter within badge");
        UpdatePointerHover(s, {}, POINT{0,0});
        s.changes.hover_deadline = GetTickCount64()-1;
        TickChangeTracking(s);
        check(!s.changes.popover.visible, "leaving dismisses change card");
        UpdatePointerHover(s, hit, POINT{500,250});
        UpdatePointerHover(s, {}, POINT{0,0});
        check(!s.changes.hover_ready_at && !s.changes.popover.visible, "brief pass still cancels pending card");
        log << "Failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    if (GetEnvironmentVariableW(L"PULSE_TEST_WORKSPACE_RENAME_ONLY", nullptr, 0)) {
        RenameWorkspace(s, 1);
        const HWND edit = s.hwndWorkspaceRenameEdit;
        check(edit && !s.workspaceRenameRoot.empty(), "workspace inline editor opens");
        if (edit) {
            int forwarded = 0;
            const auto probe = +[](HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) -> LRESULT {
                if (msg == WM_CHAR) ++*reinterpret_cast<int*>(data);
                return DefSubclassProc(hwnd, msg, wp, lp);
            };
            RemoveWindowSubclass(edit, WorkspaceRenameEditProc, 1);
            SetWindowSubclass(edit, probe, 99, reinterpret_cast<DWORD_PTR>(&forwarded));
            SetWindowSubclass(edit, WorkspaceRenameEditProc, 1, reinterpret_cast<DWORD_PTR>(&s));
            SendMessageW(edit, WM_CHAR, L'x', 0);
            check(forwarded == 1, "ordinary text reaches native editor");
            auto press = [&](WPARAM key) {
                forwarded = 0;
                MSG message{}; message.hwnd = edit; message.message = WM_KEYDOWN; message.wParam = key;
                message.lParam = 1;
                TranslateMessage(&message);
                DispatchMessageW(&message);
                while (PeekMessageW(&message, edit, WM_CHAR, WM_CHAR, PM_REMOVE)) DispatchMessageW(&message);
                return forwarded == 0;
            };
            SetWindowTextW(edit, L"Committed workspace");
            check(press(VK_RETURN) && s.workspaceRenameRoot.empty() &&
                  s.places.workspaces[1].name == L"Committed workspace",
                  "Enter commits without forwarding a beep character to native editor");
            RenameWorkspace(s, 1);
            SetWindowTextW(edit, L"Cancelled workspace");
            check(press(VK_ESCAPE) && s.workspaceRenameRoot.empty() &&
                  s.places.workspaces[1].name == L"Committed workspace",
                  "Escape cancels without forwarding a beep character to native editor");
            RemoveWindowSubclass(edit, probe, 99);
        }
        if (EnsureMenu(s)) {
            ui::FluentMenuItem action, shift, minimum;
            action.text = l10n::Get(l10n::StringId::SettingsSecArrangeWindows); action.command = 1;
            shift.text = l10n::Get(l10n::StringId::ArrangeHint); shift.heading = true;
            minimum.text = l10n::Get(l10n::StringId::ArrangeSingleHint); minimum.heading = true;
            const std::vector<ui::FluentMenuItem> items{action, shift, minimum};
            bool captured = s.menu->SaveDebugSnapshot((base/L"hint-idle.png").c_str(), items);
            captured &= s.menu->SaveDebugSnapshot((base/L"hint-shift-hover.png").c_str(), items, 1);
            captured &= s.menu->SaveDebugSnapshot((base/L"hint-minimum-hover.png").c_str(), items, 2);
            const auto bytes = [&](const wchar_t* file) {
                std::ifstream input(base/file, std::ios::binary);
                return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
            };
            check(captured && bytes(L"hint-idle.png") == bytes(L"hint-shift-hover.png") &&
                  bytes(L"hint-idle.png") == bytes(L"hint-minimum-hover.png"),
                  "instruction row hover produces identical pixels to idle");
        } else check(false, "instruction menu available");
        log << "Failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    const auto initial_tabs = s.window_tabs.items.size();
    OpenWorkspace(s, 0);
    check(s.window_tabs.items.size() == initial_tabs, "plain workspace open reuses current tab");
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
    bool file_visible = false, folder_visible = false;
    for (const auto& group : vm.sidebar) if (group.id == static_cast<int>(app::SidebarSectionId::Starred)) {
        for (const auto& row : group.items) {
            if (row.path == starred_file) file_visible = row.starred_child && row.indent == 1 && row.icon_glyph == L"\xE8A5";
            if (row.path == (base / L"export").wstring()) folder_visible = row.starred_child && row.icon_glyph == L"\xE8B7";
        }
    }
    check(file_visible && folder_visible, "expanded favorites shows both file and folder with distinct icons");
    bool dirty = false;
    for (const auto& group : vm.sidebar) for (const auto& row : group.items) if (row.workspace_active) dirty = row.workspace_dirty;
    check(dirty, "closed pane marks saved workspace changed");
    check(ReopenWorkspacePane(s) && LiveLayout(s).VisiblePanes().size() == 2 && LiveLayout(s).VisiblePanes().back() == closed, "undo restores original pane objects and split");
    UpdateWorkspaceFromCurrent(s, 0);
    vm = BuildVm(s, false); dirty = false;
    for (const auto& group : vm.sidebar) for (const auto& row : group.items) if (row.workspace_active) dirty = row.workspace_dirty;
    check(!dirty, "update clears change indication");
    const auto original_paths = s.places.workspaces[0].pane_paths;
    const auto original_panes = LiveLayout(s).VisiblePanes();
    s.places.workspaces[0].pane_views[0] = ui::ViewMode::List;
    MoveWorkspacePane(s, 0, 0, 1);
    check(s.places.workspaces[0].pane_paths[1] == original_paths[0] &&
        s.places.workspaces[0].pane_views[1] == ui::ViewMode::List &&
        LiveLayout(s).VisiblePanes()[1] == original_panes[0], "pane reorder moves path view and live pane together");
    MoveWorkspacePane(s, 0, 1, 0);
    UpdateWorkspaceFromCurrent(s, 0);
    s.places.workspaces[0].expanded = true;
    vm = BuildVm(s, false);
    D2D1_RECT_F first_child{}, last_child{};
    const int workspace_section = static_cast<int>(app::SidebarSectionId::Workspaces);
    for (const auto& group : vm.sidebar) for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
        const auto& row = group.items[static_cast<size_t>(i)];
        if (row.workspace_parent_index != 0) continue;
        D2D1_RECT_F bounds{};
        if (!s.renderer.SidebarRowRect(vm, static_cast<float>(s.compositor.Width()),
            static_cast<float>(s.compositor.Height()), workspace_section, i, &bounds)) continue;
        if (row.workspace_child_number == 1) first_child = bounds;
        if (row.workspace_child_number == 2) last_child = bounds;
    }
    const auto child_press = MAKELPARAM(static_cast<int>(first_child.right - 15),
        static_cast<int>((first_child.top + first_child.bottom) / 2));
    HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, child_press);
    check(s.workspacePaneDragParent == 0 && s.workspacePaneDragFrom == 0 && s.pinDragPending,
        "workspace pane right icon arms shared sidebar drag");
    s.pinDragActive = true;
    UpdateWorkspaceDrag(s, static_cast<int>(last_child.bottom - 1));
    check(s.pinDragToIndex == 2 && s.pinGapVisible, "pane drag targets insertion gap within workspace");
    HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0,
        MAKELPARAM(static_cast<int>(last_child.right - 15), static_cast<int>(last_child.bottom - 1)));
    check(s.places.workspaces[0].pane_paths[1] == original_paths[0] && s.workspacePaneDragParent == -1,
        "pane drag release reorders and clears capture state");
    MoveWorkspacePane(s, 0, 1, 0);

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
    UpdateWorkspaceFromCurrent(s, 0);
    s.places.workspaces[0].expanded = true;
    s.sidebarScroll = 0;
    s.sidebarCollapsedMask = 0;
    s.compositor.Resize(1280, 1100);
    s.scale = 1.0f;
    s.compositor.RecreateTextFormats(s.scale);
    s.renderer.SetScale(s.scale);
    s.renderer.SetSidebarWidthDip(240);
    s.renderer.SetSidebarCollapsed(false, false);
    const auto click_paths = CollectPanePaths(s);
    const auto click_views = CollectPaneViews(s);
    const auto click_tabs = s.window_tabs.items.size();
    bool pane_clicks = true;
    for (int target : {1, 0, 1, 0}) {
        vm = BuildVm(s, false);
        D2D1_RECT_F bounds{};
        for (const auto& group : vm.sidebar) for (int i = 0; i < static_cast<int>(group.items.size()); ++i) {
            const auto& row = group.items[static_cast<size_t>(i)];
            if (row.workspace_parent_index == 0 && row.workspace_child_number == target + 1)
                s.renderer.SidebarRowRect(vm, static_cast<float>(s.compositor.Width()),
                    static_cast<float>(s.compositor.Height()), workspace_section, i, &bounds);
        }
        const auto point = MAKELPARAM(static_cast<int>(bounds.left + 65),
            static_cast<int>((bounds.top + bounds.bottom) / 2));
        pane_clicks &= bounds.bottom > bounds.top;
        HandleLButtonDown(&s, s.hwnd, WM_LBUTTONDOWN, MK_LBUTTON, point);
        pane_clicks &= s.workspacePaneDragParent == 0 && s.workspacePaneDragFrom == target;
        HandleLButtonUp(&s, s.hwnd, WM_LBUTTONUP, 0, point);
        pane_clicks &= s.pane == LiveLayout(s).VisiblePanes()[static_cast<size_t>(target)];
        vm = BuildVm(s, false);
        for (const auto& group : vm.sidebar) for (const auto& row : group.items)
            if (row.workspace_active) pane_clicks &= !row.workspace_dirty;
    }
    check(pane_clicks && CollectPanePaths(s) == click_paths && CollectPaneViews(s) == click_views &&
        s.window_tabs.items.size() == click_tabs, "clicking workspace pane children only focuses panes without changing saved layout");
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
                    if (row.workspace_dirty) {
                        const auto drift=ui::WorkspaceDriftRect(r,scale);
                        const auto update=ui::WorkspaceActionRect(r,scale,2);
                        const auto restore=ui::WorkspaceActionRect(r,scale,3);
                        geometry &= std::abs((r.bottom-r.top)-76*scale)<0.1f &&
                            update.top==restore.top && update.bottom==restore.bottom &&
                            update.right<restore.left && update.left>drift.left+9*scale &&
                            update.top>=drift.top && update.bottom<=drift.bottom;
                    }
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
    bool hierarchy = true;
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        std::vector<ui::SidebarSlot> slots;
        ui::LayoutSidebar(vm, D2D1::RectF(0, 0, 240 * scale, 3000 * scale), scale, slots);
        for (const auto& slot : slots) {
            if (slot.group < 0 || slot.item < 0) continue;
            const auto& group = vm.sidebar[static_cast<size_t>(slot.group)];
            const auto& item = group.items[static_cast<size_t>(slot.item)];
            const auto header = std::find_if(slots.begin(), slots.end(), [&](const auto& candidate) {
                return candidate.group == slot.group && candidate.kind == ui::SidebarSlot::Header;
            });
            if (header != slots.end()) hierarchy &= std::abs(slot.rc.left - header->rc.left -
                (item.indent + 1) * 16.0f * scale) < 0.1f;
        }
    }
    check(hierarchy, "sidebar children indent one level below group headers at three DPI scales");
    s.renderer.SetSidebarWidthDip(240);
    for (bool dark : {false, true}) for (int state = 0; state < 7; ++state) {
        const float scale = 1.0f;
        s.scale = scale;
        l10n::SetLanguage(state==6 ? L"en-US" : L"zh-CN"); s.compositor.RecreateTextFormats(scale); s.renderer.SetScale(scale);
        s.renderer.SetSidebarWidthDip(state>=5 ? 190.0f : 240.0f);
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
        if (state == 1 || state == 3) {
            D2D1_RECT_F r{};
            s.renderer.SidebarRowRect(vm,1280,900,static_cast<int>(app::SidebarSectionId::Workspaces),state == 1 ? 1 : 0,&r);
            const auto h = s.renderer.HitTest(vm,D2D1::RectF(0,0,1280,900),r.left+60,(r.top+r.bottom)/2);
            vm.hover_region = H::SidebarItem; vm.hover_control_index = h.index;
        }
        for (auto& group : vm.sidebar) for (auto& row : group.items) if (row.workspace_index == 0) row.workspace_dirty = state == 2 || state>=5;
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
    s.renderer.SetSidebarWidthDip(240);
    s.scale = 1.0f;
    l10n::SetLanguage(L"zh-CN");
    const auto menu_items = BuildWorkspaceMenuItems(s, 0);
    const auto palette = TagColorPalette(s);
    check(menu_items.size() == 10 && menu_items[5].children.empty() &&
        menu_items[5].quick_swatches.size() == (std::min)(palette.size(), size_t{7}) + 1 &&
        menu_items[5].quick_swatches.back().custom_color && menu_items[5].quick_swatches.back().command == 6,
        "workspace colors form one dot strip ending in custom picker");
    const auto pane_menu = BuildWorkspacePaneMenuItems(s, 0, 0);
    check(pane_menu.size() == 5 && !pane_menu[3].enabled && pane_menu[4].enabled,
        "pane context menu exposes navigation and valid reorder commands");
    const auto saved_paths = s.places.workspaces[0].pane_paths;
    s.places.workspaces[0].pane_paths.clear();
    const auto legacy_menu = BuildWorkspacePaneMenuItems(s, 0, 0);
    check(legacy_menu.size() == 5 && !legacy_menu[3].enabled && !legacy_menu[4].enabled,
        "legacy root-only workspace has usable single-pane menu");
    s.places.workspaces[0].pane_paths = saved_paths;
    if (EnsureMenu(s)) {
        check(s.menu->SaveDebugSnapshot((base / L"workspace-menu.png").c_str(), menu_items), "workspace context menu screenshot");
        check(s.menu->SaveDebugSnapshot((base / L"workspace-colors.png").c_str(), {menu_items[5]}), "workspace palette screenshot");
        check(s.menu->SaveDebugSnapshot((base / L"workspace-pane-menu.png").c_str(), pane_menu), "workspace pane context menu screenshot");
    }
    RenameWorkspace(s, 1);
    check(!s.workspaceRenameRoot.empty() && (GetWindowLongPtrW(s.hwndWorkspaceRenameEdit, GWL_STYLE) & WS_VISIBLE) != 0, "inactive workspace opens inline editor");
    if (s.hwndWorkspaceRenameEdit) {
        SetWindowTextW(s.hwndWorkspaceRenameEdit,L"Click committed workspace");
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,0,MAKELPARAM(700,820));
        HandleLButtonUp(&s,s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(700,820));
        check(s.workspaceRenameRoot.empty() && s.places.workspaces[1].name == L"Click committed workspace", "outside click commits workspace rename");
        RenameWorkspace(s,1);
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
    s.places.ToggleStarred(starred_file, app::PlaceItemKind::File);
    vm = BuildVm(s, false);
    bool removed = !s.places.IsStarred(starred_file);
    for (const auto& group : vm.sidebar) if (group.id == static_cast<int>(app::SidebarSectionId::Starred))
        for (const auto& row : group.items) removed &= row.path != starred_file;
    check(removed && std::filesystem::exists(starred_file), "unfavorite removes sidebar file without deleting source");
    D2D1_RECT_F favorite_root{};
    const bool favorite_bounds = s.renderer.SidebarRowRect(vm, static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height()), static_cast<int>(app::SidebarSectionId::Starred), 0, &favorite_root);
    const POINT favorite_point{static_cast<LONG>(favorite_root.left + 55),
        static_cast<LONG>((favorite_root.top + favorite_root.bottom) / 2)};
    const auto favorite_folder = (base / L"design").wstring();
    const auto favorite_effect = favorite_bounds ? DropExecute(s, {starred_file, favorite_folder}, favorite_point,
        0, DROPEFFECT_LINK | DROPEFFECT_COPY, DROPEFFECT_COPY) : DROPEFFECT_NONE;
    check(favorite_effect != DROPEFFECT_NONE && s.places.IsStarred(starred_file) &&
        s.places.IsStarred(favorite_folder) && std::filesystem::exists(starred_file) &&
        std::filesystem::is_directory(favorite_folder), "drop file and folder on sidebar favorites adds references without moving sources");
    ShowStatusFeedback(s, L"Copied test item");
    check(BuildVm(s, false).status.hint_text == L"Copied test item", "routine feedback appears in status bar");
    SendMessageW(s.hwnd, WM_TIMER, kStatusFeedbackTimer, 0);
    check(s.status_feedback.empty(), "status feedback expires without a persistent timer");
    s.appPrefs.change_tracking_enabled = true;
    s.appPrefs.change_tracking_days = 7;
    s.changes.enabled = true;
    s.changes.days = 7;
    s.changes.summary_request = 900;
    const auto change_path = LiveLayout(s).VisiblePanes()[0]->ActiveTab()->current_path;
    s.changes.requested_paths = {change_path};
    index::ChangeSummary summary;
    summary.path = change_path;
    summary.count = 1;
    summary.counts[1] = 1;
    summary.last_change = app::ChangeNow();
    summary.state = index::ChangeState::Available;
    index::ChangeResponse response;
    response.summaries.push_back(summary);
    ApplyChangeSummaries(s, 900, response);
    ui::HitTestResult change_hit;
    change_hit.region = H::ChangeBadge;
    change_hit.pane_index = 0;
    change_hit.index = -1;
    s.hoverRegion = H::ChangeBadge;
    UpdateChangeHover(s, change_hit, POINT{500,250});
    check(s.changes.hover_ready_at > GetTickCount64() && !s.changes.popover.visible,
        "change badge does not open on pointer entry");
    s.changes.hover_ready_at = GetTickCount64() + 1;
    UpdateChangeHover(s, change_hit, POINT{505,250});
    check(s.changes.hover_ready_at <= GetTickCount64() + 1, "pointer movement within badge preserves dwell deadline");
    UpdateChangeHover(s, ui::HitTestResult{}, POINT{0,0});
    check(s.changes.hover_ready_at == 0 && !s.changes.popover.visible, "leaving cancels pending change-card dwell");
    UpdateChangeHover(s, change_hit, POINT{505,250});
    s.changes.hover_ready_at = GetTickCount64() - 1;
    TickChangeTracking(s);
    check(s.changes.popover.visible, "stationary dwell opens change card");
    s.changes.popover.visible = false;
    s.changes.hover_ready_at = 0;
    s.notification_toast.ShowSnack(s.hwnd, ui::SnackIcon::Undo,
        l10n::Get(l10n::StringId::WorkspaceRemoved), L"Test workspace",
        {{l10n::Get(l10n::StringId::Undo), 0, 0}}, L"Ctrl+Z", 6000);
    Sleep(220);
    vm = BuildVm(s, false);
    vm.dark = false;
    vm.window_effect = ui::WindowEffect::None;
    vm.background_image.clear();
    vm.backdrop_enabled = false;
    vm.wallpaper_look = 0;
    vm.wallpaper_blur = 0;
    auto* feedback_dc = s.compositor.Dc();
    feedback_dc->BeginDraw();
    feedback_dc->Clear(D2D1::ColorF(D2D1::ColorF::White));
    const auto feedback_theme = ui::MakeTheme(false, s.accentColor);
    s.renderer.Render(vm, D2D1::RectF(0,0,static_cast<float>(s.compositor.Width()),
        static_cast<float>(s.compositor.Height())), feedback_theme);
    s.notification_toast.Draw(s.compositor, feedback_theme, s.scale, false);
    check(SUCCEEDED(feedback_dc->EndDraw()) &&
        s.compositor.SaveSnapshot((base / L"quiet-undo-capsule.png").c_str()), "important undo capsule renders after bounded entrance");
    ClearWorkspaceActions(s);
    l10n::SetLanguage(l10n::LanguageId(old_language));
    s.compositor.RecreateTextFormats(original_scale); s.renderer.SetScale(original_scale);
    s.compositor.Resize(old_width, old_height);
    log << "failures=" << failures << std::endl;
    return failures ? 1 : 0;
}
