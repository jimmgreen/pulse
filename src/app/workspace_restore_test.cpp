#ifdef PULSE_WITH_SELFTEST
#include "app_navigation.h"
#include "app_runtime.h"
#include "quick_access.h"
#include <filesystem>
#include <cstdio>

bool RunWorkspaceRestoreTest() {
    using namespace pulse;
    namespace fsys = std::filesystem;
    int failures = 0;
    const auto parent = fsys::absolute(L"bench_data").lexically_normal();
    const auto base = parent / (L"workspace-restore-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fsys::create_directories(parent);
    if (!fsys::create_directory(base)) return false;
    FILE* log = nullptr;
    _wfopen_s(&log, (parent / L"m10004_workspace_test.log").c_str(), L"w");
    auto check = [&](bool ok, const char* text) {
        if (log) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", text); fflush(log); }
        failures += !ok;
    };
    check(log != nullptr, "workspace regression log opened");
    wchar_t previous[32768]{};
    GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous, ARRAYSIZE(previous));
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", base.c_str());
    std::vector<std::wstring> folders;
    for (const auto name : {L"one", L"two", L"three", L"other"}) {
        fsys::create_directory(base / name); folders.push_back((base / name).wstring());
    }
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto state = std::make_unique<AppState>();
        state->isolatedTest = true;
        state->shot.active = true;
        state->appPrefs.persist = state->searchHistory.persist = state->ctxMenuPrefs.persist = false;
        state->hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 960, 600,
            nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        app::Workspace workspace;
        workspace.name = L"Private workspace"; workspace.root = folders[0];
        workspace.layout = static_cast<int>(app::LayoutPreset::Three);
        if (scenario != 4) {
            workspace.pane_paths = {folders[0], folders[1], folders[2]};
            if (scenario < 3) workspace.pane_paths[scenario].clear();
            workspace.pane_views = {ui::ViewMode::Details, ui::ViewMode::List, ui::ViewMode::LargeIcons};
        }
        state->places.workspaces = {workspace};
        check(state->places.Save(), "save workspace to private profile");
        state->places.workspaces.clear();
        check(state->places.Load() && state->places.workspaces.size() == 1,
            "reload saved pane paths and view modes");
        // An unrelated tab at the same root must not be overwritten.
        state->window_tabs.NewTab(folders[0]);
        state->window_tabs.NewTab(folders[0]);
        state->window_tabs.CloseTab(1);
        BindCurrentLayout(*state);
        OpenWorkspace(*state, 0);
        std::vector<app::Pane*> visible;
        if (Root(*state)) Root(*state)->CollectPanes(visible);
        bool correct = visible.size() == 3;
        for (size_t i = 0; correct && i < visible.size(); ++i) {
            const auto* tab = visible[i]->ActiveTab();
            const auto expected = scenario == 4 ? workspace.root : workspace.pane_paths[i];
            correct = tab && tab->current_path == fs::NormalizePath(expected) &&
                (scenario == 4 || tab->view_mode == workspace.pane_views[i]);
        }
        check(correct, scenario == 4 ? "legacy absent pane_paths keeps root fallback" :
            "actual visible panes restore This PC identity, saved order and views");
        check(state->window_tabs.items.size() == 2, "reopening workspace preserves the unrelated tab");
        check(state->window_tabs.items[0]->layout == app::LayoutPreset::Single &&
            state->window_tabs.items[0]->workspace_id.empty(), "same-root unrelated tab is left untouched");
        const auto first_id = state->places.workspaces[0].id;
        OpenWorkspace(*state, 0);
        check(state->window_tabs.items.size() == 2 && LiveLayout(*state).workspace_id == first_id,
            "opening an already-open saved identity focuses its tab");
        const int second = state->places.PinWorkspace(folders[0], L"Second workspace",
            static_cast<int>(app::LayoutPreset::FourGrid), folders);
        OpenWorkspace(*state, second);
        check(state->window_tabs.items.size() == 3 && LiveLayout(*state).workspace_id != first_id &&
            LiveLayout(*state).VisiblePanes().size() == 4,
            "same-root saved layouts reopen independently with their own pane counts");
        const auto second_id = LiveLayout(*state).workspace_id;
        state->window_tabs.CloseTab(state->window_tabs.active);
        BindCurrentLayout(*state);
        OpenWorkspace(*state, second);
        check(state->window_tabs.items.size() == 3 && LiveLayout(*state).workspace_id == second_id &&
            CollectPanePaths(*state) == std::vector<std::wstring>{fs::NormalizePath(folders[0]), fs::NormalizePath(folders[1]),
                fs::NormalizePath(folders[2]), fs::NormalizePath(folders[3])},
            "closed saved layout reopens with all four original directories");
        const auto group_rows = [&] {
            const auto model = app::BuildWindowViewModel(*state->pane, app::SidebarModel{},
                true, false, true, &state->places, 0);
            for (const auto& section : model.sidebar)
                if (section.id == static_cast<int>(app::SidebarSectionId::Workspaces)) return section.items;
            return std::vector<ui::SidebarItem>{};
        };
        const auto default_rows = group_rows();
        check(default_rows.size() == 2 && default_rows[0].expandable && !default_rows[0].expanded &&
            default_rows[1].expandable && !default_rows[1].expanded && default_rows[0].indent == 0,
            "ungrouped sidebar initially shows only the two saved workspace entries");
        state->places.SetWorkspaceGroup(0, L"Projects");
        state->places.SetWorkspaceGroup(second, L"Projects");
        const auto expanded = group_rows();
        check(expanded.size() == 3 && expanded[0].expandable && expanded[0].expanded &&
            expanded[0].label == L"Projects" && expanded[1].indent == 1,
            "saved layouts appear under an expandable named sidebar group");
        check(expanded.size() == 3 && expanded[1].expandable && !expanded[1].expanded &&
            expanded[2].expandable && !expanded[2].expanded,
            "saved workspaces default collapsed and hide their directory children");
        check(ToggleWorkspaceDirectories(*state, app::MakeWorkspacePath(second)),
            "workspace arrow expands its saved directories");
        const auto second_expanded = group_rows();
        check(second_expanded.size() == 7 && !second_expanded[1].expanded && second_expanded[2].expanded &&
            second_expanded[3].path == folders[0] && second_expanded[4].path == folders[1] &&
            second_expanded[5].path == folders[2] && second_expanded[6].path == folders[3] &&
            second_expanded[3].indent == 2 && state->window_tabs.items.size() == 3 &&
            LiveLayout(*state).workspace_id == second_id,
            "expanding one same-root workspace shows only its own directories without navigation");
        OpenWorkspace(*state, 0);
        check(LiveLayout(*state).workspace_id == first_id &&
            state->places.expanded_workspace_ids.contains(second_id) &&
            !state->places.expanded_workspace_ids.contains(first_id),
            "opening a workspace label preserves independent directory expansion state");
        OpenWorkspace(*state, second);
        check(ToggleWorkspaceGroup(*state, L"pulse:workspace-group:Projects"),
            "sidebar group action recognizes the group path");
        const auto collapsed = group_rows();
        check(collapsed.size() == 1 && collapsed[0].expandable && !collapsed[0].expanded,
            "collapsed sidebar group hides its saved layouts and directories");
        ToggleWorkspaceGroup(*state, L"pulse:workspace-group:Projects");
        check(group_rows().size() == second_expanded.size() && state->window_tabs.items.size() == 3 &&
            LiveLayout(*state).workspace_id == second_id,
            "expanding the group restores each workspace expansion state without navigation");
        ToggleWorkspaceDirectories(*state, app::MakeWorkspacePath(second));
        check(group_rows().size() == expanded.size() && state->places.expanded_workspace_ids.empty(),
            "workspace arrow collapses its directory children again");
        state->watches.Stop(); state->worker.Stop();
        DestroyWindow(state->hwnd); state->hwnd = nullptr;
    }
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", previous[0] ? previous : nullptr);
    std::error_code error;
    if (base.parent_path() == parent && base.filename().wstring().starts_with(L"workspace-restore-"))
        fsys::remove_all(base, error);
    check(!error && !fsys::exists(base), "owned workspace profile and folders removed");
    if (log) fclose(log);
    return failures == 0;
}
#endif
