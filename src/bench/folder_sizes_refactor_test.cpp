#include "../app/folder_sizes.h"
#include "../app/folder_size_scanner.h"
#include "../app/folder_size_store.h"
#include <winioctl.h>
#include <windows.h>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
namespace pulse::app {
bool RunFolderSizeIndexClientTest(const fs::path& fixture, std::ofstream& log);
int RunFolderSizeFakeIndexServer(const wchar_t* pipe_name);
}
namespace {
using pulse::app::FolderSizes;
using State = pulse::app::FolderSizeState;
using Source = pulse::app::FolderSizeSource;
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* label) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    std::fflush(stdout);
}
bool Wait(const std::function<bool()>& predicate, DWORD timeout = 6000) {
    const auto deadline = GetTickCount64() + timeout;
    do { if (predicate()) return true; Sleep(5); } while (GetTickCount64() < deadline);
    return predicate();
}
void File(const fs::path& path, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::string(size, 'x');
    if (!out) throw std::runtime_error("fixture write failed");
}
bool Exact(FolderSizes& sizes, const fs::path& path, uint64_t bytes) {
    const auto value = sizes.Get(path.wstring());
    return value.state == State::Ready && value.has_value && value.bytes == bytes &&
           value.source == Source::Scan && !value.partial;
}
class Watchdog {
public:
    Watchdog() : thread_([this] {
        std::unique_lock lock(mutex_);
        if (!condition_.wait_for(lock, std::chrono::seconds(90), [this] { return done_; })) {
            std::fprintf(stderr, "[FAIL] isolated folder-size test exceeded 90 seconds\n");
            TerminateProcess(GetCurrentProcess(), 124);
        }
    }) {}
    ~Watchdog() {
        { std::lock_guard lock(mutex_); done_ = true; }
        condition_.notify_one(); thread_.join();
    }
private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool done_ = false;
    std::thread thread_;
};

void WatchedChanges(const fs::path& fixture) {
    const auto parent = fixture / L"watched";
    const auto child = parent / L"child";
    fs::create_directories(child);
    File(parent / L"a.bin", 10); File(child / L"b.bin", 20);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{parent.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 30) && sizes.Get(parent.wstring()).verified; }),
          "watched complete scan records verified source and total");
    const auto initial = sizes.Get(parent.wstring());
    Check(initial.verified_at != 0 && initial.source == Source::Scan,
          "verified scan records its verification timestamp");
    const auto before_reuse = sizes.ReadStats();
    sizes.Calculate(parent.wstring());
    Check(Wait([&] { return Exact(sizes, parent, 30) && sizes.ReadStats().jobs_completed > before_reuse.jobs_completed; }),
          "recalculating under unchanged watch returns the same real total");
    Check(sizes.ReadStats().subtree_hits > before_reuse.subtree_hits,
          "unchanged verified child subtree is reused during parent calculation");
    File(parent / L"a.bin", 40);
    Check(Wait([&] { return Exact(sizes, parent, 60); }), "watch observes larger file without explicit invalidation");
    File(parent / L"new.bin", 5);
    Check(Wait([&] { return Exact(sizes, parent, 65); }), "watch observes newly created file");
    fs::remove(child / L"b.bin");
    Check(Wait([&] { return Exact(sizes, parent, 45); }), "watch observes deletion without retaining obsolete child bytes");
    fs::rename(parent / L"new.bin", child / L"moved.bin");
    sizes.Sync({{parent.wstring()}, {child.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 45) && Exact(sizes, child, 5); }),
          "rename across subtrees invalidates both source and destination totals");
    const auto renamed = parent / L"renamed";
    fs::rename(child, renamed);
    sizes.Sync({{parent.wstring()}, {renamed.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return Exact(sizes, parent, 45) && Exact(sizes, renamed, 5); }),
          "renamed directory receives current subtree total");
    auto quiet_count = sizes.ReadStats().jobs_started;
    auto quiet_since = GetTickCount64();
    Check(Wait([&] {
        const auto count = sizes.ReadStats().jobs_started;
        if (count != quiet_count) { quiet_count = count; quiet_since = GetTickCount64(); }
        return Exact(sizes, parent, 45) && GetTickCount64() - quiet_since >= 300;
    }), "watch rename notifications settle before measuring hot Sync");
    const auto before_hot = sizes.ReadStats();
    for (unsigned repeat = 0; repeat < 30; ++repeat)
        sizes.Sync({{parent.wstring()}, {renamed.wstring()}}, {parent.wstring()});
    Sleep(80);
    Check(sizes.ReadStats().jobs_started == before_hot.jobs_started && Exact(sizes, parent, 45),
          "unchanged hot Sync does not create repeated scans");

    // Empty visibility is a temporary subscription change, not a monitoring gap.
    sizes.Sync({}, {});
    Check(sizes.Get(parent.wstring()).verified, "temporary empty view retains the recent watch lease");
    // Force a real bounded-cache eviction instead of assuming navigation stops a watch.
    for (unsigned i = 0; i < 17; ++i) {
        const auto other = fixture / (L"watch-eviction-" + std::to_wstring(i));
        fs::create_directories(other);
        sizes.Sync({{other.wstring(), false}}, {other.wstring()});
        Sleep(2);
    }
    Check(Wait([&] { return !sizes.Get(parent.wstring()).verified; }),
          "leaving watch coverage revokes verification of retained totals");
    File(renamed / L"moved.bin", 70);
    sizes.Sync({{parent.wstring()}}, {parent.wstring()});
    const auto returned = sizes.Get(parent.wstring());
    Check(!(returned.verified && returned.bytes == 45), "returning after watch gap cannot label old total verified");
    Check(Wait([&] { return Exact(sizes, parent, 110); }), "watch gap forces updated real total instead of stale subtree reuse");
    sizes.Stop();
}

void PartialAndPersistence(const fs::path& fixture) {
    const auto parent = fixture / L"partial";
    const auto offline = parent / L"child" / L"offline";
    const auto cache = (fixture / L"partial-cache.json").wstring();
    fs::create_directories(offline);
    File(parent / L"readable.bin", 17);
    File(parent / L"child" / L"readable.bin", 23);
    File(offline / L"skipped.bin", 91);
    const bool attributed = SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE) != FALSE;
    Check(attributed, "create isolated offline-subtree fixture");
    if (!attributed) return;
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.SetCachePath([cache] { return cache; });
        sizes.Sync({{parent.wstring()}}, {parent.wstring()});
        Check(Wait([&] { return sizes.Get(parent.wstring()).state == State::Partial; }), "skipped offline subtree remains partial");
        const auto value = sizes.Get(parent.wstring());
        Check(value.has_value && value.bytes == 40 && value.partial && value.source == Source::Scan && !value.verified,
              "partial child still contributes its readable bytes to parent lower bound");
        sizes.Stop();
    }
    {
        FolderSizes loaded; loaded.SetIndexEnabled(false); loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{parent.wstring(), false}}, {});
        Check(Wait([&] { return loaded.Get(parent.wstring()).has_value; }), "partial cache reload produces a retained value");
        const auto value = loaded.Get(parent.wstring());
        Check(value.bytes == 40 && value.partial && !value.verified && value.state == State::Cached && value.source == Source::Scan,
              "restart preserves lower-bound provenance while revoking verification");
        Check(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY) != FALSE, "restore isolated offline directory");
        loaded.Calculate(parent.wstring());
        Check(Wait([&] { return Exact(loaded, parent, 131); }) && !loaded.Get(parent.wstring()).verified,
              "complete scan replaces partial cache; no-watch snapshot does not claim watch verification");
        loaded.Stop();
    }
    {
        FolderSizes loaded; loaded.SetIndexEnabled(false); loaded.SetCachePath([cache] { return cache; });
        loaded.Sync({{parent.wstring(), false}}, {});
        Check(Wait([&] { return loaded.Get(parent.wstring()).has_value; }) &&
              loaded.Get(parent.wstring()).bytes == 131 && !loaded.Get(parent.wstring()).partial &&
              !loaded.Get(parent.wstring()).verified && loaded.Get(parent.wstring()).state == State::Cached,
              "complete disk cache is retained but is not misreported as a new accurate scan");
        loaded.Stop();
    }
}

void ManualMetadataRegression(const fs::path& fixture) {
    const auto parent = fixture / L"manual-metadata";
    const auto offline = parent / L"offline";
    fs::create_directories(offline);
    File(parent / L"visible.bin", 17);
    File(offline / L"value.bin", 91);
    Check(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE) != FALSE,
          "offline metadata fixture created");
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{parent.wstring()}}, {});
    Check(Wait([&] { return sizes.Get(parent.wstring()).state == State::Partial; }),
          "automatic scan keeps offline subtree as an explicit lower bound");
    sizes.Calculate(parent.wstring());
    Check(Wait([&] { return Exact(sizes, parent, 108); }),
          "manual click enumerates offline metadata and replaces partial result");
    sizes.Stop();
    SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);

    const auto wide = fixture / L"progress";
    fs::create_directories(wide);
    for (unsigned i = 0; i < 1500; ++i) File(wide / (std::to_wstring(i) + L".bin"), 13);
    pulse::app::folder_size::Scan progress(wide.wstring(), 1, 1);
    std::atomic<bool> keep_running{false};
    const auto lookup = [](const auto&) -> std::optional<uint64_t> { return std::nullopt; };
    const auto coverage = [](const auto&) -> uint64_t { return 0; };
    const auto first_progress_deadline = GetTickCount64() + 6000;
    do {
        progress.Step(keep_running, lookup, coverage);
    } while (!progress.done && progress.entries_scanned == 0 && GetTickCount64() < first_progress_deadline);
    const auto interim = progress.Progress();
    std::printf("[INFO] first progress: done=%d entries=%llu bytes=%llu has=%d partial=%d verified=%d\n",
                progress.done, static_cast<unsigned long long>(progress.entries_scanned),
                static_cast<unsigned long long>(interim.bytes), interim.has_value, interim.partial, interim.verified);
    Check(!progress.done && interim.has_value && interim.partial && !interim.verified &&
          interim.bytes == progress.entries_scanned * 13 && interim.bytes < 1500 * 13,
          "unfinished scan exposes an honest lower bound before completion");
    const auto progress_deadline = GetTickCount64() + 6000;
    while (!progress.done && GetTickCount64() < progress_deadline)
        progress.Step(keep_running, lookup, coverage);
    Check(progress.done && !progress.Progress().partial && progress.Progress().bytes == 1500 * 13,
          "completed scan replaces progress without double-counting bytes");

    const auto large = fixture / L"large-logical";
    fs::create_directories(large);
    HANDLE file = CreateFileW((large / L"sparse.bin").c_str(), GENERIC_WRITE, 0, nullptr,
                              CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    bool sparse = false;
    const uint64_t logical = (uint64_t{5} << 30) + 123;
    if (file != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        LARGE_INTEGER end{}; end.QuadPart = static_cast<LONGLONG>(logical);
        sparse = DeviceIoControl(file, FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned, nullptr) &&
                 SetFilePointerEx(file, end, nullptr, FILE_BEGIN) && SetEndOfFile(file);
        CloseHandle(file);
    }
    Check(sparse, "5 GiB sparse fixture uses logical size without allocating 5 GiB");
    if (sparse) {
        FolderSizes big; big.SetIndexEnabled(false);
        big.Sync({{large.wstring(), false}}, {}); big.Calculate(large.wstring());
        Check(Wait([&] { return Exact(big, large, logical); }),
              "manual total above 4 GiB retains all 64-bit bytes and is not partial");
        big.Stop();
    }

    const fs::path deep_root(L"\\\\?\\" + (fixture / L"deep").wstring());
    auto leaf = deep_root;
    for (unsigned depth = 0; depth < 140; ++depth) leaf /= L"d";
    fs::create_directories(leaf);
    File(leaf / L"value.bin", 59);
    pulse::app::folder_size::Scan scan(deep_root.wstring(), 1, 1, true);
    std::atomic<bool> stopping{false};
    const auto deadline = GetTickCount64() + 6000;
    while (!scan.done && GetTickCount64() < deadline)
        scan.Step(stopping, [](const auto&) -> std::optional<uint64_t> { return std::nullopt; },
                  [](const auto&) -> uint64_t { return 0; });
    Check(scan.done && scan.result.state == State::Ready && scan.result.bytes == 59 && !scan.result.partial,
          "depth above 128 completes without an artificial partial cutoff");
}

void Cancellation(const fs::path& fixture) {
    // Reuse the isolated workload generated by the real index-client checks.
    const auto large = fixture / L"index-estimate-workload";
    const auto empty = fixture / L"empty";
    // Several slices give the controller a real cancellation window even on a
    // warm local filesystem, without relying on the index worker's timing.
    for (unsigned folder = 0; folder < 64; ++folder)
        for (unsigned file = 32; file < 128; ++file)
            File(large / std::to_wstring(folder) / (std::to_wstring(file) + L".bin"), 13);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.SetScanPacing(0); // keep the walk in flight long enough to cancel it
    sizes.Sync({{large.wstring()}}, {});
    Check(Wait([&] { return sizes.ReadStats().entries_scanned > 0; }), "cancellation reaches a real in-progress scan");
    sizes.Sync({{empty.wstring()}}, {});
    Check(Wait([&] { return Exact(sizes, empty, 0); }), "new visible empty directory completes after cancelling previous scope");
    Check(sizes.ReadStats().jobs_cancelled > 0, "offscreen unfinished job is cancelled");
    const auto cancelled = sizes.Get(large.wstring());
    Check(cancelled.state != State::Ready && !cancelled.verified,
          "cancelled old epoch cannot publish a completed accurate result");
    File(large / L"after-cancel.bin", 27);
    sizes.Sync({{large.wstring()}}, {});
    Check(Wait([&] { return Exact(sizes, large, 64 * 128 * 13 + 27); }),
          "returning after cancellation rescans and includes newly created bytes");
    sizes.Stop();
}

void WatchedManualParentAndChild(const fs::path& fixture) {
    const auto parent = fixture / L"watched-manual", child = parent / L"child";
    fs::create_directories(child);
    File(child / L"value.bin", 59);
    for (unsigned i = 0; i < 2048; ++i) File(parent / (std::to_wstring(i) + L".bin"), 13);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{parent.wstring(), false}, {child.wstring(), false}}, {parent.wstring()});
    sizes.Calculate(parent.wstring());
    Check(Wait([&] { return Exact(sizes, parent, 2048 * 13 + 59) && sizes.Get(parent.wstring()).verified; }),
          "manual parent fixture has uninterrupted watch coverage");
    sizes.Calculate(parent.wstring());
    sizes.Calculate(child.wstring());
    Check(Wait([&] {
        return Exact(sizes, parent, 2048 * 13 + 59) && Exact(sizes, child, 59) &&
            !sizes.GetWork(parent.wstring()).Running() && !sizes.GetWork(child.wstring()).Running();
    }, 2000), "watched parent and child manual requests both finish without freshness-delay queueing");
    sizes.Stop();
}

void ChildCalculationCancelsAncestor(const fs::path& fixture) {
    const auto parent = fixture / L"index-estimate-workload";
    const auto child = parent / L"calculate-child";
    fs::create_directories(child);
    File(child / L"value.bin", 7);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.SetScanPacing(0); // keep the ancestor walk in flight
    sizes.Sync({{parent.wstring()}, {child.wstring()}}, {});
    bool in_flight = false;
    const auto deadline = GetTickCount64() + 6000;
    do {
        const auto stats = sizes.ReadStats();
        in_flight = stats.entries_scanned > 0 && stats.jobs_completed == 0 &&
                    sizes.Get(parent.wstring()).state != State::Ready;
        if (in_flight || stats.jobs_completed > 0) break;
        Sleep(1);
    } while (GetTickCount64() < deadline);
    Check(in_flight, "child calculation fixture reaches an unfinished ancestor scan");
    const auto before = sizes.ReadStats();
    File(child / L"value.bin", 59);
    sizes.Calculate(child.wstring());
    Check(Wait([&] { return sizes.ReadStats().jobs_cancelled > before.jobs_cancelled; }),
          "manual child calculation cancels its in-flight ancestor epoch");
    Check(Wait([&] { return Exact(sizes, parent, 64 * 128 * 13 + 27 + 59) && Exact(sizes, child, 59); }),
          "ancestor and child publish new totals without an old parent result overwriting the child");
    Check(!sizes.Get(parent.wstring()).verified && !sizes.Get(child.wstring()).verified,
          "manual no-watch calculation remains a snapshot without reusable verification");
    sizes.Stop();
}
}

// A newly armed watch reconciles results that already exist. Rows still waiting
// for their first result must keep their revision, or the index answer already
// on its way is dropped and the whole list waits for the next poll.
void StoreArmReconcile() {
    using pulse::app::folder_size::Key;
    pulse::app::folder_size::Store store;
    const std::set<std::wstring> keep;
    const auto root = Key(L"C:\\pulse-store-fixture");
    auto* pending = store.Ensure(Key(L"C:\\pulse-store-fixture\\pending"), keep);
    auto* scanned = store.Ensure(Key(L"C:\\pulse-store-fixture\\scanned"), keep);
    auto* running = store.Ensure(Key(L"C:\\pulse-store-fixture\\running"), keep);
    if (!pending || !scanned || !running) { Check(false, "store fixture entries"); return; }
    scanned->value.has_value = true; scanned->value.source = Source::Scan; scanned->completed = 1;
    running->work.activity = pulse::app::FolderSizeActivity::Scanning;
    pending->next_index_at = 99;
    const auto pending_revision = pending->revision, scanned_revision = scanned->revision;
    const auto running_epoch = running->request_epoch;
    store.Invalidate(root, 1000, 0, true);
    Check(pending->revision == pending_revision && pending->next_index_at == 99,
          "armed watch keeps the in-flight index request of a row without any result");
    Check(scanned->revision != scanned_revision && scanned->completed == 0,
          "armed watch still re-verifies previously scanned rows");
    Check(running->request_epoch != running_epoch, "armed watch restarts scans that began before coverage");
    store.Invalidate(root, 2000, 0);
    Check(pending->revision != pending_revision, "real change notifications still invalidate pending rows");
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 3 && std::wstring(argv[1]) == L"--serve-index")
        return pulse::app::RunFolderSizeFakeIndexServer(argv[2]);
    Watchdog watchdog;
    const auto root = fs::current_path() / L"bench_data" /
        (L"folder_sizes_refactor_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64()));
    try {
        if (argc == 2 && std::wstring(argv[1]) == L"--watched-manual-only") {
            WatchedManualParentAndChild(root);
            std::wprintf(L"[INFO] retained isolated watched manual fixtures: %ls\n", root.c_str());
            std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
            return failures ? 1 : 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--manual-only") {
            ManualMetadataRegression(root);
            std::wprintf(L"[INFO] retained isolated manual fixtures: %ls\n", root.c_str());
            std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
            return failures ? 1 : 0;
        }
        if (argc == 2 && std::wstring(argv[1]) == L"--watch-only") {
            WatchedChanges(root);
            std::wprintf(L"[INFO] retained isolated watch fixtures: %ls\n", root.c_str());
            std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
            return failures ? 1 : 0;
        }
        StoreArmReconcile();
        fs::create_directories(root / L"nested" / L"child");
        fs::create_directories(root / L"empty");
        File(root / L"nested" / L"a.bin", 1234);
        File(root / L"nested" / L"child" / L"b.bin", 12333);
        std::ofstream index_log(root / L"index-results.log");
        Check(pulse::app::RunFolderSizeIndexClientTest(root, index_log), "real isolated index protocol regression group");
        index_log.close();
        if (argc == 2 && std::wstring(argv[1]) == L"--index-only") {
            std::wprintf(L"[INFO] retained isolated index fixtures: %ls\n", root.c_str());
            std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
            return failures ? 1 : 0;
        }
        WatchedChanges(root);
        PartialAndPersistence(root);
        Cancellation(root);
        ChildCalculationCancelsAncestor(root);
        WatchedManualParentAndChild(root);
    } catch (const std::exception& error) {
        Check(false, error.what());
    }
    std::wprintf(L"[INFO] retained isolated fixtures and index assertion log: %ls\n", root.c_str());
    std::printf("[SUMMARY] %u assertions, %u failure(s)\n", checks, failures);
    return failures ? 1 : 0;
}