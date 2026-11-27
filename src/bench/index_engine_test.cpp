#include "../index/index_engine.h"
#include "../index/index_hierarchy.h"
#include "../index/usn_stream.h"
#include <cstring>
#include "../index/index_shard.h"
#include "../index/index_paths.h"
#include "../index/index_delta.h"
#include <algorithm>
#include <filesystem>
#include "../common/runtime_log.h"
#include <fstream>
#include <iostream>
#include <iterator>
#include <string_view>
#include <chrono>

namespace pulse::index {
struct UsnStreamTestAccess {
    static std::unique_ptr<UsnStream> Make(HANDLE signal) {
        return std::unique_ptr<UsnStream>(new UsnStream(signal));
    }
    static bool Push(UsnStream& s, const std::vector<BYTE>& data) { return s.packets_.Push(data.data(), data.size()); }
    static void Fail(UsnStream& s, DWORD error) { s.Failed(error); }
};

// Previous hash-map implementation, kept to check the array version.
class ReferenceFolderSizeIndex {
public:
    using Item = FolderSizeIndex::Item;
    void Reset() { valid_ = false; totals_.clear(); }
    bool Valid() const { return valid_; }
    uint64_t Builds() const { return builds_; }
    // Background builds fill a private copy that is published under the engine's
    // write lock; both outcomes count as one aggregation attempt.
    void Adopt(ReferenceFolderSizeIndex&& built) { totals_ = std::move(built.totals_); valid_ = built.valid_; ++builds_; }
    void NoteFailedBuild() { Reset(); ++builds_; }
    std::optional<uint64_t> Get(int32_t id) const {
        const auto it = totals_.find(id);
        return valid_ && it != totals_.end() ? std::optional(it->second) : std::nullopt;
    }
    template<class Read> bool Build(int32_t count, Read read) {
        Reset(); ++builds_;
        std::vector<uint64_t> sums(static_cast<size_t>(count));
        std::vector<uint32_t> children(static_cast<size_t>(count));
        std::vector<int32_t> queue;
        queue.reserve(static_cast<size_t>(count));
        size_t live = 0;
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive) continue;
            ++live; sums[i] = n.directory ? 0 : n.bytes;
            if (n.parent >= count || n.parent == i) return false;
            if (n.parent >= 0 && read(n.parent).alive) {
                if (!read(n.parent).directory) return false;
                ++children[n.parent];
            }
        }
        for (int32_t i = 0; i < count; ++i) if (read(i).alive && !children[i]) queue.push_back(i);
        for (size_t at = 0; at < queue.size(); ++at) {
            const auto i = queue[at]; const auto n = read(i);
            if (n.directory) totals_[i] = sums[i];
            if (n.parent >= 0 && read(n.parent).alive) {
                if (UINT64_MAX - sums[n.parent] < sums[i]) { Reset(); return false; }
                sums[n.parent] += sums[i];
                if (--children[n.parent] == 0) queue.push_back(n.parent);
            }
        }
        valid_ = queue.size() == live;
        if (!valid_) totals_.clear();
        return valid_;
    }
    template<class Read> void Replace(int32_t id, Item before, Item after, Read read) {
        if (!valid_) return;
        if (before.alive && after.alive && before.directory != after.directory) { Reset(); return; }
        const uint64_t old_size = before.directory ? Get(id).value_or(0) : before.bytes;
        const uint64_t new_size = after.directory ? (before.alive ? old_size : 0) : after.bytes;
        auto adjust = [&](int32_t parent, uint64_t bytes, bool add) {
            // Malformed chains must never hang the index service.
            for (size_t depth = 0; parent >= 0; ++depth) {
                if (depth > totals_.size()) return false;
                const auto n = read(parent);
                if (!n.alive) return true;
                const auto it = totals_.find(parent);
                if (it == totals_.end()) return false;
                auto& total = it->second;
                if (add ? UINT64_MAX - total < bytes : total < bytes) return false;
                if (add) total += bytes; else total -= bytes;
                parent = n.parent;
            }
            return true;
        };
        if ((before.alive && !adjust(before.parent, old_size, false)) ||
            (after.alive && !adjust(after.parent, new_size, true))) { Reset(); return; }
        if (after.alive && after.directory) totals_[id] = new_size;
        else totals_.erase(id);
    }
private:
    bool valid_ = false;
    uint64_t builds_ = 0;
    std::unordered_map<int32_t, uint64_t> totals_;
};

struct ChangeTrackerTestAccess {
    struct Stats { size_t records = 0, record_bytes = 0, string_bytes = 0, summaries = 0, summary_bytes = 0; };
    static void Backdate(ChangeTracker& tracker, const std::wstring& owner, uint64_t start, uint64_t expiry) {
        std::lock_guard lock(tracker.mutex_);
        auto& j = tracker.journals_[owner];
        j.start = start; j.tracking_since = start; j.expiry = expiry;
    }
    static Stats Measure(ChangeTracker& tracker) {
        auto heap = [](const std::wstring& s) { return s.capacity() > 7 ? (s.capacity() + 1) * sizeof(wchar_t) : 0; };
        Stats out;
        std::lock_guard lock(tracker.mutex_);
        for (const auto& [owner, j] : tracker.journals_) {
            out.records += j.records.size();
            out.record_bytes += j.records.capacity() * sizeof(ChangeRecord);
            for (const auto& e : j.records) out.string_bytes += heap(e.path) + heap(e.old_path);
            out.summaries += j.summaries.size();
            out.summary_bytes += j.summaries.bucket_count() * sizeof(void*) +
                j.summaries.size() * (sizeof(std::pair<const std::wstring, ChangeSummary>) + 2 * sizeof(void*));
            for (const auto& [path, summary] : j.summaries) out.summary_bytes += heap(path) + heap(summary.path);
        }
        return out;
    }
    // Order-sensitive digest of one journal's history and lifecycle state,
    // loading it from disk first when this tracker has not read it yet.
    static uint64_t Digest(ChangeTracker& tracker, const std::wstring& owner, size_t& records) {
        std::lock_guard lock(tracker.mutex_);
        auto& j = tracker.journals_[owner];
        tracker.Load(owner, j);
        uint64_t h = 1469598103934665603ull;
        auto mix = [&](const void* p, size_t n) {
            const auto* b = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
        };
        auto u64 = [&](uint64_t v) { mix(&v, sizeof(v)); };
        auto str = [&](const std::wstring& s) { u64(s.size()); mix(s.data(), s.size() * sizeof(wchar_t)); };
        u64(j.tracking_since); u64(j.paused_at); u64(j.gap_end); u64(j.gap_until); u64(j.active ? 1 : 0);
        for (const auto& e : j.records) {
            u64(e.id); u64(e.time); u64(e.file_id); u64(static_cast<uint64_t>(e.kind)); u64(e.is_dir ? 1 : 0);
            str(e.path); str(e.old_path);
        }
        records = j.records.size();
        return h;
    }
};

struct EngineTestAccess {
    static bool RegexFixture() {
        const auto base = std::filesystem::current_path() / L"bench_data" / L"advanced-search-regex-20261124" /
                          (L"engine-" + std::to_wstring(GetCurrentProcessId()));
        const auto files = base / L"files";
        std::filesystem::create_directories(files / L"nested");
        std::filesystem::create_directories(base / L"index");
        for (const auto* name : {L"aa(2022)xx.txt", L"aa[2022]xx.txt", L"aa(2023)xx.txt", L"AA(2022)xx.jpg",
                                 L"报告 2022.txt"}) std::ofstream(files / name) << "regex fixture";
        std::ofstream(files / L"nested" / L"aa(2022)xx.txt") << "nested";
        SetActiveIndexDirectory((base / L"index").wstring());
        bool ok = true;
        auto check = [&](bool passed, const char* label) {
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= passed;
        };
        {
            Engine engine;
            engine.fixture_root_ = files.wstring();
            engine.running_ = true;
            engine.FullRebuild("regex_fixture");
            Query query;
            query.limit = 100;
            query.needle = LR"(regex:"^aa[([]2022[)\]]xx\.txt$")";
            auto result = engine.Search(query);
            check(result.total == 3 && result.hits.size() == 3,
                  "production filesystem scan and Engine::Search find both bracket styles and nested file");
            query.needle = LR"(regex:"^aa[([]2022[)\]]xx\.(txt|jpg)$")";
            check(engine.Search(query).total == 4, "regex widening bypasses literal narrowing cache and ignores case");
            query.needle = LR"(regex:"^aa[([]2022[)\]]xx\.(txt|jpg)$" ext:txt)";
            check(engine.Search(query).total == 3, "production engine composes regex and extension filter");
            query.path_prefix = (files / L"nested").wstring();
            check(engine.Search(query).total == 1, "production engine restricts regex to selected folder");
            query.path_prefix.clear();
            query.needle = LR"(regex:"^报告 \d{4}\.txt$")";
            check(engine.Search(query).total == 1, "production engine matches Unicode and digit class");
            for (const auto* raw : {LR"(regex:"[")", LR"(!regex:"[")", LR"(regex:"[" | aa)"}) {
                query.needle = raw;
                check(engine.Search(query).total == 0, "invalid native regex returns no results, including NOT and OR");
            }
            query.needle = LR"(regex:"^aa")";
            std::atomic<uint32_t> generation{2};
            check(engine.Search(query, &generation, 1).hits.empty(), "stale regex search observes generation cancellation");
            query.needle = LR"(regex:"no-such-file")";
            check(engine.Search(query).total == 0, "valid regex with no matching files returns empty results");
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        return ok;
    }

    static bool ExclusionConcurrencyFixture() {
        SetMachineIndexScope(false);
        Engine engine;
        engine.fixture_root_ = L"C:\\Pulse-exclusion-test-not-enumerated";
        engine.running_ = false;
        const std::vector<std::wstring> snapshot(2048, L"C:\\excluded-fixture");
        engine.excluded_paths_ = snapshot;
        std::atomic<bool> entered{false}, finished{false};
        bool preserved = false;
        std::thread rebuild;
        {
            std::shared_lock lock(engine.mutex_);
            rebuild = std::thread([&] {
                entered = true;
                engine.FullRebuild("cold_start");
                finished = true;
            });
            while (!entered) std::this_thread::yield();
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
            preserved = !finished && engine.excluded_paths_ == snapshot &&
                engine.IsExcludedPath(L"C:\\excluded-fixture\\child.txt");
        }
        rebuild.join();
        const bool replaced = finished && engine.excluded_paths_.empty();
        std::atomic<bool> stop{false}, invalid{false};
        std::atomic<unsigned> reads{0};
        auto reader = [&] {
            while (!stop) {
                {
                    std::shared_lock lock(engine.mutex_);
                    const auto size = engine.excluded_paths_.size();
                    const bool excluded = engine.IsExcludedPath(L"C:\\excluded-fixture\\child.txt");
                    if ((size != 0 && size != snapshot.size()) || excluded != (size != 0)) invalid = true;
                    ++reads;
                }
                std::this_thread::yield();
            }
        };
        std::thread first(reader), second(reader);
        while (reads < 100) std::this_thread::yield();
        for (int i = 0; i < 100; ++i) {
            {
                std::unique_lock lock(engine.mutex_);
                engine.excluded_paths_ = snapshot;
            }
            // Exercise the production configuration replacement, with scanning
            // cancelled so this fixture never enumerates a user's volume.
            engine.FullRebuild("cold_start");
        }
        stop = true;
        first.join(); second.join();
        std::cout << (preserved ? "[PASS] " : "[FAIL] ") << "production rebuild cannot replace exclusions held by an active reader\n";
        std::cout << (replaced ? "[PASS] " : "[FAIL] ") << "production replacement completes after reader releases its lock\n";
        std::cout << (!invalid && reads >= 100 ? "[PASS] " : "[FAIL] ")
            << "two concurrent readers see coherent exclusions across 100 production replacements; reads=" << reads << '\n';
        return preserved && replaced && !invalid && reads >= 100;
    }
    static bool AuditQueryFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << "\n";
            ok = value && ok;
        };
        const auto root = std::filesystem::absolute(std::filesystem::path("bench_data") /
            ("index-audit-" + std::to_string(GetCurrentProcessId())));
        std::filesystem::create_directories(root);
        const auto cache = root / "valid.bin";
        {
            Engine fixture;
            fixture.AddForTest(L"C:\\fixture\\paper.pdf", L"paper.pdf", false, 5);
            fixture.AddForTest(L"C:\\fixture\\foobar", L"foobar", false, 10);
            fixture.AddForTest(L"C:\\fixture\\1.txt", L"1.txt", false, 1);
            fixture.AddForTest(L"C:\\fixture\\12.txt", L"12.txt", false, 12);
            fixture.AddForTest(L"C:\\other\\anchor.bin", L"anchor.bin", false, 3);
            fixture.ready_ = true;
            check(Save(fixture, cache.wstring()), "save isolated mapped-query fixture");
        }
        {
            Engine incremental, cold;
            check(Load(incremental, cache.wstring()) && Load(cold, cache.wstring()), "load production mapped fixture");
            for (const auto& pair : std::vector<std::pair<std::wstring, std::wstring>>{
                    {L"ext:p", L"ext:pdf"}, {L"size:<1", L"size:<10"},
                    {L"\"foo", L"\"foobar"}, {L"!1", L"!12"}, {L"p", L"pa"},
                    {L"ext:txt;p", L"ext:txt;pdf"}, {L"foo|p", L"foo|pa"}}) {
                Query q; q.needle = pair.first; q.limit = 100; q.rank = false;
                incremental.InvalidateFilterLocked(); incremental.Search(q);
                q.needle = pair.second;
                cold.InvalidateFilterLocked();
                auto warm = incremental.Search(q), fresh = cold.Search(q);
                std::vector<std::wstring> a, b;
                for (const auto& hit : warm.hits) a.push_back(hit.path);
                for (const auto& hit : fresh.hits) b.push_back(hit.path);
                check(warm.total == fresh.total && a == b, "incremental query equals cold mapped query");
            }
            incremental.AddForTest(L"C:\\fixture\\paper.pdf", L"paper.pdf", false, 7);
            for (const auto& needle : {L"\"paper.pdf\"", L"p", L"nopinyin: paper"}) {
                Query q; q.needle = needle; q.limit = 100; q.rank = false;
                auto result = incremental.Search(q);
                check(result.total == 1 && result.hits.size() == 1, "patched base hit appears once");
            }
            const std::wstring old_name = L"fixture\\paper.pdf", new_name = L"other\\paper.pdf";
            const size_t first_bytes = (offsetof(FILE_NOTIFY_INFORMATION, FileName) + old_name.size() * sizeof(wchar_t) + 3) & ~size_t{3};
            std::vector<BYTE> move(first_bytes + offsetof(FILE_NOTIFY_INFORMATION, FileName) + new_name.size() * sizeof(wchar_t));
            auto old_event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(move.data());
            old_event->NextEntryOffset = static_cast<DWORD>(first_bytes);
            old_event->Action = FILE_ACTION_RENAMED_OLD_NAME;
            old_event->FileNameLength = static_cast<DWORD>(old_name.size() * sizeof(wchar_t));
            memcpy(old_event->FileName, old_name.data(), old_event->FileNameLength);
            auto new_event = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(move.data() + first_bytes);
            new_event->Action = FILE_ACTION_RENAMED_NEW_NAME;
            new_event->FileNameLength = static_cast<DWORD>(new_name.size() * sizeof(wchar_t));
            memcpy(new_event->FileName, new_name.data(), new_event->FileNameLength);
            incremental.ApplyNotifyLocked(L"C:\\", move.data(), static_cast<DWORD>(move.size()));
            Query moved; moved.needle = L"ext:pdf"; moved.path_prefix = L"C:\\fixture";
            check(incremental.Search(moved).total == 0, "moved mapped node excluded from old DFS interval");
            moved.path_prefix = L"C:\\other";
            check(incremental.Search(moved).total == 1, "generic scoped scan finds node moved into another DFS interval");
            std::wstring deep = L"C:\\deep";
            for (int i = 0; i < 90; ++i) deep += L"\\d";
            incremental.AddForTest(deep, L"d", true);
            const auto id = incremental.ResolvePathLocked(deep);
            check(id >= 0 && incremental.BuildPathLocked(id) == deep, "deep path preserves drive and all ancestors");
        }
        {
            const auto file = root / L"attributes.dat";
            std::ofstream(file, std::ios::binary) << "1";
            Engine attributes;
            attributes.AddForTest(file.wstring(), L"attributes.dat", false, 1);
            attributes.ready_ = true;
            Query q; q.needle = L"attributes size:<5";
            check(attributes.Search(q).total == 1, "attribute filter cache seeded");
            std::ofstream(file, std::ios::binary | std::ios::trunc) << "1234567890";
            const std::wstring relative = file.filename().wstring();
            std::vector<BYTE> packet(offsetof(FILE_NOTIFY_INFORMATION, FileName) + relative.size() * sizeof(wchar_t));
            auto notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
            notification->Action = FILE_ACTION_MODIFIED;
            notification->FileNameLength = static_cast<DWORD>(relative.size() * sizeof(wchar_t));
            memcpy(notification->FileName, relative.data(), notification->FileNameLength);
            attributes.ApplyNotifyLocked(root.wstring(), packet.data(), static_cast<DWORD>(packet.size()));
            check(attributes.Search(q).total == 0, "actual file attribute notification invalidates cached size filter");
        }
        std::ifstream input(cache, std::ios::binary);
        std::vector<char> original((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        input.close();
        if (original.size() >= sizeof(DiskHeader)) {
            DiskHeader header{}; memcpy(&header, original.data(), sizeof(header));
            const size_t offset = static_cast<size_t>(header.prefix1_off & ~(1ull << 63));
            for (int scenario = 0; scenario < 3; ++scenario) {
                auto damaged = original;
                const uint32_t value = scenario == 0 ? UINT32_MAX : scenario == 1 ? 1u : header.node_count;
                const size_t at = scenario == 0 ? offset + 98 * sizeof(uint32_t) : scenario == 1 ? offset :
                    offset + 65537 * sizeof(uint32_t);
                if (at + sizeof(value) > damaged.size()) { check(false, "bad-cache fixture bounds"); continue; }
                memcpy(damaged.data() + at, &value, sizeof(value));
                const auto path = root / ("bad-" + std::to_string(scenario) + ".bin");
                std::ofstream output(path, std::ios::binary); output.write(damaged.data(), static_cast<std::streamsize>(damaged.size())); output.close();
                check(!ValidateCache(path.wstring()), "production loader rejects malformed bucket or posting ID");
            }
        } else check(false, "mapped cache contains header");
        std::filesystem::remove_all(root);
        return ok;
    }
    static bool ValidateCache(const std::wstring& path) {
        Engine engine;
        std::unique_ptr<Engine::MappedFile> mapped;
        const bool valid = engine.MapIndexFile(path, mapped);
        std::cout << "cache_valid=" << valid << "\n";
        return valid;
    }
    static bool RecoverCache(const std::wstring& source, const std::wstring& destination) {
        if (std::filesystem::exists(destination) || ValidateCache(source)) return false;
        std::filesystem::create_directories(destination);
        SetActiveIndexDirectory(destination);
        SetMachineIndexScope(true);
        Engine engine;
        engine.running_ = true;
        engine.ResolveIndexDirFrn();
        const auto started = std::chrono::steady_clock::now();
        engine.FullRebuild("hierarchy_recovery_fixture");
        const int32_t count = engine.LiveCount();
        const bool valid = engine.ready_ && count > 0 && engine.map_ &&
            ValidateIndexHierarchy(count, [&](int32_t i) { return engine.NodeAt(i); });
        FolderSizeIndex sizes;
        const bool totals = valid && sizes.Build(count, [&](int32_t i) { return engine.FolderSizeItem(i); });
        std::cout << "recovered_nodes=" << count << " hierarchy_valid=" << valid
            << " folder_totals_valid=" << totals << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << "\n";
        engine.Stop();
        return valid && totals;
    }
    // Real-data full-merge peak (P0-E). Copies an index directory to an isolated
    // temp folder, loads it like the service, runs one forced MergeBase and
    // prints the stage samples plus the process peak commit. The source
    // directory is only read; the copy is removed afterwards.
    // Fixed query mix for --merge-peak-probe: best-of-3 latency and a hash of
    // total + returned paths, so two builds can be compared on the same data.
    static void ProbeQueries(Engine& engine) {
        struct ProbeQuery { const char* label; const wchar_t* needle; ResultSort sort; bool desc; const wchar_t* prefix; };
        const ProbeQuery queries[]{
            {"readme", L"readme", ResultSort::Index, false, L""},
            {"x", L"x", ResultSort::Index, false, L""},
            {"dll_size_desc", L".dll", ResultSort::Size, true, L""},
            {"pulse_name", L"pulse", ResultSort::Name, false, L""},
            {"cjk_weixin", L"\u5fae\u4fe1", ResultSort::Index, false, L""},
            {"ext_png_mtime_desc", L"ext:png", ResultSort::Mtime, true, L""},
            {"folder_src", L"folder: src", ResultSort::Index, false, L""},
            {"log_in_users", L"log", ResultSort::Index, false, L"C:\\Users"},
            {"zz", L"zz", ResultSort::Index, false, L""}};
        constexpr size_t count = std::size(queries);
        std::vector<double> best(count, 1e300);
        std::vector<SearchResult> last(count);
        for (int round = 0; round < 3; ++round) {
            for (size_t i = 0; i < count; ++i) {
                Query q;
                q.needle = queries[i].needle;
                q.sort = queries[i].sort;
                q.sort_desc = queries[i].desc;
                q.path_prefix = queries[i].prefix;
                const auto started = std::chrono::steady_clock::now();
                last[i] = engine.Search(q);
                best[i] = (std::min)(best[i], std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - started).count());
            }
        }
        for (size_t i = 0; i < count; ++i) {
            uint64_t hash = 1469598103934665603ull;
            auto mix = [&](const void* data, size_t bytes) {
                const auto* p = static_cast<const unsigned char*>(data);
                for (size_t k = 0; k < bytes; ++k) hash = (hash ^ p[k]) * 1099511628211ull;
            };
            const uint64_t total = last[i].total;
            mix(&total, sizeof(total));
            for (const auto& hit : last[i].hits) mix(hit.path.data(), (hit.path.size() + 1) * sizeof(wchar_t));
            std::cout << "query=" << queries[i].label << " total=" << total << " hits=" << last[i].hits.size()
                << " unchecked=" << last[i].unchecked_dirs.size() << " best_us="
                << static_cast<uint64_t>(best[i] * 1000.0) << " hash=" << std::hex << hash << std::dec << "\n";
        }
    }
    static bool MergePeakProbe(const std::wstring& source) {
        namespace fs = std::filesystem;
        if (!SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", L"1")) return false;
        if (!fs::is_directory(source)) return false;
        const fs::path dir = fs::temp_directory_path() /
            (L"pulse-merge-peak-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        ec.clear();
        fs::copy(source, dir, fs::copy_options::recursive, ec);
        if (ec) {
            std::cout << "copy_failed=" << ec.value() << "\n";
            fs::remove_all(dir, ec);
            return false;
        }
        auto memory = [] {
            PROCESS_MEMORY_COUNTERS_EX c{};
            c.cb = sizeof(c);
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c));
            return c;
        };
        auto cpu_ms = [] {
            FILETIME created{}, exited{}, kernel{}, user{};
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
            auto value = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
            return (value(kernel) + value(user)) / 10000;
        };
        bool committed = false;
        {
            SetActiveIndexDirectory(dir.wstring());
            SetMachineIndexScope(true);
            Engine engine;
            const bool loaded = engine.TryLoadCache();
            for (int i = 0; loaded && i < 1200 && !engine.pinyin_ready_.load(); ++i) Sleep(50);
            std::cout << "pinyin_ready=" << engine.pinyin_ready_.load() << "\n";
            if (loaded) ProbeQueries(engine);
            const auto before = memory();
            const uint64_t cpu_before = cpu_ms();
            const auto started = std::chrono::steady_clock::now();
            if (loaded) engine.MergeBase(true, "probe");
            const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            const uint64_t cpu_used = cpu_ms() - cpu_before;
            const auto after = memory();
            committed = loaded && engine.merge_failures_ == 0;
            std::cout << "loaded=" << loaded << " committed=" << committed << " nodes=" << engine.LiveCount() << "\n"
                << "merge_wall_ms=" << wall << " merge_cpu_ms=" << cpu_used << "\n"
                << "private_before=" << before.PrivateUsage << " private_after=" << after.PrivateUsage
                << " peak_before=" << before.PeakPagefileUsage << " peak_after=" << after.PeakPagefileUsage
                << " peak_working_set=" << after.PeakWorkingSetSize << "\n"
                << "memory_json=" << engine.filename_timing_.Memory().Json() << "\n";
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        wchar_t keep[MAX_PATH]{};
        const DWORD keep_length = GetEnvironmentVariableW(L"PULSE_PROBE_KEEP_BASE", keep, MAX_PATH);
        if (keep_length > 0 && keep_length < MAX_PATH) {
            fs::path newest;
            fs::file_time_type newest_time{};
            for (const auto& entry : fs::recursive_directory_iterator(dir / L"v9", ec)) {
                const auto name = entry.path().filename().wstring();
                if (!entry.is_regular_file() || (name != L"base-a.bin" && name != L"base-b.bin")) continue;
                const auto time = entry.last_write_time();
                if (newest.empty() || time > newest_time) { newest = entry.path(); newest_time = time; }
            }
            const bool kept = !newest.empty() && fs::copy_file(newest, keep, fs::copy_options::overwrite_existing, ec);
            std::cout << "kept_base=" << kept << "\n";
        }
        std::cout << "legacy_shards_present=" << fs::exists(dir / L"v9" / L"volumes", ec) << "\n";
        {
            std::ifstream timing(dir / L"pulse-index-timing.jsonl");
            for (std::string line; std::getline(timing, line);) std::cout << "timing=" << line << "\n";
        }
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec)) {
            if (entry.is_regular_file())
                std::wcout << L"file=" << fs::relative(entry.path(), dir).wstring() << L" bytes=" << entry.file_size() << L"\n";
        }
        ec.clear();
        fs::remove_all(dir, ec);
        std::cout << "cleanup_error=" << ec.value() << "\n";
        return committed;
    }
    // Cold-load replay cost (P0-F). Copies an index directory, appends one
    // deterministic synthetic WAL per volume (adds under real folders, attribute
    // and name patches, file tombstones, USN checkpoints) and times TryLoadCache
    // on a fresh engine. The fixed seed yields the same final state, so two
    // builds can be compared by the printed counts and query hashes.
    static bool ReplayProbe(const std::wstring& source, uint32_t records) {
        namespace fs = std::filesystem;
        if (!fs::is_directory(source) || records == 0) return false;
        const fs::path dir = fs::temp_directory_path() /
            (L"pulse-replay-probe-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        ec.clear();
        fs::copy(source, dir, fs::copy_options::recursive, ec);
        if (ec) {
            std::cout << "copy_failed=" << ec.value() << "\n";
            fs::remove_all(dir, ec);
            return false;
        }
        SetActiveIndexDirectory(dir.wstring());
        SetMachineIndexScope(true);
        struct Range {
            std::wstring volume_id;
            uint64_t journal_id = 0;
            int64_t next_usn = 0;
            int32_t root = -1;
            std::vector<int32_t> dirs, files, added_dirs, added_files;
        };
        std::vector<Range> ranges;
        uint64_t built = 0;
        int32_t base_count = 0;
        bool ok = false;
        {
            Engine engine;
            ok = engine.TryLoadCache();
            if (ok) {
                built = engine.built_unix_;
                base_count = engine.LiveCount();
                for (const auto& v : engine.vols_) {
                    Range r;
                    r.volume_id = v.volume_id;
                    r.journal_id = v.journal_id;
                    r.next_usn = v.next_usn;
                    r.root = v.root_idx;
                    const int64_t first = v.root_idx;
                    const int64_t end = (std::min)(first + static_cast<int64_t>(v.item_count),
                                                   static_cast<int64_t>(base_count));
                    for (int64_t i = first; first >= 0 && i < end; ++i) {
                        const auto id = static_cast<int32_t>(i);
                        if (engine.IsTomb(id)) continue;
                        ((engine.NodeAt(id).flags & Engine::kFlagDir) ? r.dirs : r.files).push_back(id);
                    }
                    if (!r.dirs.empty()) ranges.push_back(std::move(r));
                }
            }
            engine.Stop();
        }
        uint64_t total_weight = 0;
        for (const auto& r : ranges) total_weight += r.dirs.size() + r.files.size();
        ok = ok && total_weight > 0;
        std::vector<std::unique_ptr<DeltaLog>> logs;
        for (size_t k = 0; ok && k < ranges.size(); ++k) {
            logs.push_back(std::make_unique<DeltaLog>());
            ok = logs.back()->Open(DeltaFilePathForVolume(ranges[k].volume_id), built);
        }
        uint64_t seed = 0x9E3779B97F4A7C15ull;
        auto next = [&] {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            return seed;
        };
        int32_t next_idx = base_count;
        uint64_t adds = 0, attr_patches = 0, name_patches = 0, tombs = 0;
        for (uint32_t n = 0; ok && n < records; ++n) {
            uint64_t pick = next() % total_weight;
            size_t k = 0;
            while (pick >= ranges[k].dirs.size() + ranges[k].files.size()) {
                pick -= ranges[k].dirs.size() + ranges[k].files.size();
                ++k;
            }
            Range& r = ranges[k];
            DeltaLog& wal = *logs[k];
            auto any_node = [&]() -> int32_t {
                uint64_t i = next() % (r.dirs.size() + r.files.size() + r.added_dirs.size() + r.added_files.size());
                if (i < r.dirs.size()) return r.dirs[i];
                i -= r.dirs.size();
                if (i < r.files.size()) return r.files[i];
                i -= r.files.size();
                if (i < r.added_dirs.size()) return r.added_dirs[i];
                return r.added_files[i - r.added_dirs.size()];
            };
            const uint64_t kind = next() % 100;
            if (kind < 35) {
                const bool folder = next() % 10 == 0;
                const int32_t parent = (!r.added_dirs.empty() && next() % 5 == 0)
                    ? r.added_dirs[next() % r.added_dirs.size()] : r.dirs[next() % r.dirs.size()];
                const std::wstring name = folder ? L"probe_dir_" + std::to_wstring(n)
                                                 : L"probe_" + std::to_wstring(n) + L".tmp";
                wal.QueueAdd(next_idx, parent, static_cast<uint8_t>(folder ? Engine::kFlagDir : 0),
                             1700000000u + n, folder ? 0 : n * 7ull, name, 0x7000000000000000ull + n);
                (folder ? r.added_dirs : r.added_files).push_back(next_idx++);
                ++adds;
            } else if (kind < 80) {
                wal.QueuePatch(any_node(), static_cast<uint8_t>(PatchBits::Attr), 0, 0,
                               1710000000u + n, n * 13ull, {});
                ++attr_patches;
            } else if (kind < 85) {
                const int32_t node = any_node();
                if (node == r.root) continue;
                const std::wstring name = L"renamed_" + std::to_wstring(n) + L".txt";
                wal.QueuePatch(node, static_cast<uint8_t>(PatchBits::Name), 0, 0, 0, 0, name);
                ++name_patches;
            } else {
                auto& pool = (!r.added_files.empty() && next() % 2 == 0) ? r.added_files : r.files;
                if (pool.empty()) continue;
                wal.QueueTomb(pool[next() % pool.size()]);
                ++tombs;
            }
            if ((n & 4095) == 4095) wal.QueueUsn(r.journal_id, r.next_usn + n);
            if ((n & 65535) == 65535)
                for (auto& l : logs) ok = l->Flush() && ok;
        }
        for (auto& l : logs) {
            ok = l->Flush() && ok;
            l->Close();
        }
        uint64_t wal_bytes = 0;
        for (const auto& entry : fs::recursive_directory_iterator(dir, ec))
            if (entry.is_regular_file() && entry.path().extension() == L".log") wal_bytes += entry.file_size();
        std::cout << "wal_ok=" << ok << " volumes=" << ranges.size() << " base_nodes=" << base_count
            << " records=" << records << " adds=" << adds << " attr_patches=" << attr_patches
            << " name_patches=" << name_patches << " tombs=" << tombs << " wal_bytes=" << wal_bytes << "\n";
        bool loaded = false;
        if (ok) {
            auto thread_cpu_ms = [] {
                FILETIME created{}, exited{}, kernel{}, user{};
                GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
                auto value = [](FILETIME t) { return (static_cast<uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
                return (value(kernel) + value(user)) / 10000;
            };
            Engine engine;
            const uint64_t cpu_before = thread_cpu_ms();
            const auto started = std::chrono::steady_clock::now();
            loaded = engine.TryLoadCache();
            const auto wall = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            std::cout << "loaded=" << loaded << " load_wall_ms=" << wall << " load_thread_cpu_ms="
                << thread_cpu_ms() - cpu_before << " live=" << engine.LiveCount() << " indexed="
                << engine.indexed_.load() << " deleted=" << engine.deleted_ << "\n";
            for (int i = 0; loaded && i < 1200 && !engine.pinyin_ready_.load(); ++i) Sleep(50);
            if (loaded) {
                ProbeQueries(engine);
                for (const wchar_t* needle : {L"probe_", L"renamed_", L"probe_dir_"}) {
                    Query q;
                    q.needle = needle;
                    const SearchResult result = engine.Search(q);
                    uint64_t hash = 1469598103934665603ull;
                    for (const auto& hit : result.hits)
                        for (wchar_t c : hit.path) hash = (hash ^ static_cast<uint16_t>(c)) * 1099511628211ull;
                    std::wcout << L"query=" << needle << L" total=" << result.total << L" hits=" << result.hits.size()
                        << L" hash=" << std::hex << hash << std::dec << L"\n";
                }
            }
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        ec.clear();
        fs::remove_all(dir, ec);
        std::cout << "cleanup_error=" << ec.value() << "\n";
        return ok && loaded;
    }
    // --usn-probe: write sessions on real C: files replayed as journal records
    // into a copy of a real index under the exclusive lock, as the worker does.
    // A session logs k records with cumulative reasons and eight sessions are
    // interleaved per batch. Mode "record" applies every record; mode "batch"
    // goes through ApplyUsnBatchLocked. Both report per-target end states.
    static bool UsnProbeMode(const std::wstring& source, uint32_t sessions, uint32_t k, bool batch,
                             std::vector<uint64_t>& states) {
        namespace fs = std::filesystem;
        const fs::path dir = fs::temp_directory_path() /
            (L"pulse-usn-probe-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        ec.clear();
        fs::copy(source, dir, fs::copy_options::recursive, ec);
        if (ec) {
            std::cout << "copy_failed=" << ec.value() << "\n";
            fs::remove_all(dir, ec);
            return false;
        }
        SetActiveIndexDirectory(dir.wstring());
        SetMachineIndexScope(true);
        bool ok = false;
        {
            Engine engine;
            ok = engine.TryLoadCache();
            for (int i = 0; ok && i < 1200 && !engine.pinyin_ready_.load(); ++i) Sleep(50);
            Engine::VolState* volume = nullptr;
            for (auto& v : engine.vols_)
                if ((v.letter == L'C' || v.letter == L'c') && v.frn_base && v.frn_base_n) volume = &v;
            ok = ok && volume;
            std::vector<std::vector<BYTE>> blobs;
            std::vector<int32_t> targets;
            size_t records = 0;
            if (ok) {
                const int32_t base = engine.BaseCount();
                std::vector<uint64_t> frn_of(static_cast<size_t>(base), 0);
                for (uint32_t i = 0; i < volume->frn_base_n; ++i) {
                    const DiskFrn& entry = volume->frn_base[i];
                    if (entry.idx >= 0 && entry.idx < base) frn_of[static_cast<size_t>(entry.idx)] = entry.frn;
                }
                std::vector<int32_t> files;
                for (uint32_t i = 0; i < volume->frn_base_n; ++i) {
                    const int32_t id = volume->frn_base[i].idx;
                    if (id < 0 || id >= base || engine.IsTomb(id)) continue;
                    const Node n = engine.NodeAt(id);
                    if ((n.flags & (Engine::kFlagDir | Engine::kFlagHidden)) || n.parent < 0 ||
                        n.parent >= base || !frn_of[static_cast<size_t>(n.parent)]) continue;
                    files.push_back(id);
                }
                constexpr uint32_t kPerBatch = 8;
                constexpr DWORD kReasons[3] = {USN_REASON_DATA_OVERWRITE,
                    USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND,
                    USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND | USN_REASON_CLOSE};
                uint64_t seed = 0x2545F4914F6CDD1Dull;
                FILETIME now{}; GetSystemTimeAsFileTime(&now);
                for (uint32_t first = 0; first < sessions && !files.empty(); first += kPerBatch) {
                    std::vector<int32_t> batch_files;
                    for (uint32_t s = first; s < std::min(sessions, first + kPerBatch); ++s) {
                        seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                        batch_files.push_back(files[seed % files.size()]);
                    }
                    std::vector<BYTE> blob;
                    for (uint32_t r = 0; r < k; ++r) {
                        const DWORD reason = r + 1 == k ? kReasons[2] : kReasons[std::min<uint32_t>(r, 1)];
                        for (const int32_t id : batch_files) {
                            const std::wstring_view name = engine.NameOf(id);
                            const auto length = static_cast<WORD>(name.size() * sizeof(wchar_t));
                            const size_t size = (sizeof(USN_RECORD_V2) + length + 7) & ~size_t{7};
                            const size_t at = blob.size();
                            blob.resize(at + size);
                            auto* rec = reinterpret_cast<USN_RECORD_V2*>(blob.data() + at);
                            rec->RecordLength = static_cast<DWORD>(size);
                            rec->MajorVersion = 2;
                            rec->FileReferenceNumber = frn_of[static_cast<size_t>(id)];
                            rec->ParentFileReferenceNumber = frn_of[static_cast<size_t>(engine.NodeAt(id).parent)];
                            rec->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
                            rec->FileNameLength = length;
                            rec->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
                            rec->Reason = reason;
                            rec->TimeStamp.QuadPart = (static_cast<int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
                            memcpy(blob.data() + at + rec->FileNameOffset, name.data(), length);
                            ++records;
                        }
                    }
                    targets.insert(targets.end(), batch_files.begin(), batch_files.end());
                    blobs.push_back(std::move(blob));
                }
                ok = !blobs.empty();
            }
            if (ok) {
                engine.running_ = true;
                if (volume->journal_id == 0) volume->journal_id = 1;
                std::unique_lock lock(engine.mutex_);
                auto cycles = [] { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return c; };
                const char* mode = batch ? "batch" : "record";
                const double count = static_cast<double>(targets.size());
                auto report = [&](const char* label, uint64_t c, std::chrono::steady_clock::duration wall) {
                    const double us = std::chrono::duration<double, std::micro>(wall).count();
                    std::cout << mode << "_" << label << "_wall_us_per_session=" << us / count << " " << mode << "_"
                              << label << "_kcycles_per_session=" << static_cast<double>(c) / 1000.0 / count << "\n";
                };
                uint64_t missing = 0;
                auto stat_pass = [&](bool timed) {
                    const uint64_t c0 = cycles();
                    const auto w0 = std::chrono::steady_clock::now();
                    for (const int32_t id : targets) {
                        WIN32_FILE_ATTRIBUTE_DATA fad{};
                        if (!GetFileAttributesExW(engine.BuildPathLocked(id).c_str(), GetFileExInfoStandard, &fad) && !timed)
                            ++missing;
                    }
                    if (timed) report("stat", cycles() - c0, std::chrono::steady_clock::now() - w0);
                };
                stat_pass(false);
                stat_pass(true);
                uint64_t applied = 0;
                bool any_struct = false, any_attr = false;
                const uint64_t c0 = cycles();
                const auto w0 = std::chrono::steady_clock::now();
                for (const auto& blob : blobs) {
                    const BYTE* begin = blob.data();
                    const BYTE* end = begin + blob.size();
                    if (batch) {
                        engine.ApplyUsnBatchLocked(*volume, begin, end, applied, any_struct, any_attr);
                        continue;
                    }
                    for (const BYTE* p = begin; p < end;
                         p += reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(p)->RecordLength)
                        if (engine.ApplyUsnLocked(*volume, reinterpret_cast<const USN_RECORD_V2*>(p)) != Engine::UsnApply::None)
                            ++applied;
                }
                report("apply", cycles() - c0, std::chrono::steady_clock::now() - w0);
                states.clear();
                for (const int32_t id : targets) {
                    const Attr a = engine.AttrAt(id);
                    const Node n = engine.NodeAt(id);
                    uint64_t h = 0xcbf29ce484222325ull;
                    for (const uint64_t x : {a.size, static_cast<uint64_t>(a.mtime), static_cast<uint64_t>(n.flags),
                                             static_cast<uint64_t>(static_cast<uint32_t>(n.parent))})
                        h = (h ^ x) * 0x100000001b3ull;
                    states.push_back(h);
                }
                std::cout << mode << " sessions=" << targets.size() << " k=" << k << " records=" << records
                          << " applied=" << applied << " missing=" << missing << " patches=" << engine.patches_.size() << "\n";
            }
            engine.running_ = false;
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        ec.clear();
        fs::remove_all(dir, ec);
        if (ec) std::cout << "cleanup_error=" << ec.value() << "\n";
        return ok;
    }

    static bool UsnProbe(const std::wstring& source, uint32_t sessions, uint32_t k, bool batch_first) {
        if (!std::filesystem::is_directory(source) || sessions == 0 || k == 0 || k > 3) return false;
        std::vector<uint64_t> first, second;
        const bool ok = UsnProbeMode(source, sessions, k, batch_first, first) &&
                        UsnProbeMode(source, sessions, k, !batch_first, second);
        size_t mismatched = first.size() == second.size() ? 0 : std::max(first.size(), second.size());
        if (first.size() == second.size())
            for (size_t i = 0; i < first.size(); ++i) mismatched += first[i] != second[i] ? 1 : 0;
        std::cout << "end_state_mismatches=" << mismatched << " of " << first.size() << "\n";
        return ok;
    }
    // --steady-memory-probe: loads a copy of a real index like the service,
    // then adds the retained state a long-running service accumulates (pinyin,
    // folder totals, query cache, change feed and an active change-tracking
    // lease filled from real C: paths) and prints private commit per phase.
    // Optional persistence rounds add 3000 events each and measure the flush.
    static bool SteadyMemoryProbe(const std::wstring& source, uint32_t events, uint32_t hot, uint32_t persist_rounds) {
        namespace fs = std::filesystem;
        if (!fs::is_directory(source) || events == 0 || hot == 0) return false;
        auto counters = [] {
            PROCESS_MEMORY_COUNTERS_EX c{}; c.cb = sizeof(c);
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c));
            return c;
        };
        auto mib = [](uint64_t bytes) { return static_cast<double>(bytes) / 1048576.0; };
        uint64_t last_private = counters().PrivateUsage;
        auto phase = [&](const char* label) {
            const auto c = counters();
            HEAP_SUMMARY hs{}; hs.cb = sizeof(hs);
            HeapSummary(GetProcessHeap(), 0, &hs);
            std::cout << "phase=" << label << " private_mib=" << mib(c.PrivateUsage) << " delta_mib="
                      << mib(c.PrivateUsage) - mib(last_private) << " ws_mib=" << mib(c.WorkingSetSize)
                      << " heap_committed_mib=" << mib(hs.cbCommitted) << " heap_allocated_mib=" << mib(hs.cbAllocated) << "\n";
            last_private = c.PrivateUsage;
        };
        auto cycles = [] { ULONG64 c = 0; QueryThreadCycleTime(GetCurrentThread(), &c); return c; };
        // Peak private commit while one call runs, sampled every millisecond.
        auto peak_of = [&](const char* label, auto&& work) {
            std::atomic<bool> done{false};
            std::atomic<uint64_t> peak{counters().PrivateUsage};
            std::thread sampler([&] {
                while (!done.load()) {
                    const uint64_t now = counters().PrivateUsage;
                    if (now > peak.load()) peak = now;
                    Sleep(1);
                }
            });
            const uint64_t before = counters().PrivateUsage;
            const uint64_t c0 = cycles();
            const auto w0 = std::chrono::steady_clock::now();
            work();
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count();
            const uint64_t c1 = cycles();
            done = true;
            sampler.join();
            std::cout << "call=" << label << " wall_ms=" << ms << " mcycles=" << static_cast<double>(c1 - c0) / 1e6
                      << " peak_extra_mib=" << mib(peak.load() - before) << "\n";
        };
        phase("start");
        const fs::path dir = fs::temp_directory_path() /
            (L"pulse-steady-memory-" + std::to_wstring(GetCurrentProcessId()));
        std::error_code ec;
        fs::remove_all(dir, ec);
        ec.clear();
        fs::copy(source, dir, fs::copy_options::recursive, ec);
        if (ec) {
            std::cout << "copy_failed=" << ec.value() << "\n";
            fs::remove_all(dir, ec);
            return false;
        }
        SetActiveIndexDirectory(dir.wstring());
        SetMachineIndexScope(true);
        bool ok = false;
        {
            Engine engine;
            phase("engine_constructed");
            ok = engine.TryLoadCache();
            phase("cache_loaded");
            for (int i = 0; ok && i < 1200 && !engine.pinyin_ready_.load(); ++i) Sleep(50);
            {
                std::shared_lock lock(engine.mutex_);
                std::cout << "pinyin_ready=" << engine.pinyin_ready_.load() << " pinyin_mapped="
                          << engine.PinyinIsMappedLocked() << " pinyin_chinese="
                          << engine.PinyinChineseIdsLocked().size() << " pinyin_private_mib="
                          << mib(engine.PinyinPrivateHeapBytesLocked()) << "\n";
            }
            phase("pinyin_ready");
            if (ok) {
                FolderSizeIndex sizes;
                const int32_t count = engine.LiveCount();
                const auto started = std::chrono::steady_clock::now();
                const bool built = sizes.Build(count, [&](int32_t i) { return engine.FolderSizeItem(i); });
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
                std::unique_lock lock(engine.mutex_);
                engine.folder_sizes_.Adopt(std::move(sizes));
                std::cout << "folder_totals_built=" << built << " live_count=" << count << " folder_totals_ms=" << ms << "\n";
            }
            phase("folder_totals");
            if (ok) ProbeQueries(engine);
            phase("queries");
            Engine::VolState* volume = nullptr;
            for (auto& v : engine.vols_)
                if ((v.letter == L'C' || v.letter == L'c') && v.frn_base && v.frn_base_n) volume = &v;
            ok = ok && volume;
            std::vector<std::pair<uint64_t, std::wstring>> targets;
            if (ok) {
                std::shared_lock lock(engine.mutex_);
                const int32_t base = engine.BaseCount();
                std::vector<int32_t> files;
                std::vector<uint64_t> frn_of(static_cast<size_t>(base), 0);
                for (uint32_t i = 0; i < volume->frn_base_n; ++i) {
                    const DiskFrn& entry = volume->frn_base[i];
                    if (entry.idx < 0 || entry.idx >= base || engine.IsTomb(entry.idx)) continue;
                    frn_of[static_cast<size_t>(entry.idx)] = entry.frn;
                    const Node n = engine.NodeAt(entry.idx);
                    if (!(n.flags & (Engine::kFlagDir | Engine::kFlagHidden))) files.push_back(entry.idx);
                }
                uint64_t seed = 0x9E3779B97F4A7C15ull;
                for (uint32_t i = 0; i < hot && !files.empty(); ++i) {
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                    const int32_t id = files[seed % files.size()];
                    targets.emplace_back(frn_of[static_cast<size_t>(id)] ^ (uint64_t{L'C'} << 56), engine.BuildPathLocked(id));
                }
                size_t chars = 0;
                for (const auto& [frn, path] : targets) chars += path.size();
                std::cout << "hot_files=" << targets.size() << " mean_path_chars="
                          << (targets.empty() ? 0.0 : static_cast<double>(chars) / static_cast<double>(targets.size())) << "\n";
                ok = !targets.empty();
            }
            phase("targets_built");
            const std::wstring owner = L"steady-memory-probe";
            const fs::path changes = dir / L"changes";
            if (ok) {
                fs::create_directories(changes, ec);
                engine.changes_.Open(changes.wstring());
                engine.changes_.Lease(owner, true);
                const uint64_t now = ChangeTracker::Now();
                ChangeTrackerTestAccess::Backdate(engine.changes_, owner, now - 7200, now + 36000);
                uint64_t seed = 0xD1B54A32D192ED03ull;
                const uint64_t c0 = cycles();
                std::unique_lock lock(engine.mutex_);
                for (uint32_t i = 0; i < events; ++i) {
                    seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                    const auto& [file_id, path] = targets[seed % targets.size()];
                    ChangeRecord event;
                    event.time = now - 3600 + static_cast<uint64_t>(i) * 3600 / events;
                    event.kind = ChangeKind::Modified;
                    event.file_id = file_id;
                    event.path = path;
                    engine.RecordFeed(event);
                    engine.changes_.Record(std::move(event));
                }
                std::cout << "record_mcycles_per_1k=" << static_cast<double>(cycles() - c0) / 1e6 / (events / 1000.0) << "\n";
            }
            auto tracker = [&](const char* label) {
                const auto s = ChangeTrackerTestAccess::Measure(engine.changes_);
                std::cout << "tracker=" << label << " records=" << s.records << " record_vector_mib=" << mib(s.record_bytes)
                          << " path_heap_mib=" << mib(s.string_bytes) << " summaries=" << s.summaries
                          << " summaries_mib=" << mib(s.summary_bytes) << " feed_records=" << engine.feed_changes_.Size()
                          << " feed_pages_mib=" << mib(engine.feed_changes_.PageCapacityBytes()) << "\n";
            };
            if (ok) {
                tracker("filled");
                phase("feed_and_tracker_filled");
                peak_of("tracker_flush", [&] { engine.changes_.Flush(true); });
                phase("after_flush");
                const uint64_t since = ChangeTracker::Now() - 86400;
                peak_of("summaries_cold", [&] { (void)engine.changes_.Summaries(owner, {L"C:\\Users"}, since); });
                tracker("after_summaries");
                phase("after_summaries");
                peak_of("summaries_warm", [&] { (void)engine.changes_.Summaries(owner, {L"C:\\Users"}, since + 1); });
                for (int round = 0; round < 3; ++round) {
                    {
                        std::unique_lock lock(engine.mutex_);
                        ChangeRecord event;
                        event.time = ChangeTracker::Now();
                        event.kind = ChangeKind::Modified;
                        event.file_id = targets[static_cast<size_t>(round)].first;
                        event.path = targets[static_cast<size_t>(round)].second;
                        engine.RecordFeed(event);
                        engine.changes_.Record(std::move(event));
                    }
                    peak_of("summaries_after_new_event", [&] { (void)engine.changes_.Summaries(owner, {L"C:\\Users"}, since); });
                    peak_of("tracker_flush_again", [&] { engine.changes_.Flush(true); });
                }
                phase("steady_after_rounds");
                if (persist_rounds) {
                    const fs::path base_file = changes / (L"changes-" + owner + L".bin");
                    const fs::path log_file = base_file.wstring() + L".log";
                    auto file_kib = [](const fs::path& p) {
                        std::error_code size_ec;
                        const auto n = fs::file_size(p, size_ec);
                        return size_ec ? 0.0 : static_cast<double>(n) / 1024.0;
                    };
                    auto written = [] { IO_COUNTERS io{}; GetProcessIoCounters(GetCurrentProcess(), &io); return io.WriteTransferCount; };
                    uint64_t seed = 0x2545F4914F6CDD1Dull, total = 0;
                    for (uint32_t round = 0; round < persist_rounds; ++round) {
                        {
                            std::unique_lock lock(engine.mutex_);
                            const uint64_t now = ChangeTracker::Now();
                            for (uint32_t i = 0; i < 3000; ++i) {
                                seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
                                const auto& [file_id, path] = targets[seed % targets.size()];
                                ChangeRecord event;
                                event.time = now - 59 + i / 50;
                                event.kind = ChangeKind::Modified;
                                event.file_id = file_id;
                                event.path = path;
                                engine.RecordFeed(event);
                                engine.changes_.Record(std::move(event));
                            }
                        }
                        const uint64_t before = written();
                        peak_of("persist_flush", [&] { engine.changes_.Flush(true); });
                        const uint64_t bytes = written() - before;
                        total += bytes;
                        std::cout << "persist round=" << round << " io_write_kib=" << static_cast<double>(bytes) / 1024.0
                                  << " base_kib=" << file_kib(base_file) << " log_kib=" << file_kib(log_file) << "\n";
                    }
                    std::cout << "persist_total_io_write_mib=" << mib(total) << " per_round_mib=" << mib(total) / persist_rounds << "\n";
                    tracker("after_persist_rounds");
                    phase("after_persist_rounds");
                    size_t live_records = 0, loaded_records = 0;
                    const uint64_t live = ChangeTrackerTestAccess::Digest(engine.changes_, owner, live_records);
                    ChangeTracker reloaded;
                    reloaded.Open(changes.wstring());
                    const uint64_t c0 = cycles();
                    const auto w0 = std::chrono::steady_clock::now();
                    const uint64_t loaded = ChangeTrackerTestAccess::Digest(reloaded, owner, loaded_records);
                    std::cout << "reload_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count()
                              << " reload_mcycles=" << static_cast<double>(cycles() - c0) / 1e6 << "\n";
                    std::cout << "reload_equal=" << (live == loaded) << " live_records=" << live_records
                              << " loaded_records=" << loaded_records << "\n";
                    ok = ok && live == loaded;
                }
            }
            engine.Stop();
        }
        phase("engine_destroyed");
        SetActiveIndexDirectory(L"");
        ec.clear();
        fs::remove_all(dir, ec);
        if (ec) std::cout << "cleanup_error=" << ec.value() << "\n";
        return ok;
    }
    static bool OverlayContainersFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        OverlayMap<int> map;
        check(map.find(5) == map.end() && !map.contains(5) && map.count(0) == 0, "empty overlay map finds nothing");
        map[5] = 50;
        map[1000000] = 7;
        map[-1] = 3;
        const auto& view = map;
        check(view.find(5) != view.end() && view.find(5)->second == 50, "overlay map finds a low key");
        check(map.contains(1000000) && map.find(1000000)->second == 7, "overlay map finds a key past its first word");
        check(map.contains(-1) && map.find(-1)->second == 3, "overlay map keeps negative keys exact");
        check(!map.contains(6) && !map.contains(999999) && !map.contains(2000000) && !map.contains(-2),
              "overlay map rejects absent neighbours");
        size_t seen = 0;
        for (auto& entry : map) { ++seen; entry.second += 1; }
        check(seen == 3 && map.size() == 3 && map.find(5)->second == 51, "overlay map iterates mutable entries");
        check(map.erase(5) == 1 && !map.contains(5) && map.erase(5) == 0 && map.size() == 2, "overlay map erase is exact");
        map[5] = 9;
        check(map.contains(5) && map.find(5)->second == 9, "overlay map re-inserts an erased key");
        map.clear();
        check(map.empty() && !map.contains(1000000) && !map.contains(-1) && map.find(5) == map.end(),
              "overlay map clear drops all keys");
        OverlaySet set;
        check(set.insert(42).second && !set.insert(42).second && set.count(42) == 1, "overlay set insert is unique");
        set.insert(-7);
        check(set.contains(-7) && !set.contains(-8) && !set.contains(41) && !set.contains(4096), "overlay set lookups are exact");
        check(set.erase(42) == 1 && !set.contains(42) && set.size() == 1, "overlay set erase is exact");
        set.clear();
        check(set.empty() && !set.contains(-7), "overlay set clear drops all keys");
        return ok;
    }
    static bool ReopenCache(const std::wstring& directory) {
        if (!std::filesystem::is_directory(directory)) return false;
        SetActiveIndexDirectory(directory);
        SetMachineIndexScope(true);
        Engine engine;
        const auto started = std::chrono::steady_clock::now();
        const bool loaded = engine.TryLoadCache();
        FolderSizeIndex sizes;
        const bool totals = loaded && sizes.Build(engine.LiveCount(),
            [&](int32_t i) { return engine.FolderSizeItem(i); });
        std::cout << "cache_loaded=" << loaded << " nodes=" << engine.LiveCount()
            << " folder_totals_valid=" << totals << " elapsed_ms="
            << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count() << "\n";
        engine.Stop();
        return loaded && totals;
    }
    static bool HierarchyFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        std::vector<Node> nodes(3);
        nodes[0].parent = 2; nodes[1].parent = -1; nodes[1].flags = 1;
        nodes[2].parent = 1; nodes[2].flags = 1;
        auto valid = [&] { return ValidateIndexHierarchy(static_cast<int32_t>(nodes.size()), [&](int32_t i) { return nodes[i]; }); };
        check(valid(), "hierarchy permits children preceding parents");
        nodes[2].flags = 0; check(!valid(), "file cannot be a parent"); nodes[2].flags = 1;
        nodes[1].parent = 2; check(!valid(), "directory cycle is rejected"); nodes[1].parent = -1;
        nodes[0].parent = 3; check(!valid(), "out-of-range parent is rejected");
        nodes[0].parent = -2; check(!valid(), "invalid negative parent is rejected");
        nodes[0].parent = -1; check(!valid(), "orphan file root is rejected");
        nodes[0].parent = 2; nodes[2].flags = 5; check(!valid(), "alive child cannot have deleted parent");
        nodes[2].flags = 1; check(valid(), "repaired hierarchy is accepted");
        Engine engine; check(Build(engine), "build valid mapped hierarchy fixture");
        const auto file = std::filesystem::temp_directory_path() / (L"pulse-hierarchy-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
        check(Save(engine, file.wstring()), "save valid hierarchy");
        check(ValidateCache(file.wstring()), "production loader accepts valid hierarchy");
        {
            std::fstream stream(file, std::ios::binary | std::ios::in | std::ios::out);
            DiskHeader header{}; stream.read(reinterpret_cast<char*>(&header), sizeof(header));
            stream.seekp(static_cast<std::streamoff>(header.nodes_off + offsetof(Node, flags)));
            const char file_flags = 0; stream.write(&file_flags, 1);
        }
        check(!ValidateCache(file.wstring()), "production loader rejects file root in otherwise readable cache");
        std::filesystem::remove(file);
        return ok;
    }
    static bool VisibilityCacheFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        Engine engine;
        check(Build(engine), "build isolated visibility fixture");
        VolumeInfo active; active.id = engine.vols_.front().volume_id;
        Query query; query.needle = L"settings.toml";
        check(engine.Search(query).total == 1, "visible volume returns matching file");
        const auto initial_epoch = engine.cache_epoch_;
        for (int i = 0; i < 1000; ++i) engine.UpdateVolumeVisibilityLocked({active});
        check(engine.filter_epoch_ == initial_epoch,
            "unchanged volume polling preserves cached query across 1000 wakes");
        engine.UpdateVolumeVisibilityLocked({});
        check(engine.filter_epoch_ != initial_epoch && engine.Search(query).total == 0,
            "offline transition invalidates cache and hides matching file");
        const auto offline_epoch = engine.cache_epoch_;
        engine.UpdateVolumeVisibilityLocked({});
        engine.UpdateVolumeVisibilityLocked({}, true);
        engine.UpdateVolumeVisibilityLocked({active}, true);
        check(engine.filter_epoch_ == offline_epoch && engine.Search(query).total == 0,
            "unchanged and hide-only polling preserve offline cache");
        engine.UpdateVolumeVisibilityLocked({active});
        check(engine.filter_epoch_ != offline_epoch && engine.Search(query).total == 1,
            "online transition invalidates cache and restores matching file");
        engine.UpdateVolumeVisibilityLocked({}, true);
        check(engine.Search(query).total == 0, "hide-only transition invalidates visible cache");
        return ok;
    }

    static bool FolderSizeIndexFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        using Item = FolderSizeIndex::Item;
        uint64_t seed = 0x5DEECE66Dull;
        auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        size_t mismatches = 0, resets = 0, rebuilds = 0, ops = 0;
        for (int round = 0; round < 60; ++round) {
            std::vector<Item> nodes;
            const int32_t initial = 40 + static_cast<int32_t>(next() % 400);
            for (int32_t i = 0; i < initial; ++i) {
                Item item;
                item.alive = next() % 10 != 0;
                item.directory = i == 0 || next() % 3 == 0;
                for (int tries = 0; i > 0 && tries < 8 && next() % 8 != 0; ++tries) {
                    const auto parent = static_cast<int32_t>(next() % static_cast<uint64_t>(i));
                    if (nodes[static_cast<size_t>(parent)].directory) { item.parent = parent; break; }
                }
                item.bytes = item.directory ? 0 : (round % 15 == 14 && next() % 40 == 0 ? UINT64_MAX / 3 : next() % 100000);
                nodes.push_back(item);
            }
            auto read = [&](int32_t i) { return nodes[static_cast<size_t>(i)]; };
            FolderSizeIndex index;
            ReferenceFolderSizeIndex reference;
            auto same = [&] {
                if (index.Valid() != reference.Valid() || index.Builds() != reference.Builds()) return false;
                for (int32_t i = -1; i < static_cast<int32_t>(nodes.size()) + 2; ++i)
                    if (index.Get(i) != reference.Get(i)) return false;
                return true;
            };
            if (index.Build(initial, read) != reference.Build(initial, read) || !same()) ++mismatches;
            for (int op = 0; op < 400; ++op, ++ops) {
                const auto count = static_cast<int32_t>(nodes.size());
                auto id = static_cast<int32_t>(next() % static_cast<uint64_t>(count));
                Item before = nodes[static_cast<size_t>(id)];
                Item after = before;
                switch (next() % 7) {
                case 0: if (!after.directory) after.bytes = next() % 100000; break;
                case 1: {
                    const int32_t parent = id > 0 ? static_cast<int32_t>(next() % static_cast<uint64_t>(id)) : -1;
                    if (parent < 0 || nodes[static_cast<size_t>(parent)].directory) after.parent = parent;
                    break;
                }
                case 2: after.alive = false; break;
                case 3: case 4: {
                    Item created;
                    created.alive = true;
                    created.directory = next() % 3 == 0;
                    const auto parent = static_cast<int32_t>(next() % static_cast<uint64_t>(count));
                    if (nodes[static_cast<size_t>(parent)].directory) created.parent = parent;
                    created.bytes = created.directory ? 0 : next() % 100000;
                    id = count; before = Item{}; after = created;
                    nodes.push_back(Item{});
                    break;
                }
                case 5: after.alive = true; break;
                default:
                    if (next() % 25 == 0) after.directory = !after.directory;
                    else if (!after.directory) after.bytes += next() % 1000;
                    break;
                }
                nodes[static_cast<size_t>(id)] = after;
                index.Replace(id, before, after, read);
                reference.Replace(id, before, after, read);
                if ((op % 8 == 0 || op == 399) && !same()) ++mismatches;
                if (!reference.Valid()) {
                    ++resets;
                    const auto size = static_cast<int32_t>(nodes.size());
                    if (index.Build(size, read) != reference.Build(size, read) || !same()) ++mismatches;
                    ++rebuilds;
                }
            }
        }
        std::cout << "folder_size_ops=" << ops << " resets=" << resets << " rebuilds=" << rebuilds << "\n";
        check(mismatches == 0, "folder totals match the hash map implementation through random edits");
        check(resets > 0 && rebuilds > 0, "random edits cover reset and rebuild paths");
        FolderSizeIndex index;
        std::vector<Item> chain{{-1, 0, true, true}, {0, 0, true, true}, {1, 7, false, true}};
        auto read = [&](int32_t i) { return chain[static_cast<size_t>(i)]; };
        check(index.Build(3, read) && index.Get(0) == 7u && index.Get(1) == 7u && !index.Get(2), "build totals a chain");
        chain.push_back({1, 0, true, true});
        index.Replace(3, {}, chain[3], read);
        chain.push_back({3, 5, false, true});
        index.Replace(4, {}, chain[4], read);
        check(index.Get(3) == 5u && index.Get(0) == 12u, "directory created after the build keeps a total");
        const Item removed = chain[1];
        chain[1].alive = false;
        index.Replace(1, removed, chain[1], read);
        check(!index.Get(1) && index.Get(0) == 0u, "removed directory drops out");
        chain[1] = removed;
        index.Replace(1, Item{removed.parent, 0, true, false}, chain[1], read);
        check(index.Get(1) == 0u && index.Get(0) == 0u, "restored directory starts empty like a new one");
        return ok;
    }
    // --folder-size-bench <nodes>: build time and retained private memory of
    // the array and hash map versions on one synthetic tree.
    static bool FolderSizeBench(uint32_t count) {
        if (count < 2) return false;
        using Item = FolderSizeIndex::Item;
        std::vector<Item> nodes(count);
        uint64_t seed = 0x2545F4914F6CDD1Dull;
        auto next = [&] { seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17; return seed; };
        std::vector<int32_t> directories{0};
        nodes[0] = {-1, 0, true, true};
        for (uint32_t i = 1; i < count; ++i) {
            const bool directory = next() % 4 == 0;
            nodes[i] = {directories[next() % directories.size()], directory ? 0 : next() % 1000000, directory, true};
            if (directory) directories.push_back(static_cast<int32_t>(i));
        }
        auto read = [&](int32_t i) { return nodes[static_cast<size_t>(i)]; };
        auto private_bytes = [] {
            PROCESS_MEMORY_COUNTERS_EX c{}; c.cb = sizeof(c);
            K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&c), sizeof(c));
            return static_cast<double>(c.PrivateUsage) / 1048576.0;
        };
        auto run = [&](const char* label, auto& index) {
            const double before = private_bytes();
            const auto started = std::chrono::steady_clock::now();
            const bool built = index.Build(static_cast<int32_t>(count), read);
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
            uint64_t sum = 0;
            const auto lookups = std::chrono::steady_clock::now();
            for (int32_t id : directories) sum += index.Get(id).value_or(0);
            const double lookup_ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - lookups).count() /
                static_cast<double>(directories.size());
            std::cout << label << " built=" << built << " build_ms=" << ms << " retained_mib=" << private_bytes() - before
                      << " lookup_ns=" << lookup_ns << " root=" << index.Get(0).value_or(0) << " sum=" << sum << "\n";
            return built;
        };
        std::cout << "nodes=" << count << " directories=" << directories.size() << "\n";
        bool ok = true;
        { ReferenceFolderSizeIndex reference; ok &= run("hash_map", reference); }
        { FolderSizeIndex index; ok &= run("arrays", index); }
        { ReferenceFolderSizeIndex reference; ok &= run("hash_map", reference); }
        { FolderSizeIndex index; ok &= run("arrays", index); }
        return ok;
    }
    static bool FolderSizesFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"folder-index-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto a = fixture / L"A", b = fixture / L"B";
        std::filesystem::create_directories(a); std::filesystem::create_directories(b);
        auto write = [](const std::filesystem::path& p, size_t n) { std::ofstream f(p, std::ios::binary | std::ios::trunc); f << std::string(n, 'x'); };
        write(a / L"one.bin", 100);
        Engine e;
        e.AddForTest((a / L"one.bin").wstring(), L"one.bin", false, 100);
        e.AddForTest(b.wstring(), L"B", true);
        Engine::VolState volume;
        volume.letter = fixture.wstring()[0]; volume.root_idx = e.ResolvePathLocked(fixture.root_path().wstring());
        volume.journal_id = 1; volume.folder_size_current = true;
        for (const auto& pair : std::vector<std::pair<uint64_t, std::wstring>>{
            {1, fixture.root_path().wstring()}, {2, fixture.wstring()}, {3, a.wstring()}, {4, b.wstring()}, {5, (a / L"one.bin").wstring()}})
            volume.frn_new.push_back({pair.first, e.ResolvePathLocked(pair.second), 0});
        e.vols_.push_back(std::move(volume)); e.running_ = true; e.ready_ = true;
        auto size = [&](const std::filesystem::path& path) { return e.FolderSizes({path.wstring()})[0]; };
        check(size(a).available && size(a).bytes == 100 && size(b).available && size(b).bytes == 0,
            "MFT metadata aggregates nested folders and known empty zero");
        const auto builds = e.folder_sizes_.Builds();
        VolumeInfo active; active.id = e.vols_[0].volume_id;
        e.UpdateVolumeVisibilityLocked({active});
        check(size(a).bytes == 100 && e.folder_sizes_.Builds() == builds,
            "routine volume visibility polling preserves size aggregates");
        auto usn = [&](uint64_t id, uint64_t parent, const wchar_t* name, DWORD reason, bool directory = false) {
            const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
            std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
            auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
            record->RecordLength = static_cast<DWORD>(bytes.size()); record->MajorVersion = 2;
            record->FileReferenceNumber = id; record->ParentFileReferenceNumber = parent;
            record->Reason = reason; record->FileAttributes = directory ? FILE_ATTRIBUTE_DIRECTORY : 0;
            record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)); record->FileNameLength = length;
            memcpy(bytes.data() + record->FileNameOffset, name, length);
            e.ApplyUsnLocked(e.vols_.front(), record);
        };
        write(a / L"one.bin", 350); usn(5, 3, L"one.bin", USN_REASON_DATA_EXTEND);
        check(size(a).bytes == 350 && size(fixture).bytes == 350, "real USN data extension updates ancestors");
        write(a / L"one.bin", 25); usn(5, 3, L"one.bin", USN_REASON_DATA_TRUNCATION);
        check(size(a).bytes == 25, "USN truncation subtracts old logical size");
        write(a / L"two.bin", 75); usn(6, 3, L"two.bin", USN_REASON_FILE_CREATE);
        check(size(a).bytes == 100, "USN creation adds file size without index rebuild");
        std::filesystem::rename(a / L"two.bin", b / L"two.bin");
        usn(6, 4, L"two.bin", USN_REASON_RENAME_NEW_NAME);
        check(size(a).bytes == 25 && size(b).bytes == 75 && size(fixture).bytes == 100,
            "USN file move updates both ancestor chains exactly once");
        std::filesystem::rename(a, b / L"A"); usn(3, 4, L"A", USN_REASON_RENAME_NEW_NAME, true);
        check(!size(a).available && size(b).bytes == 100 && size(b / L"A").bytes == 25,
            "USN subtree move transfers its cached total");
        std::filesystem::remove(b / L"A" / L"one.bin"); usn(5, 3, L"one.bin", USN_REASON_FILE_DELETE);
        check(size(b / L"A").bytes == 0 && size(b).bytes == 75, "USN deletion subtracts previous file metadata");
        usn(5, 3, L"one.bin", USN_REASON_FILE_DELETE);
        check(size(b).bytes == 75 && e.folder_sizes_.Builds() == builds,
            "duplicate deletion is harmless and all USN edits avoid aggregate rebuilds");
        e.building_ = true; check(!size(b).available, "unfinished index never returns a complete size"); e.building_ = false;
        e.vols_[0].folder_size_current = false; check(!size(b).available, "startup cache waits for journal catch-up"); e.vols_[0].folder_size_current = true;
        e.inactive_volume_roots_.push_back(e.vols_[0].root_idx);
        check(!size(b).available, "offline volume declines cached totals"); e.inactive_volume_roots_.clear();
        e.GapFeed(); check(!size(b).available, "journal gap declines stale aggregation until recovery");
        e.folder_size_gap_ = false; e.InvalidateFilterLocked();
        check(size(b).available && size(b).bytes == 75 && e.folder_sizes_.Builds() == builds + 1,
            "replacement snapshot rebuilds aggregation from current metadata");
        check(!size(L"\\\\server\\share").available && !size(fixture / L"absent").available, "uncovered UNC and missing paths never return false zero");
        e.excluded_paths_.push_back(b.wstring()); check(!size(b).available, "excluded root falls back to filesystem statistics");
        check(!size(L"\\\\?\\" + b.wstring()).available, "extended-length client path cannot bypass index exclusion");
        e.excluded_paths_.clear();
        const int32_t broken_id = e.ResolvePathLocked(b.wstring());
        const auto old_parent = e.live_.nodes[broken_id].parent;
        e.live_.nodes[broken_id].parent = broken_id;
        e.InvalidateFilterLocked();
        check(!size(fixture).available, "malformed metadata declines folder totals");
        const auto failed_builds = e.folder_sizes_.Builds();
        bool all_unknown = true;
        for (int i = 0; i < 100; ++i) all_unknown &= !size(fixture).available;
        check(all_unknown && e.folder_sizes_.Builds() == failed_builds,
            "100 client polls after failed aggregation do not repeat whole-index scans");
        e.folder_size_retry_after_ = GetTickCount64() - 1;
        check(!size(fixture).available && e.folder_sizes_.Builds() == failed_builds + 1,
            "expired failure delay permits one new aggregation attempt");
        e.live_.nodes[broken_id].parent = old_parent;
        e.InvalidateFilterLocked();
        check(size(fixture).available && size(fixture).bytes == 75,
            "replacement snapshot clears failure delay and restores correct totals");
        e.folder_size_background_ = true; e.InvalidateFilterLocked();
        const auto background_builds = e.folder_sizes_.Builds();
        check(!size(fixture).available && e.folder_size_wanted_ && e.folder_sizes_.Builds() == background_builds,
            "service clients are answered at once instead of waiting for whole-index aggregation");
        check(e.BuildFolderTotals() && size(fixture).available && size(fixture).bytes == 75 &&
              e.folder_sizes_.Builds() == background_builds + 1,
            "worker aggregation publishes totals for the next client poll");
        e.folder_size_background_ = false;
        std::error_code ec; std::filesystem::remove_all(fixture, ec); check(!ec, "isolated folder-index fixture cleanup");

        FolderSizeIndex index;
        using Item = FolderSizeIndex::Item;
        std::vector<Item> nodes{{-1, 0, true, true}, {0, UINT64_MAX, false, true}, {0, 1, false, true}};
        check(!index.Build(3, [&](int32_t i) { return nodes[i]; }), "aggregate overflow rejected without wrapped sizes");
        nodes = {{1, 0, true, true}, {0, 0, true, true}};
        check(!index.Build(2, [&](int32_t i) { return nodes[i]; }), "cyclic metadata terminates without false totals");
        nodes = {{-1, 0, true, true}};
        index = FolderSizeIndex{};
        for (int32_t i = 1; i <= 200000; ++i) nodes.push_back({0, 1, false, true});
        const auto start = std::chrono::steady_clock::now();
        check(index.Build(static_cast<int32_t>(nodes.size()), [&](int32_t i) { return nodes[i]; }) && index.Get(0) == 200000,
            "large metadata baseline sums 200000 files without filesystem enumeration");
        const auto built = std::chrono::steady_clock::now();
        for (int32_t i = 1; i <= 10000; ++i) { const auto before = nodes[i]; nodes[i].bytes = 2; index.Replace(i, before, nodes[i], [&](int32_t id) { return nodes[id]; }); }
        check(index.Get(0) == 210000 && index.Builds() == 1, "10000 incremental edits reuse a single metadata baseline");
        std::cout << "[TIME] baseline_200000_ms=" << std::chrono::duration<double, std::milli>(built - start).count()
            << " updates_10000_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - built).count() << '\n';
        ipc::PayloadWriter writer; PutFolderSizes(writer, {{true, 0}, {true, UINT64_MAX}, {false, 0}});
        ipc::PayloadReader reader(writer.data().data(), writer.data().size()); std::vector<IndexedFolderSize> values;
        check(ReadFolderSizes(reader, 3, values) && values[0].available && values[1].bytes == UINT64_MAX && !values[2].available,
            "versioned IPC preserves zero, 64-bit size and unavailable distinctly");
        ipc::PayloadReader truncated(writer.data().data(), writer.data().size() - 1);
        check(!ReadFolderSizes(truncated, 3, values), "truncated folder-size IPC rejected");
        return ok;
    }
    // Hard links: one searchable node per name, persisted through snapshots.
    static bool HardLinkBuildFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        auto has = [](Engine& e, const wchar_t* needle, const wchar_t* path) {
            Query q; q.needle = needle; q.limit = 100;
            const auto result = e.Search(q);
            return std::any_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) { return hit.path == path; });
        };
        auto build = [](Engine& e, bool hide_node_modules) {
            e.running_ = true;
            e.hide_node_modules_ = hide_node_modules;
            Engine::VolState volume;
            volume.letter = L'C';
            volume.kind = VolumeKind::Removable;
            volume.volume_id = L"hardlink-volume";
            std::vector<Engine::FrnNode> nodes;
            auto add = [&](uint64_t id, uint64_t parent, const wchar_t* name, bool dir, bool link = false) {
                Engine::FrnNode node;
                node.frn = id; node.parent = parent; node.name = name; node.is_dir = dir;
                node.extra_link = link; node.size = link ? 0 : 42;
                nodes.push_back(std::move(node));
            };
            add(1100, 1003, L"index.js", false, true);   // extra link before its primary
            add(1000, 5, L"Links", true);
            add(1001, 1000, L"store", true);
            add(1002, 1000, L"app", true);
            add(1003, 1002, L"node_modules", true);
            add(1100, 1001, L"index.js", false);
            add(1100, 1002, L"copy.js", false, true);
            add(1100, 1001, L"INDEX.JS", false, true);   // same link as the primary
            add(1100, 999999, L"orphan.js", false, true); // parent outside the index
            const bool built = e.BuildMftTree(std::move(volume), 5, std::move(nodes));
            e.live_ = std::move(e.build_);
            e.vols_ = std::move(e.build_vols_);
            e.RebuildChildMapLocked();
            e.ready_ = true;
            return built;
        };
        Engine e;
        check(build(e, true), "hard link fixture builds");
        std::vector<int32_t> links;
        e.CollectFrnLocked(e.vols_.front(), 1100, links);
        check(links.size() == 3, "MFT build keeps one node per distinct hard link name");
        check(has(e, L"index.js", L"C:\\Links\\store\\index.js") && has(e, L"copy.js", L"C:\\Links\\app\\copy.js"),
              "every hard link name is searchable");
        check(std::all_of(links.begin(), links.end(), [&](int32_t id) { return e.AttrAt(id).size == 42; }),
              "hard link names share the file size");
        check(!has(e, L"index.js", L"C:\\Links\\app\\node_modules\\index.js"),
              "node_modules link stays hidden while its group is on");
        Query orphan; orphan.needle = L"orphan.js"; orphan.limit = 10;
        check(e.Search(orphan).total == 0, "link below an unknown parent is skipped");
        const auto file = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"index-hardlink-" + std::to_wstring(GetCurrentProcessId()) + L".bin"));
        std::filesystem::create_directories(file.parent_path());
        const bool saved = Save(e, file.wstring());
        check(saved, "save hard link snapshot");
        if (saved) {
            Engine mapped;
            const bool loaded = Load(mapped, file.wstring());
            std::vector<int32_t> mapped_links;
            if (loaded) mapped.CollectFrnLocked(mapped.vols_.front(), 1100, mapped_links);
            check(loaded && mapped_links.size() == 3 && has(mapped, L"copy.js", L"C:\\Links\\app\\copy.js"),
                  "snapshot keeps every hard link mapped to its file");
        }
        std::error_code ec;
        std::filesystem::remove(file, ec);
        Engine visible;
        check(build(visible, false) && has(visible, L"index.js", L"C:\\Links\\app\\node_modules\\index.js"),
              "node_modules link is searchable when its group is off");
        return ok;
    }

    static bool HardLinkUsnFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        const auto fixture = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"hardlink-usn-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto a = fixture / L"A", b = fixture / L"B";
        std::filesystem::create_directories(a); std::filesystem::create_directories(b);
        auto write = [](const std::filesystem::path& p, size_t n) {
            std::ofstream f(p, std::ios::binary | std::ios::trunc); f << std::string(n, 'x');
        };
        auto file_id = [](const std::filesystem::path& p) -> uint64_t {
            HANDLE h = CreateFileW(p.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                   nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
            if (h == INVALID_HANDLE_VALUE) return 0;
            BY_HANDLE_FILE_INFORMATION info{};
            const BOOL got = GetFileInformationByHandle(h, &info);
            CloseHandle(h);
            return got ? (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow : 0;
        };
        write(a / L"one.bin", 100);
        const uint64_t frn = file_id(a / L"one.bin");
        Engine e;
        e.AddForTest((a / L"one.bin").wstring(), L"one.bin", false, 100);
        e.AddForTest(b.wstring(), L"B", true);
        Engine::VolState volume;
        volume.letter = fixture.wstring()[0];
        volume.root_idx = e.ResolvePathLocked(fixture.root_path().wstring());
        volume.journal_id = 1;
        for (const auto& pair : std::vector<std::pair<uint64_t, std::wstring>>{
                 {1, fixture.root_path().wstring()}, {2, fixture.wstring()}, {3, a.wstring()},
                 {4, b.wstring()}, {frn, (a / L"one.bin").wstring()}})
            volume.frn_new.push_back({pair.first, e.ResolvePathLocked(pair.second), 0});
        e.vols_.push_back(std::move(volume));
        e.running_ = true;
        e.ready_ = true;
        auto usn = [&](uint64_t parent, const wchar_t* name, DWORD reason) {
            const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
            std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
            auto* record = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
            record->RecordLength = static_cast<DWORD>(bytes.size()); record->MajorVersion = 2;
            record->FileReferenceNumber = frn; record->ParentFileReferenceNumber = parent;
            record->Reason = reason; record->FileAttributes = FILE_ATTRIBUTE_ARCHIVE;
            record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)); record->FileNameLength = length;
            memcpy(bytes.data() + record->FileNameOffset, name, length);
            e.ApplyUsnLocked(e.vols_.front(), record);
        };
        auto links = [&] {
            std::vector<int32_t> out;
            e.CollectFrnLocked(e.vols_.front(), frn, out);
            return out;
        };
        auto indexed = [&](const std::filesystem::path& p) {
            const int32_t id = e.ResolvePathLocked(p.wstring());
            return id >= 0 && !e.IsTomb(id);
        };
        const bool linked = frn != 0 && CreateHardLinkW((b / L"one.bin").c_str(), (a / L"one.bin").c_str(), nullptr);
        check(linked, "create NTFS hard link fixture");
        if (linked) {
            usn(4, L"one.bin", USN_REASON_HARD_LINK_CHANGE);
            check(links().size() == 2 && indexed(b / L"one.bin") && indexed(a / L"one.bin"),
                  "USN hard link creation adds the new name");
            usn(4, L"one.bin", USN_REASON_HARD_LINK_CHANGE);
            check(links().size() == 2, "repeated hard link record does not duplicate the name");
            write(a / L"one.bin", 350);
            usn(3, L"one.bin", USN_REASON_DATA_EXTEND);
            const auto current = links();
            check(current.size() == 2 && std::all_of(current.begin(), current.end(),
                      [&](int32_t id) { return e.AttrAt(id).size == 350; }),
                  "data change refreshes every hard link name");
            std::filesystem::rename(b / L"one.bin", b / L"two.bin");
            usn(4, L"two.bin", USN_REASON_RENAME_NEW_NAME);
            check(links().size() == 2 && indexed(a / L"one.bin") && indexed(b / L"two.bin") && !indexed(b / L"one.bin"),
                  "renaming one link moves only that name");
            std::filesystem::remove(a / L"one.bin");
            usn(3, L"one.bin", USN_REASON_HARD_LINK_CHANGE);
            check(links().size() == 1 && !indexed(a / L"one.bin") && indexed(b / L"two.bin"),
                  "removing one link keeps the file under its other name");
            const bool relinked = CreateHardLinkW((a / L"three.bin").c_str(), (b / L"two.bin").c_str(), nullptr);
            if (relinked) usn(3, L"three.bin", USN_REASON_HARD_LINK_CHANGE);
            check(relinked && links().size() == 2 && indexed(a / L"three.bin"), "a later hard link is indexed");
            std::filesystem::remove(a / L"three.bin");
            std::filesystem::remove(b / L"two.bin");
            usn(4, L"two.bin", USN_REASON_FILE_DELETE | USN_REASON_CLOSE);
            check(links().empty() && !indexed(a / L"three.bin") && !indexed(b / L"two.bin"),
                  "deleting the file removes every hard link name");
        }
        std::error_code ec;
        std::filesystem::remove_all(fixture, ec);
        check(!ec, "isolated hard link fixture cleanup");
        return ok;
    }

    static bool NamePoolFixture();
    static bool QuietDiagnosticsFixture();
    static bool UsnQueueFixture() {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"usn-queue-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetMachineIndexScope(false); SetActiveIndexDirectory(dir.wstring());
        {
            Engine engine;
            check(Build(engine), "build isolated USN consumer fixture without opening volumes");
            auto& volume = engine.vols_.front(); volume.journal_id = 1; volume.next_usn = 10;
            engine.change_signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            auto stream = UsnStreamTestAccess::Make(engine.change_signal_);
            auto* collector = stream.get();
            engine.journal_streams_[volume.volume_id] = std::move(stream);
            engine.changes_.Open(dir.wstring()); engine.changes_.Lease(L"usn-queue-fixture", true);
            const auto arm = engine.ReadFeed(true, L"", 0, 0);
            auto push = [&](int64_t next, DWORD reason, const wchar_t* name) {
                const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
                const size_t record_bytes = (sizeof(USN_RECORD_V2) + length + 7) & ~size_t{7};
                std::vector<BYTE> packet(sizeof(next) + record_bytes);
                std::memcpy(packet.data(), &next, sizeof(next));
                auto* record = reinterpret_cast<USN_RECORD_V2*>(packet.data() + sizeof(next));
                record->RecordLength = static_cast<DWORD>(record_bytes); record->MajorVersion = 2;
                record->FileReferenceNumber = 1000; record->ParentFileReferenceNumber = 20;
                record->Usn = next - 1; record->Reason = reason;
                record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName)); record->FileNameLength = length;
                FILETIME now{}; GetSystemTimeAsFileTime(&now);
                record->TimeStamp.QuadPart = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
                std::memcpy(reinterpret_cast<BYTE*>(record) + record->FileNameOffset, name, length);
                return UsnStreamTestAccess::Push(*collector, packet);
            };
            check(push(100, USN_REASON_FILE_CREATE, L"usn-queued.txt") &&
                  push(200, USN_REASON_RENAME_OLD_NAME, L"usn-queued.txt") &&
                  push(300, USN_REASON_RENAME_NEW_NAME, L"usn-renamed.txt") &&
                  push(400, USN_REASON_FILE_DELETE, L"usn-renamed.txt"), "enqueue create/rename/delete packets");
            const auto first_peak = collector->Memory();
            bool changed = false, structural = false;
            check(engine.CatchUpVolume(volume, &changed, &structural) && volume.next_usn == 400 && changed && structural,
                  "real CatchUpVolume applies queued records and commits final cursor");
            const auto page = engine.ReadFeed(true, L"", arm.epoch, arm.next);
            check(!page.gap && page.records.size() == 3 && page.records[0].kind == ChangeKind::Created &&
                  page.records[1].kind == ChangeKind::Renamed && page.records[2].kind == ChangeKind::Deleted &&
                  page.records[1].old_path == L"C:\\Users\\TestUser\\usn-queued.txt" &&
                  page.records[1].path == L"C:\\Users\\TestUser\\usn-renamed.txt",
                  "queued records preserve create/rename/delete order and rename payload");
            auto second = UsnStreamTestAccess::Make(engine.change_signal_);
            std::vector<BYTE> header(sizeof(int64_t)); int64_t next = 500;
            std::memcpy(header.data(), &next, sizeof(next));
            check(UsnStreamTestAccess::Push(*second, header), "enqueue independent diagnostic stream");
            const auto second_state = second->Memory();
            engine.journal_streams_[L"diagnostic-only"] = std::move(second);
            engine.CaptureMemoryState(); const auto& memory = engine.filename_timing_.Memory().retained;
            check(memory.usn_streams == 2 && memory.usn_queue_packets == 1 &&
                  memory.usn_queue_capacity_bytes == second_state.capacity_bytes &&
                  memory.usn_queue_charged_bytes == second_state.charged_bytes &&
                  memory.usn_sum_stream_peak_capacity_bytes == first_peak.peak_capacity_bytes + second_state.peak_capacity_bytes &&
                  memory.usn_sum_stream_peak_charged_bytes == first_peak.peak_charged_bytes + second_state.peak_charged_bytes,
                  "engine aggregates current queue bytes separately from independent lifetime peaks");
            check(UsnStreamTestAccess::Push(*collector, header) && engine.CatchUpVolume(volume, &changed, &structural) &&
                  volume.next_usn == 500, "header-only packet advances engine cursor without emitting an event");
            next = 600; std::memcpy(header.data(), &next, sizeof(next)); UsnStreamTestAccess::Push(*collector, header);
            UsnStreamTestAccess::Fail(*collector, ERROR_BUFFER_OVERFLOW);
            check(!engine.CatchUpVolume(volume, &changed, &structural) && volume.next_usn == 500 &&
                  collector->Memory().packets == 1 && engine.ReadFeed(true, L"", arm.epoch, page.next).records.empty(),
                  "stream overflow reaches engine failure path without cursor advance or silent consumption");
            engine.journal_streams_.clear(); engine.CaptureMemoryState();
            check(!memory.usn_streams && !memory.usn_queue_packets && !memory.usn_queue_capacity_bytes &&
                  !memory.usn_sum_stream_peak_charged_bytes, "collector replacement resets diagnostic aggregates");
        }
        SetActiveIndexDirectory(L""); std::error_code ec; std::filesystem::remove_all(dir, ec);
        check(!ec, "isolated USN consumer fixture cleaned up");
        return ok;
    }

    static bool FeedFixture() {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-feed-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(dir.wstring());
        {
            Engine engine;
            check(Build(engine), "build isolated feed fixture");
            auto arm = engine.ReadFeed(true, L"", 0, 0);
            check(arm.ready && arm.done && arm.records.empty(), "arming starts at current sequence");
            ChangeRecord event;
            event.path = L"C:\\feed-fixture\\新.txt"; event.old_path = L"C:\\feed-fixture\\旧.txt";
            event.kind = ChangeKind::Renamed; event.time = ChangeTracker::Now(); event.file_id = 99;
            engine.changes_.Open(dir.wstring());
            engine.changes_.Lease(L"feed-fixture", true);
            engine.changes_.Record(event);
            for (unsigned i = 0; i < 100020; ++i) engine.RecordFeed(event);
            check(engine.ReadFeed(true, L"", arm.epoch, 0).gap, "slow consumer detects count-retention gap");
            const auto page = engine.ReadFeed(true, L"", arm.epoch, 20);
            check(!page.gap && !page.done && page.records.size() == 512 && page.next == 532 &&
                page.records.front().id == 21 && page.records.front().path == event.path &&
                page.records.front().old_path == event.old_path && page.records.front().file_id == event.file_id &&
                page.records.front().kind == event.kind, "512-event page preserves cursor and rename payload");
            check(engine.ReadFeed(true, L"", arm.epoch, 100020).done, "caught-up consumer returns done");
            const auto path = (dir / L"snapshot.bin").wstring();
            check(Save(engine, path), "save isolated feed-generation snapshot");
            {
                std::unique_lock lock(engine.mutex_);
                check(Load(engine, path), "replace mapped snapshot under engine lock");
            }
            check(engine.ReadFeed(true, L"", arm.epoch, 100020).gap, "old generation still requires resynchronization");
            check(engine.feed_changes_.Size() == 0, "snapshot replacement releases unreachable old-generation feed");
            arm = engine.ReadFeed(true, L"", 0, 0);
            check(arm.ready && arm.done && arm.records.empty() && arm.next == 100020,
                "new subscription keeps monotonic sequence after generation change");
            engine.RecordFeed(event);
            const auto next = engine.ReadFeed(true, L"", arm.epoch, arm.next);
            check(!next.gap && next.done && next.records.size() == 1 && next.records[0].id == 100021,
                "new-generation event remains readable without false gaps");
            check(engine.changes_.Details(L"feed-fixture", L"C:\\feed-fixture", 0, 0, 200).records.size() == 1,
                "feed retirement does not discard persisted change history");
            auto& memory = engine.filename_timing_.Memory();
            engine.changes_.Flush(true, &memory);
            engine.changes_.Flush(false, &memory);
            check(memory.At(IndexMemoryPoint::ChangeFlushBefore).calls == 1 &&
                memory.At(IndexMemoryPoint::ChangeFlushSnapshot).calls == 1 &&
                memory.At(IndexMemoryPoint::ChangeFlushAfter).calls == 1 &&
                memory.retained.flush_snapshot_records == 1 && memory.retained.history_records == 1,
                "memory sampling preserves sixty-second batching and snapshot record counts");
            engine.CaptureMemoryState();
            check(memory.retained.feed_records == 1 && memory.retained.feed_page_capacity_bytes >= 65536 &&
                memory.retained.aggregate_mapped_file_bytes > 0,
                "memory diagnostics distinguish feed capacity from mapped file size");
            engine.filename_timing_.Flush(true);
            std::ifstream timing(dir / L"pulse-index-timing.jsonl");
            const std::string line((std::istreambuf_iterator<char>(timing)), std::istreambuf_iterator<char>());
            timing.close();
            check(line.find("\"stages\":") != std::string::npos && line.find("\"memory\":") != std::string::npos &&
                line.find("\"change_flush_snapshot\":") != std::string::npos &&
                memory.At(IndexMemoryPoint::TimingFlush).failures == 0,
                "existing timing log retains stages and adds sampled memory points");
            engine.Stop();
        }
        SetActiveIndexDirectory(L"");
        std::error_code ec;
        std::filesystem::remove_all(dir, ec);
        check(!ec, "isolated feed fixture cleaned up");
        return ok;
    }

    static bool MaintenanceFixture() {
        bool ok = true;
        auto check = [&](bool valid, const char* label) {
            std::cout << (valid ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= valid;
        };
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-maintenance-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        std::filesystem::create_directories(dir);
        SetActiveIndexDirectory(dir.wstring());
        check(MergeRetryDelayMs(0) == 0 && MergeRetryDelayMs(1) == 5000 && MergeRetryDelayMs(2) == 10000 &&
            MergeRetryDelayMs(7) == 320000 && MergeRetryDelayMs(8) == 600000 && MergeRetryDelayMs(1000) == 600000,
            "merge retry delay doubles from five seconds and caps at ten minutes");
        const bool own_runtime = !pulse::diagnostics::runtime::Enabled() &&
            pulse::diagnostics::runtime::Initialize((dir / L"runtime").wstring(), "test");
        {
            Engine engine;
            check(Build(engine), "build isolated maintenance fixture");
            engine.struct_changes_ = 1;
            engine.last_struct_tick_ = 1;
            engine.last_merge_tick_ = 1;
            check(engine.MaintenanceMergeReason(600001, 0) != nullptr, "quiet pending changes request a merge");
            engine.MergeBase(true, "fixture");
            check(engine.struct_changes_ == 0 && engine.merge_retry_after_tick_ == 0,
                "successful merge clears pending structural work");
            check(engine.MaintenanceMergeReason(GetTickCount64() + 1200000, 0) == nullptr,
                "a successful merge never repeats solely because the quiet timer expired");
            engine.Stop();
        }
        {
            Engine engine;
            check(Build(engine), "build isolated failed-merge fixture");
            const auto cache = CacheFilePath();
            HANDLE blocker = CreateFileW(cache.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(blocker != INVALID_HANDLE_VALUE, "lock fixture snapshot against replacement");
            engine.built_unix_ = 42;
            engine.struct_changes_ = 100000;
            if (blocker != INVALID_HANDLE_VALUE) {
                engine.MergeBase(true, "fixture_failure");
                const auto retry = engine.merge_retry_after_tick_;
                check(retry > GetTickCount64() && engine.built_unix_ == 42 && engine.struct_changes_ == 100000,
                    "failed merge retains generation and pending changes and schedules retry");
                engine.MergeBase(true, "fixture_failure");
                check(engine.merge_retry_after_tick_ == retry &&
                    engine.MaintenanceMergeReason(retry - 1, 100000000) == nullptr,
                    "forced compaction and delta triggers honor failure backoff");
                check(engine.merge_failures_ == 1, "a gated merge attempt is not counted as another failure");
                engine.merge_retry_after_tick_ = 0;
                const auto second_started = GetTickCount64();
                engine.MergeBase(true, "fixture_failure");
                check(engine.merge_failures_ == 2 &&
                    engine.merge_retry_after_tick_ >= second_started + MergeRetryDelayMs(2),
                    "repeated merge failure doubles the retry delay");
                CloseHandle(blocker);
                engine.merge_retry_after_tick_ = 0;
                engine.MergeBase(true, "fixture_retry");
                check(engine.struct_changes_ == 0, "failed merge succeeds once replacement becomes possible");
                check(engine.merge_failures_ == 0 && engine.merge_retry_after_tick_ == 0,
                    "successful merge resets the failure backoff");
                if (own_runtime) {
                    check(pulse::diagnostics::runtime::Flush(5000), "flush merge runtime events");
                    std::string main_log, critical_log;
                    for (const auto& entry : std::filesystem::directory_iterator(dir / L"runtime" / L"Diagnostics" / L"Runtime")) {
                        std::ifstream file(entry.path(), std::ios::binary);
                        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
                        (entry.path().filename().wstring().ends_with(L".critical.jsonl") ? critical_log : main_log) += text;
                    }
                    auto has_line = [](const std::string& text, std::initializer_list<const char*> parts) {
                        size_t begin = 0;
                        while (begin < text.size()) {
                            const size_t end = (std::min)(text.find('\n', begin), text.size());
                            const auto line = std::string_view(text).substr(begin, end - begin);
                            bool all = true;
                            for (const char* part : parts) all &= line.find(part) != std::string_view::npos;
                            if (all) return true;
                            begin = end + 1;
                        }
                        return false;
                    };
                    check(has_line(main_log, {"\"event\":\"index_merge_end\"", "\"outcome\":1", "\"failures\":1", "\"retry_ms\":5000"}) &&
                        has_line(main_log, {"\"event\":\"index_merge_end\"", "\"outcome\":0", "\"failures\":0"}),
                        "merge failure and recovery leave terminal runtime events");
                    check(has_line(critical_log, {"\"event\":\"index_merge_end\"", "\"evidence\":\"last\"", "\"occurrences\":2", "\"failures\":2"}),
                        "repeated merge failures aggregate into one critical pattern");
                }
            }
        }
        {
            Engine engine;
            engine.running_ = true;
            VolumeInfo failed; failed.id = L"failed-fixture-volume"; failed.mount_point = L"X:\\";
            VolumeInfo healthy; healthy.id = L"healthy-fixture-volume"; healthy.mount_point = L"Y:\\";
            unsigned attempts = 0;
            const auto epoch = GetTickCount64() + 1000000;
            engine.indexed_ = 123;
            engine.built_unix_ = 456;
            auto failure = [&](const VolumeInfo& volume) {
                ++attempts;
                if (volume.id != failed.id) return true;
                engine.indexed_ = 12;
                engine.built_unix_ = 45;
                return false;
            };
            engine.RecoverFailedVolumes({failed, healthy}, epoch + 1000, failure);
            for (ULONGLONG tick = 1001; tick < 61000; tick += 1000)
                engine.RecoverFailedVolumes({failed}, epoch + tick, failure);
            check(attempts == 2 && !engine.volume_retry_after_.contains(healthy.id),
                "persistent single-volume failure is attempted once per retry window; healthy volume is not retried");
            check(engine.Count() == 123 && engine.built_unix_ == 456,
                "failed single-volume recovery preserves the last published count and generation");
            engine.RecoverFailedVolumes({failed}, epoch + 61000, failure);
            check(attempts == 3, "failed volume becomes eligible after sixty seconds");
            engine.RecoverFailedVolumes({failed}, epoch + 121000, [](const VolumeInfo&) { return true; });
            check(engine.volume_retry_after_.empty(), "successful recovery clears its retry state");
            unsigned successful_attempts = 0;
            engine.RecoverFailedVolumes({failed}, epoch + 122000, [&](const VolumeInfo&) { ++successful_attempts; return true; });
            check(successful_attempts == 0, "successful recovery suppresses repeated overflow during cooldown");
            engine.RecoverFailedVolumes({failed}, epoch + 181000, [&](const VolumeInfo&) { ++successful_attempts; return true; });
            check(successful_attempts == 1, "persistent overflow becomes eligible after successful recovery cooldown");
        }
        {
            Engine engine;
            check(Build(engine), "build self-log exclusion fixture");
            const auto before = engine.LiveCount();
            Usn(engine, 9990, 20, L"pulse-index-timing.jsonl", USN_REASON_FILE_CREATE);
            check(engine.LiveCount() == before, "USN excludes the default filename timing log");
            engine.index_directory_ = L"C:\\Users\\TestUser\\index-data";
            for (const auto* name : {L"pulse-index-timing.jsonl", L"pulse-index-timing.jsonl.1", L"index-data", L"index-data\\internal.tmp"}) {
                const size_t chars = wcslen(name);
                std::vector<BYTE> packet(sizeof(FILE_NOTIFY_INFORMATION) + chars * sizeof(wchar_t));
                auto* notification = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
                notification->Action = FILE_ACTION_MODIFIED;
                notification->FileNameLength = static_cast<DWORD>(chars * sizeof(wchar_t));
                memcpy(notification->FileName, name, chars * sizeof(wchar_t));
                check(engine.ApplyNotifyLocked(L"C:\\Users\\TestUser", packet.data(), static_cast<DWORD>(packet.size())) == 0 &&
                    engine.struct_changes_ == 0 && engine.LiveCount() == before,
                    "directory notifications exclude self logs, their directory and descendants without marking changes");
            }
        }
        SetActiveIndexDirectory(L"");
        if (own_runtime) pulse::diagnostics::runtime::Shutdown();
        std::filesystem::remove_all(dir);
        return ok;
    }

    static bool IdleFixture(unsigned seconds) {
        if (seconds < 5 || seconds > 3600) return false;
        const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
            (L"filename-idle-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
        const auto root = dir / L"files";
        const auto data = root / L"index-data";
        std::filesystem::create_directories(data);
        { std::ofstream seed(root / L"stable-fixture.txt"); seed << "fixture"; }
        SetMachineIndexScope(false);
        SetActiveIndexDirectory(data.wstring());
        Engine engine;
        engine.StartFixture(nullptr, 0, root.wstring());
        auto wait_until = [](const auto& predicate, unsigned timeout_ms) {
            const auto deadline = GetTickCount64() + timeout_ms;
            while (!predicate() && GetTickCount64() < deadline) Sleep(20);
            return predicate();
        };
        const bool ready = wait_until([&] { return engine.Ready() && !engine.building_.load() && engine.Count() > 0; }, 30000);
        Sleep(2000);
        auto cpu = [] {
            FILETIME created{}, exited{}, kernel{}, user{};
            GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
            return ((static_cast<uint64_t>(kernel.dwHighDateTime) << 32) | kernel.dwLowDateTime) +
                ((static_cast<uint64_t>(user.dwHighDateTime) << 32) | user.dwLowDateTime);
        };
        const auto revision = engine.Revision();
        const auto started = GetTickCount64();
        const auto before = cpu();
        std::cout << "[INFO] isolated filename idle observation started: " << seconds << " seconds\n" << std::flush;
        while (GetTickCount64() - started < seconds * 1000ull) Sleep(250);
        const double elapsed = static_cast<double>(GetTickCount64() - started);
        const double percent = static_cast<double>(cpu() - before) / (elapsed * 100.0);
        const bool stable = revision == engine.Revision();
        auto has = [&](const wchar_t* name) { Query query; query.needle = name; return engine.Search(query).total > 0; };
        { std::ofstream item(root / L"created-fixture.txt"); item << "created"; }
        const bool created = wait_until([&] { return has(L"created-fixture.txt"); }, 3000);
        std::filesystem::rename(root / L"created-fixture.txt", root / L"renamed-fixture.txt");
        const bool renamed = wait_until([&] { return has(L"renamed-fixture.txt") && !has(L"created-fixture.txt"); }, 3000);
        std::filesystem::remove(root / L"renamed-fixture.txt");
        const bool deleted = wait_until([&] { return !has(L"renamed-fixture.txt"); }, 3000);
        const auto stop_started = GetTickCount64();
        engine.Stop();
        const bool stopped = GetTickCount64() - stop_started < 2000;
        SetActiveIndexDirectory(L"");
        const bool ok = ready && stable && percent < 1.0 && created && renamed && deleted && stopped;
        std::cout << "[INFO] elapsed_ms=" << elapsed << " single_core_cpu_percent=" << percent
            << " ready=" << ready << " revision_stable=" << stable << " create=" << created
            << " rename=" << renamed << " delete=" << deleted << " prompt_stop=" << stopped << '\n';
        std::wcout << L"[INFO] retained isolated timing log: " << (data / L"pulse-index-timing.jsonl").wstring() << L'\n';
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << "filename idle CPU and post-idle live updates\n";
        return ok;
    }

    static bool CoverageBenchmark() {
        constexpr uint32_t siblings = 1000000;
        std::vector<Node> nodes(siblings + 2);
        std::vector<Attr> attrs(siblings + 2);
        std::vector<int32_t> order(siblings + 2);
        std::vector<wchar_t> pool;
        pool.reserve(static_cast<size_t>(siblings) * 16);
        auto add = [&](uint32_t index, int32_t parent, const std::wstring& name) {
            auto& node = nodes[index]; node.parent = parent; node.flags = Engine::kFlagDir;
            node.off = static_cast<uint32_t>(pool.size()); node.len = static_cast<uint16_t>(name.size());
            pool.insert(pool.end(), name.begin(), name.end()); order[index] = static_cast<int32_t>(index);
        };
        add(0, -1, L"C:"); add(1, 0, L"Large");
        for (uint32_t i = 0; i < siblings; ++i) {
            wchar_t name[32]{}; swprintf_s(name, L"folder%08u", i); add(i + 2, 1, name);
        }
        DiskHeader header{}; header.pool_chars = pool.size(); header.node_count = static_cast<uint32_t>(nodes.size());
        Engine engine; engine.map_ = std::make_unique<Engine::MappedFile>();
        engine.map_->hdr = &header;
        engine.map_->n = static_cast<uint32_t>(nodes.size()); engine.map_->nodes = nodes.data();
        engine.map_->attrs = attrs.data(); engine.map_->pool = pool.data(); engine.map_->child_order = order.data();
        engine.ready_ = true;
        std::vector<std::wstring> paths;
        for (uint32_t i = siblings - 6; i < siblings; ++i) {
            wchar_t name[64]{}; swprintf_s(name, L"\\\\?\\C:\\Large\\folder%08u", i); paths.emplace_back(name);
        }
        bool valid = true;
        const auto start = std::chrono::steady_clock::now();
        for (const auto& path : paths) valid &= engine.ChangeCoverage(path) == ChangeState::Gap;
        const double milliseconds = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        std::cout << "[INFO] 1000000 mapped siblings / 6 coverage paths: " << milliseconds << " ms\n";
        engine.tombstones_.insert(siblings + 1);
        valid &= engine.ChangeCoverage(paths.back()) == ChangeState::NotCovered;
        valid &= engine.ChangeCoverage(L"C:\\Large\\missing") == ChangeState::NotCovered;
        std::cout << (valid && milliseconds < 1500.0 ? "[PASS] " : "[FAIL] ") << "mapped coverage bounded lookup and tombstone/missing path checks\n";
        return valid && milliseconds < 1500.0;
    }

    static bool Build(Engine& e) {
        e.running_ = true;
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        Engine::VolState volume;
        volume.letter = L'C';
        volume.kind = VolumeKind::Removable;
        volume.volume_id = L"test-volume";
        std::vector<Engine::FrnNode> nodes;
        auto add = [&](uint64_t id, uint64_t parent, const wchar_t* name, bool dir) {
            Engine::FrnNode node;
            node.frn = id;
            node.parent = parent;
            node.name = name;
            node.is_dir = dir;
            nodes.push_back(std::move(node));
        };
        // Deliberately put a descendant before its ancestors in FRN order.
        add(1, 30, L"settings.toml", false);
        add(10, 5, L"Users", true);
        add(20, 10, L"TestUser", true);
        add(30, 20, L".codex", true);
        add(40, 20, L"excluded", true);
        add(41, 40, L".codex", true);
        add(50, 20, L"excluded-neighbor", true);
        add(51, 50, L".codex", true);
        add(60, 20, L"node_modules", true);
        add(61, 60, L".codex", true);
        add(70, 20, L".config", true);
        add(80, 20, L".codex-backup", true);
        add(90, 20, L"sample.codex", false);
        add(110, 20, L"AppData", true);
        add(111, 110, L"Local", true);
        add(112, 111, L"Temp", true);
        add(113, 112, L"ShowBoxDebug.log", false);
        const bool ok = e.BuildMftTree(std::move(volume), 5, std::move(nodes));
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.ready_ = true;
        return ok;
    }

    static bool Save(Engine& e, const std::wstring& path) {
        Engine::Store store;
        std::vector<Engine::VolState> volumes;
        return e.FlattenLocked(store, volumes) && e.WriteIndexFile(path, store, volumes, 123456) &&
            MoveFileExW((path + L".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    }

    static bool Load(Engine& e, const std::wstring& path) {
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!e.MapIndexFile(path, mapped)) return false;
        e.AdoptMappedLocked(std::move(mapped));
        e.running_ = true;
        return true;
    }

    static bool NeedsRebuild(const Engine& e) { return e.NeedsSearchRebuildLocked(); }

    static bool ReplayAttrOnly(Engine& e) {
        const int32_t log = e.FindByFrnLocked(e.vols_.front(), 113);
        const int32_t folder = e.FindByFrnLocked(e.vols_.front(), 112);
        if (log < 0 || folder < 0) return false;
        DeltaLog delta;
        if (!delta.Open(DeltaFilePathForVolume(e.vols_.front().volume_id), e.built_unix_)) return false;
        for (int32_t idx : {log, folder})
            delta.QueuePatch(idx, static_cast<uint8_t>(PatchBits::Attr), 0, 0, 100, 4096, {});
        if (!delta.Flush()) return false;
        delta.Close();
        e.ReplayDeltasLocked();
        e.InvalidateFilterLocked();
        return e.AttrAt(log).size == 4096 && e.AttrAt(folder).mtime == 100 &&
            (e.NodeAt(folder).flags & Engine::kFlagDir) != 0;
    }

    static void RepairOffline(Engine& e) {
        // Load started the pinyin worker, which reads the mapping under a
        // shared lock. Dropping the snapshot without the lock unmaps it under
        // that reader (intermittent 0xC0000005 in the default suite).
        std::unique_lock lock(e.mutex_);
        e.excluded_paths_ = {L"C:\\Users\\TestUser\\excluded"};
        e.PreserveOfflineVolumesLocked({}, IndexConfig{});
        e.map_.reset();
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.InvalidateFilterLocked();
    }

    static bool RootHidden(const Engine& e) {
        return (e.NodeAt(e.vols_.front().root_idx).flags & Engine::kFlagHidden) != 0;
    }

    // Volume-level system folders are hidden whatever their case, and the NTFS
    // metadata files are hidden only directly below the volume root. The rule
    // must hold for MFT builds, USN creates and later visibility refreshes.
    static bool SystemNamesFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        Engine e;
        e.running_ = true;
        Engine::VolState volume;
        volume.letter = L'C';
        volume.kind = VolumeKind::Removable;
        volume.volume_id = L"system-names-volume";
        std::vector<Engine::FrnNode> nodes;
        auto add = [&](uint64_t id, uint64_t parent, const wchar_t* name, bool dir) {
            Engine::FrnNode node;
            node.frn = id; node.parent = parent; node.name = name; node.is_dir = dir; node.size = 1;
            nodes.push_back(std::move(node));
        };
        add(0, 5, L"$MFT", false);
        add(2, 5, L"$LogFile", false);
        add(11, 5, L"$Extend", true);
        add(25, 11, L"$UsnJrnl", false);
        add(30, 5, L"$RECYCLE.BIN", true);
        add(31, 30, L"S-1-5-21-1", true);
        add(32, 31, L"$RSAMPLE.txt", false);
        add(40, 5, L"system volume information", true);
        add(41, 40, L"tracking.log", false);
        add(50, 5, L"Work", true);
        add(51, 50, L"$Extend", true);
        add(52, 50, L"$MFT", false);
        const bool built = e.BuildMftTree(std::move(volume), 5, std::move(nodes));
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.ready_ = true;
        auto found = [&](const wchar_t* needle, const wchar_t* path) {
            Query q; q.needle = needle; q.limit = 100;
            const auto result = e.Search(q);
            return std::any_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) { return hit.path == path; });
        };
        auto expect = [&](const char* phase) {
            const std::string suffix = std::string(" (") + phase + ")";
            check(!found(L"MFT", L"C:\\$MFT") && !found(L"LogFile", L"C:\\$LogFile") &&
                  !found(L"Extend", L"C:\\$Extend") && !found(L"UsnJrnl", L"C:\\$Extend\\$UsnJrnl"),
                  ("NTFS metadata files below the volume root are hidden" + suffix).c_str());
            check(found(L"Extend", L"C:\\Work\\$Extend") && found(L"MFT", L"C:\\Work\\$MFT"),
                  ("metadata names below ordinary folders stay searchable" + suffix).c_str());
            check(!found(L"RSAMPLE", L"C:\\$RECYCLE.BIN\\S-1-5-21-1\\$RSAMPLE.txt"),
                  ("upper-case $RECYCLE.BIN content is hidden" + suffix).c_str());
            check(!found(L"tracking", L"C:\\system volume information\\tracking.log"),
                  ("System Volume Information is hidden in any case" + suffix).c_str());
        };
        check(built, "system name fixture builds");
        expect("MFT build");
        e.RefreshSubtreeVisibilityLocked(e.vols_.front().root_idx, nullptr);
        e.InvalidateFilterLocked();
        expect("visibility refresh");
        Usn(e, 3, 5, L"$Volume", USN_REASON_FILE_CREATE);
        Usn(e, 70, 50, L"$Recycle.Bin", USN_REASON_FILE_CREATE);
        Usn(e, 71, 70, L"inside-recycle", USN_REASON_FILE_CREATE);
        Usn(e, 72, 50, L"$Volume", USN_REASON_FILE_CREATE);
        check(!found(L"Volume", L"C:\\$Volume") && found(L"Volume", L"C:\\Work\\$Volume"),
              "USN create hides root metadata names only at the root");
        check(!found(L"inside-recycle", L"C:\\Work\\$Recycle.Bin\\inside-recycle"),
              "USN create below a recycle folder stays hidden");
        return ok;
    }

    // Reused MFT slots can give a deep folder a lower FRN than its ancestors,
    // so the build meets the leaf first and must walk the whole parent chain.
    static bool DeepChainBuildFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* name) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << name << '\n'; ok &= value;
        };
        constexpr int kDepth = 140;
        Engine e;
        e.running_ = true;
        Engine::VolState volume;
        volume.letter = L'C';
        volume.kind = VolumeKind::Removable;
        volume.volume_id = L"deep-chain-volume";
        std::vector<Engine::FrnNode> nodes;
        auto add = [&](uint64_t id, uint64_t parent, const wchar_t* name, bool dir) {
            Engine::FrnNode node;
            node.frn = id; node.parent = parent; node.name = name; node.is_dir = dir; node.size = 1;
            nodes.push_back(std::move(node));
        };
        add(5000, 5, L"deep", true);
        uint64_t parent = 5000;
        std::wstring path = L"C:\\deep";
        for (int depth = 1; depth <= kDepth; ++depth) {
            const uint64_t id = 4000 - static_cast<uint64_t>(depth);   // deeper = lower FRN
            add(id, parent, L"d", true);
            parent = id;
            path += L"\\d";
        }
        add(100, parent, L"value.bin", false);
        const bool built = e.BuildMftTree(std::move(volume), 5, std::move(nodes));
        e.live_ = std::move(e.build_);
        e.vols_ = std::move(e.build_vols_);
        e.RebuildChildMapLocked();
        e.ready_ = true;
        check(built, "deep chain fixture builds");
        const int32_t leaf = e.ResolvePathLocked(path + L"\\value.bin");
        check(leaf >= 0 && e.BuildPathLocked(leaf) == path + L"\\value.bin",
              "leaf met before 140 unresolved ancestors keeps its full path");
        check(e.ResolvePathLocked(L"C:\\d") < 0, "deep chain is never re-rooted below the volume root");
        // A damaged MFT can hold a parent cycle longer than the checked depth.
        Engine cyclic;
        cyclic.running_ = true;
        Engine::VolState cycle_volume;
        cycle_volume.letter = L'C';
        cycle_volume.kind = VolumeKind::Removable;
        cycle_volume.volume_id = L"cycle-volume";
        nodes.clear();
        for (uint64_t i = 0; i < 100; ++i) add(7000 + i, 7000 + (i + 1) % 100, L"loop", true);
        add(6000, 7050, L"inside-loop.txt", false);
        const auto started = GetTickCount64();
        const bool cycle_built = cyclic.BuildMftTree(std::move(cycle_volume), 5, std::move(nodes));
        check(GetTickCount64() - started < 5000 && cyclic.build_.nodes.size() <= 1 + 101,
              "parent cycle deeper than the checked depth terminates without duplicates");
        (void)cycle_built;
        return ok;
    }

    static bool RecycleFixture() {
        Engine engine; if (!Build(engine)) return false;
        Usn(engine, 200, 5, L"$Recycle.Bin", USN_REASON_FILE_CREATE);
        Usn(engine, 201, 200, L"S-1-5-21-123", USN_REASON_FILE_CREATE);
        engine.changes_.Lease(L"fixture", true);
        Usn(engine, 90, 201, L"$Rsample.codex", USN_REASON_RENAME_NEW_NAME);
        auto result = engine.changes_.Details(L"fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const bool deleted = result.records.size() == 1 && result.records[0].kind == ChangeKind::Deleted &&
            result.records[0].path == L"C:\\Users\\TestUser\\sample.codex";
        Usn(engine, 90, 20, L"sample.codex", USN_REASON_RENAME_NEW_NAME);
        result = engine.changes_.Details(L"fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const bool restored = result.records.size() == 1 && result.records[0].kind == ChangeKind::Created;
        std::cout << (deleted && restored ? "[PASS]" : "[FAIL]") << " USN recycle and restore original path\n";
        return deleted && restored;
    }

    static bool ParentCycleFixture() {
        Engine e;
        if (!Build(e)) return false;
        auto id = [&](uint64_t frn) { return e.FindByFrnLocked(e.vols_.front(), frn); };
        const int32_t user = id(20), codex = id(30);
        const Node original = e.NodeAt(user);
        bool ok = true;
        auto check = [&](bool passed, const char* label) {
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= passed;
        };
        e.vols_.front().journal_id = 123;
        Usn(e, 20, 20, L"TestUser", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(user).parent == original.parent && e.vols_.front().journal_id == 0,
              "reject self-parent USN rename and request volume recovery");
        Usn(e, 20, 30, L"TestUser", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(user).parent == original.parent, "reject ancestor moved beneath descendant");
        Usn(e, 30, 50, L".codex", USN_REASON_RENAME_NEW_NAME);
        check(e.NodeAt(codex).parent == id(50), "accept valid directory move");
        Usn(e, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
        const auto root = e.vols_.front().root_idx;
        Usn(e, 5, 5, L"root", USN_REASON_FILE_CREATE);
        check(e.NodeAt(root).parent == -1, "preserve volume root");
        // Reproduce a cycle already present in an old in-memory/delta tree.
        e.live_.nodes[user].parent = codex;
        Usn(e, 200, 20, L"new-folder", USN_REASON_FILE_CREATE);
        check(id(200) < 0, "existing parent cycle cannot hang USN processing");
        check(!e.InSubtreeLocked(user, id(50)), "existing cycle cannot hang subtree lookup");
        Term term;
        term.name = L"never-present";
        term.name_how = NameHow::Substring;
        term.name_in_path = true;
        CompiledQuery query;
        query.groups = {{term}};
        check(!e.MatchNodeLocked(user, query, -1, false, false),
              "existing cycle cannot hang path-name search");
        e.live_.nodes[user].parent = e.LiveCount() + 100;
        Usn(e, 201, 20, L"invalid-parent", USN_REASON_FILE_CREATE);
        check(id(201) < 0, "reject out-of-range ancestor");
        e.live_.nodes[user].parent = original.parent;
        e.RequestStop();
        Usn(e, 202, 20, L"cancelled", USN_REASON_FILE_CREATE);
        check(id(202) < 0, "stop cancels parent validation");
        Engine source;
        const auto file = std::filesystem::path(L"bench_data") /
            (L"index-parent-cycle-" + std::to_wstring(GetCurrentProcessId()) + L".bin");
        const bool saved = Build(source) && Save(source, file.wstring());
        check(saved, "save isolated mapped fixture");
        if (saved) {
            Engine mapped;
            const bool loaded = Load(mapped, file.wstring());
            check(loaded, "load mapped parent fixture");
            if (loaded) {
                const int32_t target = mapped.FindByFrnLocked(mapped.vols_.front(), 30);
                const int32_t excluded = mapped.FindByFrnLocked(mapped.vols_.front(), 40);
                Usn(mapped, 30, 40, L".codex", USN_REASON_RENAME_NEW_NAME);
                check(mapped.NodeAt(target).parent == excluded &&
                      (mapped.NodeAt(target).flags & Engine::kFlagHidden),
                      "mapped rename preserves excluded visibility");
                Usn(mapped, 40, 30, L"excluded", USN_REASON_RENAME_NEW_NAME);
                check(mapped.NodeAt(excluded).parent != target,
                      "reject cycle through mapped overlay even when parent is hidden");
                Usn(mapped, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
                check(!(mapped.NodeAt(target).flags & Engine::kFlagHidden),
                      "mapped valid move restores visibility");
                const int32_t config = mapped.FindByFrnLocked(mapped.vols_.front(), 70);
                const int32_t settings = mapped.FindByFrnLocked(mapped.vols_.front(), 1);
                const auto batch = UsnBlob({{30, USN_REASON_RENAME_NEW_NAME}, {80, USN_REASON_DATA_EXTEND},
                    {80, USN_REASON_DATA_EXTEND | USN_REASON_CLOSE}}, 70, L".codex");
                uint64_t applied = 0;
                bool any_struct = false, any_attr = false;
                mapped.vols_.front().journal_id = 1;
                const auto result = mapped.ApplyUsnBatchLocked(mapped.vols_.front(), batch.data(),
                    batch.data() + batch.size(), applied, any_struct, any_attr);
                check(result == Engine::UsnBatch::Done && any_struct && mapped.NodeAt(target).parent == config &&
                      mapped.NodeAt(settings).parent == target, "journal batch applies a directory move");
                const auto back = UsnBlob({{30, USN_REASON_RENAME_NEW_NAME}}, 20, L".codex");
                const auto restored = mapped.ApplyUsnBatchLocked(mapped.vols_.front(), back.data(),
                    back.data() + back.size(), applied, any_struct, any_attr);
                check(restored == Engine::UsnBatch::Done &&
                      mapped.NodeAt(target).parent == mapped.FindByFrnLocked(mapped.vols_.front(), 20),
                      "journal batch moves the directory back");
            }
        }
        std::filesystem::remove(file);
        return ok;
    }

    static std::vector<BYTE> UsnBlob(std::initializer_list<std::pair<uint64_t, DWORD>> records,
                                     uint64_t parent = 20, const wchar_t* name = L"blob.bin") {
        std::vector<BYTE> blob;
        const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
        for (const auto& [id, reason] : records) {
            const size_t size = (sizeof(USN_RECORD_V2) + length + 7) & ~size_t{7};
            std::vector<BYTE> bytes(size);
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
            rec->RecordLength = static_cast<DWORD>(size);
            rec->MajorVersion = 2;
            rec->FileReferenceNumber = id;
            rec->ParentFileReferenceNumber = parent;
            rec->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
            rec->FileNameLength = length;
            rec->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
            rec->Reason = reason;
            memcpy(bytes.data() + rec->FileNameOffset, name, length);
            blob.insert(blob.end(), bytes.begin(), bytes.end());
        }
        return blob;
    }
    static bool UsnCoalesceFixture() {
        bool ok = true;
        auto check = [&](bool value, const char* label) {
            std::cout << (value ? "[PASS] " : "[FAIL] ") << label << '\n'; ok &= value;
        };
        constexpr DWORD ext = USN_REASON_DATA_EXTEND, over = USN_REASON_DATA_OVERWRITE,
            basic = USN_REASON_BASIC_INFO_CHANGE, close = USN_REASON_CLOSE;
        auto mask = [](const std::vector<BYTE>& blob) {
            return Engine::SupersededUsnRecords(blob.data(), blob.data() + blob.size());
        };
        using Mask = std::vector<uint8_t>;
        check(mask(UsnBlob({{1, ext}, {2, ext}, {1, ext | close}})) == Mask{1, 0, 0},
              "attribute record superseded by a later attribute record of the same file");
        check(mask(UsnBlob({{1, basic}, {1, basic | over}, {1, basic | over | close}})) == Mask{1, 1, 0},
              "only the last record of an attribute run is applied");
        check(mask(UsnBlob({{1, ext}, {1, ext | USN_REASON_RENAME_NEW_NAME}})) == Mask{0, 0},
              "attribute record before a rename stays");
        check(mask(UsnBlob({{1, ext}, {1, USN_REASON_RENAME_OLD_NAME}, {1, ext}})) == Mask{0, 0, 0},
              "rename between attribute records keeps both");
        check(mask(UsnBlob({{1, USN_REASON_FILE_CREATE | ext}, {1, USN_REASON_FILE_CREATE | ext | close}})) == Mask{0, 0},
              "create session records stay");
        check(mask(UsnBlob({{1, ext}, {1, USN_REASON_FILE_DELETE | close}})) == Mask{0, 0},
              "attribute record before a delete stays");
        check(mask(UsnBlob({{1, ext}, {1, close}})) == Mask{0, 0}, "record without attribute reasons breaks the run");
        check(mask(UsnBlob({{1, ext}, {1, USN_REASON_HARD_LINK_CHANGE | ext}})) == Mask{0, 0},
              "hard link change keeps the attribute record");
        check(mask(UsnBlob({{1, ext}, {1, basic | close}})) == Mask{0, 0},
              "data record stays before a record without data reasons");
        check(mask(UsnBlob({{1, basic}, {2, basic}, {1, ext | close}, {2, basic | close}})) == Mask{1, 1, 0, 0},
              "basic info record superseded by a later attribute record");
        check(mask({}).empty(), "empty batch");
        return ok;
    }

    static void Usn(Engine& e, uint64_t id, uint64_t parent, const wchar_t* name, DWORD reason) {
        const auto length = static_cast<WORD>(wcslen(name) * sizeof(wchar_t));
        std::vector<BYTE> bytes(sizeof(USN_RECORD_V2) + length);
        auto* rec = reinterpret_cast<USN_RECORD_V2*>(bytes.data());
        rec->RecordLength = static_cast<DWORD>(bytes.size());
        rec->FileReferenceNumber = id;
        rec->ParentFileReferenceNumber = parent;
        rec->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
        rec->FileNameLength = length;
        rec->FileAttributes = FILE_ATTRIBUTE_DIRECTORY;
        rec->Reason = reason;
        FILETIME now{}; GetSystemTimeAsFileTime(&now);
        rec->TimeStamp.QuadPart = (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        memcpy(bytes.data() + rec->FileNameOffset, name, length);
        e.ApplyUsnLocked(e.vols_.front(), rec);
    }

    static bool Stage2NonNtfsFixture() {
        bool ok = true;
        auto check = [&](bool passed, const char* label) {
            std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
            ok &= passed;
        };

        auto make_file = [](const std::filesystem::path& p) {
            std::filesystem::create_directories(p.parent_path());
            std::ofstream out(p, std::ios::binary);
            out << "fixture";
        };
        auto has_hit = [](Engine& eng, const wchar_t* needle, const wchar_t* expected_path) {
            Query q;
            q.needle = needle;
            q.limit = 100;
            const SearchResult res = eng.Search(q);
            return std::any_of(res.hits.begin(), res.hits.end(), [&](const Hit& h) {
                return CompareStringOrdinal(h.path.c_str(), -1, expected_path, -1, TRUE) == CSTR_EQUAL;
            });
        };
        auto hit_count = [](Engine& eng, const wchar_t* needle) {
            Query q;
            q.needle = needle;
            q.limit = 100;
            return eng.Search(q).total;
        };
        auto make_notify = [](DWORD action, const std::wstring& rel_name) {
            const size_t bytes_needed = offsetof(FILE_NOTIFY_INFORMATION, FileName) +
                rel_name.size() * sizeof(wchar_t);
            std::vector<BYTE> buf(bytes_needed);
            auto* info = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(buf.data());
            info->NextEntryOffset = 0;
            info->Action = action;
            info->FileNameLength = static_cast<DWORD>(rel_name.size() * sizeof(wchar_t));
            std::memcpy(info->FileName, rel_name.data(), info->FileNameLength);
            return buf;
        };

        std::error_code ec;
        const std::filesystem::path fixture_dir = std::filesystem::absolute(
            std::filesystem::path(L"bench_data") /
            (L"stage2_engine_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
             std::to_wstring(GetTickCount64())));
        const std::filesystem::path index_dir = fixture_dir / L"index";
        const std::filesystem::path usb_e_v1 = fixture_dir / L"usb_e_v1";
        const std::filesystem::path usb_g = fixture_dir / L"usb_g";
        const std::filesystem::path usb_e_v2 = fixture_dir / L"usb_e_v2";
        const std::filesystem::path card_v2 = fixture_dir / L"card_v2";
        const std::filesystem::path real_root = fixture_dir / L"real_notify_root";
        std::filesystem::create_directories(index_dir, ec);
        make_file(usb_e_v1 / L"readme_fat.txt");
        make_file(usb_e_v1 / L"DCIM" / L"photo1.jpg");
        make_file(usb_g / L"exfat_archive.zip");
        make_file(usb_e_v2 / L"usb2_only.txt");
        make_file(card_v2 / L"card_swapped.bin");
        make_file(real_root / L"sub" / L"nested" / L"deep_doc.txt");

        const bool prev_machine = MachineIndexScope();
        SetActiveIndexDirectory(index_dir.wstring());
        SetMachineIndexScope(true);

        VolumeInfo vol_c;
        vol_c.id = L"\\\\?\\Volume{00000000-0000-0000-0000-000000000001}";
        vol_c.mount_point = L"C:\\";
        vol_c.file_system = L"NTFS";
        vol_c.kind = VolumeKind::Fixed;
        vol_c.backend = VolumeBackend::NtfsMftUsn;
        vol_c.supported = true;
        vol_c.online = true;
        vol_c.enabled = true;

        VolumeInfo vol_e_fat32;
        vol_e_fat32.id = L"\\\\?\\Volume{00000000-0000-0000-0000-0000000000E1}";
        vol_e_fat32.mount_point = L"E:\\";
        vol_e_fat32.file_system = L"FAT32";
        vol_e_fat32.kind = VolumeKind::Removable;
        vol_e_fat32.backend = VolumeBackend::DirectoryWalk;
        vol_e_fat32.serial = 0x11112222u;
        vol_e_fat32.supported = true;
        vol_e_fat32.online = true;
        vol_e_fat32.enabled = true;

        VolumeInfo vol_g_exfat;
        vol_g_exfat.id = L"\\\\?\\Volume{00000000-0000-0000-0000-0000000000A1}";
        vol_g_exfat.mount_point = L"G:\\";
        vol_g_exfat.file_system = L"exFAT";
        vol_g_exfat.kind = VolumeKind::Removable;
        vol_g_exfat.backend = VolumeBackend::DirectoryWalk;
        vol_g_exfat.serial = 0x33334444u;
        vol_g_exfat.supported = true;
        vol_g_exfat.online = true;
        vol_g_exfat.enabled = true;

        Engine e;
        if (!Build(e)) {
            SetMachineIndexScope(prev_machine);
            SetActiveIndexDirectory(L"");
            std::filesystem::remove_all(fixture_dir, ec);
            return false;
        }
        e.vols_[0].volume_id = vol_c.id;
        e.vols_[0].kind = VolumeKind::Fixed;
        e.vols_[0].backend = VolumeBackend::NtfsMftUsn;
        e.built_unix_ = 123456;
        {
            SetMachineIndexScope(false);
            const bool saved_legacy = Save(e, CacheFilePath());
            SetMachineIndexScope(true);
            const bool init_ok = saved_legacy && e.TryLoadCache();
            e.ready_ = init_ok;
            check(init_ok, "stage 2 seed C: base shard");
        }

        e.test_walk_roots_[NormalizeVolumeId(vol_e_fat32.id)] = usb_e_v1.wstring();
        e.test_walk_roots_[NormalizeVolumeId(vol_g_exfat.id)] = usb_g.wstring();
        const bool built_e = e.RebuildVolume(vol_e_fat32);
        const bool built_g = e.RebuildVolume(vol_g_exfat);
        check(built_e && built_g, "stage 2 RebuildVolume indexes FAT32 E: and exFAT G: alongside NTFS C:");

        {
            Engine reloaded;
            reloaded.running_ = true;
            const bool mapped_ok = reloaded.TryLoadCache();
            if (mapped_ok) reloaded.ready_ = true;
            const bool meta_ok = mapped_ok && reloaded.vols_.size() == 3 &&
                reloaded.vols_[0].backend == VolumeBackend::NtfsMftUsn &&
                reloaded.vols_[1].backend == VolumeBackend::DirectoryWalk &&
                reloaded.vols_[1].fs_tag == VolumeFsTag::Fat32 &&
                reloaded.vols_[1].serial == 0x11112222u &&
                reloaded.vols_[2].backend == VolumeBackend::DirectoryWalk &&
                reloaded.vols_[2].fs_tag == VolumeFsTag::ExFat &&
                reloaded.vols_[2].serial == 0x33334444u;
            check(meta_ok, "V9 snapshot round-trip preserves C:/E:/G: backends, fs_tag, and serial");

            check(has_hit(reloaded, L"readme_fat.txt", L"E:\\readme_fat.txt") &&
                  has_hit(reloaded, L"photo1.jpg", L"E:\\DCIM\\photo1.jpg") &&
                  has_hit(reloaded, L"exfat_archive.zip", L"G:\\exfat_archive.zip") &&
                  has_hit(reloaded, L"sample.codex", L"C:\\Users\\TestUser\\sample.codex"),
                  "mixed NTFS + FAT32 + exFAT search hits across all indexed volumes");

            const FileFeedPage e_feed = reloaded.ReadFeed(false, L"E:\\", 0, 0);
            bool only_e = e_feed.ready && e_feed.done && !e_feed.gap && !e_feed.records.empty();
            bool saw_readme = false;
            for (const auto& rec : e_feed.records) {
                if (rec.path.size() < 3 || (rec.path[0] != L'E' && rec.path[0] != L'e') ||
                    rec.path[1] != L':' || rec.path[2] != L'\\') {
                    only_e = false;
                }
                if (CompareStringOrdinal(rec.path.c_str(), -1, L"E:\\readme_fat.txt", -1, TRUE) == CSTR_EQUAL) {
                    saw_readme = true;
                }
            }
            check(only_e && saw_readme, "ReadFeed(E:\\) isolates E: span and skips C:/G:");

            // Per-volume_id WAL isolation & replay
            {
                std::unique_lock<std::shared_mutex> lock(reloaded.mutex_);
                reloaded.OpenDeltasLocked();
                const auto pkt = make_notify(FILE_ACTION_ADDED, L"wal_note.txt");
                reloaded.ApplyNotifyLocked(L"E:\\", pkt.data(), static_cast<DWORD>(pkt.size()),
                                           vol_e_fat32.id);
            }
            reloaded.FlushDeltas();
            reloaded.CloseDeltas();

            Engine wal_reloaded;
            wal_reloaded.running_ = true;
            const bool wal_ok = wal_reloaded.TryLoadCache();
            if (wal_ok) wal_reloaded.ready_ = true;
            check(wal_ok &&
                  has_hit(wal_reloaded, L"wal_note.txt", L"E:\\wal_note.txt") &&
                  has_hit(wal_reloaded, L"exfat_archive.zip", L"G:\\exfat_archive.zip") &&
                  has_hit(wal_reloaded, L"sample.codex", L"C:\\Users\\TestUser\\sample.codex"),
                  "per-volume_id WAL replay restores E: additions without disturbing C:/G:");
            wal_reloaded.Stop();

            // Unplug visibility hide + ReadFeed filtering
            {
                std::unique_lock<std::shared_mutex> lock(reloaded.mutex_);
                reloaded.UpdateVolumeVisibilityLocked({vol_c}, false);
            }
            const FileFeedPage hidden_e_feed = reloaded.ReadFeed(false, L"E:\\", 0, 0);
            const FileFeedPage all_feed = reloaded.ReadFeed(false, L"", 0, 0);
            const bool leaked_offline = std::any_of(all_feed.records.begin(), all_feed.records.end(),
                [](const ChangeRecord& r) {
                    return !r.path.empty() && (r.path[0] == L'E' || r.path[0] == L'e' ||
                                               r.path[0] == L'G' || r.path[0] == L'g');
                });
            check(hit_count(reloaded, L"readme_fat.txt") == 0 &&
                  hit_count(reloaded, L"exfat_archive.zip") == 0 &&
                  has_hit(reloaded, L"sample.codex", L"C:\\Users\\TestUser\\sample.codex") &&
                  hidden_e_feed.records.empty() && !leaked_offline,
                  "unplugging E:/G: hides offline items from Search and ReadFeed");

            // Drive-letter relocation E: -> F: and back
            VolumeInfo vol_e_at_f = vol_e_fat32;
            vol_e_at_f.mount_point = L"F:\\";
            {
                std::unique_lock<std::shared_mutex> lock(reloaded.mutex_);
                reloaded.RelocateVolumeLetterLocked(reloaded.vols_[1], L'F');
                reloaded.UpdateVolumeVisibilityLocked({vol_c, vol_e_at_f}, false);
            }
            check(has_hit(reloaded, L"readme_fat.txt", L"F:\\readme_fat.txt") &&
                  !has_hit(reloaded, L"readme_fat.txt", L"E:\\readme_fat.txt"),
                  "RelocateVolumeLetterLocked moves E: volume root to F:");
            {
                std::unique_lock<std::shared_mutex> lock(reloaded.mutex_);
                reloaded.RelocateVolumeLetterLocked(reloaded.vols_[1], L'E');
                reloaded.UpdateVolumeVisibilityLocked({vol_c, vol_e_fat32}, false);
            }
            check(has_hit(reloaded, L"readme_fat.txt", L"E:\\readme_fat.txt"),
                  "RelocateVolumeLetterLocked restores volume root back to E:");
            reloaded.Stop();
        }

        // Real on-disk ApplyNotifyLocked + IndexAddedDirectories + FILE_ACTION_REMOVED subtree tombstoning
        {
            Engine notify_eng;
            notify_eng.running_ = true;
            notify_eng.AddForTest(real_root.wstring(), real_root.filename().wstring(), true);
            notify_eng.ready_ = true;

            std::vector<std::wstring> added_dirs;
            {
                std::unique_lock<std::shared_mutex> lock(notify_eng.mutex_);
                const auto pkt = make_notify(FILE_ACTION_ADDED, L"sub");
                notify_eng.ApplyNotifyLocked(real_root.wstring(), pkt.data(),
                                             static_cast<DWORD>(pkt.size()), {}, &added_dirs);
            }
            check(added_dirs.size() == 1, "ApplyNotifyLocked queues newly created directory for subtree walk");
            notify_eng.IndexAddedDirectories(added_dirs, {});
            const std::wstring deep_doc = (real_root / L"sub" / L"nested" / L"deep_doc.txt").wstring();
            check(has_hit(notify_eng, L"deep_doc.txt", deep_doc.c_str()),
                  "IndexAddedDirectories indexes nested files inside newly added directory");

            {
                std::unique_lock<std::shared_mutex> lock(notify_eng.mutex_);
                const auto pkt = make_notify(FILE_ACTION_REMOVED, L"sub");
                notify_eng.ApplyNotifyLocked(real_root.wstring(), pkt.data(),
                                             static_cast<DWORD>(pkt.size()));
            }
            check(hit_count(notify_eng, L"deep_doc.txt") == 0 && hit_count(notify_eng, L"sub") == 0,
                  "FILE_ACTION_REMOVED tombstones removed directory and its nested subtree");
            notify_eng.Stop();
        }

        // Same-letter swap (offline USB-1 vs active USB-2 on E:) and card-reader media swap
        VolumeInfo vol_e_usb2;
        vol_e_usb2.id = L"\\\\?\\Volume{00000000-0000-0000-0000-0000000000E2}";
        vol_e_usb2.mount_point = L"E:\\";
        vol_e_usb2.file_system = L"FAT32";
        vol_e_usb2.kind = VolumeKind::Removable;
        vol_e_usb2.backend = VolumeBackend::DirectoryWalk;
        vol_e_usb2.serial = 0x55556666u;
        vol_e_usb2.supported = true;
        vol_e_usb2.online = true;
        vol_e_usb2.enabled = true;

        e.test_walk_roots_[NormalizeVolumeId(vol_e_usb2.id)] = usb_e_v2.wstring();
        const bool built_usb2 = e.RebuildVolume(vol_e_usb2);
        int32_t active_e_root = -1;
        int32_t resolved_e_root = -2;
        {
            std::unique_lock<std::shared_mutex> lock(e.mutex_);
            e.UpdateVolumeVisibilityLocked({vol_c, vol_e_usb2}, false);
            for (const auto& v : e.vols_) {
                if (NormalizeVolumeId(v.volume_id) == NormalizeVolumeId(vol_e_usb2.id)) {
                    active_e_root = v.root_idx;
                }
            }
            resolved_e_root = e.ResolvePathLocked(L"E:\\");
        }
        check(built_usb2 && active_e_root >= 0 && resolved_e_root == active_e_root &&
              has_hit(e, L"usb2_only.txt", L"E:\\usb2_only.txt") &&
              hit_count(e, L"readme_fat.txt") == 0,
              "same-letter swap shows active USB-2 on E: and keeps offline USB-1 hidden");

        VolumeInfo card_v2_info = vol_e_usb2;
        card_v2_info.serial = 0x77776666u;
        check(!VolumeIdentityMatches(card_v2_info.id, card_v2_info.file_system, card_v2_info.serial,
                                     vol_e_usb2.id, VolumeBackend::DirectoryWalk,
                                     VolumeFsTag::Fat32, vol_e_usb2.serial),
              "card-reader swap with different serial triggers VolumeIdentityMatches mismatch");
        e.test_walk_roots_[NormalizeVolumeId(card_v2_info.id)] = card_v2.wstring();
        const bool built_card2 = e.RebuildVolume(card_v2_info);
        {
            std::unique_lock<std::shared_mutex> lock(e.mutex_);
            e.UpdateVolumeVisibilityLocked({vol_c, card_v2_info}, false);
        }
        check(built_card2 &&
              has_hit(e, L"card_swapped.bin", L"E:\\card_swapped.bin") &&
              hit_count(e, L"usb2_only.txt") == 0 &&
              has_hit(e, L"sample.codex", L"C:\\Users\\TestUser\\sample.codex"),
              "card-reader RebuildVolume replaces E: span without touching C:");

        e.Stop();
        SetMachineIndexScope(prev_machine);
        SetActiveIndexDirectory(L"");
        std::filesystem::remove_all(fixture_dir, ec);
        return ok;
    }
};

} // namespace pulse::index

using namespace pulse::index;
namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
SearchResult Search(Engine& e, const wchar_t* needle) {
    Query q;
    q.needle = needle;
    q.limit = 100;
    return e.Search(q);
}
bool Has(Engine& e, const wchar_t* needle, const wchar_t* path) {
    const auto result = Search(e, needle);
    return std::any_of(result.hits.begin(), result.hits.end(), [&](const Hit& hit) {
        return hit.path == path;
    });
}
void CheckSearch(Engine& e) {
    constexpr auto log_path = L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log";
    Check(Has(e, L"ShowBoxDebug.log", log_path), "mixed-case dotted temp filename searchable");
    Check(Has(e, L"showboxdebug.LOG", log_path), "temp filename search ignores case");
    Check(Has(e, L"\"ShowBoxDebug.log\"", log_path), "quoted exact temp filename searchable");
    const std::wstring filename = L"ShowBoxDebug.log";
    bool incremental = true;
    for (size_t length = 1; length <= filename.size(); ++length)
        incremental = Has(e, filename.substr(0, length).c_str(), log_path) && incremental;
    Check(incremental, "incremental filename search preserves temp log result");
    Check(Has(e, L"Debug.log", log_path), "interior filename bigram finds temp log");
    Check(Has(e, L"ShowBox .log", log_path), "conjunctive query intersects rarest bigrams");
    Check(Has(e, L"debug s", log_path), "conjunctive query with single char selects multi-char driver");
    Check(Has(e, L"no_such_zz | Debug.log", log_path), "disjunctive query falls back without bigram starvation");
    {
        Query scoped;
        scoped.needle = L"settings";
        scoped.limit = 100;
        scoped.path_prefix = L"C:\\Users\\TestUser\\.codex";
        const auto hit = e.Search(scoped);
        Check(hit.total == 1 && hit.hits.size() == 1 &&
                  hit.hits[0].path == L"C:\\Users\\TestUser\\.codex\\settings.toml",
              "scoped literal query finds descendant via DFS subtree slice");
        scoped.path_prefix = L"C:\\Users\\TestUser\\.config";
        Check(e.Search(scoped).total == 0, "scoped literal query excludes sibling subtree");
    }
    Check(Has(e, L".codex", L"C:\\Users\\TestUser\\.codex"), "dot folder searchable");
    Check(Has(e, L"settings", L"C:\\Users\\TestUser\\.codex\\settings.toml"), "descendant searchable");
    Check(Has(e, L".config", L"C:\\Users\\TestUser\\.config"), "other dot folders searchable");
    Check(!Has(e, L".codex", L"C:\\Users\\TestUser\\excluded\\.codex"), "excluded subtree hidden");
    Check(Has(e, L".codex", L"C:\\Users\\TestUser\\excluded-neighbor\\.codex"), "exclusion respects path boundary");
    Check(!Has(e, L".codex", L"C:\\Users\\TestUser\\node_modules\\.codex"), "dependency subtree hidden");
    Check(Search(e, L"folder: \".codex\"").total == 2, "exact folder query");
    Check(!Has(e, L"file: ext:codex", L"C:\\Users\\TestUser\\.codex"), "extension query excludes dot folder");
    Check(EngineTestAccess::RootHidden(e), "root stays hidden");
    std::atomic<uint32_t> latest{2};
    Query q;
    q.needle = L".codex";
    Check(e.Search(q, &latest, 1).hits.empty(), "stale request cancelled");
}
}

#include "index_name_pool_fixture.h"
#include "index_quiet_diagnostics_fixture.h"

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--regex-only") return EngineTestAccess::RegexFixture() ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--exclusion-concurrency-only") return EngineTestAccess::ExclusionConcurrencyFixture() ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--audit-query-only") return EngineTestAccess::AuditQueryFixture() ? 0 : 1;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--validate-cache") return EngineTestAccess::ValidateCache(argv[2]) ? 0 : 1;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--diagnostic-cache") {
        if (!pulse::diagnostics::runtime::Initialize(argv[3], "test")) return 2;
        const bool valid = EngineTestAccess::ValidateCache(argv[2]);
        pulse::diagnostics::runtime::Shutdown();
        return valid ? 0 : 1;
    }
    if (argc == 4 && std::wstring_view(argv[1]) == L"--recover-cache") return EngineTestAccess::RecoverCache(argv[2], argv[3]) ? 0 : 1;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--reopen-cache") return EngineTestAccess::ReopenCache(argv[2]) ? 0 : 1;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--merge-peak-probe") return EngineTestAccess::MergePeakProbe(argv[2]) ? 0 : 1;
    if (argc == 4 && std::wstring_view(argv[1]) == L"--replay-probe")
        return EngineTestAccess::ReplayProbe(argv[2], static_cast<uint32_t>(wcstoul(argv[3], nullptr, 10))) ? 0 : 1;
    if ((argc == 5 || argc == 6) && std::wstring_view(argv[1]) == L"--usn-probe")
        return EngineTestAccess::UsnProbe(argv[2], static_cast<uint32_t>(wcstoul(argv[3], nullptr, 10)),
            static_cast<uint32_t>(wcstoul(argv[4], nullptr, 10)), argc == 6 && wcstoul(argv[5], nullptr, 10) != 0) ? 0 : 1;
    if ((argc == 5 || argc == 6) && std::wstring_view(argv[1]) == L"--steady-memory-probe")
        return EngineTestAccess::SteadyMemoryProbe(argv[2], static_cast<uint32_t>(wcstoul(argv[3], nullptr, 10)),
            static_cast<uint32_t>(wcstoul(argv[4], nullptr, 10)),
            argc == 6 ? static_cast<uint32_t>(wcstoul(argv[5], nullptr, 10)) : 0) ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--hierarchy-only") return EngineTestAccess::HierarchyFixture() ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--overlay-only") return EngineTestAccess::OverlayContainersFixture() ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--usn-coalesce-only") return EngineTestAccess::UsnCoalesceFixture() ? 0 : 1;
    const bool quiet = argc > 1 && std::wstring_view(argv[1]) == L"--quiet-maintenance-only";
    if (!SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", quiet ? nullptr : L"1")) return 2;
    if (quiet) return EngineTestAccess::QuietDiagnosticsFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--visibility-cache-only") return EngineTestAccess::VisibilityCacheFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--name-pool-only") return EngineTestAccess::NamePoolFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--folder-sizes-only") return EngineTestAccess::FolderSizesFixture() ? 0 : 1;
    if (argc == 2 && std::wstring_view(argv[1]) == L"--folder-size-index-only") return EngineTestAccess::FolderSizeIndexFixture() ? 0 : 1;
    if (argc == 3 && std::wstring_view(argv[1]) == L"--folder-size-bench")
        return EngineTestAccess::FolderSizeBench(static_cast<uint32_t>(wcstoul(argv[2], nullptr, 10))) ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--usn-only") return EngineTestAccess::UsnQueueFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--feed-only") return EngineTestAccess::FeedFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--maintenance-only") return EngineTestAccess::MaintenanceFixture() ? 0 : 1;
    if (argc > 2 && std::wstring_view(argv[1]) == L"--idle-seconds") return EngineTestAccess::IdleFixture(static_cast<unsigned>(wcstoul(argv[2], nullptr, 10))) ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--parent-cycle-only") return EngineTestAccess::ParentCycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--recycle-only") return EngineTestAccess::RecycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--system-names-only") return EngineTestAccess::SystemNamesFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--deep-chain-only") return EngineTestAccess::DeepChainBuildFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--coverage-only") return EngineTestAccess::CoverageBenchmark() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--hardlink-only")
        return EngineTestAccess::HardLinkBuildFixture() && EngineTestAccess::HardLinkUsnFixture() ? 0 : 1;
    std::cout << std::unitbuf;
    Engine engine;
    Check(EngineTestAccess::Build(engine), "actual MFT tree builder accepts fixture");
    Check(EngineTestAccess::OverlayContainersFixture(), "overlay containers keep exact membership");
    CheckSearch(engine);
    Check(EngineTestAccess::UsnCoalesceFixture(), "journal batch coalescing fixture");
    Check(EngineTestAccess::FolderSizeIndexFixture(), "folder totals array fixture");
    Check(EngineTestAccess::HardLinkBuildFixture(), "hard link MFT build fixture");
    Check(EngineTestAccess::HardLinkUsnFixture(), "hard link USN fixture");
    Check(EngineTestAccess::SystemNamesFixture(), "system name visibility fixture");
    Check(EngineTestAccess::DeepChainBuildFixture(), "deep chain MFT build fixture");
    const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"index_engine_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(dir);
    {
        const auto log_path = dir / L"partial-write.dlt";
        DeltaLog delta;
        Check(delta.Open(log_path.wstring(), 42), "open partial-write fixture");
        constexpr int records = 70000;
        for (int i = 0; i < records; ++i) delta.QueueUsn(1, i);
        HANDLE blocker = CreateFileW(log_path.c_str(), GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
        OVERLAPPED lock{};
        lock.Offset = 16 + (1u << 20);
        const bool locked = blocker != INVALID_HANDLE_VALUE &&
            LockFileEx(blocker, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &lock);
        Check(locked, "lock second write chunk");
        if (locked) {
            Check(!delta.Flush() && delta.HasPending(), "partial write retains all pending records");
            UnlockFileEx(blocker, 0, 1, 0, &lock);
        }
        if (blocker != INVALID_HANDLE_VALUE) CloseHandle(blocker);
        Check(delta.Flush() && !delta.HasPending(), "retry writes a complete log");
        delta.Close();
        Check(std::filesystem::file_size(log_path) == 16ull + records * 17ull,
              "partial write retry does not duplicate log bytes");
    }
    {
        const auto selected = dir / L"selected-index";
        const auto next = dir / L"next-index";
        std::filesystem::create_directory(selected);
        std::filesystem::create_directory(next);
        SetMachineIndexScope(true);
        SetActiveIndexDirectory(selected.wstring());
        Check(DataDir() == selected.wstring(), "running host reports actual index directory");
        Check(CacheFilePath() == selected.wstring() + L"\\pulse-index.bin",
              "base cache follows active directory");
        const auto delta_path = DeltaFilePath(L'C');
        {
            DeltaLog delta;
            Check(delta.Open(delta_path, 123), "open delta in selected directory");
            delta.QueueUsn(1, 2);
            Check(delta.Flush(), "update delta in selected directory");
        }
        const auto old_size = std::filesystem::file_size(delta_path);
        SetActiveIndexDirectory(next.wstring());
        {
            DeltaLog delta;
            Check(delta.Open(DeltaFilePath(L'C'), 124), "reopened delta uses replacement directory");
            delta.QueueUsn(1, 3);
            Check(delta.Flush(), "replacement directory receives index updates");
        }
        Check(std::filesystem::file_size(delta_path) == old_size &&
              std::filesystem::exists(next / L"pulse-index-C.dlt"),
              "closed old directory receives no replacement writes");
        Check(DeltaFilePathForVolume(L"test-volume").find(next.wstring() + L"\\") == 0,
              "volume shard delta follows active directory");
        SetActiveIndexDirectory(L"");
        SetMachineIndexScope(false);
    }
    const auto base = dir / L"base.bin";
    Check(EngineTestAccess::Save(engine, base.wstring()), "write snapshot");
    Check(ValidateShardBase(base.wstring()), "shard accepts new snapshot");
    for (bool mapped_fixture : {false, true}) {
        const auto replay_dir = dir / (mapped_fixture ? L"mapped-attr" : L"live-attr");
        std::filesystem::create_directory(replay_dir);
        SetActiveIndexDirectory(replay_dir.wstring());
        Engine replayed;
        Check(mapped_fixture ? EngineTestAccess::Load(replayed, base.wstring()) :
              EngineTestAccess::Build(replayed), "prepare attr-only replay fixture");
        Check(EngineTestAccess::ReplayAttrOnly(replayed), "attr-only replay updates attributes and preserves directory flags");
        Check(Has(replayed, L"ShowBoxDebug.log", L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log"),
              "attr-only replay preserves filename search and full path");
        const auto compact = replay_dir / L"compact.bin";
        Check(EngineTestAccess::Save(replayed, compact.wstring()), "compact attr-only replay snapshot");
        Engine reloaded;
        Check(EngineTestAccess::Load(reloaded, compact.wstring()), "reload attr-only replay snapshot");
        Check(Has(reloaded, L"ShowBoxDebug.log", L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log"),
              "attr-only replay remains searchable after compact and reload");
        SetActiveIndexDirectory(L"");
    }
    {
        Engine mapped;
        Check(EngineTestAccess::Load(mapped, base.wstring()), "load snapshot");
        Check(!EngineTestAccess::NeedsRebuild(mapped), "new snapshot does not rebuild again");
        CheckSearch(mapped);
        EngineTestAccess::Usn(mapped, 30, 40, L".codex", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L"settings.toml").total == 0, "moving directory hides existing descendants");
        EngineTestAccess::Usn(mapped, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
        Check(Has(mapped, L"settings.toml", L"C:\\Users\\TestUser\\.codex\\settings.toml"),
              "moving directory restores existing descendants");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_FILE_CREATE);
        Check(Has(mapped, L".codex-new", L"C:\\Users\\TestUser\\.codex-new"), "USN create searchable");
        EngineTestAccess::Usn(mapped, 100, 40, L".codex-new", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L".codex-new").total == 0, "USN move into excluded directory");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L".codex-new").total == 1, "USN move out of excluded directory");
        EngineTestAccess::Usn(mapped, 100, 20, L".codex-new", USN_REASON_FILE_DELETE);
        Check(Search(mapped, L".codex-new").total == 0, "USN delete removed");
        constexpr auto log_path = L"C:\\Users\\TestUser\\AppData\\Local\\Temp\\ShowBoxDebug.log";
        Check(Has(mapped, L"ShowBoxDebug", log_path), "unpatched snapshot hit found while overlay is non-empty");
        EngineTestAccess::Usn(mapped, 30, 70, L".codex", USN_REASON_RENAME_NEW_NAME);
        {
            Query q; q.needle = L"ext:toml"; q.limit = 100;
            q.path_prefix = L"C:\\Users\\TestUser\\.config";
            const auto moved = mapped.Search(q);
            Check(moved.total == 1 && moved.hits.size() == 1 &&
                  moved.hits[0].path == L"C:\\Users\\TestUser\\.config\\.codex\\settings.toml",
                  "scoped scan finds descendant of moved snapshot directory");
            q.needle = L"settings";
            Check(mapped.Search(q).total == 1, "scoped literal search finds descendant of moved directory");
            q.path_prefix = L"C:\\Users\\TestUser\\AppData";
            Check(mapped.Search(q).total == 0, "moved descendant stays out of unrelated scope");
        }
        EngineTestAccess::Usn(mapped, 30, 20, L".codex", USN_REASON_RENAME_NEW_NAME);
        Check(Has(mapped, L"settings.toml", L"C:\\Users\\TestUser\\.codex\\settings.toml"),
              "directory moved back keeps descendants searchable");
        EngineTestAccess::Usn(mapped, 113, 112, L"renamed-debug.log", USN_REASON_RENAME_NEW_NAME);
        Check(Search(mapped, L"renamed-debug").total == 1, "renamed snapshot node found by new name");
        Check(Search(mapped, L"ShowBoxDebug").total == 0, "renamed snapshot node not found by old name");
        Check(Search(mapped, L"sample.codex").total == 1, "snapshot file searchable before delete");
        EngineTestAccess::Usn(mapped, 90, 20, L"sample.codex", USN_REASON_FILE_DELETE);
        Check(Search(mapped, L"sample.codex").total == 0, "deleted snapshot file excluded from literal search");
    }
    {
        std::fstream file(base, std::ios::binary | std::ios::in | std::ios::out);
        DiskHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        header.ver = 10;
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        file.close();
        Engine legacy;
        Check(EngineTestAccess::Load(legacy, base.wstring()) && EngineTestAccess::NeedsRebuild(legacy),
              "V10 snapshot requests repair for persisted attr-only corruption");
    }
    // Reproduce a persisted V9 index whose root flag contaminated all descendants.
    {
        std::fstream file(base, std::ios::binary | std::ios::in | std::ios::out);
        DiskHeader header{};
        file.read(reinterpret_cast<char*>(&header), sizeof(header));
        header.ver = 9;
        file.seekp(0);
        file.write(reinterpret_cast<const char*>(&header), sizeof(header));
        for (uint32_t i = 0; i < header.node_count; ++i) {
            const auto offset = static_cast<std::streamoff>(header.nodes_off + i * sizeof(Node));
            Node node;
            file.seekg(offset);
            file.read(reinterpret_cast<char*>(&node), sizeof(node));
            node.flags |= 2;
            file.seekp(offset);
            file.write(reinterpret_cast<const char*>(&node), sizeof(node));
        }
    }
    {
        Engine old;
        Check(EngineTestAccess::Load(old, base.wstring()), "legacy snapshot remains readable");
        Check(EngineTestAccess::NeedsRebuild(old), "legacy snapshot triggers repair");
        Check(Search(old, L".codex").total == 0, "fixture reproduces old search failure");
        EngineTestAccess::RepairOffline(old);
        CheckSearch(old);
        Check(EngineTestAccess::Save(old, (dir / L"repaired.bin").wstring()), "repaired offline snapshot persists");
    }
    {
        const auto paths = MakeShardPaths(dir.wstring(), L"migration-test");
        ShardManifest manifest;
        Check(PublishShardBase(paths, base.wstring(), 123456, 0, manifest), "publish legacy generation");
        Check(PublishShardBase(paths, (dir / L"repaired.bin").wstring(), 123456, 0, manifest),
              "publish repaired generation");
        std::wstring active;
        Check(ResolveActiveShard(paths, manifest, active), "resolve repaired generation");
        {
            Engine repaired;
            Check(EngineTestAccess::Load(repaired, active) && !EngineTestAccess::NeedsRebuild(repaired),
                  "published repair remains current after restart");
            CheckSearch(repaired);
        }
        {
            std::ofstream corrupt(active, std::ios::binary | std::ios::trunc);
            corrupt << "corrupt";
        }
        Check(ResolveActiveShard(paths, manifest, active), "corrupt repaired slot falls back safely");
        Engine fallback;
        Check(EngineTestAccess::Load(fallback, active) && EngineTestAccess::NeedsRebuild(fallback),
              "legacy fallback still requests repair");
    }
    {
        Engine builder;
        Check(EngineTestAccess::Load(builder, (dir / L"repaired.bin").wstring()),
              "load repaired snapshot for 2-bigram intersection test");
        for (int i = 0; i < 100; ++i) {
            const std::wstring name = L"common_prefix_tag_item_" + std::to_wstring(i) + L".txt";
            EngineTestAccess::Usn(builder, 5000 + static_cast<uint64_t>(i), 20, name.c_str(), USN_REASON_FILE_CREATE);
        }
        const auto intersect_path = (dir / L"intersect.bin").wstring();
        Check(EngineTestAccess::Save(builder, intersect_path), "save snapshot with >64 postings per bigram");
        Engine intersected;
        Check(EngineTestAccess::Load(intersected, intersect_path), "reload intersection snapshot");
        Check(Search(intersected, L"common_prefix").total == 100 &&
              Search(intersected, L"common_prefix_tag_item_42").total == 1 &&
              Has(intersected, L"common_prefix_tag_item_42", L"C:\\Users\\TestUser\\common_prefix_tag_item_42.txt"),
              "2-bigram galloping intersection finds exact match cleanly");
        Query scoped_intersect;
        scoped_intersect.needle = L"common_prefix";
        scoped_intersect.path_prefix = L"C:\\Users\\TestUser";
        scoped_intersect.limit = 200;
        Check(intersected.Search(scoped_intersect).total == 100,
              "scoped 2-bigram intersection retains all descendants under TestUser");
        scoped_intersect.path_prefix = L"C:\\Users\\TestUser\\.codex";
        Check(intersected.Search(scoped_intersect).total == 0,
              "scoped 2-bigram intersection rejects unrelated subtree");
    }
    Check(EngineTestAccess::Stage2NonNtfsFixture(), "stage 2 non-NTFS per-volume engine fixture");
    // This directory was created exclusively by this process under bench_data.
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
