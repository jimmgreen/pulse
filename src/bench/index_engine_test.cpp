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

struct EngineTestAccess {
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
        check(!e.MatchNodeLocked(user, query, -1, false, false) &&
              !e.MatchQueryNodeLocked(user, query, -1, false, false),
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
            }
        }
        std::filesystem::remove(file);
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
    if (argc == 2 && std::wstring_view(argv[1]) == L"--hierarchy-only") return EngineTestAccess::HierarchyFixture() ? 0 : 1;
    const bool quiet = argc > 1 && std::wstring_view(argv[1]) == L"--quiet-maintenance-only";
    if (!SetEnvironmentVariableW(L"PULSE_INDEX_DIAGNOSTICS", quiet ? nullptr : L"1")) return 2;
    if (quiet) return EngineTestAccess::QuietDiagnosticsFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--visibility-cache-only") return EngineTestAccess::VisibilityCacheFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--name-pool-only") return EngineTestAccess::NamePoolFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--folder-sizes-only") return EngineTestAccess::FolderSizesFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--usn-only") return EngineTestAccess::UsnQueueFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--feed-only") return EngineTestAccess::FeedFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--maintenance-only") return EngineTestAccess::MaintenanceFixture() ? 0 : 1;
    if (argc > 2 && std::wstring_view(argv[1]) == L"--idle-seconds") return EngineTestAccess::IdleFixture(static_cast<unsigned>(wcstoul(argv[2], nullptr, 10))) ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--parent-cycle-only") return EngineTestAccess::ParentCycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--recycle-only") return EngineTestAccess::RecycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--coverage-only") return EngineTestAccess::CoverageBenchmark() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--hardlink-only")
        return EngineTestAccess::HardLinkBuildFixture() && EngineTestAccess::HardLinkUsnFixture() ? 0 : 1;
    std::cout << std::unitbuf;
    Engine engine;
    Check(EngineTestAccess::Build(engine), "actual MFT tree builder accepts fixture");
    CheckSearch(engine);
    Check(EngineTestAccess::HardLinkBuildFixture(), "hard link MFT build fixture");
    Check(EngineTestAccess::HardLinkUsnFixture(), "hard link USN fixture");
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
    // This directory was created exclusively by this process under bench_data.
    std::filesystem::remove_all(dir);
    return failures ? 1 : 0;
}
