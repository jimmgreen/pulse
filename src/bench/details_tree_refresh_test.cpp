#include "../app/app_internal.h"
#include "../app/app_details_tree.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int main() {
    using namespace pulse;
    namespace disk = std::filesystem;
    const auto base = disk::absolute(disk::path(L"../bench_data") /
        (L"tree-refresh-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    const auto downloads = base / L"Downloads";
    disk::create_directories(downloads);
    struct Cleanup {
        disk::path path;
        ~Cleanup() { std::error_code error; disk::remove_all(path, error); }
    } cleanup{base};
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        std::fflush(stdout);
        failures += !ok;
    };
    auto owned = std::make_unique<AppState>();
    auto& s = *owned;
    s.isolatedTest = true;
    s.appPrefs.persist = s.places.persist = s.searchHistory.persist = false;
    s.window_tabs.EnsureDefault();
    s.pane = s.window_tabs.Active()->FocusedPane();
    auto& tab = *ActiveTab(s);
    tab.current_path = fs::NormalizePath(base.wstring());
    tab.view_mode = ui::ViewMode::Details;
    auto root = std::make_shared<std::vector<fs::DirEntry>>();
    fs::EnumerateDirectory(tab.current_path, *root);
    tab.SetSnapshot(root);
    s.store.Put(tab.current_path, root);
    tab.tree = std::make_shared<app::DetailsTree>();
    const auto folder_path = fs::NormalizePath(downloads.wstring());
    auto key = folder_path;
    CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
    auto& folder = tab.tree->folders[key];
    folder.path = folder_path;
    folder.expanded = true;
    s.worker.Start([&](app::WorkResult result) {
        std::lock_guard lock(s.resultMutex);
        s.results.push(std::move(result));
    });
    const auto pump = [&] {
        DrainDirNotifies(s);
        ProcessPendingResults(s);
        SyncDetailsTrees(s);
    };
    const auto await = [&](auto ready) {
        const auto until = GetTickCount64() + 6000;
        do {
            pump();
            if (ready()) return true;
            Sleep(10);
        } while (GetTickCount64() < until);
        return ready();
    };
    const auto has = [&](const wchar_t* name, uint64_t size = UINT64_MAX) {
        if (!tab.TreeEntries()) return false;
        for (const auto& entry : *tab.TreeEntries())
            if (entry.name == name && (size == UINT64_MAX || entry.size == size)) return true;
        return false;
    };
    check(await([&] { return folder.state == app::DetailsTreeFolder::State::Ready &&
        folder.listing && folder.listing->empty() && s.watches.Armed(folder_path); }),
        "expanded empty folder is loaded and watched");
    std::ofstream(downloads / L"download.tmp") << "part";
    check(await([&] { return has(L"download.tmp"); }), "new download appears without entering the folder");
    disk::rename(downloads / L"download.tmp", downloads / L"finished.txt");
    check(await([&] { return has(L"finished.txt") && !has(L"download.tmp"); }),
        "download completion rename updates expanded rows");
    std::ofstream(downloads / L"finished.txt", std::ios::app) << "more";
    check(await([&] { return has(L"finished.txt", 8); }), "download size changes update expanded rows");
    disk::remove(downloads / L"finished.txt");
    check(await([&] { return !has(L"finished.txt") && folder.state == app::DetailsTreeFolder::State::Ready; }),
        "deleted files disappear from expanded rows");
    folder.expanded = false;
    ++tab.tree->revision;
    SyncDetailsTrees(s);
    check(!s.watches.Armed(folder_path), "collapsing a folder releases its watch");
    std::ofstream(downloads / L"while-closed.txt") << "new";
    folder.expanded = true;
    folder.state = app::DetailsTreeFolder::State::Idle;
    ++tab.tree->revision;
    check(await([&] { return has(L"while-closed.txt"); }), "reopening refreshes a cached folder");

    const auto nested = downloads / L"Nested";
    disk::create_directories(nested);
    check(await([&] { return has(L"Nested"); }), "new subfolders appear in the expanded tree");
    const auto nested_path = fs::NormalizePath(nested.wstring());
    auto nested_key = nested_path;
    CharLowerBuffW(nested_key.data(), static_cast<DWORD>(nested_key.size()));
    auto& nested_folder = tab.tree->folders[nested_key];
    nested_folder.path = nested_path;
    nested_folder.expanded = true;
    ++tab.tree->revision;
    check(await([&] { return nested_folder.state == app::DetailsTreeFolder::State::Ready &&
        s.watches.Armed(nested_path); }), "nested expansion is watched independently");
    folder.expanded = false;
    ++tab.tree->revision;
    SyncDetailsTrees(s);
    check(!s.watches.Armed(folder_path) && !s.watches.Armed(nested_path),
        "collapsing the parent releases descendant watches too");
    std::ofstream(nested / L"nested-download.txt") << "nested";
    folder.expanded = true;
    folder.state = app::DetailsTreeFolder::State::Idle;
    ++tab.tree->revision;
    check(await([&] { return has(L"nested-download.txt"); }),
        "reopening the parent refreshes still-expanded descendants");

    // Deterministic notification-during-read and late-result checks.
    s.worker.Stop();
    s.watches.Stop();
    ProcessPendingResults(s);
    folder.state = app::DetailsTreeFolder::State::Loading;
    folder.generation = 1000000;
    s.store.BeginRefresh(folder_path);
    check(NoteDetailsTreeChange(s, folder_path) && folder.generation == 1000000 &&
        folder.state == app::DetailsTreeFolder::State::Loading,
        "change during enumeration does not cancel its generation");
    app::WorkResult result{};
    result.path = folder_path;
    result.generation = 999999;
    result.snapshot = std::make_shared<std::vector<fs::DirEntry>>();
    NoteDetailsTreeResult(s, result);
    check(folder.generation == 1000000, "stale results cannot complete a newer tree read");
    result.generation = 1000000;
    ApplyWorkerResult(s, result);
    check(folder.state == app::DetailsTreeFolder::State::Idle && s.store.IsDirty(folder_path),
        "change during enumeration schedules a follow-up read");
    folder.expanded = false;
    folder.generation = 1000001;
    folder.state = app::DetailsTreeFolder::State::Loading;
    result.generation = 1000001;
    NoteDetailsTreeResult(s, result);
    check(!folder.expanded && !folder.generation, "late results do not reopen a collapsed folder");
    return failures ? 1 : 0;
}
