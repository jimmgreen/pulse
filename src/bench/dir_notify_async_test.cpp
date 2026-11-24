#include <windows.h>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <thread>
#include <stdexcept>
static DWORD caller_thread;
static DWORD injected_find_error = ERROR_SUCCESS;
static std::atomic<unsigned> caller_probes{0}, probes{0};
static HANDLE probe_entered, release_probe;
static std::atomic<bool> block_probe{false};
static HANDLE TestFindFirstFileExW(LPCWSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
    FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    ++probes;
    if (injected_find_error) { SetLastError(injected_find_error); return INVALID_HANDLE_VALUE; }
    if (GetCurrentThreadId() == caller_thread) ++caller_probes;
    if (block_probe) {
        SetEvent(probe_entered);
        WaitForSingleObject(release_probe, 5000);
    }
    return FindFirstFileExW(path, level, data, search, filter, flags);
}
#define FindFirstFileExW TestFindFirstFileExW
#include "../app/snapshot_patch.cpp"
#undef FindFirstFileExW
#include "../app/dir_notify_async.h"

using namespace pulse;
static int failures;
static void Check(bool ok, const char* label) {
    printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static fs::DirEntry Entry(const wchar_t* name) { fs::DirEntry e; e.name = name; return e; }
static app::PreparedDirNotifyBatch FakePrepare(const std::wstring&,
    const std::vector<fs::DirNotifyEvent>& events) {
    app::PreparedDirNotifyBatch result;
    result.events = events;
    for (const auto& event : events) {
        if (event.action == FILE_ACTION_REMOVED) continue;
        if (event.name == L"middle.txt") result.metadata[event.name] = std::nullopt;
        else result.metadata[event.name] = Entry(event.name.c_str());
    }
    return result;
}
static int MetadataFailureTest() {
    const auto folder = std::filesystem::absolute(L"bench_data").wstring();
    const std::vector<fs::DirNotifyEvent> events{
        {FILE_ACTION_MODIFIED, L"download.txt", {}},
        {FILE_ACTION_ADDED, L"download.txt", {}},
        {FILE_ACTION_RENAMED_NEW_NAME, L"download.txt", L"temporary.txt"}};
    for (const DWORD error : {ERROR_ACCESS_DENIED, ERROR_SHARING_VIOLATION, ERROR_NOT_READY, ERROR_INVALID_NAME}) {
        injected_find_error = error;
        for (const auto& event : events) {
            const wchar_t* old = event.old_name.empty() ? L"download.txt" : L"temporary.txt";
            auto entries = std::vector<fs::DirEntry>{Entry(old)};
            const auto result = app::ApplyDirNotify(entries, folder, event, ui::SortColumn::Name, ui::SortDirection::Asc);
            printf("[FACT] error=%lu action=%lu direct_result=%d rows=%zu\n", error, event.action, static_cast<int>(result), entries.size());
            Check(result == app::NotifyPatch::NeedFullEnum && entries.size() == 1 && entries.front().name == old,
                "uncertain metadata failure preserves direct-update row and requests full enumeration");
            // A removal earlier in the same batch must not be committed when a later stat fails.
            entries = {Entry(L"unrelated.txt"), Entry(old)};
            app::PreparedDirNotifyBatch prepared;
            std::thread worker([&] { prepared = app::PrepareDirNotifyBatch(folder,
                {{FILE_ACTION_REMOVED, L"unrelated.txt", {}}, event}); });
            worker.join();
            const auto before = probes.load();
            const auto applied = app::ApplyPreparedDirNotifyBatch(entries, prepared, ui::SortColumn::Name, ui::SortDirection::Asc);
            Check(prepared.need_full_enum && applied == app::NotifyPatch::NeedFullEnum && entries.size() == 2 &&
                entries[0].name == L"unrelated.txt" && entries[1].name == old && probes == before,
                "uncertain worker metadata preserves entire batch and replay performs no filesystem IO");
        }
    }
    for (const DWORD error : {ERROR_FILE_NOT_FOUND, ERROR_PATH_NOT_FOUND}) {
        injected_find_error = error;
        for (const auto& event : events) {
            const wchar_t* old = event.old_name.empty() ? L"download.txt" : L"temporary.txt";
            auto entries = std::vector<fs::DirEntry>{Entry(old)};
            Check(app::ApplyDirNotify(entries, folder, event, ui::SortColumn::Name, ui::SortDirection::Asc) ==
                app::NotifyPatch::Applied && entries.empty(), "confirmed absence still removes direct-update row");
            entries = {Entry(old)};
            const auto prepared = app::PrepareDirNotifyBatch(folder, {event});
            Check(!prepared.need_full_enum && app::ApplyPreparedDirNotifyBatch(entries, prepared,
                ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied && entries.empty(),
                "confirmed absence still removes prepared-update row");
        }
    }
    injected_find_error = ERROR_SUCCESS;
    return failures ? 1 : 0;
}
#include "sort_updates_test.inc"
#include "../app/new_picture_probes.h"

static int NewPictureProbeTest() {
    caller_thread = GetCurrentThreadId();
    const auto folder = std::filesystem::absolute(L"bench_data/new-photo-live-20261119/probe-" +
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(folder);
    const auto file = folder / L"outside.PNG";
    std::ofstream(file, std::ios::binary).close();
    std::vector<fs::DirEntry> rows;
    app::NewPictureProbes retries;
    const auto apply = [&](DWORD action, uint64_t now) {
        app::PreparedDirNotifyBatch prepared;
        std::thread worker([&] {
            prepared = app::PrepareDirNotifyBatch(folder.wstring(), {{action, L"outside.PNG", {}}});
        });
        worker.join();
        if (prepared.need_full_enum) return false;
        retries.Observe(folder.wstring(), prepared, now);
        return app::ApplyPreparedDirNotifyBatch(rows, prepared, ui::SortColumn::Size,
            ui::SortDirection::Asc) == app::NotifyPatch::Applied;
    };
    Check(apply(FILE_ACTION_ADDED, 0) && rows.size() == 1 && !rows[0].size, "initial ADDED captures empty external picture");
    std::ofstream(file, std::ios::binary) << "finished";
    unsigned fired = 0;
    retries.Tick(250, [&](const auto&, const auto&) { ++fired; });
    Check(fired == 1 && apply(FILE_ACTION_MODIFIED, 250) && rows.size() == 1 && rows[0].size == 8,
        "delayed worker stat repairs size without external MODIFIED");
    retries.Tick(50000, [&](const auto&, const auto&) { ++fired; });
    Check(fired == 1 && !caller_probes, "nonempty completion cancels retries; no UI-thread stat");
    std::ofstream(file, std::ios::binary).close();
    Check(apply(FILE_ACTION_ADDED, 0), "second empty arrival is tracked");
    fired = 0;
    const auto count = [&](const auto&, const auto&) { ++fired; };
    for (uint64_t now = 0; now <= 120000; now += 250) retries.Tick(now, count);
    Check(fired == 4, "permanent empty file stops timed probes");
    Check(!retries.OnForeground(L"other-window-folder", 119000), "unrelated window does not schedule a kick");
    retries.Tick(119000, count);
    Check(fired == 4 && retries.OnForeground(folder.wstring(), 120000) &&
        !retries.OnForeground(folder.wstring(), 120000), "visible pane gets one kick; duplicate panes share it");
    retries.Tick(120000, count);
    retries.OnForeground(folder.wstring(), 140000);
    retries.Tick(140000, count);
    Check(fired == 5, "foreground gets only one final probe across windows");
    std::filesystem::remove(file);
    std::filesystem::remove(folder);
    return failures ? 1 : 0;
}
int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--new-picture") return NewPictureProbeTest();
    if (argc == 2 && std::string(argv[1]) == "--metadata-failures") return MetadataFailureTest();
    if (argc == 2 && std::string(argv[1]) == "--sort-live-watch") return SortUpdatesLiveWatchTest();
    if (argc == 2 && std::string(argv[1]) == "--sort-updates") return SortUpdatesTest();
    setvbuf(stdout, nullptr, _IONBF, 0);
    caller_thread = GetCurrentThreadId();
    const auto root = std::filesystem::absolute(L"bench_data/rename_stall_fix/notify-fixture-" +
        std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directories(root);
    std::ofstream(root / L"after.txt") << "test";
    probe_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    release_probe = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!probe_entered || !release_probe) return 2;
    auto snapshot = std::make_shared<std::vector<fs::DirEntry>>(1, Entry(L"before.txt"));
    int owner;
    app::DirNotifyAsync::Guards guards{{&owner, 7, snapshot}};
    const std::vector<fs::DirNotifyEvent> rename{{FILE_ACTION_RENAMED_NEW_NAME, L"after.txt", L"before.txt"}};
    app::DirNotifyAsync queue;
    std::vector<std::thread> workers;
    const app::DirNotifyAsync::Enqueue enqueue = [&](std::function<void()> job) {
        workers.emplace_back(std::move(job));
    };
    block_probe = true;
    const auto start = GetTickCount64();
    Check(queue.Queue(root.wstring(), rename, 1, guards, enqueue), "rename accepted without waiting for metadata");
    const auto submit_ms = GetTickCount64() - start;
    Check(WaitForSingleObject(probe_entered, 1000) == WAIT_OBJECT_0, "metadata probe reached worker latch");
    Check(caller_probes == 0 && submit_ms < 100 && queue.TakeReady(enqueue).empty(),
        "UI remains runnable while metadata is blocked; no UI filesystem probe");
    Check(queue.Queue(root.wstring(), rename, 1, guards, enqueue) && workers.size() == 1,
        "same batch in another window does not duplicate the worker");
    SetEvent(release_probe);
    for (auto& worker : workers) worker.join();
    workers.clear();
    block_probe = false;
    auto done = queue.TakeReady(enqueue);
    const auto probes_before_apply = probes.load();
    auto entries = *snapshot;
    Check(done.size() == 1 && !done.front().fallback &&
        app::ApplyPreparedDirNotifyBatch(entries, done.front().prepared, ui::SortColumn::Name,
            ui::SortDirection::Asc) == app::NotifyPatch::Applied && entries.size() == 1 &&
        entries.front().name == L"after.txt" && probes == probes_before_apply && caller_probes == 0,
        "prepared rename applies in memory without any additional filesystem call");
    printf("MODIFIED caller_metadata_probes=%u submit_ms=%llu worker_metadata_probes=%u\n",
        caller_probes.load(), submit_ms, probes.load());
    const auto replacement = std::make_shared<std::vector<fs::DirEntry>>(*snapshot);
    Check(guards.front().Matches(&owner, 7, snapshot) && !guards.front().Matches(&owner, 8, snapshot) &&
        !guards.front().Matches(&owner, 7, replacement), "navigation generation and replaced snapshot reject stale metadata");

    auto retired = std::make_shared<std::vector<fs::DirEntry>>(1, Entry(L"retired.txt"));
    app::NotifySnapshotGuard retired_guard{&owner, 9, retired};
    retired.reset();
    Check(retired_guard.snapshot.expired() && !retired_guard.Matches(&owner, 9, {}),
        "pending metadata does not retain a directory snapshot after navigation");

    std::vector<std::function<void()>> tasks;
    const app::DirNotifyAsync::Enqueue hold = [&](std::function<void()> job) { tasks.push_back(std::move(job)); };
    const auto run = [&] { auto pending = std::move(tasks); tasks.clear(); for (auto& task : pending) task(); };
    app::DirNotifyAsync chain;
    const std::vector<fs::DirNotifyEvent> first{{FILE_ACTION_RENAMED_NEW_NAME, L"middle.txt", L"before.txt"}};
    const std::vector<fs::DirNotifyEvent> second{{FILE_ACTION_RENAMED_NEW_NAME, L"final.txt", L"middle.txt"}};
    chain.Queue(L"fixture", first, 1, guards, hold, FakePrepare);
    chain.Queue(L"fixture", second, 2, guards, hold, FakePrepare);
    Check(tasks.size() == 1, "later rename coalesces behind one active preparation");
    run();
    Check(chain.TakeReady(hold, FakePrepare).empty() && tasks.size() == 1,
        "superseded intermediate rename is not published");
    run();
    done = chain.TakeReady(hold, FakePrepare);
    entries = *snapshot;
    Check(done.size() == 1 && app::ApplyPreparedDirNotifyBatch(entries, done.front().prepared,
        ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied &&
        entries.size() == 1 && entries.front().name == L"final.txt", "coalesced rename chain publishes final name");

    chain.Queue(L"fixture", rename, 3, guards, hold, FakePrepare);
    chain.Queue(L"fixture", {{FILE_ACTION_REMOVED, L"after.txt", {}}}, 4, guards, hold, FakePrepare);
    run(); chain.TakeReady(hold, FakePrepare); run(); done = chain.TakeReady(hold, FakePrepare);
    entries = *snapshot;
    Check(done.size() == 1 && app::ApplyPreparedDirNotifyBatch(entries, done.front().prepared,
        ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied && entries.empty(),
        "delete arriving during preparation remains after its rename");
    entries = *snapshot;
    const auto before_delete = probes.load();
    Check(app::ApplyDirNotifyBatch(entries, L"not-a-drive", {{FILE_ACTION_REMOVED, L"before.txt", {}}},
        ui::SortColumn::Name, ui::SortDirection::Asc) == app::NotifyPatch::Applied && entries.empty() && probes == before_delete,
        "pure deletion stays synchronous and does not stat");

    const app::DirNotifyAsync::Prepare fail = [](const std::wstring&, const std::vector<fs::DirNotifyEvent>&)
        -> app::PreparedDirNotifyBatch { throw std::runtime_error("injected provider failure"); };
    chain.Queue(L"fixture", rename, 5, guards, hold, fail);
    run(); done = chain.TakeReady(hold, fail);
    Check(done.size() == 1 && done.front().fallback, "failed preparation requests background refresh");
    chain.Queue(L"fixture", rename, 6, guards, hold, FakePrepare);
    std::vector<fs::DirNotifyEvent> burst(app::DirNotifyAsync::kMaxEvents, {FILE_ACTION_MODIFIED, L"after.txt", {}});
    chain.Queue(L"fixture", burst, 7, guards, hold, FakePrepare);
    Check(tasks.size() == 1, "event overflow never spawns another worker");
    run(); done = chain.TakeReady(hold, FakePrepare);
    Check(done.size() == 1 && done.front().fallback && chain.Empty(), "event overflow drains to one background refresh");
    chain.Queue(L"changing", rename, 10, guards, hold, FakePrepare);
    for (unsigned pass = 0; pass < app::DirNotifyAsync::kMaxPreparePasses; ++pass) {
        chain.Queue(L"changing", {{FILE_ACTION_MODIFIED, L"after.txt", {}}}, 11 + pass, guards, hold, FakePrepare);
        run();
        done = chain.TakeReady(hold, FakePrepare);
    }
    Check(done.size() == 1 && done.front().fallback && chain.Empty() && tasks.empty(),
        "continuous changes stop re-preparation after three passes");
    for (size_t i = 0; i < app::DirNotifyAsync::kMaxFolders; ++i)
        chain.Queue(std::to_wstring(i), rename, 8, {}, hold, FakePrepare);
    Check(!chain.Queue(L"overflow", rename, 8, {}, hold, FakePrepare) &&
        tasks.size() == app::DirNotifyAsync::kMaxFolders, "pending directory count and queued work remain bounded");
    run(); chain.TakeReady(hold, FakePrepare);
    Check(chain.Empty(), "completed bounded queue releases all snapshots");
    CloseHandle(probe_entered);
    CloseHandle(release_probe);
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
