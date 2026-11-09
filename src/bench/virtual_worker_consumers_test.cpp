#include "../app/app_worker.h"
#include <cstdio>
#include <cstdlib>
#include <map>

namespace {
HANDLE attribute_entered = nullptr, attribute_release = nullptr;
std::atomic<bool> block_attributes{false};
BOOL WINAPI FixtureAttributes(LPCWSTR path, GET_FILEEX_INFO_LEVELS, LPVOID output) {
    const std::wstring value(path);
    if (value.find(L"blocked") != std::wstring::npos && block_attributes) {
        SetEvent(attribute_entered);
        WaitForSingleObject(attribute_release, 5000);
    }
    if (value.find(L"missing") != std::wstring::npos) {
        SetLastError(ERROR_FILE_NOT_FOUND);
        return FALSE;
    }
    auto& data = *static_cast<WIN32_FILE_ATTRIBUTE_DATA*>(output);
    data = {}; data.dwFileAttributes = FILE_ATTRIBUTE_NORMAL; data.nFileSizeLow = 7;
    const DWORD stamp = value.ends_with(L"z.txt") ? 100 : value.ends_with(L"a.txt") ? 300 : 200;
    data.ftLastWriteTime.dwLowDateTime = stamp;
    data.ftCreationTime.dwLowDateTime = stamp;
    data.ftLastAccessTime.dwLowDateTime = stamp;
    return TRUE;
}
}
#define GetFileAttributesExW FixtureAttributes
#include "../app/app_worker.cpp"
#undef GetFileAttributesExW

namespace pulse::fs {
bool IsDevicePath(const std::wstring&) { return false; }
std::wstring FormatDevicePath(const DevicePath&) { return {}; }
uint64_t WatchClock() noexcept { return 0; }
void WaitForStartingWatch(const std::wstring&, DWORD, DWORD) {}
bool IsVirtualPath(const std::wstring& p) { return p.starts_with(L"pulse:"); }
bool IsUncPath(const std::wstring& p) { return p.starts_with(L"\\\\"); }
DWORD ReadReparseTag(const std::wstring&) { return 0; }
void EnumerateDirectory(const std::wstring&, std::vector<DirEntry>&, const std::atomic_bool*) {}
void EnumerateRecycleBin(std::vector<DirEntry>&, RecycleBinInfo*) {}
bool QueryDirectoryIdentity(const std::wstring&, DirectoryIdentity&) { return true; }
NetSnapshotWrite BeginNetSnapshotWrite(const std::wstring&) { return {}; }
bool SaveNetSnapshot(const NetSnapshotWrite&, const SnapshotPtr&) { return true; }
}
namespace pulse::app {
namespace startup_trace { void Mark(const char*) noexcept {} }
void RefreshDriveEjectable(const std::wstring&, uint8_t) {}
std::vector<ThisPcLauncher> CurrentThisPcLaunchers() { return {}; }
DeviceRegistry& DeviceRegistry::Instance() { std::abort(); }
std::vector<fs::DeviceInfo> DeviceRegistry::Devices() const { return {}; }
fs::DeviceError DeviceRegistry::ListFolder(const std::wstring&, std::vector<fs::DirEntry>&,
    const std::function<bool()>&) { std::abort(); }
std::wstring FindGitRoot(const std::wstring&) { return {}; }
void ResolveLinksInPlace(const std::wstring&, std::vector<fs::DirEntry>&, const std::function<bool()>&) {}
}
namespace pulse::diagnostics::runtime {
void Event(const char*, std::initializer_list<Field>, Level) noexcept {}
}
namespace {
using namespace pulse;
struct Results {
    std::mutex mutex;
    std::condition_variable changed;
    std::map<uint64_t, app::WorkResult> values;
    void Add(app::WorkResult result) {
        { std::lock_guard lock(mutex); values.emplace(result.generation, std::move(result)); }
        changed.notify_all();
    }
    bool Wait(uint64_t generation) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(3), [&] { return values.contains(generation); });
    }
    bool Has(uint64_t generation) {
        std::lock_guard lock(mutex);
        return values.contains(generation);
    }
    bool Item(uint64_t generation, const wchar_t* name, uint64_t time = 0) {
        std::lock_guard lock(mutex);
        const auto it = values.find(generation);
        if (it == values.end() || !it->second.snapshot || it->second.snapshot->size() != 1) return false;
        const auto& item = it->second.snapshot->front();
        const auto stamp = (static_cast<uint64_t>(item.mtime.dwHighDateTime) << 32) | item.mtime.dwLowDateTime;
        return !it->second.cancelled && !it->second.error && item.name == name && (!time || stamp == time);
    }
    bool Order(uint64_t generation, const std::vector<std::wstring>& names) {
        std::lock_guard lock(mutex);
        const auto it = values.find(generation);
        if (it == values.end() || it->second.error || it->second.cancelled || !it->second.snapshot ||
            it->second.snapshot->size() != names.size()) return false;
        for (size_t i = 0; i < names.size(); ++i)
            if ((*it->second.snapshot)[i].name != names[i]) return false;
        return true;
    }
    bool Empty(uint64_t generation) {
        std::lock_guard lock(mutex);
        const auto it = values.find(generation);
        return it != values.end() && !it->second.error && it->second.snapshot && it->second.snapshot->empty();
    }
};
}
int main() {
    using namespace pulse;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        failures += !ok;
    };
    attribute_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    attribute_release = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE all_blocked = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE release_workers = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!attribute_entered || !attribute_release || !all_blocked || !release_workers) return 2;
    Results results;
    {
        app::WorkerPool worker;
        worker.Start([&](app::WorkResult result) { results.Add(std::move(result)); });
        const auto columns = ui::SortColumn::Name;
        const auto ascending = ui::SortDirection::Asc;
        const unsigned count = std::min(4u, std::max(2u, std::thread::hardware_concurrency()));
        std::atomic<unsigned> entered{0};
        for (unsigned i = 0; i < count; ++i) {
            worker.EnqueueIo([&] {
                if (++entered == count) SetEvent(all_blocked);
                WaitForSingleObject(release_workers, 5000);
            });
        }
        check(WaitForSingleObject(all_blocked, 2000) == WAIT_OBJECT_0, "all real worker threads paused before virtual submissions");
        const auto first = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\a.txt"}, columns, ascending, true, {111});
        const auto second = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\b.txt"}, columns, ascending, true, {222});
        SetEvent(release_workers);
        check(results.Wait(first) && results.Wait(second) && results.Item(first, L"a.txt", 111) &&
              results.Item(second, L"b.txt", 222),
              "same queued WorkKey delivers both consumers with distinct Recent payloads");
        for (const auto* path : {L"pulse:starred", L"pulse:tag:fixture"}) {
            block_attributes = true;
            ResetEvent(attribute_entered); ResetEvent(attribute_release);
            const auto early = worker.LoadPaths(path, {L"C:\\fixture\\blocked.txt"}, columns, ascending);
            check(WaitForSingleObject(attribute_entered, 2000) == WAIT_OBJECT_0, "early virtual request is active behind metadata barrier");
            const auto late = worker.LoadPaths(path, {L"C:\\fixture\\later.txt"}, columns, ascending);
            const bool peer_finished = results.Wait(late);
            SetEvent(attribute_release);
            check(peer_finished && results.Wait(early) && results.Item(early, L"blocked.txt") && results.Item(late, L"later.txt"),
                  "overlapping same-view active requests both finish their own generation");
        }
        block_attributes = true;
        ResetEvent(attribute_entered); ResetEvent(attribute_release);
        const auto cancelled = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\blocked.txt"}, columns, ascending);
        check(WaitForSingleObject(attribute_entered, 2000) == WAIT_OBJECT_0, "consumer to close reaches active barrier");
        const auto survivor = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\survivor.txt"}, columns, ascending);
        worker.CancelGeneration(cancelled);
        const bool survived = results.Wait(survivor);
        SetEvent(attribute_release);
        block_attributes = false;
        const auto grouped = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\grouped.txt"}, columns,
            ui::SortDirection::Desc, false, {}, 1);
        check(survived && results.Wait(grouped) && results.Item(survivor, L"survivor.txt") &&
              results.Item(grouped, L"grouped.txt"), "closing one consumer leaves peer and differently grouped request usable");
        const auto empty = worker.LoadPaths(L"pulse:recent", {}, columns, ascending);
        const auto missing = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\missing.txt"}, columns, ascending);
        check(results.Wait(empty) && results.Wait(missing) && results.Empty(empty) && results.Item(missing, L"missing.txt"),
              "empty and metadata-failed payloads still deliver terminal snapshots");
        const auto completed = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\completed.txt"}, columns, ascending);
        const bool first_done = results.Wait(completed);
        const auto subsequent = worker.LoadPaths(L"pulse:recent", {L"C:\\fixture\\subsequent.txt"}, columns, ascending);
        check(first_done && results.Wait(subsequent) && results.Item(completed, L"completed.txt") &&
              results.Item(subsequent, L"subsequent.txt"), "request completed before next submission remains independently delivered");
        const std::vector<std::wstring> manual_paths{L"C:\\fixture\\m.txt", L"C:\\fixture\\z.txt", L"C:\\fixture\\a.txt"};
        const auto original_paths = manual_paths;
        for (const auto column : {ui::SortColumn::Name, ui::SortColumn::Mtime,
                                  ui::SortColumn::Created, ui::SortColumn::Accessed}) {
            for (const auto direction : {ui::SortDirection::Asc, ui::SortDirection::Desc}) {
                std::vector<std::wstring> expected = column == ui::SortColumn::Name
                    ? std::vector<std::wstring>{L"a.txt", L"m.txt", L"z.txt"}
                    : std::vector<std::wstring>{L"z.txt", L"m.txt", L"a.txt"};
                if (direction == ui::SortDirection::Desc) std::reverse(expected.begin(), expected.end());
                for (int refresh = 0; refresh < 2; ++refresh) {
                    const auto sorted = worker.LoadPaths(L"pulse:starred", manual_paths, column, direction,
                        false, {}, 0, app::FolderSortMode::Mixed);
                    check(results.Wait(sorted) && results.Order(sorted, expected),
                        refresh ? "favorites refresh retains selected column order" : "favorites real worker sorts name/date in both directions");
                }
            }
        }
        check(manual_paths == original_paths, "favorites sorting leaves source manual order unchanged");
        const auto recent_order = worker.LoadPaths(L"pulse:recent", manual_paths, ui::SortColumn::Name,
            ui::SortDirection::Asc, true, {300, 200, 100});
        check(results.Wait(recent_order) && results.Order(recent_order, {L"m.txt", L"z.txt", L"a.txt"}),
            "recent retains supplied opened order");
        worker.Stop();
        check(!results.Has(cancelled), "cancelled consumer cannot publish late data to remaining consumers");
    }
    CloseHandle(attribute_entered); CloseHandle(attribute_release);
    CloseHandle(all_blocked); CloseHandle(release_workers);
    return failures ? 1 : 0;
}
