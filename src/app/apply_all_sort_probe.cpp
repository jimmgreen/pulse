#include "app_commands.h"
#include "app_navigation.h"
#include "group_wheel_ui.h"
#include "entry_group.h"
#include <cstdio>
#include <filesystem>

using namespace pulse;
namespace {
bool Ready(const std::vector<app::Tab*>& tabs) {
    const auto deadline = GetTickCount64() + 15000;
    while (GetTickCount64() < deadline) {
        bool ready = true;
        for (const auto* tab : tabs)
            ready = ready && !tab->loading && !tab->pending_generation && tab->EntryCount() == 4;
        if (ready) return true;
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        } else Sleep(1);
    }
    return false;
}
bool Fixture(const std::filesystem::path& folder) {
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    if (error) return false;
    const wchar_t* names[] = {L"a-old-dir", L"z-new-dir", L"a-old.mp4", L"z-new.mp4"};
    for (int i = 0; i < 4; ++i) {
        const auto path = folder / names[i];
        if (i < 2 && !CreateDirectoryW(path.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
        HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, i < 2 ? OPEN_EXISTING : CREATE_ALWAYS, i < 2 ? FILE_FLAG_BACKUP_SEMANTICS : FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        const auto clock = app::MakeGroupClock();
        ULARGE_INTEGER stamp{}; stamp.QuadPart = i % 2 ? clock.today : clock.year - 864000000000ULL;
        const FILETIME time{stamp.LowPart, stamp.HighPart};
        const bool ok = SetFileTime(file, nullptr, nullptr, &time) != FALSE;
        CloseHandle(file);
        if (!ok) return false;
    }
    return true;
}
bool Sorted(const app::Tab& tab) {
    const wchar_t* names[] = {L"z-new-dir", L"a-old-dir", L"z-new.mp4", L"a-old.mp4"};
    if (tab.sort_column != ui::SortColumn::Mtime || tab.sort_direction != ui::SortDirection::Desc ||
        tab.EffectiveFolderSort() != app::FolderSortMode::FoldersFirst || tab.EntryCount() != 4) return false;
    for (size_t i = 0; i < 4; ++i) if (tab.EntryAt(i).name != names[i]) return false;
    return true;
}
}

int RunApplyAllSortProbe(AppState& state, const wchar_t* output) {
    wchar_t data[32768]{};
    if (!state.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) return 2;
    const auto base = std::filesystem::path(data).lexically_normal();
    if (!base.is_absolute() || std::filesystem::path(output).lexically_normal().parent_path() != base) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"wb") || !log) return 3;
    struct LogCloser { FILE* file; ~LogCloser() { fclose(file); } } closer{log};
    int failures = 0;
    auto check = [&](bool ok, const char* name) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name); fflush(log); if (!ok) ++failures; return ok; };
    for (const auto* name : {L"source", L"inactive", L"other-window", L"future"})
        if (!check(Fixture(base / name), "create isolated fixture")) return 4;
    auto* source = ActiveTab(state);
    if (!source) return 4;
    StartLoadingPath(state, *source, (base / L"source").wstring());
    auto inactive_layout = app::MakeSingleLayoutTab((base / L"inactive").wstring());
    auto* inactive = inactive_layout->ActiveFolder();
    state.window_tabs.items.push_back(std::move(inactive_layout));
    StartLoadingPath(state, *inactive, inactive->current_path);
    AppState other(state.core);
    other.isolatedTest = true;
    // A hidden native host exercises shared-core registration and worker result
    // routing without requiring a second compositor or successful Present.
    other.hwnd = CreateWindowExW(0, L"STATIC", L"Apply-all regression", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!other.hwnd) return 4;
    state.core.windows.Add(other.hwnd, &other);
    // The service host must forward queued messages to the real Pulse WndProc.
    state.core.windows.Touch(state.hwnd);
    struct OtherCloser { AppState& state; ~OtherCloser() { state.core.windows.Remove(state.hwnd); DestroyWindow(state.hwnd); } } other_closer{other};
    other.window_tabs.items.push_back(app::MakeSingleLayoutTab((base / L"other-window").wstring()));
    other.pane = other.window_tabs.Active()->FocusedPane();
    auto* remote = ActiveTab(other);
    StartLoadingPath(other, *remote, remote->current_path);
    auto virtual_layout = app::MakeSingleLayoutTab(L"pulse:recent");
    auto* virtual_tab = virtual_layout->ActiveFolder();
    virtual_tab->sort_column = ui::SortColumn::Name;
    virtual_tab->sort_direction = ui::SortDirection::Asc;
    virtual_tab->folder_sort = 1;
    state.window_tabs.items.push_back(std::move(virtual_layout));
    const std::vector<app::Tab*> tabs{source, inactive, remote};
    if (!check(Ready(tabs), "initial real worker listings across both window models")) {
        for (const auto* tab : tabs)
            fprintf(log, "path=%ls loading=%d pending=%llu entries=%zu banner=%ls\n", tab->current_path.c_str(),
                tab->loading, static_cast<unsigned long long>(tab->pending_generation), tab->EntryCount(), tab->banner_message.c_str());
        return 4;
    }
    for (int round = 0; round < 4; ++round) {
        state.appPrefs.folder_sort_mode = 0;
        app::SetFolderSortMode(app::FolderSortMode::FoldersFirst);
        const app::GroupBy groups[] = {app::GroupBy::Date, app::GroupBy::Type, app::GroupBy::MediaTier, app::GroupBy::None};
        for (auto* tab : tabs) {
            tab->view_mode = ui::ViewMode::Details;
            tab->group_by = static_cast<int>(groups[round]);
            state.appPrefs.folder_groups.Set(tab->current_path, groups[round]);
            tab->sort_column = ui::SortColumn::Name;
            tab->sort_direction = ui::SortDirection::Asc;
            tab->folder_sort = tab == remote ? 1 : 2;
            state.appPrefs.folder_sorts.Set(tab->current_path, {tab->sort_column, tab->sort_direction});
            state.appPrefs.folder_sorts.SetFolderMode(tab->current_path, tab->folder_sort);
            for (size_t i = 0; i < tab->EntryCount(); ++i)
                if (tab->EntryAt(i).name == L"a-old.mp4") tab->SelectOnly(static_cast<int>(i));
        }
        source->sort_column = ui::SortColumn::Mtime;
        source->sort_direction = ui::SortDirection::Desc;
        source->folder_sort = 0;
        check(round == 0 ? ApplyViewToAllFolders(state, false) :
            ApplySortToAllFolders(state, static_cast<int>(ui::SortColumn::Mtime), true, 0, false),
            round == 0 ? "legacy apply view to all accepted" : "modern apply sort to all accepted");
        if (!check(Ready(tabs), "apply-all refresh worker results completed")) return 4;
        check(Sorted(*source), "current folder date descending with folders first");
        if (groups[round] != app::GroupBy::None) {
            ui::PaneViewModel model;
            app::FillPaneViewModel(model, *state.pane);
            const bool valid = model.groups && !model.groups->empty() &&
                model.groups->front().key == L"folders" && model.groups->front().first == 0 &&
                model.groups->front().count == 2 && !model.groups->front().label.empty();
            check(valid, "real pane model has one labeled directory group before file groups");
            if (valid) {
                source->collapsed_groups[source->current_path].insert(L"folders");
                ++source->group_collapse_rev;
                app::FillPaneViewModel(model, *state.pane);
                check(model.groups && model.groups->front().collapsed && model.groups->front().count == 2,
                      "directory group collapse uses the stable shared key");
                source->collapsed_groups[source->current_path].erase(L"folders");
                ++source->group_collapse_rev;
            }
        }
        check(Sorted(*inactive), "inactive layout receives date descending with folders first");
        check(Sorted(*remote), "shared-core other window receives date descending with folders first");
        for (const auto* tab : tabs)
            check(tab->SelectedCount() == 1 && tab->selected_index >= 0 && tab->EntryAt(tab->selected_index).name == L"a-old.mp4", "selection preserved by name after re-sort");
        check(virtual_tab->sort_column == ui::SortColumn::Name && virtual_tab->sort_direction == ui::SortDirection::Asc &&
            virtual_tab->folder_sort == 1 && virtual_tab->pending_generation == 0, "virtual view unchanged");
        check(!state.appPrefs.folder_sorts.FindFolderMode(inactive->current_path).has_value() &&
            !state.appPrefs.folder_sorts.FindFolderMode(remote->current_path).has_value(), "old Mixed and Last overrides cleared while global already Top");
        RefreshActiveTab(state, RefreshReason::Explicit);
        check(Ready(tabs) && Sorted(*source), "explicit refresh retains applied sort");
    }
    auto future_layout = app::MakeSingleLayoutTab((base / L"future").wstring());
    auto* future = future_layout->ActiveFolder();
    state.window_tabs.items.push_back(std::move(future_layout));
    StartLoadingPath(state, *future, future->current_path);
    check(Ready({future}) && Sorted(*future), "future folder inherits global date descending and folders first");
    return failures ? 5 : 0;
}
