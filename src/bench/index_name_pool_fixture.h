#pragma once
#include "../index/index_name_pool.h"

namespace pulse::index {
bool EngineTestAccess::NamePoolFixture() {
    bool ok = true;
    auto check = [&](bool passed, const char* label) {
        std::cout << (passed ? "[PASS] " : "[FAIL] ") << label << '\n';
        ok &= passed;
    };
    const auto dir = std::filesystem::absolute(std::filesystem::path(L"bench_data") /
        (L"name-pool-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64())));
    std::filesystem::create_directories(dir);
    SetMachineIndexScope(false);
    SetActiveIndexDirectory(dir.wstring());
    {
        Engine empty;
        check(CompactOverlayNamePool(empty.live_.pool, empty.live_.nodes, empty.patches_) == ERROR_SUCCESS,
            "empty name pool compacts without invalid pointer arithmetic");
        check(!empty.CompactNamePoolLocked(GetTickCount64()), "empty engine skips maintenance");
    }
    {
        Engine thresholds;
        const auto now = GetTickCount64();
        thresholds.live_.pool.resize((4ull << 20) + 1);
        thresholds.pool_waste_ = (1ull << 20) - 1;
        check(!thresholds.CompactNamePoolLocked(now), "sub-2-MiB waste does not start compaction");
        thresholds.pool_waste_ = 1ull << 20;
        check(!thresholds.CompactNamePoolLocked(now), "below-25-percent waste does not copy a large live pool");
        thresholds.live_.pool.resize(4ull << 20);
        check(thresholds.CompactNamePoolLocked(now), "exact absolute and ratio thresholds permit local compaction");
    }
    const auto base_path = dir / L"base.bin";
    {
        Engine source;
        check(Build(source) && Save(source, base_path.wstring()), "save isolated baseline without reading volumes");
        source.struct_changes_ = 100000;
        source.last_merge_tick_ = GetTickCount64();
        source.MergeBase(true, "structural_threshold");
        const auto& merge = source.filename_timing_.Maintenance().merge;
        check(merge.attempts == 1 && merge.structural_triggers == 1 && merge.last_committed &&
            merge.struct_changes == 100000 && merge.nodes > 0 && merge.last_wall_us > 0,
            "real full merge records its trigger, pre-merge work, duration and commit result");
        std::cout << "[MAINTENANCE_JSON] " << source.filename_timing_.Maintenance().Json() << '\n';
    }
    for (bool mapped : {false, true}) {
        const auto data = dir / (mapped ? L"failed-merge-mapped" : L"failed-merge-heap");
        std::filesystem::create_directories(data);
        SetActiveIndexDirectory(data.wstring());
        Engine e;
        if (!(mapped ? Load(e, base_path.wstring()) : Build(e))) {
            check(false, "prepare failed merge fixture");
            continue;
        }
        e.ready_ = true;
        const auto blocked = CacheFilePath() + L".tmp";
        check(CreateDirectoryW(blocked.c_str(), nullptr) != FALSE, "block snapshot output with isolated directory");
        for (unsigned attempt = 0; attempt < 2; ++attempt) {
            std::wstring name(240, L'x');
            for (unsigned i = 0; i < 4600; ++i) {
                name.back() = i % 2 ? L'b' : L'a';
                Usn(e, 90, 20, name.c_str(), USN_REASON_RENAME_NEW_NAME);
            }
            Usn(e, 90, 20, L"failure-final", USN_REASON_RENAME_NEW_NAME);
            const auto pending = e.struct_changes_;
            const auto capacity = e.live_.pool.capacity();
            const auto built = e.built_unix_;
            e.merge_retry_after_tick_ = 0;
            e.name_pool_retry_after_tick_ = 0;
            e.MergeBase(true, "structural_threshold");
            const auto id = e.FindByFrnLocked(e.vols_.front(), 90);
            Query query; query.needle = L"failure-final";
            check(e.merge_failures_ == attempt + 1 && e.struct_changes_ == pending &&
                e.built_unix_ == built && e.NameOf(id) == L"failure-final" && e.Search(query).total == 1,
                "repeated failed merges retain pending changes, base timestamp and searchable names");
            check(e.pool_waste_ == 0 && e.live_.pool.capacity() < capacity &&
                e.filename_timing_.Maintenance().pool.successes == attempt + 1,
                "failed merge immediately reclaims obsolete names in heap and mapped overlays");
            check(e.MaintenanceMergeReason(GetTickCount64(), 64ull << 20) == nullptr,
                "failed merge still backs off full publication");
        }
        check(RemoveDirectoryW(blocked.c_str()) != FALSE, "remove isolated snapshot failure");
        e.merge_retry_after_tick_ = 0;
        e.MergeBase(true, "structural_threshold");
        check(e.merge_failures_ == 0 && e.struct_changes_ == 0 && e.map_ &&
            e.NameOf(e.FindByFrnLocked(e.vols_.front(), 90)) == L"failure-final",
            "publication recovers with retained changes after repeated failures");
    }
    for (bool mapped : {false, true}) {
        const auto data = dir / (mapped ? L"mapped" : L"heap");
        std::filesystem::create_directories(data);
        SetActiveIndexDirectory(data.wstring());
        Engine e;
        if (!(mapped ? Load(e, base_path.wstring()) : Build(e))) {
            check(false, "prepare name pool fixture");
            continue;
        }
        e.ready_ = true;
        e.built_unix_ = 123456;
        e.OpenDeltasLocked();
        auto id = [&](uint64_t frn) { return e.FindByFrnLocked(e.vols_.front(), frn); };
        Usn(e, 90, 20, L"first-patch", USN_REASON_RENAME_NEW_NAME);
        const size_t waste_before = e.pool_waste_;
        Usn(e, 90, 20, L"final-patch", USN_REASON_RENAME_NEW_NAME);
        check(e.pool_waste_ == waste_before + wcslen(L"first-patch"),
            "both mapped patches and heap renames account superseded names");
        Usn(e, 1000, 20, L"overlay", USN_REASON_FILE_CREATE);
        std::wstring long_name(240, L'x');
        for (unsigned i = 0; i < 4600; ++i) {
            long_name.back() = i % 2 ? L'b' : L'a';
            Usn(e, 1000, 20, long_name.c_str(), USN_REASON_RENAME_NEW_NAME);
        }
        Usn(e, 1000, 20, L"overlay-final", USN_REASON_RENAME_NEW_NAME);
        Usn(e, 1001, 20, L"tomb-name", USN_REASON_FILE_CREATE);
        Usn(e, 1001, 20, L"tomb-name", USN_REASON_FILE_DELETE);
        e.vols_.front().journal_id = 123;
        e.vols_.front().next_usn = 456;
        e.DeltaFor(L'C')->QueueUsn(123, 456);
        e.FlushDeltas();
        e.changes_.Open(data.wstring());
        e.changes_.Lease(L"name-pool-fixture", true);
        ChangeRecord event;
        event.path = L"C:\\Users\\TestUser\\overlay-final";
        event.old_path = L"C:\\Users\\TestUser\\overlay";
        event.kind = ChangeKind::Renamed;
        e.changes_.Record(event);
        e.RecordFeed(event);
        auto snapshot = [&](Engine& engine) {
            std::vector<std::wstring> result;
            for (int32_t i = 0; i < engine.LiveCount(); ++i) {
                const auto n = engine.NodeAt(i);
                const auto a = engine.AttrAt(i);
                result.push_back(std::to_wstring(i) + L"|" + engine.BuildPathLocked(i) + L"|" +
                    std::to_wstring(n.parent) + L"|" + std::to_wstring(n.flags) + L"|" +
                    std::to_wstring(n.unused) + L"|" + std::to_wstring(a.size) + L"|" +
                    std::to_wstring(a.mtime) + L"|" + std::to_wstring(engine.IsTomb(i)));
            }
            return result;
        };
        const auto before = snapshot(e);
        const auto history = e.changes_.Details(L"name-pool-fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const auto feed = e.ReadFeed(true, L"", e.feed_epoch_, e.feed_sequence_ - 1);
        const auto epoch = e.feed_epoch_.load(), sequence = e.feed_sequence_, revision = e.Revision();
        const auto filter = e.filter_epoch_, built = e.built_unix_;
        const auto child_map = e.child_map_;
        const int32_t overlay = id(1000), tomb = id(1001), patched = id(90);
        auto* mapping = e.map_.get();
        e.struct_changes_ = 7;
        e.last_struct_tick_ = GetTickCount64();
        e.last_merge_tick_ = e.last_struct_tick_;
        check(e.pool_waste_ >= (1ull << 20) && e.MaintenanceMergeReason(GetTickCount64(), 0) == nullptr,
            "old absolute fragmentation threshold no longer requests a full merge");
        Query query; query.needle = L"overlay-final";
        const auto search_before = e.Search(query);
        check(search_before.total == 1, "warm query cache before pool relocation");
        std::atomic<unsigned> reads{0};
        std::atomic<bool> stop{false}, reader_ok{true};
        std::thread reader([&] {
            while (!stop.load()) {
                const auto hits = e.Search(query);
                if (hits.total != 1 || hits.hits.empty() || hits.hits.front().path != L"C:\\Users\\TestUser\\overlay-final")
                    reader_ok = false;
                ++reads;
            }
        });
        while (!reads.load()) std::this_thread::yield();
        const auto started = std::chrono::steady_clock::now();
        {
            std::unique_lock lock(e.mutex_);
            check(e.CompactNamePoolLocked(GetTickCount64()), "local pool compaction succeeds under exclusive query lock");
        }
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - started).count();
        stop = true;
        reader.join();
        check(reader_ok && reads > 0 && e.Search(query).total == 1,
            "concurrent and cached queries remain correct across relocation");
        const auto& stats = e.filename_timing_.Maintenance().pool;
        std::cout << "[INFO] mode=" << (mapped ? "mapped" : "heap") << " before_capacity_bytes=" << stats.before_capacity_bytes
            << " after_capacity_bytes=" << stats.after_capacity_bytes << " reclaimed_chars=" << stats.reclaimed_chars
            << " lock_and_compact_ms=" << elapsed << '\n';
        check(stats.successes == 1 && stats.reclaimed_chars >= (1ull << 20) &&
            stats.after_capacity_bytes < stats.before_capacity_bytes && e.pool_waste_ == 0,
            "local compaction releases obsolete pool capacity and resets only name waste");
        check(snapshot(e) == before && e.child_map_ == child_map &&
            id(1000) == overlay && id(1001) == tomb && id(90) == patched,
            "all paths, node metadata, tombstones, child lookup and FRN identities survive");
        check(e.map_.get() == mapping && e.feed_epoch_ == epoch && e.feed_sequence_ == sequence &&
            e.Revision() == revision && e.filter_epoch_ == filter && e.built_unix_ == built &&
            e.struct_changes_ == 7 && e.deleted_ == 1 && e.vols_.front().next_usn == 456,
            "pool-only maintenance preserves mapped base, generations, USN cursor and pending structural work");
        const auto history_after = e.changes_.Details(L"name-pool-fixture", L"C:\\Users\\TestUser", 0, 0, 200);
        const auto feed_after = e.ReadFeed(true, L"", epoch, sequence - 1);
        check(history.records.size() == 1 && history_after.records.size() == 1 &&
            history_after.records[0].path == history.records[0].path &&
            history_after.records[0].old_path == history.records[0].old_path &&
            !feed_after.gap && !feed_after.records.empty() && feed_after.records.size() == feed.records.size() && feed_after.next == feed.next &&
            feed_after.records.back().old_path == feed.records.back().old_path,
            "history paths and existing subscription cursors are unchanged");
        check(e.filename_timing_.Maintenance().merge.attempts == 0 &&
            e.filename_timing_.Memory().At(IndexMemoryPoint::MergeFlattened).calls == 0,
            "local compaction does not enter the full snapshot expansion path");
        {
            Engine replayed;
            check(mapped ? Load(replayed, base_path.wstring()) : Build(replayed), "prepare independent delta replay");
            replayed.built_unix_ = 123456;
            replayed.ReplayDeltasLocked();
            const auto replay_id = replayed.FindByFrnLocked(replayed.vols_.front(), 1000);
            check(replay_id >= 0 && replayed.NameOf(replay_id) == L"overlay-final" &&
                replayed.vols_.front().next_usn == 456 && replayed.deleted_ == 1 && replayed.pool_waste_ >= (1ull << 20),
                "unchanged delta format replays names, tombstone, cursor and garbage accounting after compaction");
            std::unique_lock lock(replayed.mutex_);
            const auto replay_before = snapshot(replayed);
            check(replayed.CompactNamePoolLocked(GetTickCount64()) && snapshot(replayed) == replay_before,
                "replayed live-node and patch references both survive relocation");
        }
        const auto compact_path = data / L"compact.bin";
        check(Save(e, compact_path.wstring()), "necessary full snapshot can still be saved after local compaction");
        {
            Engine reloaded;
            check(Load(reloaded, compact_path.wstring()), "reload compacted snapshot");
            const auto i = reloaded.FindByFrnLocked(reloaded.vols_.front(), 1000);
            check(i >= 0 && reloaded.NameOf(i) == L"overlay-final", "full publication preserves final filename and FRN mapping");
        }
        const auto now = GetTickCount64();
        auto reason_is = [](const char* actual, const char* expected) {
            return actual && std::strcmp(actual, expected) == 0;
        };
        e.struct_changes_ = 100000;
        check(reason_is(e.MaintenanceMergeReason(now, 0), "structural_threshold"), "structural merge trigger remains active");
        e.struct_changes_ = 0;
        check(reason_is(e.MaintenanceMergeReason(now, 64ull << 20), "delta_threshold"), "delta-size merge trigger remains active");
        e.deleted_ = static_cast<size_t>(e.LiveCount()) / 10 + 1;
        check(reason_is(e.MaintenanceMergeReason(now, 0), "deleted_ratio"), "deletion-ratio merge has its own diagnostic reason");
        e.deleted_ = 1;
        e.struct_changes_ = 7;
        check(reason_is(e.MaintenanceMergeReason(now + 600001, 0), "quiet_changes"), "quiet pending changes can still publish a new base");
        e.merge_retry_after_tick_ = now + 10000;
        check(e.MaintenanceMergeReason(now, 64ull << 20) == nullptr, "full merge failure backoff remains effective");
        e.filename_timing_.Flush(true);
        std::ifstream timing(data / L"pulse-index-timing.jsonl");
        const std::string line((std::istreambuf_iterator<char>(timing)), std::istreambuf_iterator<char>());
        check(line.find("\"maintenance\":") != std::string::npos && line.find("\"name_pool_compact\":") != std::string::npos &&
            line.find("\"reclaimed_chars\":") != std::string::npos, "bounded timing log includes maintenance counters and pool sizes");
        // Test preflight failure after valid references, before any mutation.
        const auto valid_off = e.live_.nodes.back().off;
        e.live_.nodes.back().off = UINT32_MAX;
        const auto original_pool = e.live_.pool;
        const auto first_off = e.live_.nodes.front().off;
        e.pool_waste_ = 1ull << 20;
        e.name_pool_retry_after_tick_ = 0;
        check(!e.CompactNamePoolLocked(now) && e.live_.pool == original_pool &&
            e.live_.nodes.front().off == first_off && e.pool_waste_ == (1ull << 20),
            "invalid reference aborts compaction without changing pool, offsets or waste");
        const auto attempts = e.filename_timing_.Maintenance().pool.attempts;
        check(!e.CompactNamePoolLocked(now) && e.filename_timing_.Maintenance().pool.attempts == attempts,
            "failed pool compaction is throttled without triggering full merge");
        e.live_.nodes.back().off = valid_off;
        e.pool_waste_ = 0;
        Usn(e, 1001, 20, L"restored", USN_REASON_FILE_CREATE);
        check(id(1001) == tomb && !e.IsTomb(tomb) && e.NameOf(tomb) == L"restored", "tombstoned FRN can be revived after compaction");
        auto notify = [&](DWORD action, const wchar_t* name) {
            const size_t bytes = wcslen(name) * sizeof(wchar_t);
            std::vector<BYTE> packet(sizeof(FILE_NOTIFY_INFORMATION) + bytes);
            auto* entry = reinterpret_cast<FILE_NOTIFY_INFORMATION*>(packet.data());
            entry->Action = action;
            entry->FileNameLength = static_cast<DWORD>(bytes);
            std::memcpy(entry->FileName, name, bytes);
            e.ApplyNotifyLocked(L"C:\\Users\\TestUser", packet.data(), static_cast<DWORD>(packet.size()));
        };
        const auto notify_waste = e.pool_waste_;
        notify(FILE_ACTION_RENAMED_OLD_NAME, L"final-patch");
        notify(FILE_ACTION_RENAMED_NEW_NAME, L"notify-one");
        notify(FILE_ACTION_RENAMED_OLD_NAME, L"notify-one");
        notify(FILE_ACTION_RENAMED_NEW_NAME, L"notify-two");
        check(e.NameOf(patched) == L"notify-two" && e.pool_waste_ == notify_waste + wcslen(L"final-patch") + wcslen(L"notify-one"),
            "directory notifications account repeated mapped and heap name replacement");
    }
    SetActiveIndexDirectory(L"");
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    check(!error, "only isolated fixture data was removed");
    return ok;
}
}
