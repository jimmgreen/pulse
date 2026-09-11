#include "../index/index_engine.h"
#include "../index/index_shard.h"
#include "../index/index_paths.h"
#include "../index/index_delta.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <chrono>

namespace pulse::index {

struct EngineTestAccess {
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

int wmain(int argc, wchar_t** argv) {
    if (argc > 1 && std::wstring_view(argv[1]) == L"--parent-cycle-only") return EngineTestAccess::ParentCycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--recycle-only") return EngineTestAccess::RecycleFixture() ? 0 : 1;
    if (argc > 1 && std::wstring_view(argv[1]) == L"--coverage-only") return EngineTestAccess::CoverageBenchmark() ? 0 : 1;
    std::cout << std::unitbuf;
    Engine engine;
    Check(EngineTestAccess::Build(engine), "actual MFT tree builder accepts fixture");
    CheckSearch(engine);
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
