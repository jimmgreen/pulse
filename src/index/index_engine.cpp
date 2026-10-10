#include "filename_pinyin.h"
// index_engine.cpp — mmap v6 base + heap delta (优化.md).
#include "index_engine.h"
#include "index_hierarchy.h"
#include "../common/runtime_log.h"
#include "search_trace.h"
#include "index_parent_chain.h"
#include "index_path_builder.h"
#include "index_name_pool.h"
#include "index_config.h"
#include "index_query.h"
#include "index_mft.h"
#include "index_volume_fault_hooks.h"
#include "index_mft_enum_page.h"
#include "index_paths.h"
#include "index_delta.h"
#include "index_shard.h"
#include "../fs/fs_enum.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <bit>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <queue>
#include <numeric>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pulse::index {

#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
namespace volume_fault_test {
bool SnapshotFault(uint32_t stage);
}
#endif

namespace {

// Rebuild work is synchronous on its worker. Thread-local correlation avoids
// attributing concurrent query-side cache reads to that rebuild.
thread_local uint64_t index_operation = 0;
struct IndexOperationScope {
    uint64_t previous = index_operation;
    explicit IndexOperationScope(uint64_t operation) { index_operation = operation; }
    ~IndexOperationScope() { index_operation = previous; }
};

diagnostics::runtime::Level IndexErrorLevel(DWORD error) {
    return error == ERROR_OPERATION_ABORTED ? diagnostics::runtime::Level::Info :
        diagnostics::runtime::Level::Error;
}

bool IndexFailure(const char* event, DWORD error, const std::wstring& path) {
    TraceSearch(event, error, path);
    diagnostics::runtime::Event(event, {{"operation", index_operation}, {"error", error}}, IndexErrorLevel(error));
    SetLastError(error);
    return false;
}

uint64_t MergeTriggerCode(const char* reason) noexcept {
    if (!reason) return 0;
    static constexpr const char* names[] = {"forced", "deleted_ratio", "structural_threshold", "delta_threshold", "quiet_changes"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
        if (std::strcmp(reason, names[i]) == 0) return i + 1;
    return 0;
}

// Capacity follows the on-disk signed node IDs, not a workload-sized limit.
// Real installations exceed five million entries; keep the root headroom.
constexpr size_t kIndexCap = static_cast<size_t>((std::numeric_limits<int32_t>::max)()) - 64;
constexpr size_t kFrnMergeThreshold = 4096;
constexpr ULONGLONG kMinMergeIntervalMs = 10ull * 60ull * 1000ull;
constexpr ULONGLONG kNamePoolIntervalMs = 60000;
constexpr size_t kNamePoolWasteChars = 1ull << 20;
constexpr ULONGLONG kIdleMergeQuietMs = 10ull * 60ull * 1000ull;
constexpr ULONGLONG kDeltaFlushMs = 15ull * 1000ull;
constexpr size_t kMergeStructChanges = 100000;
constexpr uint64_t kMergeDeltaBytes = 64ull * 1024ull * 1024ull;
constexpr uint32_t kIndexVer = kIndexSnapshotVersion;
constexpr uint32_t kIndexVerMin = 7;
constexpr uint64_t kUnixFtEpoch = 116444736000000000ull;
constexpr ULONGLONG kNotifyMinMs = 50;
constexpr uint32_t kPrefixBuckets = 65536;
constexpr uint64_t kPrefixAllCharsFlag = 1ull << 63;

uint32_t FtToUnix(uint64_t ft) {
    if (ft < kUnixFtEpoch) return 0;
    const uint64_t s = (ft - kUnixFtEpoch) / 10000000ull;
    return s > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(s);
}

uint64_t UnixToFt(uint32_t u) {
    if (u == 0) return 0;
    return static_cast<uint64_t>(u) * 10000000ull + kUnixFtEpoch;
}

std::wstring StatusItemCount(uint64_t count, std::wstring_view detail = {}) {
    std::wstring text = L"已索引 ";
    text += std::to_wstring(count);
    text += L" 项";
    if (!detail.empty()) {
        text += L" · ";
        text.append(detail.data(), detail.size());
    }
    return text;
}

std::wstring StatusDriveProgress(wchar_t letter, size_t count) {
    std::wstring text = L"正在索引 ";
    text += letter;
    text += L": ";
    text += std::to_wstring(count);
    text += L" 项";
    return text;
}

std::wstring Display(std::wstring p) {
    return pulse::path::StripExtendedPathPrefix(p);
}

bool IsAdmin() {
    BOOL admin = FALSE;
    PSID group = nullptr;
    SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                 DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group)) {
        CheckTokenMembership(nullptr, group, &admin);
        FreeSid(group);
    }
    return admin == TRUE;
}

bool EqualsI(std::wstring_view a, std::wstring_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (FoldChar(a[i]) != FoldChar(b[i])) return false;
    }
    return true;
}

// File reference number of the file currently at `path`, 0 when absent. Hard
// link journal records name one link; this tells whether it still exists.
uint64_t FileFrnAt(const std::wstring& path) {
    if (path.empty()) return 0;
    const std::wstring open_path = path.size() >= MAX_PATH && path.rfind(L"\\\\", 0) != 0
        ? L"\\\\?\\" + path : path;
    HANDLE h = CreateFileW(open_path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION info{};
    const BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    return ok ? (static_cast<uint64_t>(info.nFileIndexHigh) << 32) | info.nFileIndexLow : 0;
}

uint32_t NameHash(std::wstring_view name) {
    uint32_t h = 2166136261u;
    // Empty MSVC vectors yield data()==nullptr; a corrupted view can also be
    // {nullptr, n>0}. Never walk a null pointer.
    if (!name.data() || name.empty()) return h;
    for (wchar_t c : name) {
        h ^= FoldChar(c);
        h *= 16777619u;
    }
    return h;
}

std::wstring_view PoolView(const wchar_t* pool, size_t pool_chars,
                           uint32_t off, uint16_t len) {
    if (!pool || len == 0) return {};
    if (static_cast<size_t>(off) + static_cast<size_t>(len) > pool_chars) return {};
    return {pool + off, len};
}

uint64_t ChildKey(int32_t parent, std::wstring_view name) {
    return (static_cast<uint64_t>(static_cast<uint32_t>(parent)) << 32) | NameHash(name);
}

int CmpLogical(std::wstring_view a, std::wstring_view b) {
    wchar_t wa[260], wb[260];
    if (a.size() < 259 && b.size() < 259) {
        if (!a.empty()) memcpy(wa, a.data(), a.size() * 2);
        wa[a.size()] = 0;
        if (!b.empty()) memcpy(wb, b.data(), b.size() * 2);
        wb[b.size()] = 0;
        return StrCmpLogicalW(wa, wb);
    }
    std::wstring sa(a), sb(b);
    return StrCmpLogicalW(sa.c_str(), sb.c_str());
}

bool SplitPath(std::wstring path, std::wstring& root, std::vector<std::wstring_view>& segs,
               std::wstring& storage) {
    path = Display(std::move(path));
    while (path.size() > 1 && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    if (path.size() < 2) return false;
    size_t body = 0;
    if (path[0] == L'\\' && path[1] == L'\\') {
        size_t s3 = path.find(L'\\', 2);
        if (s3 == std::wstring::npos) { root = path; storage.clear(); return true; }
        size_t s4 = path.find(L'\\', s3 + 1);
        root = path.substr(0, s4 == std::wstring::npos ? path.size() : s4);
        body = (s4 == std::wstring::npos) ? path.size() : s4 + 1;
    } else if (path[1] == L':') {
        root = path.substr(0, 2);
        body = path.size() > 2 && path[2] == L'\\' ? 3 : 2;
    } else {
        return false;
    }
    storage = path.substr(body);
    size_t i = 0;
    while (i < storage.size()) {
        size_t sep = storage.find(L'\\', i);
        if (sep == std::wstring::npos) sep = storage.size();
        if (sep > i) segs.emplace_back(storage.data() + i, sep - i);
        i = sep + 1;
    }
    return true;
}

HANDLE OpenVolume(wchar_t letter) {
    wchar_t vol[16];
    swprintf_s(vol, L"\\\\.\\%c:", letter);
    return CreateFileW(vol, FILE_READ_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                       nullptr, OPEN_EXISTING, 0, nullptr);
}

bool QueryJournal(HANDLE vol, uint64_t& id, int64_t& next_usn) {
    USN_JOURNAL_DATA_V0 jd{};
    DWORD br = 0;
    if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &br, nullptr)) {
        if (GetLastError() != ERROR_JOURNAL_NOT_ACTIVE) return false;
        CREATE_USN_JOURNAL_DATA cd{};
        cd.MaximumSize = 32ull * 1024 * 1024;
        cd.AllocationDelta = 4ull * 1024 * 1024;
        if (!DeviceIoControl(vol, FSCTL_CREATE_USN_JOURNAL, &cd, sizeof(cd), nullptr, 0, &br, nullptr))
            return false;
        if (!DeviceIoControl(vol, FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &jd, sizeof(jd), &br, nullptr))
            return false;
    }
    id = jd.UsnJournalID;
    next_usn = jd.NextUsn;
    return true;
}

uint64_t RootFrn(wchar_t letter) {
    wchar_t root[] = { letter, L':', L'\\', 0 };
    HANDLE h = CreateFileW(root, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION bi{};
    uint64_t frn = 0;
    if (GetFileInformationByHandle(h, &bi))
        frn = (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
    CloseHandle(h);
    return frn;
}

std::vector<VolumeInfo> ConfiguredVolumes(const IndexConfig& config) {
    auto volumes = EnumerateLocalVolumes(config);
    volumes.erase(std::remove_if(volumes.begin(), volumes.end(), [](const VolumeInfo& volume) {
        return !volume.enabled || !volume.online || volume.mount_point.size() < 2;
    }), volumes.end());
    return volumes;
}




int CompareFolded(std::wstring_view a, std::wstring_view b) {
    const size_t n = (std::min)(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        const wchar_t ca = FoldChar(a[i]);
        const wchar_t cb = FoldChar(b[i]);
        if (ca < cb) return -1;
        if (ca > cb) return 1;
    }
    return a.size() < b.size() ? -1 : a.size() > b.size() ? 1 : 0;
}

ShardPaths AggregateShardPaths() {
    if (!MachineIndexScope()) return {};
    return MakeShardPaths(DataDir() + L"\\v9", L"aggregate");
}

int RankSingleCharMatch(const wchar_t* name, uint32_t length, bool is_dir,
                        wchar_t folded_char) {
    int score = 100;
    if (length == 1 && FoldChar(name[0]) == folded_char) {
        score = 400;
    } else if (length != 0 && FoldChar(name[0]) == folded_char) {
        score = 300;
    } else {
        for (uint32_t i = 1; i < length; ++i) {
            if (FoldChar(name[i]) != folded_char) continue;
            const wchar_t previous = name[i - 1];
            if (!(std::iswalnum(previous) || previous > 127)) {
                score = 200;
                break;
            }
        }
    }
    if (is_dir) score += 40;
    if (length < 24) score += static_cast<int>(24 - length);
    return score;
}

std::wstring CachePath() {
    if (!MachineIndexScope()) return CacheFilePath();
    const ShardPaths paths = AggregateShardPaths();
    ShardManifest manifest;
    std::wstring active;
    if (ResolveActiveShard(paths, manifest, active, nullptr)) return active;
    return paths.base_a;
}

uint32_t PrefixChar(std::wstring_view name) {
    if (name.empty()) return 0;
    return FoldChar(name[0]);
}

uint32_t PairBucket(std::wstring_view name) {
    if (name.size() < 2) return PrefixChar(name);
    return (static_cast<uint32_t>(FoldChar(name[0])) * 131u + FoldChar(name[1])) & 65535u;
}

bool IsIndexArtifactName(std::wstring_view name) {
    return IsChangeJournalName(name) || (name.size() >= 11 && _wcsnicmp(name.data(), L"pulse-index", 11) == 0);
}

uint64_t FileIndexFrn(const std::wstring& path) {
    if (path.empty()) return 0;
    HANDLE h = CreateFileW(path.c_str(), 0,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return 0;
    BY_HANDLE_FILE_INFORMATION bi{};
    uint64_t frn = 0;
    if (GetFileInformationByHandle(h, &bi))
        frn = (static_cast<uint64_t>(bi.nFileIndexHigh) << 32) | bi.nFileIndexLow;
    CloseHandle(h);
    return frn;
}

uint64_t AlignUp(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

bool WriteAll(HANDLE h, const void* p, size_t n) {
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
    if (volume_fault_test::SnapshotFault(1)) return false;
#endif
    const BYTE* b = static_cast<const BYTE*>(p);
    while (n) {
        DWORD w = 0;
        DWORD chunk = n > 1u << 20 ? (1u << 20) : static_cast<DWORD>(n);
        if (!WriteFile(h, b, chunk, &w, nullptr)) return false;
        if (w == 0) { SetLastError(ERROR_WRITE_FAULT); return false; }
        b += w;
        n -= w;
    }
    return true;
}

} // namespace

void Engine::MatchSet::Clear() {
    ids.clear();
    bits.clear();
    total = 0;
    universe = 0;
    dense = false;
}

void Engine::MatchSet::Begin(int32_t n) {
    Clear();
    universe = n;
    dense = n >= 65536;
    if (dense) bits.assign((static_cast<size_t>(n) + 63) / 64, 0);
    else ids.reserve(64);
}

void Engine::MatchSet::Add(int32_t i) {
    if (i < 0 || i >= universe) return;
    if (dense) {
        const size_t w = static_cast<size_t>(i) >> 6;
        const uint64_t mask = 1ull << (i & 63);
        if (bits[w] & mask) return;
        bits[w] |= mask;
        ++total;
        return;
    }
    ids.push_back(i);
    ++total;
}

void Engine::MappedFile::Close() {
    if (view) { UnmapViewOfFile(view); view = nullptr; }
    if (mapping) { CloseHandle(mapping); mapping = nullptr; }
    if (file != INVALID_HANDLE_VALUE) { CloseHandle(file); file = INVALID_HANDLE_VALUE; }
    hdr = nullptr;
    nodes = nullptr;
    attrs = nullptr;
    pool = nullptr;
    vols = nullptr;
    frns = nullptr;
    child_order = name_order = size_order = mtime_order = nullptr;
    prefix1_start = prefix2_start = nullptr;
    prefix1_ids = prefix2_ids = nullptr;
    prefix1_all_chars = false;
    n = nvol = nfrn = 0;
    size = 0;
}

void Engine::SetStatus(std::wstring s) {
    std::lock_guard<std::mutex> lock(status_mu_);
    status_ = std::move(s);
}
void Engine::RecordFeed(const ChangeRecord& record) {
    const auto id = ++feed_sequence_;
    TraceSearch("filename_event", id, record.path);
    feed_changes_.Append(record, id);
}
void Engine::CaptureMemoryState() {
    if (!IndexDiagnosticsEnabled()) return;
    std::shared_lock lock(mutex_);
    auto& retained = filename_timing_.Memory().retained;
    retained.containers_tick = GetTickCount64();
    retained.feed_records = feed_changes_.Size();
    retained.feed_page_capacity_bytes = feed_changes_.PageCapacityBytes();
    retained.aggregate_mapped_file_bytes = map_ ? map_->size : 0;
    retained.shard_mapped_file_bytes = 0;
    for (const auto& shard : query_shards_) if (shard.mapped) retained.shard_mapped_file_bytes += shard.mapped->size;
    retained.overlay_nodes = live_.nodes.size();
    retained.overlay_patches = patches_.size();
    // Stream ownership is worker-only; producers touch only their own queue.
    retained.usn_streams = journal_streams_.size();
    retained.usn_queue_packets = retained.usn_queue_payload_bytes = 0;
    retained.usn_queue_capacity_bytes = retained.usn_queue_charged_bytes = 0;
    retained.usn_sum_stream_peak_capacity_bytes = retained.usn_sum_stream_peak_charged_bytes = 0;
    retained.usn_queue_overflows = 0;
    for (const auto& entry : journal_streams_) {
        const auto memory = entry.second->Memory();
        retained.usn_queue_packets += memory.packets;
        retained.usn_queue_payload_bytes += memory.payload_bytes;
        retained.usn_queue_capacity_bytes += memory.capacity_bytes;
        retained.usn_queue_charged_bytes += memory.charged_bytes;
        // Independent lifetime peaks, NOT a simultaneous aggregate high-water mark.
        retained.usn_sum_stream_peak_capacity_bytes += memory.peak_capacity_bytes;
        retained.usn_sum_stream_peak_charged_bytes += memory.peak_charged_bytes;
        retained.usn_queue_overflows += memory.overflows;
    }
}
FileFeedPage Engine::ReadFeed(bool changes, const std::wstring& root, uint64_t epoch, uint64_t cursor) const {
    std::shared_lock lock(mutex_);
    FileFeedPage page; page.epoch=feed_epoch_; page.sequence=feed_sequence_;
    page.ready=ready_ && !building_;
    if(epoch && epoch!=page.epoch) {page.gap=true;return page;}
    if(!page.ready) return page;
    if(changes) {
        if(!epoch) {page.next=feed_sequence_;page.done=true;return page;}
        if(!feed_changes_.Empty() && cursor+1<feed_changes_.FirstId()) {page.gap=true;return page;}
        page.next=cursor;
        feed_changes_.ReadAfter(cursor, 512, page.records);
        if (!page.records.empty()) page.next = page.records.back().id;
        page.done=page.next==feed_sequence_;return page;
    }
    const auto count=static_cast<uint64_t>(LiveCount());
    uint64_t scan=cursor;
    for(;scan<count && scan<cursor+8192 && page.records.size()<512;++scan) {
        const auto id=static_cast<int32_t>(scan);
        if(IsTomb(id) || (NodeAt(id).flags & (kFlagHidden|kFlagDir))) continue;
        const auto path=BuildPathLocked(id);
        if (path.empty()) { page.gap = true; continue; }
        if(IsExcludedPath(path) || (!root.empty() && !(path.size()>=root.size() &&
            CompareStringOrdinal(path.data(),static_cast<int>(root.size()),root.data(),static_cast<int>(root.size()),TRUE)==CSTR_EQUAL &&
            (path.size()==root.size() || root.back()==L'\\' || path[root.size()]==L'\\')))) continue;
        ChangeRecord record;record.path=path;record.kind=ChangeKind::Created;record.file_id=scan;
        page.records.push_back(std::move(record));
    }
    page.next=scan;page.done=scan>=count;return page;
}

void Engine::PublishExcludedPaths(const IndexConfig& config,
                                 const std::function<void()>& locked_action) {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    excluded_paths_ = config.excluded_paths;
    for (auto& path : SystemExclusionPaths(config)) excluded_paths_.push_back(std::move(path));
    hide_node_modules_ = HidesNodeModules(config);
    // Rebuild initialization remains atomic with configuration publication.
    InvalidateFilterLocked();
    if (locked_action) locked_action();
}

bool Engine::IsExcludedPath(std::wstring_view path) const {
    for (const auto& excluded : excluded_paths_) {
        if (excluded.empty() || path.size() < excluded.size()) continue;
        if (CompareStringOrdinal(path.data(), static_cast<int>(excluded.size()),
                                 excluded.data(), static_cast<int>(excluded.size()), TRUE) !=
            CSTR_EQUAL) continue;
        if (path.size() == excluded.size() || path[excluded.size()] == L'\\' ||
            path[excluded.size()] == L'/') return true;
    }
    return false;
}

std::wstring Engine::Status() const {
    std::lock_guard<std::mutex> lock(status_mu_);
    return status_;
}

void Engine::RequestRebuild() {
    IndexConfig config;
    std::wstring config_error;
    if (MachineIndexScope() && !LoadMachineConfig(config, &config_error)) {
        SetStatus(config_error);
        PingNotify(true);
        return;
    }
    const auto active = ConfiguredVolumes(config);
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        UpdateVolumeVisibilityLocked(active, true);
    }
    rebuild_requested_.store(true);
    if (change_signal_) SetEvent(change_signal_);
    SetStatus(L"索引配置已更改，准备重建…");
    PingNotify(true);
}

std::vector<VolumeInfo> Engine::Volumes() const {
    IndexConfig config;
    if (MachineIndexScope() && !LoadMachineConfig(config, nullptr)) return {};
    auto result = EnumerateLocalVolumes(config);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (auto& volume : result) {
        auto it = std::find_if(vols_.begin(), vols_.end(), [&](const VolState& state) {
            if (!state.volume_id.empty() && !volume.id.empty())
                return NormalizeVolumeId(state.volume_id) == NormalizeVolumeId(volume.id);
            return volume.mount_point.size() >= 2 && state.letter == towupper(volume.mount_point[0]);
        });
        if (it != vols_.end()) {
            volume.indexed_items = it->item_count;
            volume.progress = 100;
            volume.state = volume.online ? L"已就绪" : L"离线 · 仍可搜索";
        } else if (volume.enabled && building_) {
            volume.state = L"正在建立索引";
        } else if (volume.enabled && ready_) {
            volume.state = L"索引失败";
            volume.error = L"无法读取该磁盘的文件变更记录";
        }
    }
    for (const auto& state : vols_) {
        const bool present = std::any_of(result.begin(), result.end(), [&](const VolumeInfo& volume) {
            return !state.volume_id.empty() &&
                   NormalizeVolumeId(volume.id) == NormalizeVolumeId(state.volume_id);
        });
        if (present || state.kind != VolumeKind::Removable) continue;
        VolumeInfo offline;
        offline.id = state.volume_id;
        offline.mount_point = std::wstring(1, state.letter) + L":\\";
        offline.kind = state.kind;
        offline.supported = true;
        offline.online = false;
        offline.enabled = !config.IsExcluded(offline.id);
        offline.indexed_items = state.item_count;
        offline.progress = 100;
        offline.state = L"离线 · 仍可搜索";
        result.push_back(std::move(offline));
    }
    return result;
}

void Engine::PingNotify(bool force) {
    if (!notify_ || !notify_msg_) return;
    const ULONGLONG t = GetTickCount64();
    const ULONGLONG prev = last_notify_.load();
    if (!force && t - prev < kNotifyMinMs) return;
    last_notify_.store(t);
    PostMessageW(notify_, notify_msg_, 0, 0);
}

ChangeState Engine::ChangeCoverage(const std::wstring& requested_path) const {
    const auto path = NormalizeChangePath(requested_path);
    if (!Ready()) return ChangeState::Scanning;
    if (path.size() >= 2 && path[0] == L'\\' && path[1] == L'\\') return ChangeState::NotCovered;
    std::shared_lock lock(mutex_);
    if (path.size() > 1 && path[1] == L':') {
        for (const auto& volume : vols_)
            if (towupper(volume.letter) == towupper(path[0]) &&
                std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), volume.root_idx) != inactive_volume_roots_.end())
                return ChangeState::Offline;
    }
    const auto index = ResolvePathLocked(path);
    return index >= 0 && !(NodeAt(index).flags & kFlagHidden) && !IsExcludedPath(path) ? ChangeState::Gap : ChangeState::NotCovered;
}

void Engine::SetChangeLease(const std::wstring& owner, bool enabled) {
    const bool seed = changes_.Lease(owner, enabled);
    std::lock_guard lock(change_seed_mutex_);
    if (seed) change_seed_owners_.insert(owner);
    if (!enabled) change_seed_owners_.erase(owner);
    if (change_signal_) SetEvent(change_signal_);
}

void Engine::SeedPendingChanges() {
    std::unordered_set<std::wstring> owners;
    { std::lock_guard lock(change_seed_mutex_); owners.swap(change_seed_owners_); }
    for (const auto& owner : owners) {
        if (!running_) break;
        SeedChanges(owner);
    }
}

void Engine::SeedChanges(const std::wstring& owner) {
    std::vector<ChangeRecord> baseline;
    const auto since = ChangeTracker::Now() - 7 * 86400;
    {
        std::shared_lock lock(mutex_);
        for (int32_t i = 0; running_ && i < LiveCount() && baseline.size() < 100000; ++i) {
            if (IsTomb(i) || (NodeAt(i).flags & kFlagHidden)) continue;
            const auto attr = AttrAt(i);
            if (attr.mtime < since) continue;
            ChangeRecord event; event.path = BuildPathLocked(i); event.time = attr.mtime;
            if (event.path.empty()) continue;
            event.is_dir = (NodeAt(i).flags & kFlagDir) != 0;
            baseline.push_back(std::move(event));
        }
    }
    if (running_) changes_.Seed(owner, std::move(baseline));
}

void Engine::Start(HWND notify, UINT msg) {
    Stop();
    changes_.Open(DataDir());
    notify_ = notify;
    notify_msg_ = msg;
    running_ = true;
    ready_ = false;
    folder_size_background_ = true;
    change_signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = std::thread(&Engine::Worker, this);
}

void Engine::Stop() {
    running_ = false;
    if (change_signal_) SetEvent(change_signal_);
    if (thread_.joinable()) thread_.join();
    journal_streams_.clear();
    if (change_signal_) { CloseHandle(change_signal_); change_signal_ = nullptr; }
    StopPinyinWorker();
    changes_.Flush();
    StopWalkWatches();
    FlushDeltas();
    CloseDeltas();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    query_shards_.clear();
    query_shards_ready_ = false;
    if (map_) map_->Close();
    map_.reset();
    ++layout_epoch_;
}

bool Engine::IsTomb(int32_t i) const {
    if (tombstones_.count(i)) return true;
    const Node n = NodeAt(i);
    return (n.flags & kFlagDeleted) != 0;
}

Node Engine::NodeAt(int32_t i) const {
    Node n;
    if (i < 0 || i >= LiveCount()) return n;
    const int32_t base = BaseCount();
    if (i < base) {
        if (!map_ || !map_->nodes) return n;
        n = map_->nodes[static_cast<size_t>(i)];
    } else {
        n = live_.nodes[static_cast<size_t>(i - base)];
    }
    auto it = patches_.find(i);
    if (it != patches_.end()) {
        if (it->second.has_meta) {
            n.parent = it->second.parent;
            n.flags = it->second.flags;
        }
        if (it->second.has_name) {
            n.off = it->second.off;
            n.len = it->second.len;
        }
    }
    return n;
}

Attr Engine::AttrAt(int32_t i) const {
    if (i < 0 || i >= LiveCount()) return {};
    auto it = patches_.find(i);
    if (it != patches_.end() && it->second.has_attr) return it->second.attr;
    const int32_t base = BaseCount();
    if (i < base) {
        if (!map_ || !map_->attrs) return {};
        return map_->attrs[static_cast<size_t>(i)];
    }
    return live_.attrs[static_cast<size_t>(i - base)];
}

std::wstring_view Engine::NameOf(int32_t i) const {
    if (i < 0 || i >= LiveCount()) return {};
    auto it = patches_.find(i);
    if (it != patches_.end() && it->second.has_name) {
        return PoolView(live_.pool.data(), live_.pool.size(),
                        it->second.off, it->second.len);
    }
    const int32_t base = BaseCount();
    if (i < base) {
        if (!map_ || !map_->pool || !map_->nodes || !map_->hdr) return {};
        const Node& n = map_->nodes[static_cast<size_t>(i)];
        return PoolView(map_->pool, static_cast<size_t>(map_->hdr->pool_chars),
                        n.off, n.len);
    }
    const Node& n = live_.nodes[static_cast<size_t>(i - base)];
    return PoolView(live_.pool.data(), live_.pool.size(), n.off, n.len);
}

void Engine::ChildMapAdd(int32_t parent, std::wstring_view name, int32_t idx) {
    child_map_.emplace(ChildKey(parent, name), idx);
}

void Engine::ChildMapRemove(int32_t parent, std::wstring_view name, int32_t idx) {
    const uint64_t key = ChildKey(parent, name);
    auto range = child_map_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        if (it->second == idx) { child_map_.erase(it); return; }
    }
}

void Engine::RebuildChildMapLocked() {
    child_map_.clear();
    const int32_t base = BaseCount();
    const int32_t n = LiveCount();
    child_map_.reserve(static_cast<size_t>(n - base) + patches_.size());
    for (int32_t i = base; i < n; ++i) {
        if (IsTomb(i)) continue;
        const Node node = NodeAt(i);
        ChildMapAdd(node.parent, NameOf(i), i);
    }
    for (const auto& [i, patch] : patches_) {
        if (i < 0 || i >= base || IsTomb(i) || (!patch.has_meta && !patch.has_name)) continue;
        const Node node = NodeAt(i);
        ChildMapAdd(node.parent, NameOf(i), i);
    }
}

int32_t Engine::AddNodeLocked(Store& s, int32_t parent, std::wstring_view name, uint8_t flags,
                              uint64_t frn, uint64_t size, uint64_t mtime_ft,
                              bool index_live, VolState* vol) {
    Node n;
    n.parent = parent;
    n.off = static_cast<uint32_t>(s.pool.size());
    n.len = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
    n.flags = flags;
    s.pool.insert(s.pool.end(), name.begin(), name.begin() + n.len);
    s.nodes.push_back(n);
    Attr a;
    a.mtime = FtToUnix(mtime_ft);
    a.size = size;
    s.attrs.push_back(a);
    const int32_t idx = index_live
        ? BaseCount() + static_cast<int32_t>(s.nodes.size()) - 1
        : static_cast<int32_t>(s.nodes.size()) - 1;
    if (index_live) ChildMapAdd(parent, std::wstring_view(s.pool.data() + n.off, n.len), idx);
    if (frn && vol) MapFrnLocked(*vol, frn, idx);
    return idx;
}

int32_t Engine::FindChildInStore(const Store& s, int32_t parent, std::wstring_view name) {
    for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i) {
        const Node& n = s.nodes[static_cast<size_t>(i)];
        if (n.parent != parent || (n.flags & kFlagDeleted)) continue;
        std::wstring_view have{ s.pool.data() + n.off, n.len };
        if (EqualsI(have, name)) return i;
    }
    return -1;
}

int32_t Engine::FindChildLiveLocked(int32_t parent, std::wstring_view name) const {
    const uint64_t key = ChildKey(parent, name);
    auto range = child_map_.equal_range(key);
    for (auto it = range.first; it != range.second; ++it) {
        if (IsTomb(it->second)) continue;
        if (NodeAt(it->second).parent != parent) continue;
        if (EqualsI(NameOf(it->second), name)) return it->second;
    }
    if (map_ && map_->child_order) {
        const int32_t* first = map_->child_order;
        const int32_t* last = first + map_->n;
        // The immutable child order is sorted by (parent, folded base name).
        auto it = std::lower_bound(first, last, parent, [&](int32_t idx, int32_t value) {
            const auto& node = map_->nodes[static_cast<size_t>(idx)];
            if (node.parent != value) return node.parent < value;
            return CompareFolded(std::wstring_view(map_->pool + node.off, node.len), name) < 0;
        });
        for (; it != last; ++it) {
            const int32_t idx = *it;
            const auto& base_node = map_->nodes[static_cast<size_t>(idx)];
            if (base_node.parent != parent ||
                CompareFolded(std::wstring_view(map_->pool + base_node.off, base_node.len), name) != 0) break;
            if (IsTomb(idx) || NodeAt(idx).parent != parent) continue;
            if (EqualsI(NameOf(idx), name)) return idx;
        }
    }
    return -1;
}

int32_t Engine::EnsureChainLocked(Store& s, const std::wstring& path, bool leaf_is_dir,
                                  bool index_live) {
    std::wstring root, storage;
    std::vector<std::wstring_view> segs;
    if (!SplitPath(path, root, segs, storage)) return -1;

    int32_t cur = -1;
    if (index_live) {
        cur = FindChildLiveLocked(-1, root);
        if (cur < 0) cur = AddNodeLocked(s, -1, root, kFlagDir | kFlagHidden, 0, 0, 0, true);
        for (size_t k = 0; k < segs.size(); ++k) {
            int32_t child = FindChildLiveLocked(cur, segs[k]);
            if (child < 0) {
                const bool leaf = (k + 1 == segs.size());
                child = AddNodeLocked(s, cur, segs[k], (!leaf || leaf_is_dir) ? kFlagDir : 0,
                                      0, 0, 0, true);
            }
            cur = child;
        }
        return cur;
    }

    cur = FindChildInStore(s, -1, root);
    if (cur < 0) cur = AddNodeLocked(s, -1, root, kFlagDir | kFlagHidden);
    for (size_t k = 0; k < segs.size(); ++k) {
        int32_t child = FindChildInStore(s, cur, segs[k]);
        if (child < 0) {
            const bool leaf = (k + 1 == segs.size());
            child = AddNodeLocked(s, cur, segs[k], (!leaf || leaf_is_dir) ? kFlagDir : 0);
        }
        cur = child;
    }
    return cur;
}

std::wstring Engine::BuildPathLocked(int32_t node) const {
    return BuildCompleteIndexPath(node, LiveCount(),
        [&](int32_t id) { return NodeAt(id).parent; },
        [&](int32_t id) { return NameOf(id); });
}

bool Engine::IsUnderLocked(int32_t node, int32_t ancestor) const {
    return InSubtreeLocked(node, ancestor);
}

int32_t Engine::SubtreeEndLocked(int32_t node) const {
    // DFS intervals describe the immutable snapshot, not overlay parent links.
    // Until the overlay is merged, use the current parent chain for membership.
    if (node < 0 || !subtree_intervals_valid_ || !patches_.empty()) return -1;
    const Node n = NodeAt(node);
    if (n.unused > static_cast<uint32_t>(node) &&
        n.unused <= static_cast<uint32_t>(LiveCount()))
        return static_cast<int32_t>(n.unused);
    return -1;
}

bool Engine::InSubtreeLocked(int32_t node, int32_t ancestor) const {
    if (ancestor < 0) return true;
    const int32_t end = SubtreeEndLocked(ancestor);
    if (end > ancestor && node >= ancestor && node < end && node < BaseCount() &&
        ancestor < BaseCount())
        return true;
    ParentChainGuard chain(LiveCount());
    for (int32_t i = node; chain.Visit(i);) {
        if (i == ancestor) return true;
        i = NodeAt(i).parent;
    }
    return false;
}

bool Engine::VolumeSpan(const VolState& v, int32_t& lo, int32_t& hi) const {
    if (!map_ || !map_->hdr || map_->hdr->ver < 8) return false;
    if (v.root_idx < 0) return false;
    const int32_t n = BaseCount();
    lo = v.first_idx;
    hi = v.first_idx + static_cast<int32_t>(v.item_count);
    if (lo < 0 || hi > n || lo >= hi) {
        const int32_t end = SubtreeEndLocked(v.root_idx);
        if (end <= v.root_idx) return false;
        lo = v.root_idx;
        hi = (std::min)(end, n);
    }
    return lo >= 0 && hi > lo;
}

int32_t Engine::ResolvePathLocked(const std::wstring& path) const {
    std::wstring root, storage;
    std::vector<std::wstring_view> segs;
    if (!SplitPath(path, root, segs, storage)) return -1;
    int32_t cur = FindChildLiveLocked(-1, root);
    for (size_t k = 0; cur >= 0 && k < segs.size(); ++k)
        cur = FindChildLiveLocked(cur, segs[k]);
    return cur;
}

void Engine::AddForTest(std::wstring path, std::wstring name, bool is_dir,
                        uint64_t size, uint64_t mtime) {
    (void)name;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    Store& dest = building_ ? build_ : live_;
    const bool live = !building_;
    int32_t idx = EnsureChainLocked(dest, path, is_dir, live);
    if (idx >= 0 && (size || mtime) && live) {
        const int32_t base = BaseCount();
        if (idx >= base) {
            live_.attrs[static_cast<size_t>(idx - base)].size = size;
            live_.attrs[static_cast<size_t>(idx - base)].mtime = FtToUnix(mtime);
        } else {
            Patch& p = patches_[idx];
            p.parent = NodeAt(idx).parent;
            p.flags = NodeAt(idx).flags;
            p.has_meta = true;
            p.has_attr = true;
            p.attr.size = size;
            p.attr.mtime = FtToUnix(mtime);
        }
    } else if (idx >= 0 && (size || mtime) && !live) {
        dest.attrs[static_cast<size_t>(idx)].size = size;
        dest.attrs[static_cast<size_t>(idx)].mtime = FtToUnix(mtime);
    }
    indexed_.store(live ? static_cast<size_t>(LiveCount()) : dest.nodes.size());
    InvalidateFilterLocked();
}

bool Engine::MatchNodeLocked(int32_t i, const CompiledQuery& q, int32_t prefix_node,
                             bool folders_only, bool use_attrs) const {
    if (IsTomb(i)) return false;
    for (int32_t root : inactive_volume_roots_)
        if (IsUnderLocked(i, root)) return false;
    const Node n = NodeAt(i);
    if (n.parent < 0) return false;
    if (n.flags & (kFlagHidden | kFlagDeleted)) return false;
    const bool is_dir = (n.flags & kFlagDir) != 0;
    if (folders_only && !is_dir) return false;
    if (prefix_node >= 0 && !IsUnderLocked(i, prefix_node)) return false;
    if (q.groups.empty()) return true;
    const std::wstring_view nm = NameOf(i);
    const Attr a = AttrAt(i);

    auto match_term = [&](const Term& t) -> bool {
        if (t.folder && !is_dir) return false;
        if (t.file && is_dir) return false;
        if (!t.exts.empty()) {
            bool ok = MatchExt(nm.data(), static_cast<uint32_t>(nm.size()), t);
            if (t.ext_not) ok = !ok;
            if (!ok) return false;
        }
        if (use_attrs && t.size_how != SizeHow::Any) {
            bool ok = MatchSize(a.size, t);
            if (t.size_not) ok = !ok;
            if (!ok) return false;
        }
        if (use_attrs && t.date_how != DateHow::Any) {
            uint64_t mt = UnixToFt(a.mtime);
            bool ok = mt != 0 && MatchDate(mt, t);
            if (t.date_not) ok = !ok;
            if (!ok) return false;
        }
        if (t.name_how != NameHow::Any) {
            bool ok = false;
            if (t.name_in_path) {
                if (t.name_how == NameHow::Wildcard) {
                    std::wstring path = BuildPathLocked(i);
                    ok = WildcardFolded(path.data(), static_cast<uint32_t>(path.size()), t.name);
                } else {
                    ParentChainGuard chain(LiveCount());
                    for (int32_t j = i; chain.Visit(j); j = NodeAt(j).parent) {
                        std::wstring_view pn = NameOf(j);
                        if (MatchName(pn.data(), static_cast<uint32_t>(pn.size()), t)) {
                            ok = true;
                            break;
                        }
                    }
                }
            } else {
                ok = MatchName(nm.data(), static_cast<uint32_t>(nm.size()), t);
            }
            if (t.name_not) ok = !ok;
            if (!ok) return false;
        }
        return true;
    };

    for (const auto& group : q.groups) {
        bool and_ok = true;
        for (const auto& t : group) {
            if (!match_term(t)) { and_ok = false; break; }
        }
        if (and_ok) return true;
    }
    return false;
}

void Engine::UpdateVolumeVisibilityLocked(const std::vector<VolumeInfo>& active, bool only_hide) {
    std::vector<int32_t> inactive;
    for (const auto& state : vols_) {
        const bool on = std::any_of(active.begin(), active.end(), [&](const VolumeInfo& volume) {
            return NormalizeVolumeId(volume.id) == NormalizeVolumeId(state.volume_id);
        });
        if (!on && state.root_idx >= 0) inactive.push_back(state.root_idx);
    }
    bool changed = false;
    if (only_hide) {
        for (int32_t root : inactive) {
            if (std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), root) ==
                inactive_volume_roots_.end()) {
                inactive_volume_roots_.push_back(root);
                changed = true;
            }
        }
    } else {
        changed = inactive_volume_roots_ != inactive;
        inactive_volume_roots_ = std::move(inactive);
    }
    // Visibility is checked at query time; it does not change file lengths or
    // parent links. Preserve query caches and size aggregates on unchanged wakes.
    if (changed) ++filter_epoch_;
}

void Engine::CollectMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                                  bool folders_only, bool use_attrs,
                                  MatchSet& ids,
                                  const std::atomic<uint32_t>* latest, uint32_t expected) const {
    const int32_t n = LiveCount();
    ids.Begin(n);
    if (n <= 0) return;

    // The immutable snapshot carries a disk-backed bigram inverted table.
    // Use it for ordinary two-or-more character name searches; only the
    // small mutable overlay still needs a direct scan.
    if (map_ && QueryIsSimpleName(cq) && !cq.groups[0][0].name.empty() &&
        ((cq.groups[0][0].name.size() == 1 && map_->prefix1_all_chars) ||
         (cq.groups[0][0].name.size() >= 2 && map_->prefix2_start))) {
        const auto& term = cq.groups[0][0];
        CompiledQuery literal_cq = cq;
        literal_cq.groups[0][0].pinyin = false;
        const bool single_char = term.name.size() == 1;
        const uint32_t bucket = single_char ? PrefixChar(term.name) : PairBucket(term.name);
        const bool direct_literal = term.name_how == NameHow::Substring && prefix_node < 0 && !folders_only &&
            live_.nodes.empty() && patches_.empty() && tombstones_.empty() &&
            inactive_volume_roots_.empty();
        auto collect_table = [&](const MappedFile& mapped, int32_t global_first) {
            const uint32_t* starts = single_char ? mapped.prefix1_start : mapped.prefix2_start;
            const int32_t* postings = single_char ? mapped.prefix1_ids : mapped.prefix2_ids;
            if (!starts || !postings || (single_char && !mapped.prefix1_all_chars)) return;
            const uint32_t first = starts[bucket];
            const uint32_t last = starts[bucket + 1];
            if (first > last || last > starts[kPrefixBuckets]) return;
            for (uint32_t p = first; p < last; ++p) {
                if ((p & 0x3ffu) == 0 && latest && latest->load() != expected) return;
                const int32_t local_id = postings[p];
                if (local_id < 0 || static_cast<uint32_t>(local_id) >= mapped.n ||
                    global_first < 0 || local_id >= BaseCount() - global_first) continue;
                const int32_t id = global_first + local_id;
                if (id < 0 || id >= BaseCount()) continue;
                if (patches_.contains(id)) continue;
                if (direct_literal) {
                    if (local_id < 0 || local_id >= static_cast<int32_t>(mapped.n)) continue;
                    const Node& node = mapped.nodes[static_cast<size_t>(local_id)];
                    if (node.parent >= 0 && !(node.flags & (kFlagHidden | kFlagDeleted)) &&
                        (single_char || ContainsFolded(mapped.pool + node.off, node.len, term.name)))
                        ids.Add(id);
                } else if (MatchQueryNodeLocked(id, literal_cq, prefix_node, folders_only, use_attrs)) {
                    ids.Add(id);
                }
            }
        };
        if (query_shards_ready_) {
            for (const auto& shard : query_shards_)
                collect_table(*shard.mapped, shard.first);
        } else {
            collect_table(*map_, 0);
        }
        if (term.pinyin && pinyin_ready_.load() && pinyin_snapshot_ == map_.get() &&
            pinyin_version_ == kPinyinDataVersion) {
            const std::vector<int32_t>* candidate_ids = &pinyin_chinese_ids_;
            // Every adjacent Latin pair in a successful match must exist in the
            // pronunciation graph. Pick its smallest posting without expanding paths.
            for (size_t i = 1; i < term.name.size(); ++i) {
                const wchar_t a = FoldChar(term.name[i - 1]), b = FoldChar(term.name[i]);
                if (a < L'a' || a > L'z' || b < L'a' || b > L'z') continue;
                const auto& posting = pinyin_pair_ids_[(a - L'a') * 26 + b - L'a'];
                if (posting.size() < candidate_ids->size()) candidate_ids = &posting;
            }
            const auto& candidates = *candidate_ids;
            size_t checked = 0;
            for (int32_t id : candidates) {
                if ((checked++ & 0x3ff) == 0 && latest && latest->load() != expected) return;
                if (patches_.contains(id)) continue; // current names below
                const auto name = QueryNameOfLocked(id);
                // Literal matches were already visited by the disk postings.
                if (ContainsFolded(name.data(), static_cast<uint32_t>(name.size()), term.name)) continue;
                if (MatchQueryNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
            }
        }
        for (int32_t id = BaseCount(); id < n; ++id) {
            if (MatchQueryNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
        }
        for (const auto& [id, patch] : patches_) {
            (void)patch;
            if (id >= 0 && id < BaseCount() &&
                MatchQueryNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
        }
        return;
    }

    std::vector<std::pair<int32_t, int32_t>> spans;
    auto add_span = [&](int32_t lo, int32_t hi) {
        lo = (std::max)(lo, 0);
        hi = (std::min)(hi, n);
        if (lo < hi) spans.emplace_back(lo, hi);
    };

    bool scoped = false;
    if (prefix_node >= 0) {
        const int32_t end = SubtreeEndLocked(prefix_node);
        if (end > prefix_node) {
            add_span(prefix_node, (std::min)(end, BaseCount()));
            if (n > BaseCount()) add_span(BaseCount(), n);
            scoped = true;
        }
    }
    if (!scoped && patches_.empty() && map_ && map_->hdr && map_->hdr->ver >= 8 && !vols_.empty()) {
        std::vector<char> skip(vols_.size(), 0);
        for (size_t vi = 0; vi < vols_.size(); ++vi) {
            for (int32_t root : inactive_volume_roots_)
                if (vols_[vi].root_idx == root) skip[vi] = 1;
        }
        bool any = false;
        for (size_t vi = 0; vi < vols_.size(); ++vi) {
            if (skip[vi]) continue;
            int32_t lo = 0, hi = 0;
            if (!VolumeSpan(vols_[vi], lo, hi)) { any = false; spans.clear(); break; }
            add_span(lo, hi);
            any = true;
        }
        if (any) {
            if (n > BaseCount()) add_span(BaseCount(), n);
            scoped = true;
        }
    }
    if (!scoped) add_span(0, n);

    auto consider = [&](int32_t i) {
        if ((i & 0x3ff) == 0 && latest && latest->load() != expected) return false;
        if (MatchQueryNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
            ids.Add(i);
        return true;
    };

    unsigned hw = std::thread::hardware_concurrency();
    unsigned T = hw < 2 ? 1u : (std::min)(hw, 8u);
    if (n < 250000) T = 1;
    if (T == 1 || spans.size() != 1) {
        for (auto [lo, hi] : spans) {
            for (int32_t i = lo; i < hi; ++i)
                if (!consider(i)) return;
        }
        return;
    }
    const int32_t lo = spans[0].first, hi = spans[0].second;
    const int32_t span_n = hi - lo;
    std::vector<MatchSet> parts(T);
    std::vector<std::thread> threads;
    threads.reserve(T);
    for (unsigned t = 0; t < T; ++t) {
        threads.emplace_back([&, t] {
            const int32_t a = lo + static_cast<int32_t>(static_cast<uint64_t>(span_n) * t / T);
            const int32_t b = lo + static_cast<int32_t>(static_cast<uint64_t>(span_n) * (t + 1) / T);
            parts[t].Begin(n);
            for (int32_t i = a; i < b; ++i) {
                if ((i & 0x3ff) == 0 && latest && latest->load() != expected)
                    return;
                if (MatchQueryNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
                    parts[t].Add(i);
            }
        });
    }
    for (auto& th : threads) th.join();
    if (latest && latest->load() != expected) return;
    for (auto& part : parts) {
        part.ForEach([&](int32_t i) { ids.Add(i); });
    }
}

void Engine::NarrowMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                                 bool folders_only, bool use_attrs,
                                 const MatchSet& prev, MatchSet& out,
                                 const std::atomic<uint32_t>* latest, uint32_t expected) const {
    out.Begin(LiveCount());
    int32_t seen = 0;
    prev.ForEach([&](int32_t i) {
        if ((++seen & 0x3ff) == 0 && latest && latest->load() != expected) return;
        if (i < 0 || i >= LiveCount()) return;
        if (MatchQueryNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
            out.Add(i);
    });
}

void Engine::SortIdsLocked(std::vector<int32_t>& ids, ResultSort sort, bool desc) const {
    if (ids.size() <= 1 || sort == ResultSort::Index) {
        if (desc && sort == ResultSort::Index) std::reverse(ids.begin(), ids.end());
        return;
    }
    if (query_shards_ready_ && live_.nodes.empty() && patches_.empty() &&
        tombstones_.empty()) {
        std::vector<uint8_t> selected(static_cast<size_t>(BaseCount()), 0);
        for (int32_t id : ids)
            if (id >= 0 && id < BaseCount()) selected[static_cast<size_t>(id)] = 1;
        auto less = [&](int32_t a, int32_t b) {
            if (sort == ResultSort::Name) {
                int c = CmpLogical(QueryNameOfLocked(a), QueryNameOfLocked(b));
                if (c == 0) c = a < b ? -1 : a > b ? 1 : 0;
                return desc ? c > 0 : c < 0;
            }
            const bool da = (QueryNodeAtLocked(a).flags & kFlagDir) != 0;
            const bool db = (QueryNodeAtLocked(b).flags & kFlagDir) != 0;
            if (da != db) return da;
            const Attr aa = QueryAttrAtLocked(a);
            const Attr ab = QueryAttrAtLocked(b);
            const uint64_t va = sort == ResultSort::Size ? aa.size : aa.mtime;
            const uint64_t vb = sort == ResultSort::Size ? ab.size : ab.mtime;
            if (va != vb) return desc ? va > vb : va < vb;
            int c = CmpLogical(QueryNameOfLocked(a), QueryNameOfLocked(b));
            if (c == 0) c = a < b ? -1 : a > b ? 1 : 0;
            return desc ? c > 0 : c < 0;
        };
        std::vector<int32_t> sorted;
        for (const auto& shard : query_shards_) {
            const int32_t* order = sort == ResultSort::Name ? shard.mapped->name_order :
                sort == ResultSort::Size ? shard.mapped->size_order : shard.mapped->mtime_order;
            if (!order) { sorted.clear(); break; }
            std::vector<int32_t> stream;
            stream.reserve(static_cast<size_t>(shard.last - shard.first));
            auto append = [&](size_t first, size_t last, bool reverse) {
                if (!reverse) {
                    for (size_t i = first; i < last; ++i) {
                        const int32_t id = shard.first + order[i];
                        if (selected[static_cast<size_t>(id)]) stream.push_back(id);
                    }
                } else {
                    for (size_t i = last; i > first; --i) {
                        const int32_t id = shard.first + order[i - 1];
                        if (selected[static_cast<size_t>(id)]) stream.push_back(id);
                    }
                }
            };
            const size_t count = static_cast<size_t>(shard.last - shard.first);
            if (!desc || sort == ResultSort::Name) {
                append(0, count, desc);
            } else {
                size_t split = 0;
                while (split < count &&
                       (shard.mapped->nodes[static_cast<size_t>(order[split])].flags & kFlagDir))
                    ++split;
                append(0, split, true);
                append(split, count, true);
            }
            if (sorted.empty()) {
                sorted = std::move(stream);
            } else {
                std::vector<int32_t> merged;
                merged.reserve(sorted.size() + stream.size());
                std::merge(sorted.begin(), sorted.end(), stream.begin(), stream.end(),
                           std::back_inserter(merged), less);
                sorted.swap(merged);
            }
        }
        if (sorted.size() == ids.size()) {
            ids.swap(sorted);
            return;
        }
    }
    if (map_) {
        const int32_t* order = sort == ResultSort::Name ? map_->name_order :
                               sort == ResultSort::Size ? map_->size_order : map_->mtime_order;
        if (order) {
            std::vector<uint8_t> selected(map_->n, 0);
            for (int32_t id : ids)
                if (id >= 0 && id < static_cast<int32_t>(map_->n)) selected[static_cast<size_t>(id)] = 1;
            const int32_t base = BaseCount();
            std::vector<int32_t> delta;
            delta.reserve(patches_.size() + live_.nodes.size());
            for (int32_t id : ids) {
                if (id >= base || patches_.find(id) != patches_.end())
                    delta.push_back(id);
            }
            auto less = [&](int32_t a, int32_t b) {
                if (sort == ResultSort::Name) {
                    int c = CmpLogical(NameOf(a), NameOf(b));
                    if (c == 0) c = a < b ? -1 : a > b ? 1 : 0;
                    return desc ? c > 0 : c < 0;
                }
                const bool da = (NodeAt(a).flags & kFlagDir) != 0;
                const bool db = (NodeAt(b).flags & kFlagDir) != 0;
                if (da != db) return da;
                const uint64_t va = sort == ResultSort::Size ? AttrAt(a).size : AttrAt(a).mtime;
                const uint64_t vb = sort == ResultSort::Size ? AttrAt(b).size : AttrAt(b).mtime;
                if (va != vb) return desc ? va > vb : va < vb;
                int c = CmpLogical(NameOf(a), NameOf(b));
                if (c == 0) c = a < b ? -1 : a > b ? 1 : 0;
                return desc ? c > 0 : c < 0;
            };
            std::sort(delta.begin(), delta.end(), less);
            std::vector<int32_t> base_sorted;
            base_sorted.reserve(ids.size() - delta.size());
            auto emit = [&](size_t first, size_t last, bool reverse) {
                if (!reverse) {
                    for (size_t i = first; i < last; ++i)
                        if (selected[static_cast<size_t>(order[i])] &&
                            patches_.find(order[i]) == patches_.end())
                            base_sorted.push_back(order[i]);
                } else {
                    for (size_t i = last; i > first; --i)
                        if (selected[static_cast<size_t>(order[i - 1])] &&
                            patches_.find(order[i - 1]) == patches_.end())
                            base_sorted.push_back(order[i - 1]);
                }
            };
            if (!desc || sort == ResultSort::Name) {
                emit(0, map_->n, desc);
            } else {
                size_t split = 0;
                while (split < map_->n &&
                       (map_->nodes[static_cast<size_t>(order[split])].flags & kFlagDir)) ++split;
                emit(0, split, true);
                emit(split, map_->n, true);
            }
            std::vector<int32_t> sorted;
            sorted.reserve(base_sorted.size() + delta.size());
            std::merge(base_sorted.begin(), base_sorted.end(), delta.begin(), delta.end(),
                       std::back_inserter(sorted), less);
            ids.swap(sorted);
            return;
        }
    }
    auto name_cmp = [&](int32_t a, int32_t b) {
        int c = CmpLogical(NameOf(a), NameOf(b));
        if (c == 0) c = (a < b) ? -1 : (a > b ? 1 : 0);
        return desc ? c > 0 : c < 0;
    };
    if (sort == ResultSort::Name) {
        std::sort(ids.begin(), ids.end(), name_cmp);
        return;
    }
    std::sort(ids.begin(), ids.end(), [&](int32_t a, int32_t b) {
        const bool da = (NodeAt(a).flags & kFlagDir) != 0;
        const bool db = (NodeAt(b).flags & kFlagDir) != 0;
        if (da != db) return da;
        uint64_t va = 0, vb = 0;
        if (sort == ResultSort::Size) {
            va = AttrAt(a).size;
            vb = AttrAt(b).size;
        } else {
            va = AttrAt(a).mtime;
            vb = AttrAt(b).mtime;
        }
        if (va != vb) return desc ? va > vb : va < vb;
        return name_cmp(a, b);
    });
}

void Engine::PartialSortPage(std::vector<int32_t>& ids, size_t offset, size_t limit,
                             ResultSort sort, bool desc) const {
    if (ids.size() <= 1 || limit == 0) return;
    // Immutable order arrays already contain the complete comparator order.
    // Walking that order is faster and bounded in memory than nth_element's
    // repeated logical-name comparisons for million-item result sets.
    if (map_ && sort != ResultSort::Index &&
        ((sort == ResultSort::Name && map_->name_order) ||
         (sort == ResultSort::Size && map_->size_order) ||
         (sort == ResultSort::Mtime && map_->mtime_order))) {
        SortIdsLocked(ids, sort, desc);
        return;
    }
    const size_t keep = (std::min)(ids.size(), offset + limit);
    if (keep >= ids.size()) {
        SortIdsLocked(ids, sort, desc);
        return;
    }
    auto less = [&](int32_t a, int32_t b) {
        if (sort == ResultSort::Index) return desc ? a > b : a < b;
        if (sort == ResultSort::Name) {
            int c = CmpLogical(NameOf(a), NameOf(b));
            if (c == 0) c = (a < b) ? -1 : (a > b ? 1 : 0);
            return desc ? c > 0 : c < 0;
        }
        const bool da = (NodeAt(a).flags & kFlagDir) != 0;
        const bool db = (NodeAt(b).flags & kFlagDir) != 0;
        if (da != db) return da;
        uint64_t va = sort == ResultSort::Size ? AttrAt(a).size : AttrAt(a).mtime;
        uint64_t vb = sort == ResultSort::Size ? AttrAt(b).size : AttrAt(b).mtime;
        if (va != vb) return desc ? va > vb : va < vb;
        int c = CmpLogical(NameOf(a), NameOf(b));
        if (c == 0) c = (a < b) ? -1 : (a > b ? 1 : 0);
        return desc ? c > 0 : c < 0;
    };
    std::nth_element(ids.begin(), ids.begin() + static_cast<std::ptrdiff_t>(keep), ids.end(), less);
    ids.resize(keep);
    SortIdsLocked(ids, sort, desc);
}

SearchResult Engine::Search(const Query& q, const std::atomic<uint32_t>* latest,
                            uint32_t expected, const DirVisibility* visibility) const {
    SearchResult out;
    std::lock_guard<std::mutex> query_guard(query_mu_);
    if (latest && latest->load() != expected) return out;
    CompiledQuery cq = ParseQuery(q.needle);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    out.layout = layout_epoch_.load();
    int32_t prefix_node = -1;
    const std::wstring& prefix = !q.path_prefix.empty() ? q.path_prefix : cq.path_prefix;
    if (!prefix.empty()) {
        prefix_node = ResolvePathLocked(prefix);
        if (prefix_node < 0) return out;
    }
    bool use_attrs = false;
    if (QueryUsesAttrs(cq)) {
        use_attrs = true;
    }
    const size_t cap = q.limit;

    MatchSet matches;
    const bool same_scope = cache_path_prefix_ == prefix &&
        cache_folders_only_ == q.folders_only;
    const bool exact_cached_page = cache_epoch_ == filter_epoch_ && same_scope &&
        cache_raw_ == q.needle && cache_set_.universe == LiveCount();
    if (exact_cached_page) {
        matches = cache_set_;
    } else if (cache_epoch_ == filter_epoch_ && same_scope &&
               QueryCanNarrow(cache_raw_, q.needle) && cache_set_.universe == LiveCount()) {
        NarrowMatchesLocked(cq, prefix_node, q.folders_only, use_attrs, cache_set_, matches,
                            latest, expected);
    } else {
        CollectMatchesLocked(cq, prefix_node, q.folders_only, use_attrs, matches, latest, expected);
    }
    if (latest && latest->load() != expected) return out;
    // Caller filter (#66). The shared cache keeps the caller-independent set;
    // only this result is narrowed to the folders the caller may list.
    MatchSet unfiltered;
    bool filtered = false;
    if (visibility && matches.total) {
        MatchSet visible;
        visible.universe = matches.universe;
        std::vector<int32_t> unchecked;
        int32_t last_parent = INT32_MIN;
        int last_state = 0;
        matches.ForEach([&](int32_t id) {
            // Ids follow the folder layout, so siblings usually arrive together.
            const int32_t parent = QueryNodeAtLocked(id).parent;
            if (parent != last_parent) {
                last_parent = parent;
                last_state = parent < 0 ? 1 : visibility->State(parent);
                if (last_state < 0) unchecked.push_back(parent);
            }
            if (last_state > 0) visible.ids.push_back(id);
        });
        if (!unchecked.empty()) {
            std::sort(unchecked.begin(), unchecked.end());
            unchecked.erase(std::unique(unchecked.begin(), unchecked.end()), unchecked.end());
            out.unchecked_dirs.reserve(unchecked.size());
            for (int32_t dir : unchecked) out.unchecked_dirs.emplace_back(dir, BuildQueryPathLocked(dir));
            cache_raw_ = q.needle;
            cache_path_prefix_ = prefix;
            cache_folders_only_ = q.folders_only;
            cache_set_ = std::move(matches);
            cache_epoch_ = filter_epoch_;
            return out;
        }
        visible.total = visible.ids.size();
        unfiltered = std::move(matches);
        matches = std::move(visible);
        filtered = true;
    }
    const MatchSet& cacheable = filtered ? unfiltered : matches;
    out.total = matches.total;
    if (cap == 0 || matches.total == 0) {
        cache_raw_ = q.needle;
        cache_path_prefix_ = prefix;
        cache_folders_only_ = q.folders_only;
        cache_ranked_ = q.rank;
        cache_sort_ = q.sort;
        cache_sort_desc_ = q.sort_desc;
        cache_set_ = cacheable;
        cache_epoch_ = filter_epoch_;
        return out;
    }

    cache_raw_ = q.needle;
    cache_path_prefix_ = prefix;
    cache_folders_only_ = q.folders_only;
    cache_ranked_ = q.rank;
    cache_sort_ = q.rank ? ResultSort::Index : q.sort;
    cache_sort_desc_ = q.rank ? false : q.sort_desc;
    cache_set_ = cacheable;
    cache_epoch_ = filter_epoch_;

    const size_t start = (std::min)(q.offset, matches.total);
    const size_t want = cap;
    std::vector<int32_t> chosen;
    chosen.reserve((std::min)(want, matches.total));
    if (q.rank) {
        struct Ranked {
            int32_t id = -1;
            int score = 0;
        };
        auto better = [](const Ranked& a, const Ranked& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.id < b.id;
        };
        std::priority_queue<Ranked, std::vector<Ranked>, decltype(better)> top(better);
        const size_t heap_n = start + want;
        const bool rank_single_char = QueryIsSimpleName(cq) &&
            cq.groups[0][0].name_how == NameHow::Substring &&
            cq.groups[0][0].name.size() == 1;
        const wchar_t rank_char = rank_single_char ? cq.groups[0][0].name[0] : 0;
        const bool rank_from_base = map_ && live_.nodes.empty() && patches_.empty() &&
            tombstones_.empty();
        matches.ForEach([&](int32_t id) {
            if (latest && latest->load() != expected) return;
            Node n{};
            std::wstring_view nm;
            if (rank_from_base && id >= 0 && id < BaseCount()) {
                n = map_->nodes[static_cast<size_t>(id)];
                nm = { map_->pool + n.off, n.len };
            } else {
                n = QueryNodeAtLocked(id);
                nm = QueryNameOfLocked(id);
            }
            const int score = rank_single_char
                ? RankSingleCharMatch(nm.data(), static_cast<uint32_t>(nm.size()),
                                      (n.flags & kFlagDir) != 0, rank_char)
                : RankName(nm.data(), static_cast<uint32_t>(nm.size()),
                           (n.flags & kFlagDir) != 0, cq);
            Ranked candidate{ id, score };
            if (top.size() < heap_n) {
                top.push(candidate);
            } else if (better(candidate, top.top())) {
                top.pop();
                top.push(candidate);
            }
        });
        if (latest && latest->load() != expected) return SearchResult{};
        std::vector<Ranked> ranked;
        ranked.reserve(top.size());
        while (!top.empty()) {
            ranked.push_back(top.top());
            top.pop();
        }
        std::sort(ranked.begin(), ranked.end(), better);
        for (size_t k = start; k < ranked.size() && chosen.size() < want; ++k)
            chosen.push_back(ranked[k].id);
    } else {
        std::vector<int32_t> ids;
        if (!matches.dense) {
            ids = matches.ids;
        } else {
            ids.reserve(matches.total);
            matches.ForEach([&](int32_t id) { ids.push_back(id); });
        }
        if (q.sort != ResultSort::Index)
            PartialSortPage(ids, start, want, q.sort, q.sort_desc);
        else if (q.sort_desc)
            std::reverse(ids.begin(), ids.end());
        const size_t end = (std::min)(ids.size(), start + want);
        for (size_t k = start; k < end; ++k) chosen.push_back(ids[k]);
    }

    out.hits.reserve(chosen.size());
    for (size_t k = 0; k < chosen.size(); ++k) {
        const Node n = QueryNodeAtLocked(chosen[k]);
        const Attr a = QueryAttrAtLocked(chosen[k]);
        Hit h;
        h.path = BuildQueryPathLocked(chosen[k]);
        if (h.path.empty()) continue;
        h.name.assign(QueryNameOfLocked(chosen[k]));
        h.is_dir = (n.flags & kFlagDir) != 0;
        h.size = a.size;
        h.mtime = UnixToFt(a.mtime);
        out.hits.push_back(std::move(h));
    }
    return out;
}

bool Engine::WriteIndexFile(const std::wstring& path, const Store& s,
                            const std::vector<VolState>& vols, uint64_t built_unix) const {
    if (s.nodes.size() > kIndexCap + 64 || s.pool.size() > UINT32_MAX) {
        return IndexFailure("filename_write_limit_failed", ERROR_FILE_TOO_LARGE, path);
    }
    HierarchyIssue hierarchy_issue;
    if (!ValidateIndexHierarchy(static_cast<int32_t>(s.nodes.size()),
            [&](int32_t i) { return s.nodes[static_cast<size_t>(i)]; }, &hierarchy_issue)) {
        diagnostics::runtime::Event("index_write_invalid_hierarchy", {{"operation", index_operation},
            {"nodes", s.nodes.size()}, {"error", ERROR_INVALID_DATA},
            {"reason", static_cast<uint64_t>(hierarchy_issue.error)},
            {"node", static_cast<uint32_t>(hierarchy_issue.node)},
            {"parent", static_cast<uint32_t>(hierarchy_issue.parent)}}, diagnostics::runtime::Level::Error);
        return IndexFailure("filename_write_hierarchy_failed", ERROR_INVALID_DATA, path);
    }
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return IndexFailure("filename_write_open_failed", error, tmp);
    }

    auto name_of = [&](int32_t id) -> std::wstring_view {
        const Node& node = s.nodes[static_cast<size_t>(id)];
        return { s.pool.data() + node.off, node.len };
    };
    bool prefix_overflow = false;
    auto build_prefix = [&](bool pair) {
        std::vector<uint32_t> counts(kPrefixBuckets, 0);
        std::vector<uint32_t> local;
        auto visit = [&](std::wstring_view name, auto&& fn) {
            if (!pair) {
                local.clear();
                local.reserve(name.size());
                for (wchar_t c : name)
                    local.push_back(static_cast<uint16_t>(FoldChar(c)));
                std::sort(local.begin(), local.end());
                local.erase(std::unique(local.begin(), local.end()), local.end());
                for (uint32_t bucket : local) fn(bucket);
                return;
            }
            local.clear();
            local.reserve(name.size());
            for (size_t k = 0; k + 1 < name.size(); ++k)
                local.push_back(PairBucket(name.substr(k, 2)));
            std::sort(local.begin(), local.end());
            local.erase(std::unique(local.begin(), local.end()), local.end());
            for (uint32_t bucket : local) fn(bucket);
        };
        for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i)
            visit(name_of(i), [&](uint32_t bucket) { ++counts[bucket]; });

        std::vector<uint32_t> start(kPrefixBuckets + 1, 0);
        uint64_t posting_count = 0;
        for (uint32_t i = 0; i < kPrefixBuckets; ++i) {
            posting_count += counts[i];
            if (posting_count > UINT32_MAX) {
                prefix_overflow = true;
                return std::make_pair(std::vector<uint32_t>{}, std::vector<int32_t>{});
            }
            start[i + 1] = static_cast<uint32_t>(posting_count);
        }
        std::vector<uint32_t> cursor(start.begin(), start.end() - 1);
        std::vector<int32_t> ids(start.back(), -1);
        for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i)
            visit(name_of(i), [&](uint32_t bucket) { ids[cursor[bucket]++] = i; });
        return std::make_pair(std::move(start), std::move(ids));
    };
    const auto prefix1 = build_prefix(false);
    const auto prefix2 = build_prefix(true);
    if (prefix_overflow) {
        CloseHandle(h);
        DeleteFileW(tmp.c_str());
        return IndexFailure("filename_write_prefix_limit_failed", ERROR_FILE_TOO_LARGE, path);
    }

    uint64_t off = 128;
    DiskHeader hdr{};
    memcpy(hdr.magic, "PIDX", 4);
    hdr.ver = kIndexVer;
    hdr.node_count = static_cast<uint32_t>(s.nodes.size());
    hdr.vol_count = static_cast<uint32_t>(vols.size());
    hdr.built_unix = built_unix;
    hdr.nodes_off = off;
    off = AlignUp(off + sizeof(Node) * s.nodes.size(), 16);
    hdr.attrs_off = off;
    off = AlignUp(off + sizeof(Attr) * s.attrs.size(), 16);
    hdr.pool_off = off;
    hdr.pool_chars = s.pool.size();
    off = AlignUp(off + s.pool.size() * sizeof(wchar_t), 16);
    hdr.child_order_off = off;
    off = AlignUp(off + s.nodes.size() * sizeof(int32_t), 16);
    // Column sorting is performed on the current result set. Keeping three
    // full ordering arrays here multiplied build time and added 12 bytes per
    // indexed item to every immutable snapshot.
    hdr.name_order_off = 0;
    hdr.size_order_off = 0;
    hdr.mtime_order_off = 0;
    const uint64_t prefix1_position = off;
    hdr.prefix1_off = prefix1_position | kPrefixAllCharsFlag;
    off = AlignUp(off + prefix1.first.size() * sizeof(uint32_t) +
                         prefix1.second.size() * sizeof(int32_t), 16);
    hdr.prefix2_off = off;
    off = AlignUp(off + prefix2.first.size() * sizeof(uint32_t) +
                         prefix2.second.size() * sizeof(int32_t), 16);
    hdr.vols_off = off;
    off = AlignUp(off + sizeof(DiskVol) * vols.size(), 16);
    hdr.frn_off = off;
    uint64_t frn_count = 0;
    for (const auto& v : vols) frn_count += v.frn_build.size();
    hdr.frn_count = static_cast<uint32_t>(frn_count);

    std::vector<int32_t> child_order(s.nodes.size());
    std::iota(child_order.begin(), child_order.end(), 0);
    std::sort(child_order.begin(), child_order.end(), [&](int32_t a, int32_t b) {
        const Node& na = s.nodes[static_cast<size_t>(a)];
        const Node& nb = s.nodes[static_cast<size_t>(b)];
        if (na.parent != nb.parent) return na.parent < nb.parent;
        const int c = CompareFolded(name_of(a), name_of(b));
        return c != 0 ? c < 0 : a < b;
    });

    std::vector<BYTE> pad(128, 0);
    memcpy(pad.data(), &hdr, sizeof(hdr));
    DWORD write_error = ERROR_SUCCESS;
    auto checked = [&](bool result) {
        if (!result) write_error = GetLastError();
        return result;
    };
    bool ok = checked(WriteAll(h, pad.data(), pad.size()));
    if (ok && !s.nodes.empty())
        ok = checked(WriteAll(h, s.nodes.data(), s.nodes.size() * sizeof(Node)));
    if (ok) {
        const uint64_t at = hdr.nodes_off + s.nodes.size() * sizeof(Node);
        if (hdr.attrs_off > at) {
            std::vector<BYTE> z(static_cast<size_t>(hdr.attrs_off - at), 0);
            ok = checked(WriteAll(h, z.data(), z.size()));
        }
    }
    if (ok && !s.attrs.empty())
        ok = checked(WriteAll(h, s.attrs.data(), s.attrs.size() * sizeof(Attr)));
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.pool_off);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
    }
    if (ok && !s.pool.empty())
        ok = checked(WriteAll(h, s.pool.data(), s.pool.size() * sizeof(wchar_t)));
    auto write_order = [&](uint64_t position, const std::vector<int32_t>& order) {
        if (!ok) return;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(position);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
        if (ok && !order.empty()) ok = checked(WriteAll(h, order.data(), order.size() * sizeof(int32_t)));
    };
    write_order(hdr.child_order_off, child_order);
    auto write_prefix = [&](uint64_t position, const std::vector<uint32_t>& start,
                            const std::vector<int32_t>& ids) {
        if (!ok) return;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(position);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
        if (ok) ok = checked(WriteAll(h, start.data(), start.size() * sizeof(uint32_t)));
        if (ok && !ids.empty()) ok = checked(WriteAll(h, ids.data(), ids.size() * sizeof(int32_t)));
    };
    write_prefix(prefix1_position, prefix1.first, prefix1.second);
    write_prefix(hdr.prefix2_off, prefix2.first, prefix2.second);
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.vols_off);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
    }
    std::vector<DiskVol> dvols(vols.size());
    uint64_t frn_cur = hdr.frn_off;
    for (size_t i = 0; i < vols.size(); ++i) {
        dvols[i].letter = static_cast<uint16_t>(vols[i].letter);
        dvols[i].kind = static_cast<uint16_t>(vols[i].kind);
        dvols[i].item_count = vols[i].item_count;
        dvols[i].journal_id = vols[i].journal_id;
        dvols[i].next_usn = vols[i].next_usn;
        dvols[i].root_idx = vols[i].root_idx;
        dvols[i].first_idx = vols[i].first_idx;
        dvols[i].frn_count = static_cast<uint32_t>(vols[i].frn_build.size());
        dvols[i].frn_off = frn_cur;
        wcsncpy_s(dvols[i].volume_id, vols[i].volume_id.c_str(), _TRUNCATE);
        frn_cur += dvols[i].frn_count * sizeof(DiskFrn);
    }
    if (ok && !dvols.empty())
        ok = checked(WriteAll(h, dvols.data(), dvols.size() * sizeof(DiskVol)));
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.frn_off);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
    }
    if (ok) {
        for (const auto& v : vols) {
            if (v.frn_build.empty()) continue;
            std::vector<DiskFrn> rows(v.frn_build.size());
            for (size_t i = 0; i < v.frn_build.size(); ++i) {
                rows[i].frn = v.frn_build[i].first;
                rows[i].idx = v.frn_build[i].second;
            }
            std::sort(rows.begin(), rows.end(),
                      [](const DiskFrn& a, const DiskFrn& b) { return a.frn < b.frn; });
            if (!checked(WriteAll(h, rows.data(), rows.size() * sizeof(DiskFrn)))) { ok = false; break; }
        }
    }
    // Seeking beyond EOF does not extend the file when the final tables are
    // empty. Persist their aligned endpoint so the reader's range checks hold.
    if (ok) ok = checked(SetEndOfFile(h) != FALSE);
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return IndexFailure("filename_write_data_failed", write_error, tmp);
    }
    return true;
}

bool Engine::CommitMappedFile(const std::wstring& path) {
    const std::wstring tmp = path + L".tmp";
    std::unique_ptr<MappedFile> mapped;
    auto fail = [&](const char* event, DWORD error) {
        mapped.reset();
        return IndexFailure(event, error, path);
    };
    auto move = [](const std::wstring& source, const std::wstring& target, uint32_t stage) {
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
        if (volume_fault_test::SnapshotFault(stage)) return false;
#else
        (void)stage;
#endif
        return MoveFileExW(source.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != FALSE;
    };
    if (!MapIndexFile(tmp, mapped)) {
        const DWORD error = GetLastError();
        return fail("filename_publish_map_failed", error);
    }
    std::wstring retired;
    if (MachineIndexScope()) {
        const ShardPaths paths = AggregateShardPaths();
        ShardManifest published;
        if (!PublishShardBase(paths, tmp, built_unix_, 0, published, nullptr)) {
            const DWORD error = GetLastError();
            return fail("filename_publish_shard_failed", error);
        }
    } else {
        // Windows cannot replace a file with a live mapped view. Rename the
        // old generation first, retaining its mapping until publication works.
        static std::atomic<uint64_t> publication_id{0};
        retired = path + L".previous." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(++publication_id);
        if (!move(path, retired, 2)) {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND) {
                return fail("filename_publish_retire_failed", error);
            }
            retired.clear();
        }
        if (!move(tmp, path, 3)) {
            const DWORD error = GetLastError();
            if (!retired.empty() && !move(retired, path, 4)) {
                const DWORD rollback_error = GetLastError();
                IndexFailure("filename_publish_rollback_failed", rollback_error, retired);
            }
            return fail("filename_publish_replace_failed", error);
        }
    }
    AdoptMappedLocked(std::move(mapped));
    if (!retired.empty()) DeleteFileW(retired.c_str());
    return true;
}

void Engine::WriteVolumeShards(const Store& aggregate, const std::vector<VolState>& vols,
                               uint64_t built_unix) const {
    if (!MachineIndexScope() || aggregate.nodes.empty()) return;
    const std::wstring root = DataDir() + L"\\v9\\volumes";
    for (const auto& source : vols) {
        if (source.volume_id.empty() || source.item_count == 0) continue;
        const int64_t first = source.first_idx;
        const int64_t last = first + static_cast<int64_t>(source.item_count);
        if (first < 0 || last > static_cast<int64_t>(aggregate.nodes.size()) || first >= last)
            continue;

        Store shard;
        shard.nodes.reserve(static_cast<size_t>(last - first));
        shard.attrs.reserve(static_cast<size_t>(last - first));
        for (int64_t i = first; i < last; ++i) {
            const Node& source_node = aggregate.nodes[static_cast<size_t>(i)];
            Node node = source_node;
            node.parent = (i == first) ? -1 :
                ((source_node.parent >= first && source_node.parent < last)
                    ? static_cast<int32_t>(source_node.parent - first) : -1);
            const size_t name_off = shard.pool.size();
            if (source_node.off + source_node.len <= aggregate.pool.size()) {
                shard.pool.insert(shard.pool.end(), aggregate.pool.begin() + source_node.off,
                                  aggregate.pool.begin() + source_node.off + source_node.len);
            }
            node.off = static_cast<uint32_t>(name_off);
            node.unused = source_node.unused >= static_cast<uint32_t>(first) &&
                          source_node.unused <= static_cast<uint32_t>(last)
                ? source_node.unused - static_cast<uint32_t>(first) : 0;
            shard.nodes.push_back(node);
            shard.attrs.push_back(aggregate.attrs[static_cast<size_t>(i)]);
        }
        VolState v = source;
        v.root_idx = 0;
        v.first_idx = 0;
        v.item_count = shard.nodes.size();
        v.frn_base = nullptr;
        v.frn_base_n = 0;
        v.frn_new.clear();
        v.frn_build.clear();
        for (const auto& entry : source.frn_build) {
            if (entry.second >= first && entry.second < last)
                v.frn_build.emplace_back(entry.first, entry.second - static_cast<int32_t>(first));
        }
        const ShardPaths paths = MakeShardPaths(root, source.volume_id);
        const std::wstring temp = paths.base_a + L".tmp";
        DeleteFileW(temp.c_str());
        if (!WriteIndexFile(paths.base_a, shard, {v}, built_unix)) continue;
        ShardManifest published;
        if (!PublishShardBase(paths, temp, built_unix, 0, published, nullptr, source.volume_id)) {
            const DWORD error = GetLastError();
            IndexFailure("filename_publish_volume_shard_failed", error, temp);
        }
    }
}

bool Engine::MapIndexFile(const std::wstring& path, std::unique_ptr<MappedFile>& out) const {
    auto m = std::make_unique<MappedFile>();
    auto fail = [&](const char* stage, DWORD error) {
        m.reset();
        return IndexFailure(stage, error, path);
    };
    m->file = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m->file == INVALID_HANDLE_VALUE) return fail("filename_map_open_failed", GetLastError());
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(m->file, &sz)) return fail("filename_map_size_failed", GetLastError());
    if (sz.QuadPart < static_cast<LONGLONG>(sizeof(DiskHeader)))
        return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    m->size = static_cast<size_t>(sz.QuadPart);
    m->mapping = CreateFileMappingW(m->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m->mapping) return fail("filename_map_create_failed", GetLastError());
    m->view = static_cast<const uint8_t*>(MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0));
    if (!m->view) return fail("filename_map_view_failed", GetLastError());
    m->hdr = reinterpret_cast<const DiskHeader*>(m->view);
    if (memcmp(m->hdr->magic, "PIDX", 4) != 0 ||
        m->hdr->ver < kIndexVerMin || m->hdr->ver > kIndexVer) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (m->hdr->node_count > kIndexCap + 64 || m->hdr->pool_chars > UINT32_MAX)
        return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    auto in_range = [&](uint64_t o, uint64_t n) {
        return o <= m->size && n <= m->size - o;
    };
    if (!in_range(m->hdr->nodes_off, sizeof(Node) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (!in_range(m->hdr->attrs_off, sizeof(Attr) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (!in_range(m->hdr->pool_off, sizeof(wchar_t) * m->hdr->pool_chars)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (!in_range(m->hdr->child_order_off, sizeof(int32_t) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (m->hdr->name_order_off &&
        !in_range(m->hdr->name_order_off, sizeof(int32_t) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (m->hdr->size_order_off &&
        !in_range(m->hdr->size_order_off, sizeof(int32_t) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (m->hdr->mtime_order_off &&
        !in_range(m->hdr->mtime_order_off, sizeof(int32_t) * m->hdr->node_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    const uint64_t prefix_start_bytes = (kPrefixBuckets + 1ull) * sizeof(uint32_t);
    auto prefix_in_range = [&](uint64_t raw_off, bool allow_all_chars_flag,
                               const uint32_t*& starts, const int32_t*& ids,
                               bool& all_chars) {
        if (!allow_all_chars_flag && (raw_off & kPrefixAllCharsFlag)) return false;
        all_chars = allow_all_chars_flag && (raw_off & kPrefixAllCharsFlag) != 0;
        const uint64_t off = raw_off & ~kPrefixAllCharsFlag;
        if (!off || !in_range(off, prefix_start_bytes)) return !off;
        starts = reinterpret_cast<const uint32_t*>(m->view + off);
        const uint32_t count = starts[kPrefixBuckets];
        if (!in_range(off + prefix_start_bytes,
                      static_cast<uint64_t>(count) * sizeof(int32_t))) return false;
        ids = reinterpret_cast<const int32_t*>(m->view + off + prefix_start_bytes);
        if (starts[0] != 0) return false;
        for (uint32_t bucket = 0; bucket < kPrefixBuckets; ++bucket) {
            if (starts[bucket] > starts[bucket + 1] || starts[bucket + 1] > count)
                return false;
        }
        for (uint32_t p = 0; p < count; ++p) {
            if (ids[p] < 0 || static_cast<uint32_t>(ids[p]) >= m->hdr->node_count)
                return false;
        }
        return true;
    };
    if (!prefix_in_range(m->hdr->prefix1_off, true, m->prefix1_start, m->prefix1_ids,
                         m->prefix1_all_chars)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    bool unused_prefix_flag = false;
    if (!prefix_in_range(m->hdr->prefix2_off, false, m->prefix2_start, m->prefix2_ids,
                         unused_prefix_flag)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (!in_range(m->hdr->vols_off, sizeof(DiskVol) * m->hdr->vol_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    if (!in_range(m->hdr->frn_off, sizeof(DiskFrn) * m->hdr->frn_count)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    m->nodes = reinterpret_cast<const Node*>(m->view + m->hdr->nodes_off);
    m->attrs = reinterpret_cast<const Attr*>(m->view + m->hdr->attrs_off);
    m->pool = reinterpret_cast<const wchar_t*>(m->view + m->hdr->pool_off);
    m->n = m->hdr->node_count;
    for (uint32_t i = 0; i < m->n; ++i) {
        const Node& node = m->nodes[i];
        if (static_cast<uint64_t>(node.off) + node.len > m->hdr->pool_chars) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    }
    m->vols = m->hdr->vol_count
        ? reinterpret_cast<const DiskVol*>(m->view + m->hdr->vols_off) : nullptr;
    m->nvol = m->hdr->vol_count;
    HierarchyIssue hierarchy_issue;
    if (!ValidateIndexHierarchy(static_cast<int32_t>(m->n),
            [&](int32_t i) { return m->nodes[i]; }, &hierarchy_issue)) {
        diagnostics::runtime::Event("index_hierarchy_rejected", {{"operation", index_operation}, {"reason", static_cast<uint64_t>(hierarchy_issue.error)},
            {"node", static_cast<uint32_t>(hierarchy_issue.node)}, {"parent", static_cast<uint32_t>(hierarchy_issue.parent)},
            {"nodes", m->n}, {"cache_version", m->hdr->ver}}, diagnostics::runtime::Level::Error);
        return fail("filename_map_hierarchy_failed", ERROR_INVALID_DATA);
    }
    m->frns = m->hdr->frn_count
        ? reinterpret_cast<const DiskFrn*>(m->view + m->hdr->frn_off) : nullptr;
    m->nfrn = static_cast<uint32_t>(m->hdr->frn_count);
    m->child_order = reinterpret_cast<const int32_t*>(m->view + m->hdr->child_order_off);
    m->name_order = m->hdr->name_order_off
        ? reinterpret_cast<const int32_t*>(m->view + m->hdr->name_order_off) : nullptr;
    m->size_order = m->hdr->size_order_off
        ? reinterpret_cast<const int32_t*>(m->view + m->hdr->size_order_off) : nullptr;
    m->mtime_order = m->hdr->mtime_order_off
        ? reinterpret_cast<const int32_t*>(m->view + m->hdr->mtime_order_off) : nullptr;
    for (uint32_t i = 0; i < m->n; ++i) {
        if (m->child_order[i] < 0 || m->child_order[i] >= static_cast<int32_t>(m->n)) return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
        if (m->name_order && (m->name_order[i] < 0 || m->name_order[i] >= static_cast<int32_t>(m->n)))
            return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
        if (m->size_order && (m->size_order[i] < 0 || m->size_order[i] >= static_cast<int32_t>(m->n)))
            return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
        if (m->mtime_order && (m->mtime_order[i] < 0 || m->mtime_order[i] >= static_cast<int32_t>(m->n)))
            return fail("filename_map_validation_failed", ERROR_INVALID_DATA);
    }
    out = std::move(m);
    return true;
}

void Engine::AdoptMappedLocked(std::unique_ptr<MappedFile> mapped) {
    ++feed_epoch_;
    // Old subscribers must already resynchronize; these events cannot bridge
    // the snapshot generation. Keep sequence IDs monotonic, not stale payloads.
    feed_changes_.Clear();
    pinyin_snapshot_ = nullptr;
    pinyin_version_ = 0;
    pinyin_chinese_ids_.clear();
    pinyin_pair_ids_.clear();
    query_shards_.clear();
    query_shards_ready_ = false;
    map_ = std::move(mapped);
    ++layout_epoch_;
    live_.Clear();
    live_.Shrink();
    tombstones_.clear();
    patches_.clear();
    vols_.clear();
    inactive_volume_roots_.clear();
    deleted_ = 0;
    pool_waste_ = 0;
    struct_changes_ = 0;
    subtree_intervals_valid_ = true;
    last_struct_tick_ = 0;
    built_unix_ = map_ && map_->hdr ? map_->hdr->built_unix : 0;
    if (map_) {
        vols_.resize(map_->nvol);
        for (uint32_t i = 0; i < map_->nvol; ++i) {
            const DiskVol& d = map_->vols[i];
            VolState& v = vols_[i];
            v.letter = static_cast<wchar_t>(d.letter);
            v.kind = static_cast<VolumeKind>(d.kind);
            v.item_count = d.item_count;
            v.volume_id = d.volume_id;
            v.journal_id = d.journal_id;
            v.next_usn = d.next_usn;
            v.root_idx = d.root_idx;
            v.first_idx = d.first_idx;
            if (d.frn_count && d.frn_off + d.frn_count * sizeof(DiskFrn) <= map_->size) {
                v.frn_base = reinterpret_cast<const DiskFrn*>(map_->view + d.frn_off);
                v.frn_base_n = d.frn_count;
            }
        }
    }
    RebuildChildMapLocked();
    indexed_.store(static_cast<size_t>(LiveCount()));
    RefreshQueryShardsLocked();
    InvalidateFilterLocked();
    RequestPinyinBuildLocked();
}

void Engine::RefreshQueryShardsLocked() {
    query_shards_.clear();
    query_shards_ready_ = false;
    if (!MachineIndexScope() || !map_ || vols_.empty()) return;
    const std::wstring root = DataDir() + L"\\v9\\volumes";
    std::vector<QueryShard> loaded;
    loaded.reserve(vols_.size());
    for (const auto& volume : vols_) {
        int32_t first = 0, last = 0;
        if (volume.volume_id.empty() || !VolumeSpan(volume, first, last)) return;
        const ShardPaths paths = MakeShardPaths(root, volume.volume_id);
        ShardManifest manifest;
        std::wstring active;
        if (!ResolveActiveShard(paths, manifest, active, nullptr) ||
            manifest.active_built != built_unix_) return;
        std::unique_ptr<MappedFile> mapped;
        if (!MapIndexFile(active, mapped) || !mapped || mapped->hdr->ver < kIndexVer ||
            mapped->nvol != 1 ||
            mapped->n != static_cast<uint32_t>(last - first) ||
            NormalizeVolumeId(mapped->vols[0].volume_id) != NormalizeVolumeId(volume.volume_id))
            return;
        QueryShard shard;
        shard.volume_id = volume.volume_id;
        shard.first = first;
        shard.last = last;
        shard.mapped = std::move(mapped);
        loaded.push_back(std::move(shard));
    }
    std::sort(loaded.begin(), loaded.end(), [](const QueryShard& a, const QueryShard& b) {
        return a.first < b.first;
    });
    query_shards_ = std::move(loaded);
    query_shards_ready_ = query_shards_.size() == vols_.size();
    if (query_shards_ready_) InvalidateFilterLocked();
}

const Engine::QueryShard* Engine::QueryShardForLocked(int32_t id) const {
    if (!query_shards_ready_ || id < 0 || id >= BaseCount()) return nullptr;
    for (const auto& shard : query_shards_)
        if (id >= shard.first && id < shard.last) return &shard;
    return nullptr;
}

Node Engine::QueryNodeAtLocked(int32_t id) const {
    if (patches_.contains(id)) return NodeAt(id);
    const QueryShard* shard = QueryShardForLocked(id);
    if (!shard) return NodeAt(id);
    Node node = shard->mapped->nodes[static_cast<size_t>(id - shard->first)];
    if (node.parent >= 0) node.parent += shard->first;
    if (node.unused) node.unused += static_cast<uint32_t>(shard->first);
    return node;
}

Attr Engine::QueryAttrAtLocked(int32_t id) const {
    if (patches_.contains(id)) return AttrAt(id);
    const QueryShard* shard = QueryShardForLocked(id);
    if (!shard) return AttrAt(id);
    return shard->mapped->attrs[static_cast<size_t>(id - shard->first)];
}

std::wstring_view Engine::QueryNameOfLocked(int32_t id) const {
    if (patches_.contains(id)) return NameOf(id);
    const QueryShard* shard = QueryShardForLocked(id);
    if (!shard) return NameOf(id);
    const Node& node = shard->mapped->nodes[static_cast<size_t>(id - shard->first)];
    return { shard->mapped->pool + node.off, node.len };
}

std::wstring Engine::BuildQueryPathLocked(int32_t id) const {
    return BuildCompleteIndexPath(id, LiveCount(),
        [&](int32_t current) { return QueryNodeAtLocked(current).parent; },
        [&](int32_t current) { return QueryNameOfLocked(current); });
}

bool Engine::MatchQueryNodeLocked(int32_t id, const CompiledQuery& query,
                                  int32_t prefix_node, bool folders_only,
                                  bool use_attrs) const {
    if (!query_shards_ready_ || id >= BaseCount() || patches_.contains(id))
        return MatchNodeLocked(id, query, prefix_node, folders_only, use_attrs);
    if (IsTomb(id)) return false;
    for (int32_t root : inactive_volume_roots_)
        if (InSubtreeLocked(id, root)) return false;
    const Node node = QueryNodeAtLocked(id);
    if (node.parent < 0) return false;
    if ((node.flags & kFlagHidden) || (folders_only && !(node.flags & kFlagDir))) return false;
    if (prefix_node >= 0 && !InSubtreeLocked(id, prefix_node)) return false;
    const std::wstring_view name = QueryNameOfLocked(id);
    const Attr attr = use_attrs ? QueryAttrAtLocked(id) : Attr{};
    for (const auto& group : query.groups) {
        bool matched = true;
        for (const auto& term : group) {
            bool name_ok = true;
            if (term.name_how != NameHow::Any) {
                if (!term.name_in_path) {
                    name_ok = MatchName(name.data(), static_cast<uint32_t>(name.size()), term);
                } else if (term.name_how == NameHow::Wildcard) {
                    const std::wstring path = BuildQueryPathLocked(id);
                    name_ok = WildcardFolded(path.data(), static_cast<uint32_t>(path.size()),
                                             term.name);
                } else {
                    name_ok = false;
                    ParentChainGuard chain(LiveCount());
                    for (int32_t current = id; chain.Visit(current);
                         current = QueryNodeAtLocked(current).parent) {
                        const std::wstring_view part = QueryNameOfLocked(current);
                        if (MatchName(part.data(), static_cast<uint32_t>(part.size()), term)) {
                            name_ok = true;
                            break;
                        }
                    }
                }
            }
            if (term.name_not) name_ok = !name_ok;
            bool ext_ok = MatchExt(name.data(), static_cast<uint32_t>(name.size()), term);
            if (term.ext_not) ext_ok = !ext_ok;
            bool size_ok = MatchSize(attr.size, term);
            if (term.size_not) size_ok = !size_ok;
            bool date_ok = MatchDate(UnixToFt(attr.mtime), term);
            if (term.date_not) date_ok = !date_ok;
            const bool is_dir = (node.flags & kFlagDir) != 0;
            if ((term.folder && !is_dir) || (term.file && is_dir) ||
                !name_ok || !ext_ok || !size_ok || !date_ok) {
                matched = false;
                break;
            }
        }
        if (matched) return true;
    }
    return query.groups.empty();
}

void Engine::PreserveOfflineVolumesLocked(const std::vector<VolumeInfo>& active,
                                          const IndexConfig& config) {
    const int32_t n = LiveCount();
    if (n <= 0) return;
    std::vector<int32_t> remap(static_cast<size_t>(n), -1);
    for (const auto& old : vols_) {
        if (old.kind != VolumeKind::Removable || config.IsExcluded(old.volume_id)) continue;
        const bool online = std::any_of(active.begin(), active.end(), [&](const VolumeInfo& volume) {
            return NormalizeVolumeId(volume.id) == NormalizeVolumeId(old.volume_id);
        });
        if (online || old.root_idx < 0) continue;
        std::fill(remap.begin(), remap.end(), -1);
        VolState kept = old;
        kept.frn_base = nullptr;
        kept.frn_base_n = 0;
        kept.frn_new.clear();
        kept.frn_build.clear();
        kept.item_count = 0;
        for (int32_t i = 0; i < n; ++i) {
            if (IsTomb(i) || !IsUnderLocked(i, old.root_idx)) continue;
            const Node source = NodeAt(i);
            if (source.parent >= 0 && remap[static_cast<size_t>(source.parent)] < 0) continue;
            Node node = source;
            const std::wstring_view name = NameOf(i);
            node.parent = source.parent < 0 ? -1 : remap[static_cast<size_t>(source.parent)];
            node.off = static_cast<uint32_t>(build_.pool.size());
            node.len = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
            node.pad = 0;
            node.unused = 0;
            const bool excluded_parent = source.parent >= 0 && source.parent != old.root_idx &&
                (build_.nodes[static_cast<size_t>(node.parent)].flags & kFlagHidden) != 0;
            node.flags &= static_cast<uint8_t>(~kFlagHidden);
            if (i == old.root_idx || excluded_parent || ShouldSkipName(name) ||
                IsExcludedPath(BuildPathLocked(i))) node.flags |= kFlagHidden;
            build_.pool.insert(build_.pool.end(), name.begin(), name.begin() + node.len);
            remap[static_cast<size_t>(i)] = static_cast<int32_t>(build_.nodes.size());
            build_.nodes.push_back(node);
            build_.attrs.push_back(AttrAt(i));
            ++kept.item_count;
        }
        if (old.root_idx >= n || remap[static_cast<size_t>(old.root_idx)] < 0) continue;
        kept.root_idx = remap[static_cast<size_t>(old.root_idx)];
        auto keep_frn = [&](uint64_t frn, int32_t idx) {
            if (idx >= 0 && idx < n && remap[static_cast<size_t>(idx)] >= 0)
                kept.frn_build.emplace_back(frn, remap[static_cast<size_t>(idx)]);
        };
        for (uint32_t i = 0; i < old.frn_base_n; ++i)
            keep_frn(old.frn_base[i].frn, old.frn_base[i].idx);
        for (const auto& entry : old.frn_new) keep_frn(entry.frn, entry.idx);
        for (const auto& entry : old.frn_build) keep_frn(entry.first, entry.second);
        build_vols_.push_back(std::move(kept));
    }
}

bool Engine::FlattenLocked(Store& out, std::vector<VolState>& vols_out) const {
    const int32_t n = LiveCount();
    std::vector<int32_t> remap(static_cast<size_t>(n), -1);
    out.Clear();
    out.nodes.reserve(static_cast<size_t>(n));
    out.attrs.reserve(static_cast<size_t>(n));
    out.pool.reserve(static_cast<size_t>(n) * 12);

    std::vector<uint32_t> child_count(static_cast<size_t>(n), 0);
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
        const int32_t p = NodeAt(i).parent;
        if (p >= 0 && p < n) ++child_count[static_cast<size_t>(p)];
    }
    std::vector<uint32_t> head(static_cast<size_t>(n) + 1, 0);
    for (int32_t i = 0; i < n; ++i) head[static_cast<size_t>(i) + 1] = head[static_cast<size_t>(i)] +
        child_count[static_cast<size_t>(i)];
    std::vector<int32_t> kids(head[static_cast<size_t>(n)]);
    std::vector<uint32_t> fill = head;
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
        const int32_t p = NodeAt(i).parent;
        if (p >= 0 && p < n) kids[fill[static_cast<size_t>(p)]++] = i;
    }
    auto name_of = [&](int32_t id) { return NameOf(id); };
    for (int32_t p = 0; p < n; ++p) {
        const uint32_t a = head[static_cast<size_t>(p)];
        const uint32_t b = head[static_cast<size_t>(p) + 1];
        if (b - a <= 1) continue;
        std::sort(kids.begin() + a, kids.begin() + b, [&](int32_t x, int32_t y) {
            const int c = CompareFolded(name_of(x), name_of(y));
            return c != 0 ? c < 0 : x < y;
        });
    }

    auto emit = [&](auto&& self, int32_t src) -> int32_t {
        if (src < 0 || src >= n || remap[static_cast<size_t>(src)] >= 0 || IsTomb(src)) return -1;
        const Node node = NodeAt(src);
        const std::wstring_view name = NameOf(src);
        Node nn = node;
        nn.parent = node.parent < 0 ? -1 : remap[static_cast<size_t>(node.parent)];
        nn.off = static_cast<uint32_t>(out.pool.size());
        nn.len = static_cast<uint16_t>(name.size());
        nn.pad = 0;
        nn.unused = 0;
        out.pool.insert(out.pool.end(), name.begin(), name.end());
        const int32_t dst = static_cast<int32_t>(out.nodes.size());
        remap[static_cast<size_t>(src)] = dst;
        out.nodes.push_back(nn);
        out.attrs.push_back(AttrAt(src));
        const uint32_t a = head[static_cast<size_t>(src)];
        const uint32_t b = head[static_cast<size_t>(src) + 1];
        for (uint32_t k = a; k < b; ++k) self(self, kids[k]);
        out.nodes[static_cast<size_t>(dst)].unused = static_cast<uint32_t>(out.nodes.size());
        return dst;
    };

    std::vector<int32_t> roots;
    for (const auto& v : vols_)
        if (v.root_idx >= 0) roots.push_back(v.root_idx);
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
        if (NodeAt(i).parent < 0 &&
            std::find(roots.begin(), roots.end(), i) == roots.end())
            roots.push_back(i);
    }
    for (int32_t r : roots) emit(emit, r);
    for (int32_t i = 0; i < n; ++i)
        if (remap[static_cast<size_t>(i)] < 0) emit(emit, i);

    vols_out = vols_;
    for (auto& v : vols_out) {
        if (v.root_idx >= 0 && v.root_idx < n && remap[static_cast<size_t>(v.root_idx)] >= 0) {
            v.root_idx = remap[static_cast<size_t>(v.root_idx)];
            v.first_idx = v.root_idx;
            const uint32_t end = out.nodes[static_cast<size_t>(v.root_idx)].unused;
            v.item_count = end > static_cast<uint32_t>(v.root_idx)
                ? end - static_cast<uint32_t>(v.root_idx) : 0;
        }
        const auto previous_build = std::move(v.frn_build);
        v.frn_build.clear();
        auto push = [&](uint64_t frn, int32_t idx) {
            if (idx >= 0 && idx < n && remap[static_cast<size_t>(idx)] >= 0)
                v.frn_build.emplace_back(frn, remap[static_cast<size_t>(idx)]);
        };
        for (uint32_t k = 0; k < v.frn_base_n; ++k)
            push(v.frn_base[k].frn, v.frn_base[k].idx);
        for (const auto& e : v.frn_new) push(e.frn, e.idx);
        for (const auto& e : previous_build) push(e.first, e.second);
        std::sort(v.frn_build.begin(), v.frn_build.end());
        v.frn_build.erase(std::unique(v.frn_build.begin(), v.frn_build.end()), v.frn_build.end());
        v.frn_base = nullptr;
        v.frn_base_n = 0;
        v.frn_new.clear();
    }
    return true;
}

void Engine::SaveCache() {
    MergeBase(true);
}

DeltaLog* Engine::DeltaFor(wchar_t letter) {
    wchar_t key = letter ? static_cast<wchar_t>(towupper(letter)) : 0;
    auto it = delta_logs_.find(key);
    return it == delta_logs_.end() ? nullptr : it->second.get();
}

void Engine::CloseDeltas() {
    for (auto& [k, log] : delta_logs_)
        if (log) log->Close();
    delta_logs_.clear();
}

void Engine::FlushDeltas() {
    const auto timing = FilenameTiming::Begin();
    bool saved = true;
    for (auto& [k, log] : delta_logs_)
        if (log && !log->Flush()) saved = false;
    if (!saved) SetStatus(L"索引更新暂时无法保存，正在等待重试；请检查磁盘空间和权限");
    last_delta_flush_tick_ = GetTickCount64();
    filename_timing_.End(FilenameStage::DeltaFlush, timing, 0,
        saved ? ERROR_SUCCESS : ERROR_WRITE_FAULT, saved ? "scheduled" : "write_failed");
}

void Engine::OpenDeltasLocked() {
    CloseDeltas();
    if (vols_.empty()) {
        auto log = std::make_unique<DeltaLog>();
        const std::wstring path = DeltaFilePath(0);
        if (path.empty()) return;
        if (!log->Open(path, built_unix_)) {
            DeleteFileW(path.c_str());
            if (!log->Open(path, built_unix_)) return;
        }
        delta_logs_[0] = std::move(log);
        return;
    }
    for (const auto& v : vols_) {
        // Key the log file by the stable volume id, not the drive letter:
        // a remounted or replaced volume reusing a letter must never replay
        // or append into another volume's delta.
        std::wstring path = DeltaFilePathForVolume(v.volume_id);
        if (path.empty()) path = DeltaFilePath(v.letter);
        if (path.empty()) continue;
        // Retire the legacy letter-keyed file once; it can only alias.
        const std::wstring legacy = DeltaFilePath(v.letter);
        if (!legacy.empty() && legacy != path) DeleteFileW(legacy.c_str());
        auto log = std::make_unique<DeltaLog>();
        if (!log->Open(path, built_unix_)) {
            DeleteFileW(path.c_str());
            log->Open(path, built_unix_);
        }
        delta_logs_[static_cast<wchar_t>(towupper(v.letter))] = std::move(log);
    }
}

bool Engine::ReplayDeltasLocked() {
    struct Addition {
        std::wstring volume, name;
        int32_t idx, parent;
        uint8_t flags;
        uint32_t mtime;
        uint64_t size, frn;
    };
    std::vector<std::pair<std::wstring, std::wstring>> logs;
    if (vols_.empty()) logs.emplace_back(L"", DeltaFilePath(0));
    else for (const auto& volume : vols_) {
        auto path = DeltaFilePathForVolume(volume.volume_id);
        if (path.empty()) path = DeltaFilePath(volume.letter);
        logs.emplace_back(volume.volume_id, std::move(path));
    }
    std::vector<Addition> additions;
    bool valid = true;
    int32_t greatest_reference = -1;
    // IDs are global, while WALs are per volume. Validate every log before
    // changing the snapshot, then restore additions in their original ID order.
    // Version 1 omitted these IDs and cannot safely reconstruct interleaving.
    for (const auto& [volume, path] : logs) {
        if (!DeltaLog::Replay(path, built_unix_,
            [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                uint64_t, int64_t) {
                if (op == DeltaOp::Add) {
                    valid &= idx >= LiveCount() && parent >= 0 && parent < idx;
                    additions.push_back({volume, std::wstring(name), idx, parent, flags, mtime, size, frn});
                } else if (op == DeltaOp::Patch || op == DeltaOp::Tomb) {
                    valid &= idx >= 0;
                    greatest_reference = (std::max)(greatest_reference, idx);
                    if (op == DeltaOp::Patch && (which & static_cast<uint8_t>(PatchBits::Meta))) {
                        valid &= parent >= -1;
                        greatest_reference = (std::max)(greatest_reference, parent);
                    }
                }
            })) return false;
    }
    if (!valid || static_cast<size_t>(LiveCount()) > kIndexCap + 64 ||
        additions.size() > kIndexCap + 64 - static_cast<size_t>(LiveCount())) return false;
    std::sort(additions.begin(), additions.end(), [](const Addition& a, const Addition& b) { return a.idx < b.idx; });
    const int32_t original_count = LiveCount();
    for (size_t i = 0; i < additions.size(); ++i)
        if (additions[i].idx != original_count + static_cast<int32_t>(i)) return false;
    if (greatest_reference >= original_count + static_cast<int32_t>(additions.size())) return false;
    for (const auto& add : additions) {
        VolState* volume = nullptr;
        for (auto& state : vols_) if (NormalizeVolumeId(state.volume_id) == NormalizeVolumeId(add.volume)) {
            volume = &state; break;
        }
        AddNodeLocked(live_, add.parent, add.name, add.flags, add.frn, add.size, UnixToFt(add.mtime), true, volume);
    }
    std::unordered_map<int32_t, size_t> owners;
    for (size_t i = 0; i < vols_.size(); ++i) owners.emplace(vols_[i].root_idx, i);
    for (const auto& add : additions) {
        for (size_t i = 0; i < vols_.size(); ++i)
            if (NormalizeVolumeId(vols_[i].volume_id) == NormalizeVolumeId(add.volume)) owners.emplace(add.idx, i);
    }
    auto owner_of = [&](int32_t idx) {
        std::vector<int32_t> ancestors;
        ParentChainGuard chain(LiveCount());
        while (chain.Visit(idx)) {
            const auto found = owners.find(idx);
            if (found != owners.end()) {
                const auto owner = found->second;
                for (int32_t ancestor : ancestors) owners.emplace(ancestor, owner);
                return owner;
            }
            ancestors.push_back(idx);
            idx = NodeAt(idx).parent;
        }
        return SIZE_MAX;
    };
    if (!vols_.empty()) for (const auto& add : additions)
        if (owner_of(add.idx) == SIZE_MAX || owner_of(add.parent) != owner_of(add.idx)) return false;
    auto apply = [&](const std::wstring& vid, DeltaOp op, int32_t idx, int32_t parent,
                     uint8_t flags, uint8_t which, uint32_t mtime, uint64_t size,
                     std::wstring_view name, uint64_t, uint64_t journal_id,
                     int64_t next_usn) {
        // Match by stable volume id: a letter reused across sessions must not
        // pull another volume's records into this one.
        VolState* vol = nullptr;
        for (auto& v : vols_)
            if (vid.empty() || NormalizeVolumeId(v.volume_id) == NormalizeVolumeId(vid)) {
                vol = &v;
                break;
            }
        if (!valid) return;
        if (!vols_.empty() && (op == DeltaOp::Patch || op == DeltaOp::Tomb)) {
            const auto owner = vol ? static_cast<size_t>(vol - vols_.data()) : SIZE_MAX;
            if (owner == SIZE_MAX || owner_of(idx) != owner ||
                (op == DeltaOp::Patch && (which & static_cast<uint8_t>(PatchBits::Meta)) &&
                 (parent < 0 ? idx != vol->root_idx : owner_of(parent) != owner))) {
                valid = false;
                return;
            }
        }
        if (op == DeltaOp::UsnCkpt) {
            if (vol) {
                vol->journal_id = journal_id;
                vol->next_usn = next_usn;
            }
            return;
        }
        if (op == DeltaOp::Tomb) {
            if (idx < 0 || idx >= LiveCount() || IsTomb(idx)) return;
            ChildMapRemove(NodeAt(idx).parent, NameOf(idx), idx);
            tombstones_.insert(idx);
            ++deleted_;
            return;
        }
        if (op == DeltaOp::Add) {
            return;
        }
        if (op == DeltaOp::Patch && idx >= 0) {
            if (idx >= LiveCount()) return;
            Patch& p = patches_[idx];
            if (which & static_cast<uint8_t>(PatchBits::Meta)) {
                if (NodeAt(idx).parent != parent) subtree_intervals_valid_ = false;
                p.parent = parent;
                p.flags = flags;
                p.has_meta = true;
            }
            if (which & static_cast<uint8_t>(PatchBits::Attr)) {
                p.attr.mtime = mtime;
                p.attr.size = size;
                p.has_attr = true;
                // Attribute-only records preserve the existing parent and flags.
            }
            if (which & static_cast<uint8_t>(PatchBits::Name)) {
                if (!name.data() && !name.empty()) return;
                if (p.has_name) pool_waste_ += p.len;
                p.off = static_cast<uint32_t>(live_.pool.size());
                p.len = static_cast<uint16_t>(name.size());
                live_.pool.insert(live_.pool.end(), name.begin(), name.end());
                p.has_name = true;
            }
        }
    };
    for (const auto& [volume, path] : logs) {
        if (!DeltaLog::Replay(path, built_unix_,
                [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                    uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                    uint64_t journal_id, int64_t next_usn) {
                    apply(volume, op, idx, parent, flags, which, mtime, size, name, frn,
                          journal_id, next_usn);
                }) || !valid) {
            return false;
        }
    }
    HierarchyIssue hierarchy_issue;
    if (!ValidateIndexHierarchy(LiveCount(), [this](int32_t id) {
            auto node = NodeAt(id);
            if (IsTomb(id)) node.flags |= kFlagDeleted;
            return node;
        }, &hierarchy_issue)) {
        diagnostics::runtime::Event("index_replay_invalid_hierarchy", {{"operation", index_operation},
            {"reason", static_cast<uint64_t>(hierarchy_issue.error)},
            {"node", static_cast<uint32_t>(hierarchy_issue.node)},
            {"parent", static_cast<uint32_t>(hierarchy_issue.parent)},
            {"nodes", static_cast<uint64_t>(LiveCount())}}, diagnostics::runtime::Level::Error);
        SetLastError(ERROR_INVALID_DATA);
        return false;
    }
    RebuildChildMapLocked();
    indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
    InvalidateFilterLocked();
    return true;
}

bool Engine::CompactNamePoolLocked(ULONGLONG now) {
    // Amortize copying: at least 2 MiB of garbage, at least 25% of the pool,
    // and no more than one attempt per minute (including allocation failures).
    if (now < name_pool_retry_after_tick_ || pool_waste_ < kNamePoolWasteChars ||
        pool_waste_ < (live_.pool.size() + 3) / 4) return false;
    const auto timing = FilenameTiming::Begin();
    const bool diagnostics = IndexDiagnosticsEnabled();
    auto& stats = filename_timing_.Maintenance().pool;
    if (diagnostics) {
        ++stats.attempts;
        stats.tick = now;
        stats.waste_chars = pool_waste_;
        stats.before_chars = live_.pool.size();
        stats.before_capacity_bytes = live_.pool.capacity() * sizeof(wchar_t);
    }
    const DWORD error = CompactOverlayNamePool(live_.pool, live_.nodes, patches_);
    if (error == ERROR_SUCCESS) pool_waste_ = 0;
    name_pool_retry_after_tick_ = GetTickCount64() + kNamePoolIntervalMs;
    if (diagnostics) {
        stats.after_chars = live_.pool.size();
        stats.after_capacity_bytes = live_.pool.capacity() * sizeof(wchar_t);
        if (error == ERROR_SUCCESS) {
            ++stats.successes;
            stats.reclaimed_chars += stats.before_chars - stats.after_chars;
        } else {
            ++stats.failures;
        }
        filename_timing_.End(FilenameStage::NamePoolCompact, timing,
            error == ERROR_SUCCESS ? stats.before_chars - stats.after_chars : 0,
            error, "overlay_name_waste");
    }
    return error == ERROR_SUCCESS;
}

const char* Engine::MaintenanceMergeReason(ULONGLONG now, uint64_t delta_bytes) const {
    if (now < merge_retry_after_tick_) return nullptr;
    const auto count = static_cast<size_t>(LiveCount());
    if (count && deleted_ > count / 10) return "deleted_ratio";
    if (struct_changes_ >= kMergeStructChanges) return "structural_threshold";
    if (delta_bytes >= kMergeDeltaBytes) return "delta_threshold";
    if (struct_changes_ && last_struct_tick_ && now - last_struct_tick_ >= kIdleMergeQuietMs &&
        now - last_merge_tick_ >= kMinMergeIntervalMs) return "quiet_changes";
    return nullptr;
}

void Engine::MergeBase(bool force, const char* reason) {
    if (merging_.exchange(true)) return;
    const ULONGLONG now = GetTickCount64();
    // A failed forced compaction must not rewrite the full base for every
    // notification. Retain the pending changes and retry off the hot path.
    if (now < merge_retry_after_tick_) {
        merging_ = false;
        return;
    }
    if (!force && last_merge_tick_ && now - last_merge_tick_ < kMinMergeIntervalMs) {
        merging_ = false;
        return;
    }
    const std::wstring path = CachePath();
    if (path.empty()) {
        merging_ = false;
        return;
    }
    const uint64_t operation = diagnostics::runtime::NextId();
    const IndexOperationScope operation_scope(operation);
    const ULONGLONG merge_started = GetTickCount64();
    Store snap;
    const auto timing = FilenameTiming::Begin();
    const auto pending = struct_changes_;
    const auto previous_built = built_unix_;
    filename_timing_.Memory().Capture(IndexMemoryPoint::MergeBefore);
    std::vector<VolState> vols;
    uint64_t built = static_cast<uint64_t>(std::time(nullptr));
    TraceSearch("filename_merge_begin", revision_.load());
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (LiveCount() == 0) {
            TraceSearch("filename_merge_empty", revision_.load());
            filename_timing_.End(FilenameStage::Merge, timing, 0, ERROR_SUCCESS, "empty");
            merging_ = false;
            return;
        }
        if (IndexDiagnosticsEnabled()) {
            auto& stats = filename_timing_.Maintenance();
            stats.RecordMerge(reason);
            stats.merge.tick = now;
            stats.merge.since_previous_ms = last_merge_tick_ ? now - last_merge_tick_ : 0;
            stats.merge.nodes = static_cast<uint64_t>(LiveCount());
            stats.merge.deleted = deleted_;
            stats.merge.pool_chars = live_.pool.size();
            stats.merge.pool_waste_chars = pool_waste_;
            stats.merge.base_pool_chars = map_ && map_->hdr ? map_->hdr->pool_chars : 0;
            stats.merge.struct_changes = struct_changes_;
            stats.merge.delta_bytes = 0;
            for (const auto& entry : delta_logs_)
                if (entry.second) stats.merge.delta_bytes += entry.second->BytesOnDisk();
        }
        FlattenLocked(snap, vols);
        filename_timing_.Memory().Capture(IndexMemoryPoint::MergeFlattened);
        built_unix_ = built;
    }
    const bool wrote = WriteIndexFile(path, snap, vols, built);
    DWORD save_error = wrote ? ERROR_SUCCESS : GetLastError();
    bool committed = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (wrote && CommitMappedFile(path)) {
            committed = true;
            struct_changes_ = 0;
            last_merge_tick_ = GetTickCount64();
            // OpenDeltasLocked resets every log whose header predates the new
            // base (delete + recreate at the canonical per-volume path), so
            // no separate truncate pass is needed.
            OpenDeltasLocked();
        } else {
            if (wrote) save_error = GetLastError();
            built_unix_ = previous_built;
        }
    }
    if (committed && MachineIndexScope()) {
        WriteVolumeShards(snap, vols, built);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        RefreshQueryShardsLocked();
    }
    merge_failures_ = committed ? 0 : merge_failures_ + 1;
    const ULONGLONG retry_ms = MergeRetryDelayMs(merge_failures_);
    merge_retry_after_tick_ = committed ? 0 : GetTickCount64() + retry_ms;
    const DWORD merge_error = committed ? ERROR_SUCCESS : (save_error ? save_error : ERROR_WRITE_FAULT);
    TraceSearch(committed ? "filename_merge_done" : "filename_merge_failed", revision_.load());
    // Terminal record for every attempted merge; repeats aggregate in the
    // critical sidecar because only the error code splits the pattern.
    diagnostics::runtime::Event("index_merge_end", {{"operation", operation},
        {"trigger", MergeTriggerCode(reason)}, {"outcome", committed ? 0ull : 1ull},
        {"error", merge_error}, {"pending", pending}, {"elapsed_ms", GetTickCount64() - merge_started},
        {"failures", merge_failures_}, {"retry_ms", retry_ms}},
        committed ? diagnostics::runtime::Level::Info : diagnostics::runtime::Level::Error);
    filename_timing_.End(FilenameStage::Merge, timing, pending, merge_error, reason);
    if (IndexDiagnosticsEnabled()) {
        auto& merge_stats = filename_timing_.Maintenance().merge;
        merge_stats.last_wall_us = FilenameTiming::Begin().wall - timing.wall;
        merge_stats.last_committed = committed;
    }
    CaptureMemoryState();
    filename_timing_.Flush();
    merging_ = false;
}

bool Engine::TryLoadCache() {
    const auto load_started = GetTickCount64();
    const std::wstring path = CachePath();
    if (path.empty()) return false;
    std::unique_ptr<MappedFile> mapped;
    if (!MapIndexFile(path, mapped)) {
        // V8 compatibility: import the old monolithic cache into the first
        // V9 slot only after it has passed the normal mmap validation.
        if (!MachineIndexScope()) {
            DeleteFileW(path.c_str());
            return false;
        }
        const std::wstring legacy = CacheFilePath();
        if (legacy.empty() || !MapIndexFile(legacy, mapped)) return false;
        const ShardPaths paths = AggregateShardPaths();
        const std::wstring temp = paths.base_a + L".tmp";
        DeleteFileW(temp.c_str());
        if (!CopyFileW(legacy.c_str(), temp.c_str(), FALSE)) return false;
        ShardManifest published;
        if (!PublishShardBase(paths, temp, static_cast<uint64_t>(std::time(nullptr)), 0,
                               published, nullptr)) return false;
        mapped.reset();
        const std::wstring active = published.active_slot == 0 ? paths.base_a : paths.base_b;
        if (!MapIndexFile(active, mapped)) return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    AdoptMappedLocked(std::move(mapped));
    if (!ReplayDeltasLocked()) {
        diagnostics::runtime::Event("index_replay_rejected", {{"nodes", static_cast<uint64_t>(LiveCount())},
            {"elapsed_ms", GetTickCount64() - load_started}, {"rebuild_required", 1}}, diagnostics::runtime::Level::Error);
        // A rejected/partial replay must not be exposed while Worker rebuilds.
        AdoptMappedLocked(nullptr);
        ready_ = false;
        folder_size_gap_ = true;
        SetStatus(L"索引增量记录不兼容或已损坏，正在重建…");
        return false;
    }
    OpenDeltasLocked();
    SetStatus(L"已加载 " + std::to_wstring(indexed_.load()) + L" 项");
    ready_ = true;
    diagnostics::runtime::Event("index_cache_loaded", {{"nodes", indexed_.load()},
        {"volumes", vols_.size()}, {"elapsed_ms", GetTickCount64() - load_started}});
    return true;
}

void Engine::CompactLocked() {
    Store neu;
    std::vector<VolState> vols;
    FlattenLocked(neu, vols);
    if (map_) {
        map_->Close();
        map_.reset();
    }
    live_ = std::move(neu);
    ++layout_epoch_;
    vols_ = std::move(vols);
    tombstones_.clear();
    patches_.clear();
    deleted_ = 0;
    pool_waste_ = 0;
    RebuildChildMapLocked();
    indexed_.store(live_.nodes.size());
    InvalidateFilterLocked();
}

int32_t Engine::FindByFrnLocked(const VolState& v, uint64_t frn) const {
    for (auto it = v.frn_new.rbegin(); it != v.frn_new.rend(); ++it)
        if (it->frn == frn) return it->idx;
    if (v.frn_base && v.frn_base_n) {
        const DiskFrn* b = v.frn_base;
        const DiskFrn* e = v.frn_base + v.frn_base_n;
        auto it = std::lower_bound(b, e, frn, [](const DiskFrn& a, uint64_t k) { return a.frn < k; });
        if (it != e && it->frn == frn) return it->idx;
    }
    for (const auto& p : v.frn_build)
        if (p.first == frn) return p.second;
    return -1;
}

void Engine::CollectFrnLocked(const VolState& v, uint64_t frn, std::vector<int32_t>& out) const {
    out.clear();
    const int32_t live = LiveCount();
    auto add = [&](int32_t idx) {
        if (idx < 0 || idx >= live || IsTomb(idx)) return;
        if (std::find(out.begin(), out.end(), idx) == out.end()) out.push_back(idx);
    };
    for (const auto& entry : v.frn_new)
        if (entry.frn == frn) add(entry.idx);
    if (v.frn_base && v.frn_base_n) {
        const DiskFrn* b = v.frn_base;
        const DiskFrn* e = v.frn_base + v.frn_base_n;
        for (auto it = std::lower_bound(b, e, frn, [](const DiskFrn& a, uint64_t k) { return a.frn < k; });
             it != e && it->frn == frn; ++it)
            add(it->idx);
    }
    for (const auto& entry : v.frn_build)
        if (entry.first == frn) add(entry.second);
}

void Engine::MapFrnLocked(VolState& v, uint64_t frn, int32_t idx) {
    v.frn_new.push_back(DiskFrn{ frn, idx, 0 });
    if (v.frn_build.size() || (!v.frn_base && v.frn_new.size() < kFrnMergeThreshold)) {
        v.frn_build.emplace_back(frn, idx);
    }
}

void Engine::ResolveIndexDirFrn() {
    const std::wstring dir = DataDir();
    index_directory_ = NormalizeChangePath(dir);
    index_dir_frn_ = FileIndexFrn(dir);
    index_dir_letter_ = 0;
    if (dir.size() >= 2 && dir[1] == L':')
        index_dir_letter_ = static_cast<wchar_t>(towupper(dir[0]));
}

bool Engine::IsIndexNoiseLocked(const VolState& v, const USN_RECORD_V2* rec) const {
    std::wstring_view name(
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset),
        rec->FileNameLength / sizeof(WCHAR));
    if (IsIndexArtifactName(name)) return true;
    if (index_dir_frn_ && v.letter == index_dir_letter_) {
        if (rec->FileReferenceNumber == index_dir_frn_ ||
            rec->ParentFileReferenceNumber == index_dir_frn_)
            return true;
    }
    return false;
}

Engine::UsnApply Engine::ApplyUsnLocked(VolState& v, const USN_RECORD_V2* rec) {
    if (!running_) return UsnApply::None;
    if (IsIndexNoiseLocked(v, rec)) return UsnApply::None;
    std::wstring_view name(
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset),
        rec->FileNameLength / sizeof(WCHAR));
    if (name.empty()) return UsnApply::None;
    DWORD reason = rec->Reason;
    const bool is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    constexpr DWORD kAttrReasons = USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
                                   USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE;
    // The last link going away is FILE_DELETE; adding or removing any other
    // link is HARD_LINK_CHANGE naming that link.
    const bool delete_all = (reason & USN_REASON_FILE_DELETE) != 0;
    const bool link_change = !is_dir && !delete_all && (reason & USN_REASON_HARD_LINK_CHANGE) != 0;
    bool structural_reason = (reason & (USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                                        USN_REASON_RENAME_NEW_NAME)) != 0;
    const bool attr_reason = (reason & kAttrReasons) != 0;
    if (!structural_reason && !attr_reason && !link_change) return UsnApply::None;

    const uint64_t frn = rec->FileReferenceNumber;
    int32_t idx = FindByFrnLocked(v, frn);
    // Files keep one node per hard link, so pick the node this record names.
    std::vector<int32_t> links;
    if (!is_dir) {
        CollectFrnLocked(v, frn, links);
        if (!links.empty() && (idx < 0 || IsTomb(idx) ||
                               std::find(links.begin(), links.end(), idx) == links.end()))
            idx = links.front();
    }
    if (link_change || (links.size() > 1 && !delete_all &&
                        (reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME)))) {
        const int32_t link_parent = FindByFrnLocked(v, rec->ParentFileReferenceNumber);
        int32_t named = -1;
        if (link_parent >= 0) {
            for (const int32_t i : links) {
                if (NodeAt(i).parent == link_parent && EqualsI(NameOf(i), name)) { named = i; break; }
            }
        }
        if (link_change) {
            if (link_parent < 0) return UsnApply::None;
            std::wstring link_path = BuildPathLocked(link_parent);
            if (link_path.empty()) return UsnApply::None;
            if (link_path.back() != L'\\') link_path += L'\\';
            link_path += name;
            const bool exists = FileFrnAt(link_path) == frn;
            if (exists && named < 0) {
                idx = -1;                                  // a new name appeared
                reason = USN_REASON_FILE_CREATE | (reason & kAttrReasons);
                structural_reason = true;
            } else if (!exists && named >= 0) {
                idx = named;                               // this name went away
                reason = USN_REASON_FILE_DELETE;
                structural_reason = true;
            } else if (exists && attr_reason) {
                idx = named;
            } else {
                return UsnApply::None;
            }
        } else if (named >= 0) {
            idx = named;
        } else {
            // A rename moves exactly one link: the one whose path is gone.
            idx = -1;
            for (const int32_t i : links) {
                if (FileFrnAt(BuildPathLocked(i)) != frn) { idx = i; break; }
            }
        }
    }
    const auto size_before = FolderSizeItem(idx);
    folder_size_usn_update_ = true;
    struct SizeUpdate {
        std::function<void()> finish;
        ~SizeUpdate() { finish(); }
    } size_update{[&] {
        folder_size_usn_update_ = false;
        ++folder_size_mutations_;
        if (idx >= 0) folder_sizes_.Replace(idx, size_before, FolderSizeItem(idx),
            [this](int32_t id) { return FolderSizeItem(id); });
    }};
    // NTFS reports the volume root with its own FRN as parent.
    if (idx == v.root_idx) return UsnApply::None;
    const auto tracked_path = v.tracking_paths.find(frn);
    const bool had_tracking_path = tracked_path != v.tracking_paths.end();
    const std::wstring previous_path = had_tracking_path ? tracked_path->second :
        (idx >= 0 ? BuildPathLocked(idx) : std::wstring());
    const bool previous_visible = idx >= 0 && !(NodeAt(idx).flags & kFlagHidden) && !IsExcludedPath(previous_path);
    std::wstring resolved_parent;
    if ((reason & USN_REASON_RENAME_NEW_NAME) && FindByFrnLocked(v, rec->ParentFileReferenceNumber) < 0) {
        // Resolve excluded parents by FRN; never infer recycle status from $R names.
        HANDLE volume = OpenVolume(v.letter);
        if (volume != INVALID_HANDLE_VALUE) {
            FILE_ID_DESCRIPTOR id{}; id.dwSize = sizeof(id); id.Type = FileIdType;
            id.FileId.QuadPart = rec->ParentFileReferenceNumber;
            HANDLE directory = OpenFileById(volume, &id, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, FILE_FLAG_BACKUP_SEMANTICS);
            if (directory != INVALID_HANDLE_VALUE) {
                std::wstring buffer(32768, L'\0');
                const DWORD count = GetFinalPathNameByHandleW(directory, buffer.data(), static_cast<DWORD>(buffer.size()), FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
                if (count && count < buffer.size()) { buffer.resize(count); resolved_parent = std::move(buffer); }
                CloseHandle(directory);
            }
            CloseHandle(volume);
        }
    }
    auto track = [&](ChangeKind kind, int32_t target) {
        if (kind == ChangeKind::Renamed && resolved_parent.empty() &&
            FindByFrnLocked(v, rec->ParentFileReferenceNumber) < 0) {
            GapFeed(); return;
        }
        ChangeRecord event;
        event.time = FtToUnix(static_cast<uint64_t>(rec->TimeStamp.QuadPart));
        event.kind = kind; event.is_dir = is_dir;
        event.file_id = frn ^ (static_cast<uint64_t>(v.letter) << 56);
        event.path = target >= 0 ? BuildPathLocked(target) : previous_path;
        if (kind == ChangeKind::Renamed) {
            event.old_path = previous_path;
            if (!resolved_parent.empty()) {
                event.path = resolved_parent + L"\\" + std::wstring(name);
                if (!v.tracking_paths.contains(frn) && v.tracking_paths.size() >= 1024) {
                    v.tracking_paths.erase(v.tracking_paths.begin()); GapFeed();
                }
                v.tracking_paths[frn] = event.path;
            } else v.tracking_paths.erase(frn);
        }
        if (kind != ChangeKind::Renamed && had_tracking_path) event.path = previous_path;
        if (event.path.empty()) { GapFeed(); return; }
        const bool visible = target >= 0 && !(NodeAt(target).flags & kFlagHidden) && !IsExcludedPath(event.path);
        if (!visible && !previous_visible) return;
        if (kind == ChangeKind::Renamed && !previous_visible && !had_tracking_path) {
            event.old_path.clear(); event.kind = ChangeKind::Created;
        }
        RecordFeed(event); changes_.Record(std::move(event));
    };
    DeltaLog* delta = DeltaFor(v.letter);
    UsnApply effect = UsnApply::None;

    auto refresh = [&](int32_t i) {
        std::wstring path = BuildPathLocked(i);
        if (path.empty()) return;
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
        Attr a;
        a.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        a.mtime = FtToUnix((static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                           fad.ftLastWriteTime.dwLowDateTime);
        const Attr previous = AttrAt(i);
        if (a.size == previous.size && a.mtime == previous.mtime &&
            !(reason & (USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION | USN_REASON_DATA_OVERWRITE))) return;
        const int32_t base = BaseCount();
        if (i >= base) live_.attrs[static_cast<size_t>(i - base)] = a;
        else {
            Patch& p = patches_[i];
            if (!p.has_meta) {
                Node n = NodeAt(i);
                p.parent = n.parent;
                p.flags = n.flags;
                p.has_meta = true;
            }
            p.has_attr = true;
            p.attr = a;
        }
        if (delta) delta->QueuePatch(i, static_cast<uint8_t>(PatchBits::Attr), 0, 0, a.mtime, a.size, {});
        InvalidateFilterLocked();
        if (effect == UsnApply::None) effect = UsnApply::Attr;
    };

    // Other hard link names of the same file share its data and lifetime.
    auto other_link = [&](int32_t i, bool remove) {
        if (i == idx || i < 0 || i >= LiveCount() || IsTomb(i)) return;
        const auto before = FolderSizeItem(i);
        if (remove) {
            const std::wstring path = BuildPathLocked(i);
            if (!path.empty() && !(NodeAt(i).flags & kFlagHidden) && !IsExcludedPath(path)) {
                ChangeRecord event;
                event.time = FtToUnix(static_cast<uint64_t>(rec->TimeStamp.QuadPart));
                event.kind = ChangeKind::Deleted;
                event.file_id = frn ^ (static_cast<uint64_t>(v.letter) << 56);
                event.path = path;
                RecordFeed(event); changes_.Record(std::move(event));
            }
            ChildMapRemove(NodeAt(i).parent, NameOf(i), i);
            tombstones_.insert(i);
            ++deleted_;
            InvalidateFilterLocked();
            if (delta) delta->QueueTomb(i);
        } else {
            refresh(i);
        }
        ++folder_size_mutations_;
        folder_sizes_.Replace(i, before, FolderSizeItem(i),
            [this](int32_t id) { return FolderSizeItem(id); });
    };

    if (reason & USN_REASON_FILE_DELETE) {
        if (delete_all)
            for (const int32_t i : links) other_link(i, true);
        if (idx >= 0 && idx < LiveCount() && !IsTomb(idx)) {
            track(ChangeKind::Deleted, idx);
            v.tracking_paths.erase(frn);
            ChildMapRemove(NodeAt(idx).parent, NameOf(idx), idx);
            tombstones_.insert(idx);
            ++deleted_;
            InvalidateFilterLocked();
            if (delta) delta->QueueTomb(idx);
            return UsnApply::Structure;
        }
        return UsnApply::None;
    }
    if (idx >= 0 && idx < LiveCount() && attr_reason) {
        refresh(idx);
        for (const int32_t i : links) other_link(i, false);
    }
    if (!structural_reason) { if (effect != UsnApply::None) track(ChangeKind::Modified, idx); return effect; }

    int32_t parent = FindByFrnLocked(v, rec->ParentFileReferenceNumber);
    if (parent < 0) parent = v.root_idx;

    uint8_t flags = is_dir ? kFlagDir : 0;
    // A root fallback is not a real indexed location for an unresolved parent.
    if ((reason & USN_REASON_RENAME_NEW_NAME) && FindByFrnLocked(v, rec->ParentFileReferenceNumber) < 0) flags |= kFlagHidden;
    if (ShouldSkipName(name)) flags |= kFlagHidden;
    std::wstring parent_path = BuildPathLocked(parent);
    if (!parent_path.empty() && parent_path.back() != L'\\') parent_path += L'\\';
    if (IsExcludedPath((resolved_parent.empty() ? parent_path : resolved_parent + L"\\") + std::wstring(name))) flags |= kFlagHidden;
    ParentChainGuard chain(LiveCount());
    for (int32_t a = parent; a != v.root_idx;) {
        if (!running_) return effect;
        // Journal rename order can temporarily place an ancestor below its own
        // descendant. Keep the old location rather than persist a parent cycle.
        if (a == idx || !chain.Visit(a)) {
            GapFeed();
            v.journal_id = 0;
            return effect;
        }
        const Node an = NodeAt(a);
        if (!(an.flags & kFlagDir) || IsTomb(a)) {
            GapFeed();
            v.journal_id = 0;
            return effect;
        }
        if (an.flags & kFlagHidden) flags |= kFlagHidden;
        a = an.parent;
    }

    if (idx >= 0 && idx < LiveCount()) {
        const Node old = NodeAt(idx);
        if (old.parent != parent) subtree_intervals_valid_ = false;
        ChildMapRemove(old.parent, NameOf(idx), idx);
        if (tombstones_.erase(idx)) --deleted_;
        const int32_t base = BaseCount();
        if (idx >= base) {
            Node& n = live_.nodes[static_cast<size_t>(idx - base)];
            pool_waste_ += n.len;
            n.parent = parent;
            n.off = static_cast<uint32_t>(live_.pool.size());
            n.len = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
            n.flags = flags;
            live_.pool.insert(live_.pool.end(), name.begin(), name.begin() + n.len);
        } else {
            Patch& p = patches_[idx];
            p.parent = parent;
            p.flags = flags;
            p.has_meta = true;
            if (p.has_name) pool_waste_ += p.len;
            p.has_name = true;
            p.off = static_cast<uint32_t>(live_.pool.size());
            p.len = static_cast<uint16_t>((std::min)(name.size(), static_cast<size_t>(65535)));
            live_.pool.insert(live_.pool.end(), name.begin(), name.begin() + p.len);
        }
        ChildMapAdd(parent, name, idx);
        if (is_dir && (old.parent != parent || old.flags != flags ||
                       (reason & USN_REASON_RENAME_NEW_NAME)))
            RefreshSubtreeVisibilityLocked(idx, delta);
        refresh(idx);
        InvalidateFilterLocked();
        if (delta) {
            const Attr a = AttrAt(idx);
            delta->QueuePatch(idx,
                              static_cast<uint8_t>(PatchBits::Meta) | static_cast<uint8_t>(PatchBits::Name) |
                                  static_cast<uint8_t>(PatchBits::Attr),
                              parent, flags, a.mtime, a.size, name);
        }
        track((reason & USN_REASON_RENAME_NEW_NAME) ? ChangeKind::Renamed : ChangeKind::Created, idx);
        return UsnApply::Structure;
    }
    if (static_cast<size_t>(LiveCount()) >= kIndexCap + 64) return effect;
    idx = AddNodeLocked(live_, parent, name, flags, frn, 0, 0, true, &v);
    refresh(idx);
    InvalidateFilterLocked();
    if (delta) {
        const Attr a = AttrAt(idx);
        delta->QueueAdd(idx, parent, flags, a.mtime, a.size, name, frn);
    }
    track(ChangeKind::Created, idx);
    return UsnApply::Structure;
}

bool Engine::CatchUpVolume(VolState& v, bool* changed, bool* structural) {
    const auto timing = FilenameTiming::Begin();
    const auto stream = journal_streams_.find(v.volume_id);
    uint64_t journal_id = 0;
    int64_t start_usn = 0;
    wchar_t letter = 0;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        v.folder_size_current = false;
        if (v.journal_id == 0) {
            filename_timing_.End(FilenameStage::Journal, timing, 0, ERROR_JOURNAL_NOT_ACTIVE, "journal_unavailable", v.letter);
            return false;
        }
        journal_id = v.journal_id;
        start_usn = v.next_usn;
        letter = v.letter;
    }
    HANDLE h = stream == journal_streams_.end() ? OpenVolume(letter) : INVALID_HANDLE_VALUE;
    if (stream == journal_streams_.end() && h == INVALID_HANDLE_VALUE) {
        filename_timing_.End(FilenameStage::Journal, timing, 0, GetLastError(), "open_failed", letter);
        return false;
    }

    READ_USN_JOURNAL_DATA_V0 rud{};
    rud.StartUsn = start_usn;
    rud.ReasonMask = 0xFFFFFFFF;
    rud.UsnJournalID = journal_id;
    std::vector<BYTE> blob;
    std::vector<BYTE> buf;
    if (stream == journal_streams_.end()) { blob.reserve(256 * 1024); buf.resize(256 * 1024); }
    bool ok = true;
    USN last = start_usn;
    const auto catchup_started = GetTickCount64();
    constexpr size_t catchup_bytes = 16 * 1024 * 1024;
    if (stream != journal_streams_.end()) ok = stream->second->Take(blob, last);
    else for (;;) {
        if (!running_) { ok = false; break; }
        DWORD br = 0;
        if (!DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &rud, sizeof(rud),
                             buf.data(), static_cast<DWORD>(buf.size()), &br, nullptr)) {
            const DWORD error = GetLastError();
            if (error == ERROR_JOURNAL_ENTRY_DELETED || error == ERROR_JOURNAL_NOT_ACTIVE ||
                error == ERROR_INVALID_PARAMETER) {
                std::unique_lock<std::shared_mutex> state_lock(mutex_);
                v.journal_id = 0;
                v.next_usn = 0;
                SetStatus(std::wstring(1, v.letter) + L": 的变更跟踪失效，正在重建索引…");
            }
            ok = false;
            break;
        }
        if (br < sizeof(USN)) break;
        const USN next = *reinterpret_cast<USN*>(buf.data());
        BYTE* p = buf.data() + sizeof(USN);
        BYTE* end = buf.data() + br;
        size_t nrec = 0;
        while (p + sizeof(USN_RECORD_COMMON_HEADER) <= end) {
            auto* hdr = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(p);
            if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
            if (hdr->RecordLength > catchup_bytes - blob.size() ||
                GetTickCount64() - catchup_started >= 2000) {
                ok = false;
                SetLastError(ERROR_BUFFER_OVERFLOW);
                break;
            }
            blob.insert(blob.end(), p, p + hdr->RecordLength);
            ++nrec;
            p += hdr->RecordLength;
        }
        if (!ok) break;
        rud.StartUsn = next;
        last = next;
        if (nrec == 0) break;
    }
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok || !running_) {
        const DWORD error = stream == journal_streams_.end() ? GetLastError() : stream->second->Error();
        filename_timing_.End(FilenameStage::Journal, timing, 0, error ? error : ERROR_OPERATION_ABORTED, "read_failed", letter);
        return false;
    }
    if (blob.empty() && last == start_usn) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        v.folder_size_current = true;
        filename_timing_.End(FilenameStage::Journal, timing, 0, ERROR_SUCCESS, "no_changes", letter);
        return true;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    BYTE* p = blob.empty() ? nullptr : blob.data();
    BYTE* end = p + blob.size();
    bool any_struct = false;
    bool any_attr = false;
    uint64_t applied = 0;
    while (p && p + sizeof(USN_RECORD_COMMON_HEADER) <= end) {
        if (!running_) return false;
        auto* hdr = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(p);
        if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
        if (hdr->MajorVersion == 2) {
            const UsnApply apply = ApplyUsnLocked(v, reinterpret_cast<USN_RECORD_V2*>(p));
            if (apply != UsnApply::None) ++applied;
            // An invalid parent relation needs a fresh volume snapshot, not a
            // journal cursor advanced past a change we could not apply safely.
            if (v.journal_id == 0) {
                filename_timing_.End(FilenameStage::Journal, timing, applied, ERROR_INVALID_DATA, "invalid_parent", letter);
                return false;
            }
            if (apply == UsnApply::Structure) any_struct = true;
            else if (apply == UsnApply::Attr) any_attr = true;
        }
        p += hdr->RecordLength;
    }
    v.next_usn = last;
    v.folder_size_current = true;
    if (DeltaLog* delta = DeltaFor(v.letter)) delta->QueueUsn(v.journal_id, v.next_usn);
    indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
    if (any_struct) {
        ++struct_changes_;
        last_struct_tick_ = GetTickCount64();
        if (changed) *changed = true;
        if (structural) *structural = true;
    } else if (any_attr) {
        if (changed) *changed = true;
    }
    if (any_struct || any_attr) { ++revision_; TraceSearch("filename_commit", revision_.load()); }
    filename_timing_.End(FilenameStage::Journal, timing, applied, ERROR_SUCCESS, "apply", letter);
    return true;
}

bool Engine::IndexVolumeMft(const VolumeInfo& volume) {
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
    using volume_fault_test::OpenVolume;
    using volume_fault_test::CloseHandle;
    using volume_fault_test::QueryJournal;
    using volume_fault_test::RootFrn;
    const auto enumerate_mft = &volume_fault_test::EnumerateMft;
    const auto enum_usn = &volume_fault_test::DeviceIoControl;
#else
    const auto enumerate_mft = &EnumerateMft;
    const auto enum_usn = &::DeviceIoControl;
#endif
    if (volume.mount_point.size() < 2) { SetLastError(ERROR_INVALID_NAME); return false; }
    const wchar_t letter = towupper(volume.mount_point[0]);
    size_t scan_records = 0;
    // Phase: 1=open, 2=MFT stopped, 3=USN read, 4=USN page,
    // 5=capacity, 6=empty/cancelled scan, 7=tree construction.
    auto fail = [&](DWORD error, uint64_t phase) {
        if (!error) error = ERROR_READ_FAULT;
        diagnostics::runtime::Event(error == ERROR_OPERATION_ABORTED ? "index_volume_scan_cancelled" : "index_volume_scan_failed", {
            {"operation", index_operation},
            {"drive", static_cast<uint64_t>(letter)}, {"phase", phase},
            {"records", scan_records}, {"limit", kIndexCap}, {"error", error}}, IndexErrorLevel(error));
        SetLastError(error);
        return false;
    };
    HANDLE h = OpenVolume(letter);
    if (h == INVALID_HANDLE_VALUE) return fail(GetLastError(), 1);

    VolState vol;
    vol.letter = letter;
    vol.kind = volume.kind;
    vol.volume_id = volume.id;
    if (!QueryJournal(h, vol.journal_id, vol.next_usn)) {
        vol.journal_id = 0;
        vol.next_usn = 0;
    }

    std::vector<FrnNode> frn_nodes;
    frn_nodes.reserve(256000);

    auto progress = [&](size_t n) {
        SetStatus(StatusDriveProgress(letter, n));
        PingNotify();
    };
    bool capacity_reached = false;
    const MftReadResult mft_result = enumerate_mft(h, &running_, progress, [&](MftFile&& f) {
        if (frn_nodes.size() >= kIndexCap) { capacity_reached = true; return false; }
        FrnNode node;
        node.frn = f.frn;
        node.parent = f.parent;
        node.size = f.size;
        node.mtime = f.mtime;
        node.name = std::move(f.name);
        node.is_dir = f.is_dir;
        node.name_type = f.name_type;
        frn_nodes.push_back(std::move(node));
        // Hard links (pnpm stores, WeChat files, WinSxS) are searchable under
        // every name, like Explorer and Everything list them.
        for (auto& link : f.links) {
            if (frn_nodes.size() >= kIndexCap) { capacity_reached = true; return false; }
            FrnNode extra;
            extra.frn = f.frn;
            extra.parent = link.parent;
            extra.size = f.size;
            extra.mtime = f.mtime;
            extra.name = std::move(link.name);
            extra.extra_link = true;
            frn_nodes.push_back(std::move(extra));
        }
        return true;
    });
    scan_records = frn_nodes.size();

    if (mft_result == MftReadResult::Stopped || !running_) {
        CloseHandle(h);
        return fail(capacity_reached ? ERROR_FILE_TOO_LARGE : ERROR_OPERATION_ABORTED,
            capacity_reached ? 5 : 2);
    }
    if (mft_result == MftReadResult::Failed) {
        // A fallback must not inherit records from a failed partial scan.
        frn_nodes.clear();
        scan_records = 0;
        MFT_ENUM_DATA_V0 med{};
        med.StartFileReferenceNumber = 0;
        med.LowUsn = 0;
        med.HighUsn = MAXLONGLONG;
        std::vector<BYTE> buffer(1024 * 1024);
        DWORD br = 0;
        while (running_) {
            if (!enum_usn(h, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                                 buffer.data(), static_cast<DWORD>(buffer.size()), &br, nullptr)) {
                DWORD err = GetLastError();
                if (err != ERROR_HANDLE_EOF) {
                    CloseHandle(h);
                    return fail(err, 3);
                }
                break;
            }
            MftEnumPage page;
            if (br > buffer.size() ||
                !ParseMftEnumPage({buffer.data(), br}, med.StartFileReferenceNumber, page)) {
                CloseHandle(h);
                return fail(ERROR_INVALID_DATA, 4);
            }
            if (page.records > kIndexCap - frn_nodes.size()) {
                CloseHandle(h);
                return fail(ERROR_FILE_TOO_LARGE, 5);
            }
            med.StartFileReferenceNumber = page.next_cursor;
            auto* rec = reinterpret_cast<PUSN_RECORD_V2>(buffer.data() + sizeof(USN));
            BYTE* end = buffer.data() + br;
            while (reinterpret_cast<BYTE*>(rec) < end) {
                FrnNode n;
                n.frn = rec->FileReferenceNumber;
                n.parent = rec->ParentFileReferenceNumber;
                n.name.assign(reinterpret_cast<const WCHAR*>(reinterpret_cast<const BYTE*>(rec) +
                    rec->FileNameOffset), rec->FileNameLength / sizeof(WCHAR));
                n.is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                frn_nodes.push_back(std::move(n));
                rec = reinterpret_cast<PUSN_RECORD_V2>(reinterpret_cast<BYTE*>(rec) + rec->RecordLength);
            }
            scan_records = frn_nodes.size();
            SetStatus(StatusDriveProgress(letter, frn_nodes.size()));
            PingNotify();
        }
    }
    CloseHandle(h);
    if (!running_ || frn_nodes.empty())
        return fail(running_ ? ERROR_NO_MORE_FILES : ERROR_OPERATION_ABORTED, 6);

    if (!BuildMftTree(std::move(vol), RootFrn(letter), std::move(frn_nodes)))
        return fail(running_ ? ERROR_INVALID_DATA : ERROR_OPERATION_ABORTED, 7);
    return true;
}

bool Engine::BuildMftTree(VolState vol, uint64_t root_frn, std::vector<FrnNode> frn_nodes) {
    const wchar_t letter = vol.letter;
    auto rank = [](uint8_t t) { return (t == 1 || t == 3) ? 0 : (t == 0 ? 1 : 2); };
    // Extra hard link names never take part in FRN parent resolution; they are
    // attached below their own directory once the primary tree exists.
    std::vector<FrnNode> links;
    {
        size_t kept = 0;
        for (size_t read = 0; read < frn_nodes.size(); ++read) {
            if (frn_nodes[read].extra_link) {
                links.push_back(std::move(frn_nodes[read]));
                continue;
            }
            if (kept != read) frn_nodes[kept] = std::move(frn_nodes[read]);
            ++kept;
        }
        frn_nodes.resize(kept);
    }
    std::sort(frn_nodes.begin(), frn_nodes.end(),
              [](const FrnNode& a, const FrnNode& b) { return a.frn < b.frn; });
    size_t unique_count = 0;
    for (size_t read = 0; read < frn_nodes.size(); ++read) {
        if (unique_count && frn_nodes[unique_count - 1].frn == frn_nodes[read].frn) {
            FrnNode& slot = frn_nodes[unique_count - 1];
            FrnNode& duplicate = frn_nodes[read];
            if (slot.name.empty() || rank(duplicate.name_type) < rank(slot.name_type)) {
                const int32_t existing_index = slot.index;
                slot = std::move(duplicate);
                slot.index = existing_index;
            } else {
                if (duplicate.size) slot.size = duplicate.size;
                if (duplicate.mtime) slot.mtime = duplicate.mtime;
                if (duplicate.is_dir) slot.is_dir = true;
            }
            continue;
        }
        if (unique_count != read) frn_nodes[unique_count] = std::move(frn_nodes[read]);
        ++unique_count;
    }
    frn_nodes.resize(unique_count);

    auto find_node = [&](uint64_t frn) -> FrnNode* {
        auto it = std::lower_bound(frn_nodes.begin(), frn_nodes.end(), frn,
            [](const FrnNode& node, uint64_t value) { return node.frn < value; });
        return it != frn_nodes.end() && it->frn == frn ? &*it : nullptr;
    };

    const wchar_t root_name[3] = { letter, L':', 0 };
    vol.root_idx = AddNodeLocked(build_, -1, root_name, kFlagDir | kFlagHidden, root_frn);
    vol.first_idx = vol.root_idx;
    if (root_frn) {
        if (FrnNode* root = find_node(root_frn)) root->index = vol.root_idx;
        vol.frn_build.emplace_back(root_frn, vol.root_idx);
    }

    std::vector<FrnNode*> stack;
    auto build_name = [&](int32_t id) -> std::wstring_view {
        const Node& node = build_.nodes[static_cast<size_t>(id)];
        return std::wstring_view(build_.pool.data() + node.off, node.len);
    };
    auto build_path = [&](int32_t id) {
        return BuildCompleteIndexPath(id, static_cast<int32_t>(build_.nodes.size()),
            [&](int32_t current) { return build_.nodes[static_cast<size_t>(current)].parent; },
            build_name);
    };
    size_t added = 0;
    for (FrnNode& node : frn_nodes) {
        if (!running_ || added >= kIndexCap) break;
        if (node.index >= 0) continue;
        stack.clear();
        uint64_t cur = node.frn;
        int32_t parent_idx = vol.root_idx;
        for (int hop = 0; hop < 48; ++hop) {
            if (index_dir_frn_ && cur == index_dir_frn_ && letter == index_dir_letter_) {
                stack.clear();
                break;
            }
            if (cur == root_frn) { parent_idx = vol.root_idx; break; }
            FrnNode* current = find_node(cur);
            if (!current || current->name.empty()) break;
            if (current->index >= 0) { parent_idx = current->index; break; }
            stack.push_back(current);
            if (current->parent == cur) break;
            cur = current->parent;
        }
        std::wstring candidate = build_path(parent_idx);
        if (candidate.empty()) return false;
        bool hidden_parent = parent_idx >= 0 && parent_idx != vol.root_idx &&
            (build_.nodes[static_cast<size_t>(parent_idx)].flags & kFlagHidden) != 0;
        for (auto rit = stack.rbegin(); rit != stack.rend(); ++rit) {
            FrnNode& n = **rit;
            if (IsIndexArtifactName(n.name)) continue;
            uint8_t flags = n.is_dir ? kFlagDir : 0;
            if (!candidate.empty() && candidate.back() != L'\\') candidate += L'\\';
            candidate += n.name;
            if (candidate.size() > 32766) return false;
            if (hidden_parent || ShouldSkipName(n.name) || IsExcludedPath(candidate))
                flags |= kFlagHidden;
            parent_idx = AddNodeLocked(build_, parent_idx, n.name, flags, n.frn, n.size, n.mtime);
            n.index = parent_idx;
            hidden_parent = (flags & kFlagHidden) != 0;
            vol.frn_build.emplace_back(n.frn, parent_idx);
            ++added;
        }
        indexed_.store(build_.nodes.size());
        if ((added & 0x3FFF) == 0) PingNotify();
    }
    for (FrnNode& link : links) {
        if (!running_ || added >= kIndexCap) break;
        if (link.name.empty() || IsIndexArtifactName(link.name)) continue;
        int32_t parent_idx = -1;
        if (root_frn && link.parent == root_frn) {
            parent_idx = vol.root_idx;
        } else if (const FrnNode* parent = find_node(link.parent); parent && parent->is_dir) {
            parent_idx = parent->index;
        }
        if (parent_idx < 0) continue;
        if (const FrnNode* primary = find_node(link.frn)) {
            if (primary->is_dir) continue;
            if (primary->index >= 0 &&
                build_.nodes[static_cast<size_t>(primary->index)].parent == parent_idx &&
                EqualsI(build_name(primary->index), link.name)) continue;
            link.size = primary->size;
            link.mtime = primary->mtime;
        }
        std::wstring path = build_path(parent_idx);
        if (path.empty()) continue;
        if (path.back() != L'\\') path += L'\\';
        path += link.name;
        if (path.size() > 32766) continue;
        const bool hidden_parent = parent_idx != vol.root_idx &&
            (build_.nodes[static_cast<size_t>(parent_idx)].flags & kFlagHidden) != 0;
        const uint8_t flags = hidden_parent || ShouldSkipName(link.name) || IsExcludedPath(path)
            ? kFlagHidden : 0;
        const int32_t idx = AddNodeLocked(build_, parent_idx, link.name, flags, link.frn, link.size, link.mtime);
        vol.frn_build.emplace_back(link.frn, idx);
        ++added;
        if ((added & 0x3FFF) == 0) {
            indexed_.store(build_.nodes.size());
            PingNotify();
        }
    }
    indexed_.store(build_.nodes.size());
    if (added == 0) return false;
    vol.item_count = added + 1;
    build_vols_.push_back(std::move(vol));
    return true;
}

bool Engine::RebuildVolumeMft(const VolumeInfo& volume) {
    if (!MachineIndexScope() || volume.id.empty()) return false;

    // Flatten the current live view first. This gives the replacement build a
    // stable snapshot of every healthy volume while excluding only the failed
    // volume's DFS span. No query-visible state changes until the new V9 base
    // is fully written and mmap validation succeeds.
    Store current;
    std::vector<VolState> current_volumes;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (LiveCount() == 0 || !FlattenLocked(current, current_volumes)) return false;
    }

    const std::wstring target_id = NormalizeVolumeId(volume.id);
    auto target_it = std::find_if(current_volumes.begin(), current_volumes.end(),
        [&](const VolState& state) {
            return NormalizeVolumeId(state.volume_id) == target_id;
        });
    if (target_it == current_volumes.end()) return false;

    int32_t skip_first = target_it->first_idx;
    int32_t skip_last = skip_first + static_cast<int32_t>(target_it->item_count);
    if (skip_first < 0 || skip_last <= skip_first ||
        skip_last > static_cast<int32_t>(current.nodes.size())) return false;

    Store replacement;
    replacement.pool.reserve(current.pool.size());
    replacement.nodes.reserve(current.nodes.size() - static_cast<size_t>(skip_last - skip_first));
    replacement.attrs.reserve(replacement.nodes.capacity());
    std::vector<int32_t> remap(current.nodes.size(), -1);
    std::vector<uint32_t> kept_before(current.nodes.size() + 1, 0);
    for (size_t i = 0; i < current.nodes.size(); ++i)
        kept_before[i + 1] = kept_before[i] +
            (i >= static_cast<size_t>(skip_first) && i < static_cast<size_t>(skip_last) ? 0u : 1u);

    for (int32_t i = 0; i < static_cast<int32_t>(current.nodes.size()); ++i) {
        if (i >= skip_first && i < skip_last) continue;
        const Node source = current.nodes[static_cast<size_t>(i)];
        Node node = source;
        node.parent = source.parent >= 0 ? remap[static_cast<size_t>(source.parent)] : -1;
        const std::wstring_view name(current.pool.data() + source.off, source.len);
        node.off = static_cast<uint32_t>(replacement.pool.size());
        replacement.pool.insert(replacement.pool.end(), name.begin(), name.end());
        if (source.unused > 0 && source.unused <= current.nodes.size())
            node.unused = kept_before[source.unused];
        else
            node.unused = 0;
        remap[static_cast<size_t>(i)] = static_cast<int32_t>(replacement.nodes.size());
        replacement.nodes.push_back(node);
        replacement.attrs.push_back(current.attrs[static_cast<size_t>(i)]);
    }

    std::vector<VolState> healthy;
    healthy.reserve(current_volumes.size() - 1);
    for (const auto& source : current_volumes) {
        if (NormalizeVolumeId(source.volume_id) == target_id) continue;
        VolState state = source;
        if (state.root_idx < 0 || state.root_idx >= static_cast<int32_t>(remap.size()) ||
            remap[static_cast<size_t>(state.root_idx)] < 0) return false;
        state.root_idx = remap[static_cast<size_t>(state.root_idx)];
        state.first_idx = state.root_idx;
        const uint32_t old_end = source.first_idx + static_cast<uint32_t>(source.item_count);
        const uint32_t new_end = old_end <= current.nodes.size()
            ? kept_before[old_end] : static_cast<uint32_t>(replacement.nodes.size());
        state.item_count = new_end > static_cast<uint32_t>(state.first_idx)
            ? new_end - static_cast<uint32_t>(state.first_idx) : 0;
        state.frn_base = nullptr;
        state.frn_base_n = 0;
        state.frn_new.clear();
        state.frn_build.clear();
        for (const auto& entry : source.frn_build) {
            if (entry.second >= 0 && entry.second < static_cast<int32_t>(remap.size()) &&
                remap[static_cast<size_t>(entry.second)] >= 0)
                state.frn_build.emplace_back(entry.first, remap[static_cast<size_t>(entry.second)]);
        }
        healthy.push_back(std::move(state));
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_ = std::move(replacement);
        build_vols_ = std::move(healthy);
        building_ = true;
        indexed_.store(build_.nodes.size());
    }
    SetStatus(L"正在重建 " + volume.mount_point + L" 的索引…");
    if (!IndexVolumeMft(volume)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_vols_.clear();
        building_ = false;
        return false;
    }
    const uint64_t built = static_cast<uint64_t>(std::time(nullptr));
    const std::wstring path = CachePath();
    const bool wrote = !path.empty() && WriteIndexFile(path, build_, build_vols_, built);
    if (!wrote) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_vols_.clear();
        building_ = false;
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);
    built_unix_ = built;
    if (!CommitMappedFile(path)) {
        build_.Clear();
        build_vols_.clear();
        building_ = false;
        return false;
    }
    if (MachineIndexScope()) {
        WriteVolumeShards(build_, build_vols_, built);
        RefreshQueryShardsLocked();
    }
    build_.Clear();
    build_.Shrink();
    build_vols_.clear();
    OpenDeltasLocked();
    building_ = false;
    ready_ = true;
    SetStatus(volume.mount_point + L" 的索引已重建完成");
    PingNotify(true);
    return true;
}

void Engine::WalkTree(int32_t parent, const std::wstring& dir, int depth) {
    if (!running_ || depth > 18) return;
    std::vector<fs::DirEntry> entries;
    try {
        fs::EnumerateDirectory(dir, entries);
    } catch (...) {
        return;
    }
    std::wstring base = dir;
    if (!base.empty() && base.back() != L'\\' && !fs::IsVirtualPath(base)) base += L'\\';

    struct Child {
        std::wstring name;
        bool is_dir = false;
        bool rec = false;
        uint64_t size = 0;
        uint64_t mtime = 0;
        int32_t idx = -1;
    };
    std::vector<Child> kids;
    kids.reserve(entries.size());
    for (const auto& e : entries) {
        if (!running_) return;
        if (ShouldSkipName(e.name) || IsIndexArtifactName(e.name)) continue;
        if (IsExcludedPath(base + e.name)) continue;
        Child c;
        c.name = e.name;
        c.is_dir = e.is_dir;
        c.rec = e.is_dir && !e.is_reparse;
        c.size = e.size;
        c.mtime = (static_cast<uint64_t>(e.mtime.dwHighDateTime) << 32) | e.mtime.dwLowDateTime;
        kids.push_back(std::move(c));
    }
    for (auto& c : kids) {
        c.idx = AddNodeLocked(build_, parent, c.name, c.is_dir ? kFlagDir : 0, 0, c.size, c.mtime);
        indexed_.store(build_.nodes.size());
        if (indexed_.load() >= kIndexCap) return;
    }
    PingNotify();
    for (const auto& c : kids) {
        if (!running_ || indexed_.load() >= kIndexCap) return;
        if (c.rec) WalkTree(c.idx, base + c.name, depth + 1);
    }
}

void Engine::FullRebuild(const char* reason) {
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
    using volume_fault_test::ConfiguredVolumes;
    using volume_fault_test::IsAdmin;
#endif
    const auto operation = diagnostics::runtime::NextId();
    const IndexOperationScope operation_scope(operation);
    const auto started = GetTickCount64();
    const uint64_t reason_code = !strcmp(reason, "cold_start") ? 1 : !strcmp(reason, "startup_stale") ? 2 :
        !strcmp(reason, "snapshot_upgrade") ? 3 : 0;
    diagnostics::runtime::Event("index_rebuild_begin", {{"operation", operation}, {"reason", reason_code}, {"nodes", indexed_.load()}});
    const auto timing = FilenameTiming::Begin();
    DWORD build_error = ERROR_SUCCESS;
    bool used_mft = false;
    auto finish = [&] {
        const DWORD error = running_ ? build_error : ERROR_OPERATION_ABORTED;
        filename_timing_.End(FilenameStage::Rebuild, timing, indexed_.load(), error, reason);
        diagnostics::runtime::Event("index_rebuild_end", {{"operation", operation}, {"nodes", indexed_.load()},
            {"elapsed_ms", GetTickCount64() - started}, {"error", error},
            {"outcome", error == ERROR_OPERATION_ABORTED ? 2ull : error ? 1ull : 0ull},
            {"used_mft", used_mft ? 1ull : 0ull}});
        if (error && error != ERROR_OPERATION_ABORTED)
            diagnostics::runtime::Event("index_rebuild_failed", {{"operation", operation}, {"error", error}},
                diagnostics::runtime::Level::Error);
        filename_timing_.Flush();
    };
    IndexConfig config;
    std::wstring config_error;
    bool config_loaded = true;
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
    config_loaded = !volume_fault_test::SnapshotFault(5);
#endif
    if (config_loaded && MachineIndexScope()) config_loaded = LoadMachineConfig(config, &config_error);
    if (!config_loaded) {
        build_error = GetLastError();
        if (!build_error) build_error = ERROR_INVALID_DATA;
        IndexFailure("index_rebuild_config_failed", build_error, {});
        SetStatus(config_error);
        PingNotify(true);
        finish();
        return;
    }
#ifdef PULSE_INDEX_VOLUME_FAULT_TEST
    const auto configured_volumes = ConfiguredVolumes();
#else
    const auto configured_volumes = ConfiguredVolumes(config);
#endif
    PublishExcludedPaths(config, [&] {
        build_.Clear();
        build_.pool.reserve(1u << 20);
        build_.nodes.reserve(1u << 16);
        building_ = true;
        build_vols_.clear();
        if (MachineIndexScope()) PreserveOfflineVolumesLocked(configured_volumes, config);
    });
    if (indexed_.load() == 0) SetStatus(L"正在建立索引…");
    else SetStatus(L"已索引 " + std::to_wstring(indexed_.load()) + L" 项，正在重建…");
    PingNotify(true);

    bool incomplete = false;
    const auto previous_count = indexed_.load();
    if (IsAdmin() && fixture_root_.empty()) {
        for (const auto& volume : configured_volumes) {
            if (!running_) break;
            if (IndexVolumeMft(volume)) used_mft = true;
            else {
                const DWORD error = GetLastError();
                if (!incomplete) build_error = error ? error : ERROR_READ_FAULT;
                incomplete = true;
            }
        }
    }
    if (!used_mft && !incomplete && running_ && !MachineIndexScope()) {
        walk_roots_.clear();
        auto walk_root = [this](const std::wstring& path) {
            walk_roots_.push_back(path);
            // Arm before scanning so changes during reconciliation are retained.
            StartWalkWatches(walk_roots_);
            int32_t parent = EnsureChainLocked(build_, path, true, false);
            if (parent >= 0) WalkTree(parent, path, 0);
        };
        wchar_t profile[MAX_PATH] = {};
        if (!fixture_root_.empty()) walk_root(fixture_root_);
        else if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile)))
            walk_root(Display(fs::NormalizePath(profile)));
        // R2: non-admin default is the user profile only (RDCW can actually watch it).
    }

    if (running_ && !incomplete) {
        const uint64_t built = (std::max)(static_cast<uint64_t>(std::time(nullptr)), built_unix_ + 1);
        const std::wstring path = CachePath();
        if (path.empty()) IndexFailure("filename_write_path_failed", ERROR_PATH_NOT_FOUND, path);
        const bool wrote = !path.empty() && WriteIndexFile(path, build_, build_vols_, built);
        if (!wrote) build_error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
        bool committed = false;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            // CommitMappedFile uses built_unix_ when writing the V9 manifest.
            // Set it before publishing so the manifest and base header agree.
            built_unix_ = built;
            if (wrote && CommitMappedFile(path)) {
                committed = true;
                OpenDeltasLocked();
                ready_ = true;
                if (MachineIndexScope())
                    SetStatus(StatusItemCount(indexed_.load(), L"正在完成"));
            } else {
                if (wrote) build_error = GetLastError() ? GetLastError() : ERROR_WRITE_FAULT;
                if (map_) { map_->Close(); map_.reset(); }
                live_ = std::move(build_);
                ++layout_epoch_;
                vols_ = std::move(build_vols_);
                tombstones_.clear();
                patches_.clear();
                deleted_ = 0;
                RebuildChildMapLocked();
                indexed_.store(live_.nodes.size());
                InvalidateFilterLocked();
            }
        }
        if (committed && MachineIndexScope()) {
            PingNotify(true);
            WriteVolumeShards(build_, build_vols_, built);
            std::unique_lock<std::shared_mutex> lock(mutex_);
            RefreshQueryShardsLocked();
        }
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            build_.Clear();
            build_.Shrink();
            build_vols_.clear();
            UpdateVolumeVisibilityLocked(configured_volumes);
            const bool live_tracked = !vols_.empty() &&
                std::all_of(vols_.begin(), vols_.end(),
                            [](const VolState& v) { return v.journal_id != 0; });
            SetStatus(committed ? StatusItemCount(indexed_.load(),
                      live_tracked ? L"实时更新" : L"正在监视")
                      : L"索引可用，但保存失败，请检查索引目录权限及磁盘空间");
            ready_ = true;
            building_ = false;
            folder_size_gap_ = false;
        }
        if (used_mft) StopWalkWatches();
        else PollWalkWatches();
        PingNotify(true);
        if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 1, 0);
    } else {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_.Shrink();
        build_vols_.clear();
        building_ = false;
    }
    if (incomplete) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        indexed_.store(previous_count);
        folder_size_gap_ = true;
        SetStatus(L"索引扫描未完成，保留上次结果，请稍后重试");
        PingNotify(true);
    }
    finish();
}

bool Engine::NeedsSearchRebuildLocked() const {
    return map_ && (map_->hdr->ver < kIndexVer || !map_->prefix1_all_chars);
}

void Engine::StartJournalStreams() {
    journal_streams_.clear();
    for (const auto& volume : vols_) {
        if (volume.journal_id && !volume_retry_after_.contains(volume.volume_id)) journal_streams_[volume.volume_id] =
            std::make_unique<UsnStream>(volume.letter, volume.journal_id, volume.next_usn, change_signal_);
    }
}
void Engine::RecoverFailedVolumes(const std::vector<VolumeInfo>& volumes, ULONGLONG now,
                                  const std::function<bool(const VolumeInfo&)>& rebuild) {
    for (const auto& volume : volumes) {
        if (!running_) break;
        const auto cooldown = volume_recovered_after_.find(volume.id);
        if (cooldown != volume_recovered_after_.end() && now < cooldown->second) {
            volume_retry_after_[volume.id] = cooldown->second;
            journal_streams_.erase(volume.id);
            SetStatus(volume.mount_point + L" 的变更跟踪暂不可用，稍后重试");
            continue;
        }
        auto retry = volume_retry_after_.find(volume.id);
        if (retry != volume_retry_after_.end() && now < retry->second) continue;
        journal_streams_.erase(volume.id);
        const auto previous_count = indexed_.load();
        const auto previous_built = built_unix_;
        const auto timing = FilenameTiming::Begin();
        const bool recovered = rebuild(volume);
        const DWORD error = recovered ? ERROR_SUCCESS : GetLastError();
        filename_timing_.End(FilenameStage::Recovery, timing, 1,
            recovered ? ERROR_SUCCESS : (error ? error : ERROR_GEN_FAILURE), "journal_gap_single_volume",
            volume.mount_point.empty() ? 0 : volume.mount_point.front());
        if (!recovered) {
            {
                std::unique_lock lock(mutex_);
                indexed_ = previous_count;
                built_unix_ = previous_built;
            }
            // Keep healthy collectors running and the last good snapshot
            // queryable. A persistent failed disk must not trigger full scans.
            volume_retry_after_[volume.id] = (std::max)(now, GetTickCount64()) + 60000;
            SetStatus(volume.mount_point + L" 的变更跟踪暂不可用，稍后重试");
            continue;
        }
        volume_retry_after_.erase(volume.id);
        volume_recovered_after_[volume.id] = (std::max)(now, GetTickCount64()) + 60000;
        const auto state = std::find_if(vols_.begin(), vols_.end(), [&](const VolState& value) {
            return NormalizeVolumeId(value.volume_id) == NormalizeVolumeId(volume.id);
        });
        if (state != vols_.end() && state->journal_id && running_)
            journal_streams_[state->volume_id] = std::make_unique<UsnStream>(
                state->letter, state->journal_id, state->next_usn, change_signal_);
    }
    if (volume_retry_after_.empty()) folder_size_gap_ = false;
    filename_timing_.Flush();
}
void Engine::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    ResolveIndexDirFrn();
    IndexConfig startup_config;
    std::wstring config_error;
    if (MachineIndexScope() && !LoadMachineConfig(startup_config, &config_error)) {
        SetStatus(config_error); PingNotify(true);
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END); return;
    }
    PublishExcludedPaths(startup_config);
    const bool have_cache = TryLoadCache();
    const auto initial_drives = ConfiguredVolumes(startup_config);
    bool needs_search_rebuild = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        UpdateVolumeVisibilityLocked(initial_drives);
        needs_search_rebuild = NeedsSearchRebuildLocked();
    }
    // A config from before the system folder exclusions gets the defaults
    // persisted once; its snapshot still shows those folders, so rebuild it.
    if (MachineIndexScope() && !startup_config.system_groups_saved &&
        SaveMachineConfig(startup_config) && have_cache)
        needs_search_rebuild = true;
    PingNotify(true);

    bool fresh = false;
    if (have_cache && !needs_search_rebuild && IsAdmin()) {
        bool have_vols = false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            have_vols = !vols_.empty();
        }
        if (have_vols) {
            auto drives = initial_drives;
            const auto mounted = EnumerateLocalVolumes(startup_config);
            fresh = true;
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                for (const auto& drive : drives)
                    if (std::none_of(vols_.begin(), vols_.end(), [&](const VolState& v) {
                            return NormalizeVolumeId(drive.id) == NormalizeVolumeId(v.volume_id);
                        })) fresh = false;
                for (const auto& v : vols_)
                    if (std::any_of(mounted.begin(), mounted.end(), [&](const VolumeInfo& drive) {
                            return !drive.enabled &&
                                   NormalizeVolumeId(drive.id) == NormalizeVolumeId(v.volume_id);
                        })) fresh = false;
            }
            if (fresh) {
                std::vector<VolumeInfo> startup_failed;
                std::unique_lock<std::shared_mutex> lock(mutex_);
                for (auto& v : vols_) {
                    const bool online = std::any_of(drives.begin(), drives.end(), [&](const VolumeInfo& drive) {
                        return NormalizeVolumeId(drive.id) == NormalizeVolumeId(v.volume_id);
                    });
                    if (!online) continue;
                    lock.unlock();
                    if (!CatchUpVolume(v, nullptr)) {
                        auto it = std::find_if(drives.begin(), drives.end(), [&](const VolumeInfo& drive) {
                            return NormalizeVolumeId(drive.id) == NormalizeVolumeId(v.volume_id);
                        });
                        if (it != drives.end()) startup_failed.push_back(*it);
                    }
                    lock.lock();
                }
                lock.unlock();
                if (!startup_failed.empty()) {
                    fresh = true;
                    for (const auto& volume : startup_failed) {
                        if (!RebuildVolumeMft(volume)) {
                            fresh = false;
                            break;
                        }
                    }
                }
            }
            if (fresh)
                SetStatus(StatusItemCount(indexed_.load(), L"实时更新"));
        }
    }
    // Walk snapshots have no offline journal cursor. Replay provides a useful
    // initial view, but every startup must reconcile changes while we were away.

    if (fresh) {
        if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 1, 0);
    } else if (running_) {
        // A cold install has no snapshot yet. Expose an empty, queryable
        // engine while the first build runs so the UI and named-pipe service
        // become available immediately instead of appearing hung.
        if (!have_cache) {
            ready_ = true;
            SetStatus(L"索引正在后台建立…");
            PingNotify(true);
        }
        FullRebuild(have_cache ? (needs_search_rebuild ? "snapshot_upgrade" : "startup_stale") : "cold_start");
    }
    bool walk_mode = false;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        walk_mode = vols_.empty() && !MachineIndexScope();
    }
    if (running_ && walk_mode && watches_.empty()) {
        std::vector<std::wstring> roots = walk_roots_;
        if (roots.empty()) {
            wchar_t profile[MAX_PATH] = {};
            if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile)))
                roots.push_back(Display(fs::NormalizePath(profile)));
        }
        StartWalkWatches(roots);
    }

    last_merge_tick_ = GetTickCount64();
    auto online = initial_drives;
    auto topology_tick = GetTickCount64();
    StartJournalStreams();
    while (running_) {
        SeedPendingChanges();
        std::vector<HANDLE> events{change_signal_};
        for (auto& watch : watches_) if (events.size() < MAXIMUM_WAIT_OBJECTS) events.push_back(watch.event);
        const auto wait_timing = FilenameTiming::Begin();
        const DWORD wake = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, 15000);
        filename_timing_.End(FilenameStage::Wait, wait_timing, wake == WAIT_TIMEOUT ? 0 : 1,
            wake == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS, wake == WAIT_TIMEOUT ? "maintenance" : "event");
        PollWalkWatches();
        changes_.Flush(false, &filename_timing_.Memory());
        if (!running_) break;
        if (rebuild_requested_.exchange(false)) {
            journal_streams_.clear();
            volume_retry_after_.clear();
            FullRebuild();
            StartJournalStreams();
            last_merge_tick_ = GetTickCount64();
            topology_tick = 0;
            continue;
        }
        // Clients asked for folder totals: aggregate here, never in a pipe thread
        // that would exceed the client's IPC deadline.
        if (folder_size_wanted_ && ready_ && !building_ && !folder_size_gap_) BuildFolderTotals();
        bool changed = false;
        bool structural = false;
        bool failed = false;
        std::vector<VolumeInfo> failed_volumes;
        const auto loop_tick = GetTickCount64();
        if (fixture_root_.empty() && loop_tick - topology_tick >= 30000) {
            const auto timing = FilenameTiming::Begin();
            IndexConfig topology_config;
            if (!MachineIndexScope() || LoadMachineConfig(topology_config, nullptr))
                online = ConfiguredVolumes(topology_config);
            topology_tick = loop_tick;
            filename_timing_.End(FilenameStage::Topology, timing, online.size(), ERROR_SUCCESS, "periodic");
        }
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!vols_.empty()) {
                std::vector<size_t> idx(vols_.size());
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
                UpdateVolumeVisibilityLocked(online);
                lock.unlock();
                for (size_t i : idx) {
                    const bool present = std::any_of(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                        return NormalizeVolumeId(drive.id) == NormalizeVolumeId(vols_[i].volume_id);
                    });
                    if (!present) continue;
                    const auto retry = volume_retry_after_.find(vols_[i].volume_id);
                    if (retry != volume_retry_after_.end() && loop_tick < retry->second) continue;
                    if (retry != volume_retry_after_.end() || !CatchUpVolume(vols_[i], &changed, &structural)) {
                        GapFeed();
                        failed = true;
                        auto it = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                            return NormalizeVolumeId(drive.id) == NormalizeVolumeId(vols_[i].volume_id);
                        });
                        if (it != online.end()) failed_volumes.push_back(*it);
                    }
                }
                lock.lock();
                if (changed) {
                    const bool live_tracked = std::all_of(vols_.begin(), vols_.end(),
                        [](const VolState& v) { return v.journal_id != 0; });
                    SetStatus(StatusItemCount(indexed_.load(),
                              live_tracked ? L"实时更新" : L"部分磁盘未实时更新"));
                }
            }
        }
        if (failed && running_) {
            RecoverFailedVolumes(failed_volumes, loop_tick,
                [this](const VolumeInfo& volume) { return RebuildVolumeMft(volume); });
        }
        if (changed && notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);

        const ULONGLONG now = GetTickCount64();
        if (now - last_delta_flush_tick_ >= kDeltaFlushMs)
            FlushDeltas();

        uint64_t delta_bytes = 0;
        const char* merge_reason = nullptr;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            for (const auto& [k, log] : delta_logs_)
                if (log) delta_bytes += log->BytesOnDisk() + (log->HasPending() ? 1 : 0);
            merge_reason = MaintenanceMergeReason(now, delta_bytes);
            // A pending full merge already reclaims the pool. Otherwise keep
            // all IDs/generations and compact only heap-owned name references.
            if (!merge_reason) CompactNamePoolLocked(now);
        }
        // Merge triggers must measure unmerged work only. (A live-node count
        // here forced a full rewrite every loop on any machine over the old
        // 500k threshold, e.g. the 2.6M-item dev box.)
        if (merge_reason) {
            MergeBase(std::strcmp(merge_reason, "quiet_changes") != 0, merge_reason);
        }
        if (filename_timing_.Due()) CaptureMemoryState();
        filename_timing_.Flush();
    }
    journal_streams_.clear();
    FlushDeltas();
    StopWalkWatches();
    filename_timing_.Flush(true);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

void Engine::StopWalkWatches() {
    for (auto& w : watches_) {
        if (w.dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(w.dir, &w.ov);
            DWORD completed = 0; GetOverlappedResult(w.dir, &w.ov, &completed, TRUE);
            CloseHandle(w.dir);
            w.dir = INVALID_HANDLE_VALUE;
        }
        if (w.event) { CloseHandle(w.event); w.event = nullptr; }
    }
    watches_.clear();
}

void Engine::StartWalkWatches(const std::vector<std::wstring>& roots) {
    StopWalkWatches();
    // OVERLAPPED must stay at the address passed to the kernel until its IO
    // completes. Reserve and construct in place before arming any watch.
    watches_.reserve(roots.size());
    for (const auto& path : roots) {
        if (path.empty()) continue;
        watches_.emplace_back();
        auto& w = watches_.back();
        w.path = path;
        w.buf.resize(64 * 1024);
        w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.dir = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (w.dir == INVALID_HANDLE_VALUE) {
            if (w.event) CloseHandle(w.event);
            watches_.pop_back();
            continue;
        }
        w.ov.hEvent = w.event;
        const BOOL subtree = TRUE;
        ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size()), subtree,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &w.ov, nullptr);
    }
}

void Engine::PollWalkWatches() {
    for (auto& w : watches_) {
        if (w.dir == INVALID_HANDLE_VALUE) continue;
        DWORD n = 0;
        if (!GetOverlappedResult(w.dir, &w.ov, &n, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) continue;
            n = 0;
        }
        if (n == 0) { GapFeed(); walk_pending_renames_.erase(w.path); rebuild_requested_ = true; }
        if (n > 0) {
            const auto timing = FilenameTiming::Begin();
            std::unique_lock<std::shared_mutex> lock(mutex_);
            const auto applied = ApplyNotifyLocked(w.path, w.buf.data(), n);
            if (applied) {
                ++revision_; TraceSearch("filename_commit", revision_.load(), w.path);
                if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);
            }
            filename_timing_.End(FilenameStage::Notify, timing, applied, ERROR_SUCCESS, "directory_changes");
        }
        ResetEvent(w.event);
        ZeroMemory(&w.ov, sizeof(w.ov));
        w.ov.hEvent = w.event;
        const BOOL subtree = TRUE;
        ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size()), subtree,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &w.ov, nullptr);
    }
}

uint64_t Engine::ApplyNotifyLocked(const std::wstring& root, const BYTE* buf, DWORD len) {
    uint64_t applied = 0;
    std::wstring& pending_old = walk_pending_renames_[root];
    const BYTE* p = buf;
    const BYTE* end = buf + len;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const auto name_offset = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if (info->FileNameLength % sizeof(WCHAR) || info->FileNameLength > static_cast<size_t>(end - p) - name_offset) {
            GapFeed(); break;
        }
        std::wstring rel(info->FileName, info->FileNameLength / sizeof(WCHAR));
        const auto relative_slash = rel.find_last_of(L"\\/");
        if (IsIndexArtifactName(std::wstring_view(rel).substr(relative_slash == std::wstring::npos ? 0 : relative_slash + 1))) {
            if (info->NextEntryOffset == 0) break;
            if (info->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) || info->NextEntryOffset > static_cast<size_t>(end - p)) {
                GapFeed(); break;
            }
            p += info->NextEntryOffset;
            continue;
        }
        std::wstring full = root;
        if (!full.empty() && full.back() != L'\\' && !rel.empty()) full += L'\\';
        full += rel;
        // A recursive directory watch also reports metadata updates for the
        // index directory itself when a diagnostic/cache file is written.
        const auto normalized_full = NormalizeChangePath(full);
        if (!index_directory_.empty() && normalized_full.size() >= index_directory_.size() &&
            CompareStringOrdinal(normalized_full.data(), static_cast<int>(index_directory_.size()),
                index_directory_.data(), static_cast<int>(index_directory_.size()), TRUE) == CSTR_EQUAL &&
            (normalized_full.size() == index_directory_.size() || normalized_full[index_directory_.size()] == L'\\')) {
            if (info->NextEntryOffset == 0) break;
            if (info->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) || info->NextEntryOffset > static_cast<size_t>(end - p)) {
                GapFeed(); break;
            }
            p += info->NextEntryOffset;
            continue;
        }
        ++applied;
        if (info->Action != FILE_ACTION_MODIFIED) {
            ++struct_changes_;
            last_struct_tick_ = GetTickCount64();
        }
        ChangeRecord event; event.path = full;
        const int32_t known = ResolvePathLocked(full);
        event.is_dir = known >= 0 && (NodeAt(known).flags & kFlagDir) != 0;
        if (info->Action == FILE_ACTION_ADDED) {
            event.kind = ChangeKind::Created;
            DWORD attributes = GetFileAttributesW(full.c_str());
            event.is_dir = attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        } else if (info->Action == FILE_ACTION_REMOVED) event.kind = ChangeKind::Deleted;
        else if (info->Action == FILE_ACTION_RENAMED_NEW_NAME) { event.kind = ChangeKind::Renamed; event.old_path = pending_old;
            const int32_t old_index = ResolvePathLocked(pending_old);
            if (old_index >= 0) event.is_dir = (NodeAt(old_index).flags & kFlagDir) != 0;
            else GapFeed(); }
        if (info->Action != FILE_ACTION_RENAMED_OLD_NAME) { RecordFeed(event); changes_.Record(std::move(event)); }
        const int32_t previous_count = LiveCount();
        const int32_t changed_id = info->Action == FILE_ACTION_RENAMED_NEW_NAME
            ? ResolvePathLocked(pending_old) : known;
        switch (info->Action) {
        case FILE_ACTION_ADDED: {
            WIN32_FILE_ATTRIBUTE_DATA fad{};
            const bool ok = GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad);
            const bool is_dir = ok && (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
            int32_t idx = EnsureChainLocked(live_, full, is_dir, true);
            if (idx >= 0 && ok) {
                Attr a;
                a.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                a.mtime = FtToUnix((static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                   fad.ftLastWriteTime.dwLowDateTime);
                const int32_t base = BaseCount();
                if (idx >= base) live_.attrs[static_cast<size_t>(idx - base)] = a;
                else {
                    Patch& pt = patches_[idx];
                    Node n = NodeAt(idx);
                    pt.parent = n.parent;
                    pt.flags = n.flags;
                    pt.has_meta = true;
                    pt.has_attr = true;
                    pt.attr = a;
                }
            }
            indexed_.store(static_cast<size_t>(LiveCount()));
            InvalidateFilterLocked();
            break;
        }
        case FILE_ACTION_REMOVED: {
            int32_t idx = ResolvePathLocked(full);
            if (idx >= 0 && !IsTomb(idx)) {
                ChildMapRemove(NodeAt(idx).parent, NameOf(idx), idx);
                tombstones_.insert(idx);
                ++deleted_;
                InvalidateFilterLocked();
            }
            break;
        }
        case FILE_ACTION_MODIFIED: {
            int32_t idx = ResolvePathLocked(full);
            if (idx >= 0) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &fad)) {
                    Attr a;
                    a.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
                    a.mtime = FtToUnix((static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                                       fad.ftLastWriteTime.dwLowDateTime);
                    const Attr previous = AttrAt(idx);
                    const int32_t base = BaseCount();
                    if (idx >= base) live_.attrs[static_cast<size_t>(idx - base)] = a;
                    else {
                        Patch& pt = patches_[idx];
                        Node n = NodeAt(idx);
                        pt.parent = n.parent;
                        pt.flags = n.flags;
                        pt.has_meta = true;
                        pt.has_attr = true;
                        pt.attr = a;
                    }
                    if (previous.size != a.size || previous.mtime != a.mtime)
                        InvalidateFilterLocked();
                }
            }
            break;
        }
        case FILE_ACTION_RENAMED_OLD_NAME:
            pending_old = full;
            break;
        case FILE_ACTION_RENAMED_NEW_NAME: {
            int32_t idx = pending_old.empty() ? -1 : ResolvePathLocked(pending_old);
            if (idx >= 0) {
                auto slash = full.find_last_of(L"\\/");
                std::wstring_view new_name = (slash == std::wstring::npos)
                    ? std::wstring_view(full) : std::wstring_view(full).substr(slash + 1);
                const Node old = NodeAt(idx);
                const int32_t new_parent = slash == std::wstring::npos ? old.parent : EnsureChainLocked(live_, full.substr(0, slash), true, true);
                if (old.parent != new_parent) subtree_intervals_valid_ = false;
                ChildMapRemove(old.parent, NameOf(idx), idx);
                const int32_t base = BaseCount();
                if (idx >= base) {
                    Node& n = live_.nodes[static_cast<size_t>(idx - base)];
                    n.parent = new_parent;
                    pool_waste_ += n.len;
                    n.off = static_cast<uint32_t>(live_.pool.size());
                    n.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + n.len);
                } else {
                    Patch& pt = patches_[idx];
                    pt.parent = new_parent;
                    pt.flags = old.flags;
                    pt.has_meta = true;
                    if (pt.has_name) pool_waste_ += pt.len;
                    pt.has_name = true;
                    pt.off = static_cast<uint32_t>(live_.pool.size());
                    pt.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + pt.len);
                }
                ChildMapAdd(new_parent, new_name, idx);
                InvalidateFilterLocked();
            } else {
                EnsureChainLocked(live_, full, false, true);
                InvalidateFilterLocked();
            }
            pending_old.clear();
            break;
        }
        default: break;
        }
        if (info->Action != FILE_ACTION_RENAMED_OLD_NAME &&
            info->Action >= FILE_ACTION_ADDED && info->Action <= FILE_ACTION_RENAMED_NEW_NAME) {
            if (auto* delta = DeltaFor(0)) {
                // EnsureChain may create missing ancestors: persist each new ID
                // in order before records that refer to it.
                for (int32_t id = previous_count; id < LiveCount(); ++id) {
                    const auto node = NodeAt(id);
                    const auto attr = AttrAt(id);
                    delta->QueueAdd(id, node.parent, node.flags, attr.mtime, attr.size, NameOf(id), 0);
                }
                if (changed_id >= 0 && changed_id < previous_count) {
                    if (IsTomb(changed_id)) delta->QueueTomb(changed_id);
                    else {
                        const auto node = NodeAt(changed_id);
                        const auto attr = AttrAt(changed_id);
                        delta->QueuePatch(changed_id,
                            static_cast<uint8_t>(PatchBits::Meta) | static_cast<uint8_t>(PatchBits::Name) |
                            static_cast<uint8_t>(PatchBits::Attr), node.parent, node.flags,
                            attr.mtime, attr.size, NameOf(changed_id));
                    }
                }
            } else {
                rebuild_requested_ = true;
                SetStatus(L"索引更新尚未保存，需要重新扫描；请检查索引目录权限及磁盘空间");
            }
        }
        if (info->NextEntryOffset == 0) break;
        if (info->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) || info->NextEntryOffset > static_cast<size_t>(end - p)) { GapFeed(); break; }
        p += info->NextEntryOffset;
    }
    return applied;
}

} // namespace pulse::index
