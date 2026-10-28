#include "../app/folder_sizes.h"
#include "../app/folder_size_store.h"
#include "../index/index_feed.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <stdexcept>
#include <thread>

namespace fs = std::filesystem;
using namespace pulse::app;
namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks; if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", message); std::fflush(stdout);
}
bool Wait(const std::function<bool()>& fn, DWORD ms = 10000) {
    const auto end = GetTickCount64() + ms;
    do { if (fn()) return true; Sleep(2); } while (GetTickCount64() < end);
    return fn();
}
void File(const fs::path& path, size_t bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << std::string(bytes, 'x');
    if (!out) throw std::runtime_error("fixture creation failed");
}
bool Exact(FolderSizes& sizes, const fs::path& path, uint64_t bytes) {
    const auto value = sizes.Get(path.wstring());
    return value.has_value && value.bytes == bytes && !value.partial && value.state == FolderSizeState::Ready;
}
void Navigation(const fs::path& root) {
    const auto path = root / L"watched";
    fs::create_directories(path);
    File(path / L"one.bin", 137);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{path.wstring()}}, {path.wstring()});
    Check(Wait([&] { return Exact(sizes, path, 137) && sizes.Get(path.wstring()).verified; }), "initial watched exact result");
    Sleep(250);
    const auto before = sizes.ReadStats();
    double slowest = 0;
    for (unsigned i = 0; i < 20; ++i) {
        // The UI emits an empty subscription during loading/Up, then requests the row again.
        sizes.Sync({}, {});
        const auto start = std::chrono::steady_clock::now();
        sizes.Sync({{path.wstring()}}, {path.wstring()});
        const auto value = sizes.Get(path.wstring());
        slowest = (std::max)(slowest, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count());
        Check(value.bytes == 137 && value.state == FolderSizeState::Ready && value.verified,
              "Up/Back returns stable verified published value immediately");
        Sleep(5);
    }
    Sleep(250);
    const auto after = sizes.ReadStats();
    Check(after.jobs_started == before.jobs_started && after.entries_scanned == before.entries_scanned,
          "20 hot navigation cycles cause zero new scans and zero enumerated entries");
    std::printf("[METRIC] hot_return_max_ms=%.3f jobs_delta=%llu entries_delta=%llu watches=%llu\n", slowest,
        after.jobs_started - before.jobs_started, after.entries_scanned - before.entries_scanned, after.active_watches);
    File(path / L"one.bin", 593);
    Check(Wait([&] { return Exact(sizes, path, 593); }), "real filesystem modification still updates published total");
    sizes.Stop();
}
void AutomaticBudget(const fs::path& path) {
    FolderSizes automatic; automatic.SetIndexEnabled(false);
    automatic.Sync({{path.wstring()}}, {});
    const bool deferred = Wait([&] { return automatic.GetWork(path.wstring()).activity == FolderSizeActivity::Deferred; });
    const auto work = automatic.GetWork(path.wstring());
    const auto stats = automatic.ReadStats();
    std::printf("[BUDGET] deferred=%d activity=%d entries=%llu jobs_started=%llu jobs_completed=%llu jobs_cancelled=%llu deferred_jobs=%llu\n",
        deferred, static_cast<int>(work.activity), stats.entries_scanned, stats.jobs_started,
        stats.jobs_completed, stats.jobs_cancelled, stats.deferred_jobs);
    Check(deferred, "large automatic scan reaches bounded work budget");
    Check(!automatic.Get(path.wstring()).has_value, "budget exhaustion does not publish a subtotal as the folder size");
    const auto paused = automatic.ReadStats();
    for (unsigned i = 0; i < 20; ++i) { automatic.Sync({}, {}); automatic.Sync({{path.wstring()}}, {}); }
    Sleep(300);
    Check(automatic.ReadStats().jobs_started == paused.jobs_started, "navigation does not reset the exhausted automatic budget");
    automatic.Stop();
}
void StableManualAndBudget(const fs::path& root) {
    const auto path = root / L"bulk"; fs::create_directories(path);
    File(path / L"seed.bin", 13);
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.Sync({{path.wstring()}}, {});
    Check(Wait([&] { return Exact(sizes, path, 13); }), "seed result for stable refresh");
    for (unsigned d = 0; d < 24; ++d) {
        const auto dir = path / std::to_wstring(d); fs::create_directories(dir);
        // Exercise logical per-entry counting without 24,576 payload writes.
        // NTFS permits 1,024 names per file: one seed and 1,023 hard links.
        File(dir / L"0.bin", 13);
        for (unsigned f = 1; f < 1024; ++f)
            if (!CreateHardLinkW((dir / (std::to_wstring(f) + L".bin")).c_str(), (dir / L"0.bin").c_str(), nullptr))
                throw std::runtime_error("bulk hard-link fixture creation failed");
    }
    const uint64_t total = 13 + uint64_t{24} * 1024 * 13;
    sizes.Calculate(path.wstring());
    Check(sizes.Get(path.wstring()).bytes == 13, "starting manual refresh does not erase the old published bytes");
    Check(Wait([&] { return sizes.GetWork(path.wstring()).entries > 0 || !sizes.GetWork(path.wstring()).Running(); }),
          "manual scan makes progress");
    const auto work = sizes.GetWork(path.wstring());
    Check(work.Running() && work.entries > 0 && sizes.Get(path.wstring()).bytes == 13,
          "in-flight subtotal stays separate from published result");
    sizes.Cancel(path.wstring());
    const auto cancelled_jobs = sizes.ReadStats().jobs_started;
    Sleep(250);
    Check(sizes.Get(path.wstring()).bytes == 13 && sizes.GetWork(path.wstring()).activity == FolderSizeActivity::Cancelled,
          "explicit cancellation retains previous result and exposes cancelled work state");
    Check(sizes.ReadStats().jobs_started == cancelled_jobs, "cancelled manual work is not automatically restarted");
    sizes.Calculate(path.wstring());
    sizes.Sync({}, {});
    Check(Wait([&] { return Exact(sizes, path, total); }, 30000), "explicit calculation finishes offscreen and publishes once complete");
    sizes.Sync({{path.wstring()}}, {});
    Check(Exact(sizes, path, total), "offscreen completed result is reused on return");
    sizes.Stop();
    AutomaticBudget(path);
}
void FailureAndPersistence(const fs::path& root) {
    const auto path = root / L"partial", offline = path / L"offline";
    fs::create_directories(offline); File(path / L"one.bin", 17); File(offline / L"two.bin", 91);
    Check(SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_OFFLINE) != FALSE,
          "offline fixture created without changing user folders");
    const auto cache = (root / L"results.json").wstring();
    FolderSizes sizes; sizes.SetIndexEnabled(false); sizes.SetCachePath([cache] { return cache; });
    sizes.Sync({{path.wstring()}}, {});
    Check(Wait([&] { return sizes.Get(path.wstring()).state == FolderSizeState::Partial; }), "partial result reaches a terminal state");
    const auto value = sizes.Get(path.wstring());
    Check(value.bytes == 17 && value.skipped == 1 && (value.issues & SizeOffline), "partial result includes skipped-directory reason and count");
    const auto count = sizes.ReadStats().jobs_started;
    for (unsigned i = 0; i < 20; ++i) { sizes.Sync({}, {}); sizes.Sync({{path.wstring()}}, {}); }
    Sleep(250);
    Check(sizes.ReadStats().jobs_started == count && sizes.Get(path.wstring()).bytes == 17,
          "failed coverage does not cause a retry loop on navigation");
    sizes.Stop();
    const auto saved = folder_size::ReadValues(cache);
    std::printf("[PERSISTENCE] saved_records=%zu saved_bytes=%llu saved_partial=%d saved_skipped=%llu saved_issues=%u\n",
        saved.size(), saved.empty() ? 0 : saved.front().second.bytes,
        saved.empty() ? 0 : saved.front().second.partial, saved.empty() ? 0 : saved.front().second.skipped,
        saved.empty() ? 0 : saved.front().second.issues);
    FolderSizes restored; restored.SetIndexEnabled(false); restored.SetCachePath([cache] { return cache; });
    restored.Sync({{path.wstring(), false}}, {});
    const bool loaded = Wait([&] { return restored.Get(path.wstring()).has_value; });
    const auto recovered = restored.Get(path.wstring());
    const auto started = restored.ReadStats().jobs_started;
    const bool retained = loaded && recovered.bytes == 17 && recovered.partial && recovered.skipped == 1 &&
        (recovered.issues & SizeOffline) && started == 0;
    if (!retained) std::printf("[PERSISTENCE-FAIL] loaded=%d bytes=%llu partial=%d skipped=%llu issues=%u jobs=%llu\n",
        loaded, recovered.bytes, recovered.partial, recovered.skipped, recovered.issues, started);
    Check(retained,
          "restart restores published lower bound without a background rescan");
    restored.Calculate(path.wstring());
    Check(Wait([&] { return Exact(restored, path, 108); }), "explicit retry can replace partial cached result with complete metadata scan");
    restored.Stop(); SetFileAttributesW(offline.c_str(), FILE_ATTRIBUTE_DIRECTORY);
}
uint64_t FileId(const fs::path& path) {
    const HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(file, &info) != FALSE;
    CloseHandle(file);
    return ok ? (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow : 0;
}
// Each save replaces the cache with a new file, so a new file id is a rewrite.
unsigned Rewrites(const fs::path& file, DWORD ms) {
    unsigned count = 0;
    auto last = FileId(file);
    const auto end = GetTickCount64() + ms;
    while (GetTickCount64() < end) {
        Sleep(15);
        const auto id = FileId(file);
        if (id && id != last) { ++count; last = id; }
    }
    return count;
}
void CacheWrites(const fs::path& root) {
    const auto path = root / L"quiet" / L"watched";
    fs::create_directories(path); File(path / L"one.bin", 41);
    const auto cache = root / L"quiet" / L"folder_sizes.json";
    {
        FolderSizes sizes; sizes.SetIndexEnabled(false);
        sizes.SetCachePath([file = cache.wstring()] { return file; });
        sizes.Sync({{path.wstring()}}, {path.wstring()});
        Check(Wait([&] { return Exact(sizes, path, 41); }), "cache fixture reaches its first total");
        Check(Wait([&] { return FileId(cache) != 0; }, 8000), "the first total is saved");
        const DWORD attrs = GetFileAttributesW(cache.c_str());
        Check(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_HIDDEN),
              "the cache is written as a hidden file");
        const auto id = FileId(cache);
        const auto jobs = sizes.ReadStats().jobs_completed;
        sizes.Calculate(path.wstring());
        Check(Wait([&] { return sizes.ReadStats().jobs_completed > jobs && Exact(sizes, path, 41); }),
              "a recalculation finishes with the same total");
        Sleep(3600);
        Check(FileId(cache) == id, "a recalculation with the same total does not rewrite the cache");
        File(path / L"one.bin", 77);
        Check(Wait([&] { return Exact(sizes, path, 77); }), "a real change updates the total");
        Check(Wait([&] { return FileId(cache) != id; }, 8000), "a changed total is saved");
        sizes.Stop();
    }
    Check(!fs::exists(cache.wstring() + L".tmp"), "no temporary cache file is left behind");
    // The folder holding Pulse's data folder (%LOCALAPPDATA%) on screen: each
    // save changed the data folder, whose size was recalculated and saved
    // again, every 3 s for as long as the folder stayed open.
    const auto parent = root / L"loop", data = parent / L"Pulse";
    fs::create_directories(data); File(data / L"session.json", 300);
    const auto looped = data / L"folder_sizes.json";
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    sizes.SetCachePath([file = looped.wstring()] { return file; });
    sizes.Sync({{data.wstring()}}, {parent.wstring()});
    Check(Wait([&] { return FileId(looped) != 0; }, 8000), "the data folder total is saved");
    Sleep(10000);
    const auto rewrites = Rewrites(looped, 7000);
    std::printf("[CACHE] rewrites in 7 s with the parent folder shown: %u\n", rewrites);
    Check(rewrites == 0, "the cache is not rewritten over and over while its parent folder is shown");
    sizes.Stop();
    Check(folder_size::ReadValues(looped.wstring()).empty(),
          "cache ancestors are live-only and cannot feed back into persisted totals");
    const auto stable = FileId(looped);
    FolderSizeValue value{};
    value.has_value = true; value.bytes = 999999;
    Check(folder_size::WriteValues(looped.wstring(), {{data.wstring(), value}}) && FileId(looped) == stable,
          "changing a cache ancestor total does not recreate the temporary file");
}
void Attributes(const fs::path& path, DWORD attrs) {
    if (!SetFileAttributesW(path.c_str(), attrs)) throw std::runtime_error("attribute fixture failed");
}
void EmptinessProbe(const fs::path& root) {
    // Same rule as Tab::AllowsAttributes; unknown never claims emptiness.
    const uint8_t K = FolderContentKnown, V = FolderContentVisible, H = FolderContentHidden, S = FolderContentSystem;
    Check(!FolderLooksEmpty(0, false, false) && !FolderLooksEmpty(H, false, false), "unknown content never looks empty");
    Check(FolderLooksEmpty(K, false, false) && FolderLooksEmpty(K, true, true), "probed folder without entries looks empty");
    Check(!FolderLooksEmpty(K | V, false, false) && !FolderLooksEmpty(K | V | H | S, false, false),
          "a visible child is never empty");
    Check(FolderLooksEmpty(K | H, false, false) && !FolderLooksEmpty(K | H, true, false) && !FolderLooksEmpty(K | H, true, true),
          "hidden-only folder follows show hidden files");
    Check(FolderLooksEmpty(K | S, false, false) && FolderLooksEmpty(K | S, true, false) && !FolderLooksEmpty(K | S, true, true),
          "hidden system-only folder also needs protected OS files");
    Check(!FolderLooksEmpty(K | H | S, true, false), "hidden child shows even when system children stay hidden");

    const auto base = root / L"probe";
    const auto empty = base / L"empty", hidden = base / L"hidden_only", system = base / L"system_only";
    const auto visible = base / L"visible", nested = base / L"nested", crowded = base / L"crowded";
    for (const auto& dir : {empty, hidden, system, visible, nested / L"child", crowded}) fs::create_directories(dir);
    File(hidden / L"desktop.ini", 4); Attributes(hidden / L"desktop.ini", FILE_ATTRIBUTE_HIDDEN);
    File(system / L"thumbs.db", 4); Attributes(system / L"thumbs.db", FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM);
    File(visible / L"note.txt", 4);
    for (int i = 0; i < 70; ++i) {
        const auto file = crowded / (L"h" + std::to_wstring(i));
        File(file, 1); Attributes(file, FILE_ATTRIBUTE_HIDDEN);
    }
    FolderSizes sizes; sizes.SetIndexEnabled(false);
    const std::vector<fs::path> dirs = {empty, hidden, system, visible, nested, crowded};
    const auto request = [&] {
        std::vector<std::wstring> paths;
        for (const auto& dir : dirs) paths.push_back(dir.wstring());
        paths.push_back(L"\\\\server\\share\\folder");
        sizes.Probe(std::move(paths));
    };
    const auto content = [&](const fs::path& dir) { return sizes.Content(dir.wstring()); };
    request();
    Check(Wait([&] {
        return std::all_of(dirs.begin(), dirs.end() - 1, [&](const auto& dir) { return content(dir) & K; });
    }), "visible folders are probed without a size request");
    Check(content(empty) == K, "empty folder probes as known with no children");
    Check(content(hidden) == (K | H), "hidden-only folder reports a hidden child");
    Check(content(system) == (K | S), "hidden system-only folder reports a system child");
    Check((content(visible) & V) && (content(nested) & V), "visible file or subfolder reports visible content");
    Sleep(200);
    Check(content(crowded) == 0, "folder past the hidden-entry cap stays unknown");
    Check(sizes.Content(L"\\\\server\\share\\folder") == 0, "network folder is never probed");
    Check(sizes.ReadStats().jobs_started == 0, "emptiness probes start no size scans");

    // Watched: a change inside a probed folder updates it without waiting for expiry.
    sizes.Sync({}, {base.wstring()});
    Sleep(500);
    request();
    File(empty / L"new.txt", 1);
    Check(Wait([&] { return (content(empty) & V) != 0; }, 3000), "file created in empty folder clears empty state");
    fs::remove(empty / L"new.txt");
    Check(Wait([&] { return content(empty) == K; }, 3000), "deleting the last file restores empty state");
    sizes.Stop();
}
std::vector<FolderSizeRequest> Children(const std::wstring& path) {
    std::vector<FolderSizeRequest> rows;
    WIN32_FIND_DATAW data{};
    const auto pattern = path + (path.back() == L'\\' ? L"*" : L"\\*");
    const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot enumerate authorized test directory");
    do {
        if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || !wcscmp(data.cFileName,L".") || !wcscmp(data.cFileName,L"..")) continue;
        rows.push_back({path + (path.back() == L'\\' ? L"" : L"\\") + data.cFileName});
    } while (rows.size() < 64 && FindNextFileW(find, &data));
    FindClose(find); return rows;
}
void RealMachine() {
    const std::wstring volume = L"C:\\";
    wchar_t buffer[32768]{};
    if (!GetEnvironmentVariableW(L"ProgramFiles(x86)", buffer, ARRAYSIZE(buffer))) throw std::runtime_error("ProgramFiles(x86) not configured");
    const std::wstring programs = buffer;
    const auto root_rows = Children(volume), program_rows = Children(programs);
    pulse::index::IndexFeedConnection diagnostic(nullptr);
    std::vector<pulse::index::IndexedFolderSize> diagnostic_values;
    const bool index_reply = diagnostic.FolderSizes({programs}, diagnostic_values);
    std::printf("[REAL-INDEX] protocol_reply=%d available=%d\n", index_reply,
        !diagnostic_values.empty() && diagnostic_values[0].available);
    FolderSizes sizes;
    const auto cache = (fs::current_path()/L"bench_data"/L"folder-experience-real-cache.json").wstring();
    sizes.SetCachePath([cache] { return cache; });
    sizes.Sync(root_rows, {volume}); Sleep(4000);
    const auto baseline = sizes.ReadStats();
    Check(baseline.jobs_started == 0 && baseline.active_watches == 0,
          "real C drive overview starts no full-tree scan or volume-recursive watch");
    std::map<std::wstring,FolderSizeValue> published;
    for (const auto& row : root_rows) published[row.path] = sizes.Get(row.path);
    const auto start = GetTickCount64();
    unsigned changes = 0;
    for (unsigned i = 0; i < 20; ++i) {
        sizes.Sync(program_rows, {programs}); Sleep(20);
        sizes.Sync({}, {});
        sizes.Sync(root_rows, {volume});
        for (const auto& row : root_rows) {
            const auto value = sizes.Get(row.path), previous = published[row.path];
            if (previous.has_value && (previous.bytes != value.bytes || previous.state != value.state || !value.has_value)) ++changes;
        }
        Sleep(20);
    }
    Sleep(300);
    Check(sizes.ReadStats().jobs_started == 0, "real Program Files and C drive navigation does not launch repeated recursive scans");
    Check(changes == 0, "known real overview labels stay stable through 20 navigation cycles");
    std::printf("[REAL] cycles_ms=%llu known_changes=%u index_queries=%llu watches=%llu\n",
                GetTickCount64()-start, changes, sizes.ReadStats().index_queries, sizes.ReadStats().active_watches);
    size_t known = 0;
    for (const auto& row : root_rows) {
        const auto value = sizes.Get(row.path); known += value.has_value;
        std::wprintf(L"[REAL-VALUE] %ls bytes=%llu known=%d state=%d source=%d partial=%d\n", row.path.c_str(),
                     value.bytes, value.has_value, int(value.state), int(value.source), value.partial);
    }
    std::printf("[REAL] known_root_rows=%zu total_root_rows=%zu\n",known,root_rows.size());
    const auto old = sizes.Get(programs);
    sizes.Calculate(programs);
    bool kept = true;
    const auto until = GetTickCount64() + 60000;
    while (sizes.GetWork(programs).Running() && GetTickCount64() < until) {
        const auto value = sizes.Get(programs);
        const auto work = sizes.GetWork(programs);
        if (work.Running() && old.has_value && (!value.has_value || value.bytes != old.bytes)) kept = false;
        Sleep(20);
    }
    const auto work = sizes.GetWork(programs);
    Check(kept, "real manual calculation does not replace displayed value with moving subtotals");
    if (work.Running()) {
        sizes.Cancel(programs);
        std::printf("[REAL] explicit scan cancelled at 60-second test limit; entries=%llu subtotal=%llu (not a final total)\n",work.entries,work.bytes);
    } else {
        const auto result = sizes.Get(programs);
        std::printf("[REAL] explicit scan finished bytes=%llu partial=%d skipped=%llu issues=%u\n",
                    result.bytes,result.partial,result.skipped,result.issues);
        Check(result.has_value && (result.partial || result.state == FolderSizeState::Ready), "real calculation reports completeness honestly");
    }
    const auto final = sizes.Get(programs);
    if (final.has_value) {
        const auto started = sizes.ReadStats().jobs_started;
        for (unsigned i=0; i<20; ++i) {
            sizes.Sync(program_rows,{programs}); sizes.Sync({},{}); sizes.Sync(root_rows,{volume});
            Check(sizes.Get(programs).has_value && sizes.Get(programs).bytes == final.bytes,
                  "real measured Program Files bytes survive navigation without subtotals");
        }
        Sleep(300);
        Check(sizes.ReadStats().jobs_started==started,"real measured Program Files is not rescanned on return");
    } else std::printf("[LIMIT] no completed real total available for the measured-value navigation check\n");
    sizes.Stop();
}
}
int wmain(int argc,wchar_t** argv) {
    try {
        if (argc==2 && std::wstring(argv[1])==L"--real-readonly") RealMachine();
        else if (argc == 3 && std::wstring(argv[1]) == L"--budget-only") AutomaticBudget(argv[2]);
        else {
            const auto root=fs::current_path()/L"bench_data"/(L"folder_experience_"+std::to_wstring(GetCurrentProcessId())+L"_"+std::to_wstring(GetTickCount64()));
            fs::create_directories(root);
            if (argc == 2 && std::wstring(argv[1]) == L"--probe-only") {
                EmptinessProbe(root);
            } else if (argc == 2 && std::wstring(argv[1]) == L"--cache-only") {
                CacheWrites(root);
            } else if (argc == 2 && std::wstring(argv[1]) == L"--persistence-only") {
                for (unsigned i = 0; i < 30; ++i) FailureAndPersistence(root / std::to_wstring(i));
            } else {
                Navigation(root); StableManualAndBudget(root); FailureAndPersistence(root); EmptinessProbe(root);
                CacheWrites(root);
            }
            std::wprintf(L"[FIXTURE] %ls\n",root.c_str());
        }
    } catch(const std::exception& e) {Check(false,e.what());}
    std::printf("[SUMMARY] checks=%u failures=%u\n",checks,failures);
    return failures ? 1 : 0;
}
