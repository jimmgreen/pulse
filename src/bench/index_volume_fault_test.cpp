#include "../index/index_engine.h"
#include "../index/index_volume_fault_hooks.h"
#include "../index/index_paths.h"
#include "../common/runtime_log.h"
#include <winioctl.h>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <unordered_set>

namespace pulse::index::volume_fault_test {
enum class Mode { Fallback, Stopped, Cancelled, UsnFailure, BadPage, CancelUsn, Complete, SecondVolumeFails,
    OpenFailure, LargeMft, LargeUsn, WriteFailure, RetireFailure, ReplaceFailure, RollbackFailure, ConfigFailure };
constexpr size_t kLargeRecordCount = 5000001;
Mode mode = Mode::Fallback;
unsigned opens = 0, closes = 0, mft_calls = 0, usn_calls = 0;
size_t emitted = 0;
std::atomic<bool>* active_running = nullptr;
void Reset(Mode value) { mode = value; opens = closes = mft_calls = usn_calls = 0; emitted = 0; active_running = nullptr; }
bool SnapshotFault(uint32_t stage) {
    if ((stage == 1 && mode == Mode::WriteFailure) ||
        (stage == 2 && mode == Mode::RetireFailure) ||
        (stage == 3 && (mode == Mode::ReplaceFailure || mode == Mode::RollbackFailure)) ||
        (stage == 4 && mode == Mode::RollbackFailure) ||
        (stage == 5 && mode == Mode::ConfigFailure)) {
        SetLastError(stage == 4 ? ERROR_ACCESS_DENIED : stage == 1 ? ERROR_DISK_FULL : ERROR_SHARING_VIOLATION);
        return true;
    }
    return false;
}
HANDLE OpenVolume(wchar_t letter) {
    ++opens;
    if (mode == Mode::OpenFailure) { SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(letter));
}
BOOL CloseHandle(HANDLE) { ++closes; SetLastError(ERROR_INVALID_HANDLE); return TRUE; }
bool QueryJournal(HANDLE, uint64_t& id, int64_t& next) { id = 123; next = 456; return true; }
uint64_t RootFrn(wchar_t) { return 5; }
bool IsAdmin() { return true; }
std::vector<VolumeInfo> ConfiguredVolumes() {
    std::vector<VolumeInfo> result;
    for (const wchar_t letter : {L'Q', L'R'}) {
        VolumeInfo volume;
        volume.id = std::wstring(1, letter); volume.mount_point = std::wstring(1, letter) + L":\\";
        volume.kind = VolumeKind::Fixed;
        volume.online = volume.enabled = volume.supported = true;
        volume.file_system = L"NTFS";
        result.push_back(std::move(volume));
    }
    return result;
}
MftReadResult EnumerateMft(HANDLE volume, std::atomic<bool>* running,
    const std::function<void(size_t)>& progress, const std::function<bool(MftFile&&)>& emit) {
    ++mft_calls; active_running = running;
    if (mode == Mode::LargeMft) {
        // Duplicate FRNs keep tree construction small while exercising the real
        // record accumulation limit, which is checked before deduplication.
        for (size_t i = 0; i < kLargeRecordCount; ++i) {
            MftFile item; item.frn = 42; item.parent = 5; item.name = L"x"; item.name_type = 1;
            if (!emit(std::move(item))) return MftReadResult::Stopped;
            ++emitted;
        }
        return MftReadResult::Complete;
    }
    MftFile file;
    file.frn = 42; file.parent = 5; file.name_type = 1;
    const bool complete = mode == Mode::Complete ||
        (mode == Mode::SecondVolumeFails && reinterpret_cast<uintptr_t>(volume) == L'Q');
    file.name = complete ? L"candidate.txt" : L"partial.txt";
    if (!emit(std::move(file))) return MftReadResult::Stopped;
    progress(1);
    if (mode == Mode::Cancelled) { *running = false; return MftReadResult::Failed; }
    if (mode == Mode::Stopped) return MftReadResult::Stopped;
    return complete ? MftReadResult::Complete : MftReadResult::Failed;
}
BOOL DeviceIoControl(HANDLE, DWORD code, LPVOID input, DWORD, LPVOID output, DWORD capacity,
    LPDWORD returned, LPOVERLAPPED) {
    ++usn_calls;
    if (code != FSCTL_ENUM_USN_DATA) { SetLastError(ERROR_INVALID_FUNCTION); return FALSE; }
    if (mode == Mode::SecondVolumeFails) { SetLastError(ERROR_READ_FAULT); return FALSE; }
    const auto* med = static_cast<MFT_ENUM_DATA_V0*>(input);
    if (mode == Mode::LargeUsn) {
        if (emitted == kLargeRecordCount) { SetLastError(ERROR_HANDLE_EOF); return FALSE; }
        const DWORD record_size = static_cast<DWORD>((offsetof(USN_RECORD_V2, FileName) + 2 + 7) & ~size_t{7});
        const size_t records = (std::min)(kLargeRecordCount - emitted,
            static_cast<size_t>((capacity - sizeof(USN)) / record_size));
        const DWORD bytes = static_cast<DWORD>(sizeof(USN) + records * record_size);
        std::memset(output, 0, bytes);
        const uint64_t cursor = med->StartFileReferenceNumber + records;
        std::memcpy(output, &cursor, sizeof(cursor));
        for (size_t i = 0; i < records; ++i) {
            auto* rec = reinterpret_cast<USN_RECORD_V2*>(static_cast<BYTE*>(output) + sizeof(USN) + i * record_size);
            rec->RecordLength = record_size; rec->MajorVersion = 2;
            rec->FileReferenceNumber = 77; rec->ParentFileReferenceNumber = 5;
            rec->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
            rec->FileNameLength = sizeof(wchar_t); rec->FileName[0] = L'x';
        }
        emitted += records;
        *returned = bytes;
        return TRUE;
    }
    if (med->StartFileReferenceNumber) {
        SetLastError(mode == Mode::UsnFailure ? ERROR_READ_FAULT : ERROR_HANDLE_EOF);
        return FALSE;
    }
    const std::wstring name = L"fallback.txt";
    const DWORD record_size = static_cast<DWORD>((offsetof(USN_RECORD_V2, FileName) + name.size() * 2 + 7) & ~size_t{7});
    const DWORD size = static_cast<DWORD>(sizeof(USN)) + record_size;
    if (capacity < size) { SetLastError(ERROR_INSUFFICIENT_BUFFER); return FALSE; }
    std::memset(output, 0, size);
    const uint64_t cursor = 100;
    std::memcpy(output, &cursor, sizeof(cursor));
    auto* record = reinterpret_cast<USN_RECORD_V2*>(static_cast<BYTE*>(output) + sizeof(USN));
    record->RecordLength = record_size;
    record->MajorVersion = mode == Mode::BadPage ? 3 : 2;
    record->FileReferenceNumber = 77; record->ParentFileReferenceNumber = 5;
    record->FileNameOffset = static_cast<WORD>(offsetof(USN_RECORD_V2, FileName));
    record->FileNameLength = static_cast<WORD>(name.size() * sizeof(wchar_t));
    std::memcpy(record->FileName, name.data(), record->FileNameLength);
    *returned = size;
    if (mode == Mode::CancelUsn && active_running) *active_running = false;
    return TRUE;
}
}

namespace pulse::index {
struct EngineTestAccess {
    static bool Volume(Engine& engine) {
        engine.running_ = true;
        return engine.IndexVolumeMft(volume_fault_test::ConfiguredVolumes().front());
    }
    static bool Candidate(Engine& engine, const wchar_t* name) {
        for (const auto& node : engine.build_.nodes)
            if (std::wstring_view(engine.build_.pool.data() + node.off, node.len) == name) return true;
        return false;
    }
    static bool EmptyCandidate(Engine& engine) { return engine.build_.nodes.empty() && engine.build_vols_.empty(); }
    static bool Seed(Engine& engine, const std::wstring& path) {
        Engine::Store store;
        engine.AddNodeLocked(store, -1, L"Q:", Engine::kFlagDir);
        engine.AddNodeLocked(store, 0, L"previous.txt", 0);
        if (!engine.WriteIndexFile(path, store, {}, 123456) ||
            !MoveFileExW((path + L".tmp").c_str(), path.c_str(), 0)) return false;
        std::unique_ptr<Engine::MappedFile> mapped;
        if (!engine.MapIndexFile(path, mapped)) return false;
        engine.AdoptMappedLocked(std::move(mapped));
        engine.ready_ = true;
        return true;
    }
    static const void* Mapping(Engine& engine) { return engine.map_.get(); }
    static bool Write(Engine& engine, const std::wstring& path, bool invalid = false) {
        Engine::Store store;
        engine.AddNodeLocked(store, -1, L"Q:", Engine::kFlagDir);
        if (invalid) store.nodes.front().parent = 0;
        return engine.WriteIndexFile(path, store, {}, 654321);
    }
    static bool Map(Engine& engine, const std::wstring& path) {
        std::unique_ptr<Engine::MappedFile> mapped;
        return engine.MapIndexFile(path, mapped);
    }
    static bool Commit(Engine& engine, const std::wstring& path) { return engine.CommitMappedFile(path); }
    static bool Gap(Engine& engine) { return engine.folder_size_gap_; }
    static void Rebuild(Engine& engine) { engine.running_ = true; engine.FullRebuild("fault-test"); }
    static void Stop(Engine& engine) { engine.running_ = false; }
};
}

int main(int argc, char** argv) {
    using namespace pulse::index;
    namespace vf = volume_fault_test;
    namespace fs = std::filesystem;
    int failures = 0;
    auto check = [&](bool ok, const char* label) { printf("[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(stdout); failures += !ok; };
    const auto parent = fs::absolute(L"bench_data").lexically_normal();
    const auto root = parent / (L"volume-fault-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    fs::create_directories(parent);
    if (!fs::create_directory(root)) return 2;
    SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str());
    SetMachineIndexScope(false);
    SetActiveIndexDirectory(root.wstring());
    check(pulse::diagnostics::runtime::Initialize(root.wstring(), "index"), "enable isolated scan failure diagnostics");
    {
        Engine engine;
        const auto candidate = (root / L"diagnostic-snapshot.bin").wstring();
        vf::Reset(vf::Mode::Complete);
        check(!EngineTestAccess::Write(engine, (root / L"absent" / L"base").wstring()) &&
            GetLastError() == ERROR_PATH_NOT_FOUND, "snapshot open failure preserves native error");
        check(!EngineTestAccess::Write(engine, candidate, true) && GetLastError() == ERROR_INVALID_DATA,
            "snapshot hierarchy rejection reports deterministic error");
        vf::Reset(vf::Mode::WriteFailure);
        check(!EngineTestAccess::Write(engine, candidate) && GetLastError() == ERROR_DISK_FULL &&
            !fs::exists(candidate + L".tmp"), "injected write failure preserves disk-full after cleanup");
        vf::Reset(vf::Mode::Complete);
        { std::ofstream invalid(fs::path(candidate), std::ios::binary); invalid << "invalid"; }
        check(!EngineTestAccess::Map(engine, candidate) && GetLastError() == ERROR_INVALID_DATA,
            "mapping validation preserves error after closing file");
        check(!EngineTestAccess::Commit(engine, candidate) && GetLastError() == ERROR_FILE_NOT_FOUND,
            "publish map failure retains underlying open error");
        fs::remove(candidate);
        for (const auto mode : {vf::Mode::RetireFailure, vf::Mode::ReplaceFailure, vf::Mode::RollbackFailure}) {
            vf::Reset(vf::Mode::Complete);
            check(EngineTestAccess::Seed(engine, candidate), "seed isolated publication failure fixture");
            const auto* previous = EngineTestAccess::Mapping(engine);
            check(EngineTestAccess::Write(engine, candidate), "write candidate for injected publication failure");
            vf::Reset(mode);
            check(!EngineTestAccess::Commit(engine, candidate) && GetLastError() == ERROR_SHARING_VIOLATION,
                "publication preserves primary error across mapping cleanup and rollback");
            check(EngineTestAccess::Mapping(engine) == previous,
                "publication failure retains previous live mapping");
            fs::remove(candidate + L".tmp");
            fs::remove(candidate);
        }
        vf::Reset(vf::Mode::ConfigFailure);
        EngineTestAccess::Rebuild(engine);
        check(vf::opens == 0, "configuration failure ends rebuild before volume enumeration");
        EngineTestAccess::Stop(engine);
    }
    for (const auto mode : {vf::Mode::Fallback, vf::Mode::Stopped, vf::Mode::Cancelled,
                           vf::Mode::UsnFailure, vf::Mode::BadPage, vf::Mode::CancelUsn, vf::Mode::OpenFailure}) {
        vf::Reset(mode);
        Engine engine;
        const bool result = EngineTestAccess::Volume(engine);
        const DWORD error = GetLastError();
        check(result == (mode == vf::Mode::Fallback), "production volume scan returns expected terminal result");
        if (!result) {
            const DWORD expected = mode == vf::Mode::OpenFailure ? ERROR_ACCESS_DENIED :
                mode == vf::Mode::UsnFailure ? ERROR_READ_FAULT :
                mode == vf::Mode::BadPage ? ERROR_INVALID_DATA : ERROR_OPERATION_ABORTED;
            check(error == expected, "volume failure preserves cause despite handle cleanup changing last error");
        }
        check(!EngineTestAccess::Candidate(engine, L"partial.txt"), "failed partial MFT records never enter tree");
        if (mode == vf::Mode::Fallback)
            check(EngineTestAccess::Candidate(engine, L"fallback.txt") && vf::usn_calls == 2,
                "USN fallback builds its own records and reaches real EOF branch");
        else
            check(EngineTestAccess::EmptyCandidate(engine), "stopped or failed enumeration publishes no partial tree");
        if (mode == vf::Mode::Stopped || mode == vf::Mode::Cancelled)
            check(vf::usn_calls == 0, "stopped/cancelled MFT never enters USN fallback");
        check(vf::opens == 1 && vf::closes == (mode == vf::Mode::OpenFailure ? 0u : 1u), "fake volume lifetime is balanced");
        EngineTestAccess::Stop(engine);
    }
    for (const auto mode : {vf::Mode::LargeMft, vf::Mode::LargeUsn}) {
        if (argc > 1 && std::strcmp(argv[1], "--diagnostics-only") == 0) break;
        vf::Reset(mode);
        Engine engine;
        check(EngineTestAccess::Volume(engine) && vf::emitted == vf::kLargeRecordCount &&
            EngineTestAccess::Candidate(engine, L"x"),
            mode == vf::Mode::LargeMft ? "MFT scan completes above five million records" :
                "USN fallback completes above five million records");
        check(vf::opens == 1 && vf::closes == 1, "large scan releases fake volume");
        EngineTestAccess::Stop(engine);
    }
    const auto snapshot = fs::path(CacheFilePath());
    auto bytes = [&] { std::ifstream file(snapshot, std::ios::binary); return std::vector<char>(std::istreambuf_iterator<char>(file), {}); };
    auto matches = [](Engine& engine, const wchar_t* name) { Query query; query.needle = L"\"" + std::wstring(name) + L"\""; return engine.Search(query).total; };
    {
        Engine engine;
        check(EngineTestAccess::Seed(engine, snapshot.wstring()), "write and map real private old snapshot");
        const auto original = bytes();
        const auto* mapping = EngineTestAccess::Mapping(engine);
        const auto count = engine.Count();
        for (const auto mode : {vf::Mode::SecondVolumeFails, vf::Mode::Cancelled, vf::Mode::Stopped, vf::Mode::OpenFailure}) {
            vf::Reset(mode);
            EngineTestAccess::Rebuild(engine);
            check(!original.empty() && bytes() == original, "failed rebuild preserves disk snapshot byte for byte");
            check(EngineTestAccess::Mapping(engine) == mapping && engine.Count() == count &&
                matches(engine, L"previous.txt") == 1 && matches(engine, L"candidate.txt") == 0,
                "failed rebuild preserves mapped search results and count");
            check(EngineTestAccess::EmptyCandidate(engine) && EngineTestAccess::Gap(engine),
                "failed rebuild discards candidate and marks incomplete coverage");
            check(engine.Status().find(L"未完成") != std::wstring::npos,
                "failed rebuild reports incomplete scan instead of ready success");
            if (mode == vf::Mode::SecondVolumeFails)
                check(vf::mft_calls == 2 && vf::usn_calls == 1, "later volume fails after earlier volume built successfully");
            else check(vf::usn_calls == 0, "cancelled/stopped rebuild never starts fallback");
        }
        vf::Reset(vf::Mode::Complete);
        EngineTestAccess::Rebuild(engine);
        check(matches(engine, L"candidate.txt") == 2 && matches(engine, L"previous.txt") == 0,
            "all-success control publishes both new volume trees");
        check(bytes() != original && !bytes().empty() && !EngineTestAccess::Gap(engine),
            "all-success control replaces real disk snapshot and clears gap");
        EngineTestAccess::Stop(engine);
    }
    pulse::diagnostics::runtime::Shutdown();
    const auto log_path = root / L"Diagnostics" / L"Runtime" / (L"index-" + std::to_wstring(GetCurrentProcessId()) + L".jsonl");
    std::ifstream log_file(log_path);
    bool scan_error = false, rebuild_error = false, write_error = false, rollback_error = false;
    bool config_error = false, hierarchy_error = false, cancelled = false, correlated = false;
    std::unordered_set<uint64_t> rebuilds;
    std::unordered_set<uint64_t> all_rebuilds;
    bool balanced = true;
    auto number = [](const std::string& line, const char* field) {
        const auto at = line.find(std::string("\"") + field + "\":");
        return at == std::string::npos ? 0ull : std::stoull(line.substr(at + std::strlen(field) + 3));
    };
    for (std::string line; std::getline(log_file, line);) {
        const auto operation = number(line, "operation");
        const bool is_error = number(line, "severity") == static_cast<uint64_t>(pulse::diagnostics::runtime::Level::Error);
        if (line.find("\"event\":\"index_rebuild_begin\"") != std::string::npos) {
            all_rebuilds.insert(operation);
            balanced = rebuilds.insert(operation).second && balanced;
        }
        if (line.find("\"event\":\"index_rebuild_end\"") != std::string::npos)
            balanced = rebuilds.erase(operation) == 1 && balanced;
        if (line.find("filename_write_data_failed") != std::string::npos)
            write_error = is_error && number(line, "error") == ERROR_DISK_FULL;
        if (line.find("filename_publish_rollback_failed") != std::string::npos)
            rollback_error = is_error && number(line, "error") == ERROR_ACCESS_DENIED;
        if (line.find("index_rebuild_config_failed") != std::string::npos)
            config_error = is_error && operation != 0 && rebuilds.contains(operation);
        if (line.find("index_write_invalid_hierarchy") != std::string::npos)
            hierarchy_error = is_error && number(line, "reason") == 3 && number(line, "node") == 0 && number(line, "parent") == 0;
        if (line.find("index_volume_scan_cancelled") != std::string::npos)
            cancelled = !is_error && number(line, "error") == ERROR_OPERATION_ABORTED;
        if (line.find("index_volume_scan_failed") != std::string::npos && operation)
            correlated = is_error && rebuilds.contains(operation);
        if (line.find("\"error\":5") == std::string::npos) continue;
        if (line.find("\"event\":\"index_volume_scan_failed\"") != std::string::npos &&
            line.find("\"phase\":1") != std::string::npos && line.find("\"drive\":81") != std::string::npos) scan_error = true;
        if (line.find("\"event\":\"index_rebuild_end\"") != std::string::npos) rebuild_error = true;
    }
    log_file.close();
    const auto critical_path = root / L"Diagnostics" / L"Runtime" /
        (L"index-" + std::to_wstring(GetCurrentProcessId()) + L".critical.jsonl");
    std::ifstream critical_file(critical_path);
    for (std::string line; std::getline(critical_file, line);) {
        if (line.find("index_volume_scan_failed") != std::string::npos &&
            all_rebuilds.contains(number(line, "operation"))) correlated = true;
    }
    critical_file.close();
    check(scan_error && rebuild_error, "scan and rebuild logs retain access-denied cause instead of generic read fault");
    check(write_error && rollback_error && hierarchy_error,
        "default error events retain write, rollback and hierarchy failure details");
    check(config_error && balanced && rebuilds.empty(), "every rebuild including configuration failure has paired terminal event");
    check(cancelled && correlated, "cancellation is informational and scan failures correlate to active rebuild");
    SetActiveIndexDirectory({});
    if (root.parent_path() != parent || !root.filename().wstring().starts_with(L"volume-fault-")) return 2;
    std::error_code error;
    fs::remove_all(root, error);
    check(!error && !fs::exists(root), "release mapping then remove exclusive fixture");
    return failures ? 1 : 0;
}
