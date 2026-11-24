#include "app_commands.h"
#include "app_navigation.h"
#include "app_runtime.h"
#include "context_menu.h"
#include "entry_group.h"
#include "../ui/ui_renderer.h"
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>

using namespace pulse;
void Render(AppState& state);

int RunSortUpdatesProbe(AppState& state, const wchar_t* output) {
    wchar_t data[32768]{};
    if (!state.isolatedTest || !output ||
        !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", data, ARRAYSIZE(data))) return 2;
    const auto base = std::filesystem::path(data).lexically_normal();
    if (!base.is_absolute() || std::filesystem::path(output).lexically_normal().parent_path() != base) return 2;
    FILE* log = nullptr;
    if (_wfopen_s(&log, output, L"wb") || !log) return 3;
    struct CloseLog { FILE* file; ~CloseLog() { fclose(file); } } close_log{log};
    auto* tab = ActiveTab(state);
    if (!tab) return 3;
    const bool copied_frames = ui::Compositor::FrameCopyEnabled();
    ui::Compositor::SetFrameCopyEnabled(true);
    struct RestoreFrameCopy {
        bool enabled;
        ~RestoreFrameCopy() { ui::Compositor::SetFrameCopyEnabled(enabled); }
    } restore_frame_copy{copied_frames};
    state.appPrefs.persist = false;
    const auto root = base / (L"live-sort-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) return 3;
    int failures = 0, checks = 0;
    const auto dump = [&] {
        fprintf(log, "STATE path=%ls loading=%d pending=%llu count=%zu sort=%d direction=%d group=%d armed=%d\n",
            tab->current_path.c_str(), tab->loading, static_cast<unsigned long long>(tab->pending_generation),
            tab->EntryCount(), static_cast<int>(tab->sort_column), static_cast<int>(tab->sort_direction),
            tab->group_by, state.watches.Armed(root.wstring()));
        for (size_t i = 0; i < tab->EntryCount(); ++i) {
            const auto& entry = tab->EntryAt(i);
            fprintf(log, "ROW %zu name=%ls size=%llu mtime=%08lx%08lx selected=%d\n", i, entry.name.c_str(),
                static_cast<unsigned long long>(entry.size), entry.mtime.dwHighDateTime, entry.mtime.dwLowDateTime,
                tab->IsSelected(static_cast<int>(i)));
        }
        fflush(log);
    };
    const auto check = [&](bool ok, const char* label) {
        ++checks; failures += !ok;
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label);
        if (!ok) dump();
        fflush(log);
        return ok;
    };
    const auto pump = [&] {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { PostQuitMessage(static_cast<int>(message.wParam)); return false; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        return true;
    };
    const auto wait = [&](const std::function<bool()>& predicate) {
        const auto deadline = GetTickCount64() + 15000;
        while (GetTickCount64() < deadline) {
            if (!pump()) return false;
            if (!tab->loading && !tab->pending_generation && predicate()) return true;
            Sleep(5);
        }
        return false;
    };
    const auto write = [&](const wchar_t* name, size_t size, ULONGLONG stamp) {
        std::ofstream file(root / name, std::ios::binary | std::ios::trunc);
        file << std::string(size, 'x'); file.close();
        if (!file) return false;
        HANDLE handle = CreateFileW((root / name).c_str(), FILE_WRITE_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (handle == INVALID_HANDLE_VALUE) return false;
        const FILETIME time{static_cast<DWORD>(stamp), static_cast<DWORD>(stamp >> 32)};
        const bool ok = SetFileTime(handle, nullptr, nullptr, &time) != FALSE;
        CloseHandle(handle);
        return ok;
    };
    const auto names = [&] {
        std::wstring result;
        for (size_t i = 0; i < tab->EntryCount(); ++i) result += tab->EntryAt(i).name + L"|";
        return result;
    };
    const auto ordered = [&] {
        app::ScopedFolderSortMode folder_mode(tab->EffectiveFolderSort());
        app::ScopedEntryGrouping grouping(tab->EffectiveGroup(), tab->current_path);
        for (size_t i = 1; i < tab->EntryCount(); ++i)
            if (app::EntryLess(tab->EntryAt(i), tab->EntryAt(i - 1), tab->sort_column, tab->sort_direction)) return false;
        return true;
    };
    const auto selected = [&](const wchar_t* name) {
        return tab->SelectedCount() == 1 && tab->selected_index >= 0 &&
            tab->EntryAt(tab->selected_index).name == name;
    };
    const auto shot = [&](const wchar_t* name) {
        for (int frame = 0; frame < 12; ++frame) { pump(); Render(state); Sleep(20); }
        check(state.compositor.SaveSnapshot((base / name).c_str()), "save application compositor PNG");
        dump();
    };
    FILETIME current_time{};
    GetSystemTimeAsFileTime(&current_time);
    const ULONGLONG now = (static_cast<ULONGLONG>(current_time.dwHighDateTime) << 32) |
        current_time.dwLowDateTime;
    constexpr ULONGLONG hour = 60ULL * 60 * 10000000;
    const ULONGLONG old_time = now - 3 * hour;
    const ULONGLONG middle_time = now - 2 * hour;
    const ULONGLONG new_time = now - hour;
    if (!check(write(L"alpha.txt", 100, old_time) && write(L"bravo.txt", 200, middle_time) &&
        write(L"charlie.txt", 300, new_time), "create isolated real files")) return 4;
    StartLoadingPath(state, *tab, root.wstring());
    if (!check(wait([&] { return tab->EntryCount() == 3 && state.watches.Armed(root.wstring()); }),
        "real listing completes and production watcher arms")) return 4;
    tab->view_mode = ui::ViewMode::Details;
    DispatchMenuCommand(state, app::CmdGroupNone);
    DispatchMenuCommand(state, app::CmdSortModified);
    DispatchMenuCommand(state, app::CmdSortDescending);
    check(wait([&] { return names() == L"charlie.txt|bravo.txt|alpha.txt|"; }), "application commands set modified descending");
    for (size_t i = 0; i < tab->EntryCount(); ++i)
        if (tab->EntryAt(i).name == L"alpha.txt") tab->SelectOnly(static_cast<int>(i));
    shot(L"sort-mtime-before.png");
    check(write(L"alpha.txt", 400, new_time + 1000000000ULL), "save selected file with newer metadata");
    check(wait([&] { return names() == L"alpha.txt|charlie.txt|bravo.txt|" && tab->EntryAt(0).size == 400; }),
        "production watcher moves modified file to newest position without refresh");
    check(selected(L"alpha.txt"), "selection follows modified file");
    shot(L"sort-mtime-after.png");
    const auto old_snapshot = tab->snapshot;
    RefreshPath(state, root.wstring(), RefreshReason::Background);
    check(wait([&] { return tab->snapshot != old_snapshot && names() == L"alpha.txt|charlie.txt|bravo.txt|"; }),
        "background worker refresh retains new modified sort");
    DispatchMenuCommand(state, app::CmdSortSize);
    DispatchMenuCommand(state, app::CmdSortAscending);
    check(wait([&] { return names() == L"bravo.txt|charlie.txt|alpha.txt|"; }), "application commands set size ascending");
    check(write(L"alpha.txt", 50, new_time + 2000000000ULL), "shrink selected file");
    check(wait([&] { return names() == L"alpha.txt|bravo.txt|charlie.txt|" && tab->EntryAt(0).size == 50; }),
        "watcher moves smaller file to first size position");
    check(write(L"delta.txt", 250, new_time), "create file during active watching");
    check(wait([&] { return names() == L"alpha.txt|bravo.txt|delta.txt|charlie.txt|"; }), "watcher inserts new file in size order");
    check(write(L"replacement.tmp", 500, new_time + 3000000000ULL) &&
        MoveFileExW((root / L"replacement.tmp").c_str(), (root / L"alpha.txt").c_str(), MOVEFILE_REPLACE_EXISTING),
        "perform real atomic replacement");
    check(wait([&] { return names() == L"bravo.txt|delta.txt|charlie.txt|alpha.txt|" && tab->EntryAt(3).size == 500; }),
        "watcher drops temporary entry and sorts atomic replacement");
    check(selected(L"alpha.txt"), "selection remains on atomic replacement target");
    check(MoveFileExW((root / L"alpha.txt").c_str(), (root / L"renamed.txt").c_str(), 0) != FALSE, "rename selected file on disk");
    check(wait([&] { return names() == L"bravo.txt|delta.txt|charlie.txt|renamed.txt|" && selected(L"renamed.txt"); }),
        "production rename event keeps sorting and selection identity");
    shot(L"sort-size-after.png");
    DispatchMenuCommand(state, app::CmdGroupSize);
    check(wait([&] { return tab->group_by == static_cast<int>(app::GroupBy::Size) && ordered(); }), "application command enables size grouping");
    const auto group_before = app::GroupKey(tab->EntryAt(0), app::GroupBy::Size, app::MakeGroupClock());
    check(write(L"bravo.txt", 2 * 1024 * 1024, new_time + 4000000000ULL), "grow file across size group boundary");
    check(wait([&] {
        if (tab->EntryCount() != 4 || !ordered()) return false;
        for (size_t i = 0; i < tab->EntryCount(); ++i)
            if (tab->EntryAt(i).name == L"bravo.txt") return tab->EntryAt(i).size == 2 * 1024 * 1024 &&
                app::GroupKey(tab->EntryAt(i), app::GroupBy::Size, app::MakeGroupClock()) != group_before;
        return false;
    }), "production watcher moves file to new size group");
    const auto vm = BuildVm(state);
    bool grouped = false;
    for (const auto& pane : vm.pane_slots) if (pane.focused)
        grouped = pane.pane.groups && pane.pane.groups->size() >= 2;
    check(grouped, "real pane view model exposes distinct size group headers");
    shot(L"sort-grouped-after.png");
    fprintf(log, "SORT_UPDATES_UI checks=%d failures=%d\n", checks, failures);
    fflush(log);
    return failures ? 5 : 0;
}

