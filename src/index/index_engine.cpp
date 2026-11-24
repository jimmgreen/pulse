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
#include <array>
#include <bit>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <filesystem>
#include <queue>
#include <numeric>
#include <iterator>
#include <limits>
#include <malloc.h>
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

VolumeBackend EffectiveBackend(const VolumeInfo& volume) {
    if (volume.backend != VolumeBackend::Unsupported) return volume.backend;
    return SelectVolumeBackend(volume.file_system);
}

std::wstring DeltaKeyForId(std::wstring_view id) {
    return NormalizeVolumeId(std::wstring(id));
}

bool SameVolumeEntry(const VolumeInfo& drive, std::wstring_view state_id,
                     VolumeBackend state_backend, VolumeFsTag state_fs_tag,
                     uint32_t state_serial) {
    return VolumeIdentityMatches(drive.id, drive.file_system, drive.serial,
                                 state_id, state_backend, state_fs_tag, state_serial);
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

// Before 1.0.57 every merge also wrote v9\volumes\<id>: a per-volume copy of the
// aggregate base of the same generation, read only as an alternative query
// view. Queries use the aggregate alone now; reclaim the copy once (about as
// large as the aggregate itself, twice with both A/B slots).
void RemoveLegacyVolumeShards() {
    if (!MachineIndexScope()) return;
    const std::wstring data_dir = DataDir();
    if (data_dir.empty()) return;
    const std::wstring root = data_dir + L"\\v9\\volumes";
    const DWORD attributes = GetFileAttributesW(root.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return;
    std::error_code error;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        std::filesystem::remove(root, error);  // never follow a link out of the index directory
    } else {
        std::filesystem::remove_all(root, error);
    }
    if (error) IndexFailure("filename_remove_volume_shards_failed", static_cast<DWORD>(error.value()), root);
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
    retained.shard_mapped_file_bytes = 0;  // volume shards removed; field kept for the JSON schema
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

void Engine::TrimIdleMemory() {
    const ULONGLONG last = last_search_tick_.load(std::memory_order_relaxed);
    if (!last) return;
    const ULONGLONG now = GetTickCount64();
    if (now < last || now - last < 30000) return;
    std::unique_lock<std::mutex> query_guard(query_mu_, std::try_to_lock);
    if (!query_guard.owns_lock()) return;
    std::unique_lock<std::shared_mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const ULONGLONG recheck = last_search_tick_.load(std::memory_order_relaxed);
    if (!recheck || now < recheck || now - recheck < 30000) return;
    last_search_tick_.store(0, std::memory_order_relaxed);
    const bool large = cache_set_.ids.capacity() > 1024 || cache_set_.bits.capacity() > 64 || !cache_raw_.empty();
    if (!large) return;
    std::vector<int32_t>().swap(cache_set_.ids);
    std::vector<uint64_t>().swap(cache_set_.bits);
    cache_set_.dense = false;
    cache_set_.total = 0;
    cache_set_.universe = 0;
    std::wstring().swap(cache_raw_);
    std::wstring().swap(cache_path_prefix_);
    cache_epoch_ = 0;
    lock.unlock();
    query_guard.unlock();
    _heapmin();
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
    const auto count = static_cast<uint64_t>(LiveCount());
    int32_t filter_root = -1;
    if (!root.empty()) {
        filter_root = ResolvePathLocked(root);
        if (filter_root < 0 && (!child_map_.empty() || (map_ && map_->child_order))) {
            page.next = count;
            page.done = true;
            return page;
        }
    }
    const bool check_base_spans = subtree_intervals_valid_ && !vols_.empty();
    uint64_t scan = cursor;
    for (; scan < count && scan < cursor + 8192 && page.records.size() < 512; ++scan) {
        const auto id = static_cast<int32_t>(scan);
        // Skip entire inactive/unrelated volume spans in O(1) when base intervals are valid.
        if (check_base_spans && id < BaseCount() && NodeAt(id).parent < 0) {
            const VolState* hit_vol = nullptr;
            int32_t span_hi = -1;
            for (const auto& v : vols_) {
                int32_t lo = -1, hi = -1;
                if (VolumeSpan(v, lo, hi) && id == lo) {
                    hit_vol = &v;
                    span_hi = hi;
                    break;
                }
            }
            if (hit_vol && (IsRootInactiveLocked(hit_vol->root_idx) ||
                            (filter_root >= 0 && !InSubtreeLocked(filter_root, hit_vol->root_idx) &&
                             !InSubtreeLocked(hit_vol->root_idx, filter_root)))) {
                const uint64_t skip_to = static_cast<uint64_t>(span_hi);
                if (skip_to > scan) {
                    scan = skip_to - 1;
                    continue;
                }
            }
        }
        if (IsTomb(id)) continue;
        bool hidden_root = false;
        for (int32_t inactive_root : inactive_volume_roots_) {
            if (InSubtreeLocked(id, inactive_root)) {
                hidden_root = true;
                break;
            }
        }
        if (hidden_root || (NodeAt(id).flags & (kFlagHidden | kFlagDir))) continue;
        if (filter_root >= 0 && !InSubtreeLocked(id, filter_root)) continue;
        const auto path = BuildPathLocked(id);
        if (path.empty()) { page.gap = true; continue; }
        if (IsExcludedPath(path) || (!root.empty() && !(path.size() >= root.size() &&
            CompareStringOrdinal(path.data(), static_cast<int>(root.size()), root.data(), static_cast<int>(root.size()), TRUE) == CSTR_EQUAL &&
            (path.size() == root.size() || root.back() == L'\\' || path[root.size()] == L'\\')))) continue;
        ChangeRecord record; record.path = path; record.kind = ChangeKind::Created; record.file_id = scan;
        page.records.push_back(std::move(record));
    }
    page.next = scan; page.done = scan >= count; return page;
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

void Engine::NotifyTopologyChange() {
    topology_dirty_ = true;
    if (change_signal_) SetEvent(change_signal_);
}

void Engine::QueueVolumeReconcile(std::wstring_view id) {
    if (id.empty()) return;
    const std::wstring norm = NormalizeVolumeId(std::wstring(id));
    if (norm.empty()) return;
    std::lock_guard<std::mutex> lock(reconcile_mu_);
    pending_reconcile_volumes_.insert(norm);
}

bool Engine::IsVolumeReconcilePending(std::wstring_view id) const {
    if (id.empty()) return false;
    const std::wstring norm = NormalizeVolumeId(std::wstring(id));
    if (norm.empty()) return false;
    std::lock_guard<std::mutex> lock(reconcile_mu_);
    return pending_reconcile_volumes_.contains(norm);
}

void Engine::ClearVolumeReconcile(std::wstring_view id) {
    if (id.empty()) return;
    const std::wstring norm = NormalizeVolumeId(std::wstring(id));
    std::lock_guard<std::mutex> lock(reconcile_mu_);
    pending_reconcile_volumes_.erase(norm);
}

std::vector<std::wstring> Engine::TakePendingReconcileVolumes() {
    std::vector<std::wstring> out;
    std::lock_guard<std::mutex> lock(reconcile_mu_);
    out.assign(pending_reconcile_volumes_.begin(), pending_reconcile_volumes_.end());
    pending_reconcile_volumes_.clear();
    return out;
}

DWORD CALLBACK Engine::VolumeInterfaceCallback(
    HCMNOTIFICATION, void* context, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA, DWORD) {
    if (context && (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL ||
                    action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)) {
        static_cast<Engine*>(context)->NotifyTopologyChange();
    }
    return ERROR_SUCCESS;
}

void Engine::RegisterVolumeNotifications() {
    if (volume_interface_notify_) return;
    static constexpr GUID kVolumeInterfaceGuid = {
        0x53F5630D, 0xB6BF, 0x11D0, {0x94, 0xF2, 0x00, 0xA0, 0xC9, 0x1E, 0xFB, 0x8B}};
    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    filter.u.DeviceInterface.ClassGuid = kVolumeInterfaceGuid;
    if (CM_Register_Notification(&filter, this, &Engine::VolumeInterfaceCallback,
                                 &volume_interface_notify_) != CR_SUCCESS) {
        volume_interface_notify_ = nullptr;
    }
}

void Engine::UnregisterVolumeNotifications() {
    if (volume_interface_notify_) {
        CM_Unregister_Notification(volume_interface_notify_);
        volume_interface_notify_ = nullptr;
    }
}

std::vector<VolumeInfo> Engine::Volumes() const {
    IndexConfig config;
    if (MachineIndexScope() && !LoadMachineConfig(config, nullptr)) return {};
    auto result = EnumerateLocalVolumes(config);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (auto& volume : result) {
        auto it = std::find_if(vols_.begin(), vols_.end(), [&](const VolState& state) {
            if (!state.volume_id.empty() && !volume.id.empty()) {
                return SameVolumeEntry(volume, state.volume_id, state.backend,
                                       state.fs_tag, state.serial);
            }
            return volume.mount_point.size() >= 2 && state.letter == towupper(volume.mount_point[0]);
        });
        const bool pending_reconcile = IsVolumeReconcilePending(volume.id);
        if (it != vols_.end()) {
            volume.indexed_items = it->item_count;
            volume.progress = 100;
            if (!volume.online) volume.state = L"离线 · 仍可搜索";
            else if (pending_reconcile) volume.state = L"已就绪 · 正在重新核对";
            else volume.state = L"已就绪";
        } else if (volume.enabled && (building_ || pending_reconcile)) {
            volume.state = L"正在建立索引";
        } else if (volume.enabled && ready_) {
            volume.state = L"索引失败";
            volume.error = EffectiveBackend(volume) == VolumeBackend::DirectoryWalk
                ? L"无法扫描该磁盘的目录内容"
                : L"无法读取该磁盘的文件变更记录";
        }
    }
    for (const auto& state : vols_) {
        const bool present = std::any_of(result.begin(), result.end(), [&](const VolumeInfo& volume) {
            return SameVolumeEntry(volume, state.volume_id, state.backend,
                                   state.fs_tag, state.serial);
        });
        if (present || state.kind != VolumeKind::Removable) continue;
        VolumeInfo offline;
        offline.id = state.volume_id;
        offline.mount_point = std::wstring(1, state.letter) + L":\\";
        offline.kind = state.kind;
        offline.backend = state.backend;
        offline.serial = state.serial;
        offline.file_system = std::wstring(FsNameFromTag(state.fs_tag, state.backend));
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
        const wchar_t target_letter = static_cast<wchar_t>(towupper(path[0]));
        bool has_active = false;
        bool has_offline = false;
        for (const auto& volume : vols_) {
            if (static_cast<wchar_t>(towupper(volume.letter)) != target_letter) continue;
            if (IsRootInactiveLocked(volume.root_idx)) has_offline = true;
            else has_active = true;
        }
        if (!has_active && has_offline) return ChangeState::Offline;
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
            bool inactive = false;
            for (int32_t root : inactive_volume_roots_) {
                if (IsUnderLocked(i, root)) { inactive = true; break; }
            }
            if (inactive) continue;
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
    topology_dirty_ = false;
    {
        std::lock_guard<std::mutex> rlock(reconcile_mu_);
        pending_reconcile_volumes_.clear();
    }
    running_ = true;
    ready_ = false;
    folder_size_background_ = true;
    change_signal_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    RegisterVolumeNotifications();
    thread_ = std::thread(&Engine::Worker, this);
}

void Engine::Stop() {
    running_ = false;
    if (change_signal_) SetEvent(change_signal_);
    if (thread_.joinable()) thread_.join();
    UnregisterVolumeNotifications();
    journal_streams_.clear();
    if (change_signal_) { CloseHandle(change_signal_); change_signal_ = nullptr; }
    StopPinyinWorker();
    changes_.Flush();
    StopWalkWatches();
    FlushDeltas();
    CloseDeltas();
    {
        std::lock_guard<std::mutex> rlock(reconcile_mu_);
        pending_reconcile_volumes_.clear();
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    pinyin_snapshot_ = nullptr;
    pinyin_version_ = 0;
    pinyin_postings_.Reset();
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
        if (parent < 0 && IsRootInactiveLocked(it->second)) continue;
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
            if (parent < 0 && IsRootInactiveLocked(idx)) continue;
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

bool Engine::IsRootInactiveLocked(int32_t root_idx) const {
    return root_idx >= 0 &&
        std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), root_idx) !=
            inactive_volume_roots_.end();
}

void Engine::CollectLiveSubtreeLocked(int32_t root, std::vector<int32_t>& out) const {
    out.clear();
    const int32_t total = LiveCount();
    if (root < 0 || root >= total || IsTomb(root)) return;
    const int32_t base_n = BaseCount();
    const bool has_child_order = map_ && map_->child_order && map_->nodes && base_n > 0;
    std::unordered_map<int32_t, std::vector<int32_t>> overlay;
    if (!has_child_order) {
        for (int32_t i = 0; i < base_n; ++i) {
            if (IsTomb(i)) continue;
            overlay[NodeAt(i).parent].push_back(i);
        }
    } else {
        for (const auto& [id, pt] : patches_) {
            if (id < 0 || id >= base_n || IsTomb(id)) continue;
            const int32_t cur_parent = pt.has_meta
                ? pt.parent
                : map_->nodes[static_cast<size_t>(id)].parent;
            if (cur_parent != map_->nodes[static_cast<size_t>(id)].parent) {
                overlay[cur_parent].push_back(id);
            }
        }
    }
    for (int32_t i = base_n; i < total; ++i) {
        if (IsTomb(i)) continue;
        overlay[NodeAt(i).parent].push_back(i);
    }

    std::vector<int32_t> stack{root};
    std::unordered_set<int32_t> seen;
    seen.insert(root);
    while (!stack.empty()) {
        const int32_t cur = stack.back();
        stack.pop_back();
        out.push_back(cur);
        if ((NodeAt(cur).flags & kFlagDir) == 0) continue;
        if (has_child_order && cur < base_n) {
            const int32_t* first = map_->child_order;
            const int32_t* last = first + map_->n;
            auto it = std::lower_bound(first, last, cur, [&](int32_t idx, int32_t value) {
                return map_->nodes[static_cast<size_t>(idx)].parent < value;
            });
            for (; it != last; ++it) {
                const int32_t child = *it;
                if (map_->nodes[static_cast<size_t>(child)].parent != cur) break;
                if (!IsTomb(child) && NodeAt(child).parent == cur && seen.insert(child).second) {
                    stack.push_back(child);
                }
            }
        }
        if (const auto ov = overlay.find(cur); ov != overlay.end()) {
            for (int32_t child : ov->second) {
                if (!IsTomb(child) && NodeAt(child).parent == cur && seen.insert(child).second) {
                    stack.push_back(child);
                }
            }
        }
    }
}

void Engine::RelocateVolumeLetterLocked(VolState& v, wchar_t new_letter) {
    const wchar_t upper = static_cast<wchar_t>(towupper(new_letter));
    const wchar_t old_letter = v.letter;
    if (!upper || v.root_idx < 0 || v.root_idx >= LiveCount() ||
        static_cast<wchar_t>(towupper(old_letter)) == upper) {
        return;
    }
    if (!v.volume_id.empty()) journal_streams_.erase(v.volume_id);
    ChildMapRemove(-1, NameOf(v.root_idx), v.root_idx);
    v.letter = upper;
    const std::wstring new_name = std::wstring(1, upper) + L":";
    const int32_t base = BaseCount();
    if (v.root_idx >= base) {
        Node& n = live_.nodes[static_cast<size_t>(v.root_idx - base)];
        pool_waste_ += n.len;
        n.off = static_cast<uint32_t>(live_.pool.size());
        n.len = static_cast<uint16_t>(new_name.size());
        live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.end());
    } else {
        const Node old = NodeAt(v.root_idx);
        Patch& pt = patches_[v.root_idx];
        pt.parent = -1;
        pt.flags = old.flags;
        pt.has_meta = true;
        if (pt.has_name) pool_waste_ += pt.len;
        pt.has_name = true;
        pt.off = static_cast<uint32_t>(live_.pool.size());
        pt.len = static_cast<uint16_t>(new_name.size());
        live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.end());
    }
    ChildMapAdd(-1, new_name, v.root_idx);
    DeltaLog* delta = DeltaForVolume(v.volume_id, old_letter ? old_letter : upper);
    if (delta) {
        const Node node = NodeAt(v.root_idx);
        const Attr attr = AttrAt(v.root_idx);
        delta->QueuePatch(v.root_idx,
            static_cast<uint8_t>(PatchBits::Meta) | static_cast<uint8_t>(PatchBits::Name) |
            static_cast<uint8_t>(PatchBits::Attr),
            -1, node.flags, attr.mtime, attr.size, new_name);
    }
    RefreshSubtreeVisibilityLocked(v.root_idx, delta);
    GapFeed();
    InvalidateFilterLocked();
    ++struct_changes_;
    last_struct_tick_ = GetTickCount64();
    ++revision_;
}

void Engine::UpdateVolumeVisibilityLocked(const std::vector<VolumeInfo>& active, bool only_hide) {
    std::vector<int32_t> inactive;
    for (const auto& state : vols_) {
        const bool on = std::any_of(active.begin(), active.end(), [&](const VolumeInfo& volume) {
            if (!state.volume_id.empty()) {
                return SameVolumeEntry(volume, state.volume_id, state.backend,
                                       state.fs_tag, state.serial);
            }
            return volume.mount_point.size() >= 2 &&
                towupper(volume.mount_point[0]) == towupper(state.letter);
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
    // A conjunctive query such as "report type:image" or "report dm:today"
    // still requires every hit's name to contain its literal name term, so
    // that term's postings bound the candidates; the remaining filters are
    // verified per candidate instead of scanning every record.
    size_t driver = SIZE_MAX;
    uint32_t driver_bucket = 0;
    uint32_t driver_count = UINT32_MAX;
    bool has_second_bucket = false;
    uint32_t second_bucket = 0;
    uint32_t second_count = UINT32_MAX;
    if (map_ && map_->n > 0 && cq.groups.size() == 1) {
        const auto& group = cq.groups[0];
        auto bucket_span = [&](const uint32_t* starts, uint32_t bucket) -> uint32_t {
            if (!starts) return UINT32_MAX;
            const uint32_t a = starts[bucket], b = starts[bucket + 1];
            return (a <= b && b <= starts[kPrefixBuckets]) ? (b - a) : UINT32_MAX;
        };
        for (size_t i = 0; i < group.size(); ++i) {
            const Term& t = group[i];
            if (t.name_not || t.name_in_path || t.name.empty() ||
                (t.name_how != NameHow::Substring && t.name_how != NameHow::Exact)) continue;
            if (t.name.size() == 1) {
                if (!map_->prefix1_all_chars || !map_->prefix1_start || !map_->prefix1_ids) continue;
                const uint32_t b = PrefixChar(t.name);
                const uint32_t cnt = bucket_span(map_->prefix1_start, b);
                if (cnt != UINT32_MAX && (driver == SIZE_MAX || cnt < driver_count)) {
                    driver = i;
                    driver_bucket = b;
                    driver_count = cnt;
                }
                continue;
            }
            if (!map_->prefix2_start || !map_->prefix2_ids) continue;
            const size_t max_k = map_->prefix1_all_chars ? (t.name.size() - 1) : 1;
            for (size_t k = 0; k < max_k; ++k) {
                const uint32_t b = PairBucket(std::wstring_view(t.name).substr(k, 2));
                const uint32_t cnt = bucket_span(map_->prefix2_start, b);
                if (cnt == UINT32_MAX) continue;
                if (driver == SIZE_MAX || cnt < driver_count) {
                    if (driver != SIZE_MAX && !group[driver].pinyin &&
                        group[driver].name.size() >= 2 && driver_bucket != b &&
                        (!has_second_bucket || driver_count < second_count)) {
                        has_second_bucket = true;
                        second_bucket = driver_bucket;
                        second_count = driver_count;
                    }
                    driver = i;
                    driver_bucket = b;
                    driver_count = cnt;
                } else if (driver != SIZE_MAX && group[driver].name.size() >= 2 &&
                           b != driver_bucket && (!t.pinyin || i == driver) &&
                           (!has_second_bucket || cnt < second_count)) {
                    has_second_bucket = true;
                    second_bucket = b;
                    second_count = cnt;
                }
            }
        }
        // Reset or populate the second bigram consistently after picking driver.
        if (driver != SIZE_MAX) {
            if (group[driver].name.size() == 1) {
                has_second_bucket = false;
            } else if (!has_second_bucket && map_->prefix1_all_chars && group[driver].name.size() >= 3) {
                for (size_t k = 0; k + 1 < group[driver].name.size(); ++k) {
                    const uint32_t b = PairBucket(std::wstring_view(group[driver].name).substr(k, 2));
                    if (b == driver_bucket) continue;
                    const uint32_t cnt = bucket_span(map_->prefix2_start, b);
                    if (cnt != UINT32_MAX && (!has_second_bucket || cnt < second_count)) {
                        has_second_bucket = true;
                        second_bucket = b;
                        second_count = cnt;
                    }
                }
            }
        }
    }
    if (driver != SIZE_MAX) {
        const auto& term = cq.groups[0][driver];
        CompiledQuery literal_cq = cq;
        literal_cq.groups[0][driver].pinyin = false;
        const bool single_char = term.name.size() == 1;
        const uint32_t bucket = driver_bucket;
        const int32_t subtree_end = (prefix_node >= 0) ? SubtreeEndLocked(prefix_node) : -1;
        // Overlay visibility changes are written as patches (see
        // RefreshSubtreeVisibilityLocked), so an id without a patch or tombstone
        // still matches exactly as its mapped node says, even with a backlog.
        const bool direct_literal = QueryIsSimpleName(cq) &&
            term.name_how == NameHow::Substring && prefix_node < 0 && !folders_only &&
            inactive_volume_roots_.empty();
        auto collect_table = [&](const MappedFile& mapped, int32_t global_first) {
            const uint32_t* starts = single_char ? mapped.prefix1_start : mapped.prefix2_start;
            const int32_t* postings = single_char ? mapped.prefix1_ids : mapped.prefix2_ids;
            if (!starts || !postings || (single_char && !mapped.prefix1_all_chars)) return;
            uint32_t first = starts[bucket];
            uint32_t last = starts[bucket + 1];
            if (first > last || last > starts[kPrefixBuckets]) return;
            const bool can_slice_subtree = global_first == 0 && subtree_end > prefix_node && patches_.empty();
            const int32_t slice_lo = prefix_node;
            const int32_t slice_hi = can_slice_subtree ? (std::min)(subtree_end, BaseCount()) : INT32_MAX;
            if (can_slice_subtree) {
                first = static_cast<uint32_t>(
                    std::lower_bound(postings + first, postings + last, slice_lo) - postings);
                last = static_cast<uint32_t>(
                    std::lower_bound(postings + first, postings + last, slice_hi) - postings);
            }
            const int32_t* postings2 = nullptr;
            uint32_t p2 = 0;
            uint32_t last2 = 0;
            if (!single_char && has_second_bucket && second_bucket != bucket &&
                mapped.prefix2_start && mapped.prefix2_ids) {
                uint32_t f2 = mapped.prefix2_start[second_bucket];
                uint32_t l2 = mapped.prefix2_start[second_bucket + 1];
                if (f2 <= l2 && l2 <= mapped.prefix2_start[kPrefixBuckets]) {
                    if (can_slice_subtree) {
                        f2 = static_cast<uint32_t>(
                            std::lower_bound(mapped.prefix2_ids + f2, mapped.prefix2_ids + l2, slice_lo) - mapped.prefix2_ids);
                        l2 = static_cast<uint32_t>(
                            std::lower_bound(mapped.prefix2_ids + f2, mapped.prefix2_ids + l2, slice_hi) - mapped.prefix2_ids);
                    }
                    const uint32_t len1 = last - first;
                    const uint32_t len2 = l2 - f2;
                    if (len1 > 64 && len2 <= len1 * 32) {
                        postings2 = mapped.prefix2_ids;
                        p2 = f2;
                        last2 = l2;
                    }
                }
            }
            for (uint32_t p = first; p < last; ++p) {
                if ((p & 0x3ffu) == 0 && latest && latest->load() != expected) return;
                const int32_t local_id = postings[p];
                if (postings2) {
                    if (p2 >= last2) break;
                    if (postings2[p2] < local_id) {
                        uint32_t step = 1;
                        while (p2 + step < last2 && postings2[p2 + step] < local_id) {
                            p2 += step;
                            step <<= 1;
                        }
                        const uint32_t hi = (std::min)(last2, p2 + step + 1);
                        p2 = static_cast<uint32_t>(
                            std::lower_bound(postings2 + p2, postings2 + hi, local_id) - postings2);
                    }
                    if (p2 >= last2) break;
                    if (postings2[p2] != local_id) continue;
                }
                if (local_id < 0 || static_cast<uint32_t>(local_id) >= mapped.n ||
                    global_first < 0 || local_id >= BaseCount() - global_first) continue;
                const int32_t id = global_first + local_id;
                if (id < 0 || id >= BaseCount()) continue;
                // Postings depend only on the snapshot name, so they still bound
                // ids whose patch kept that name; renamed ids are checked below.
                const auto patch = patches_.find(id);
                if (patch != patches_.end() && patch->second.has_name) continue;
                if (direct_literal && patch == patches_.end() && !tombstones_.contains(id)) {
                    if (local_id < 0 || local_id >= static_cast<int32_t>(mapped.n)) continue;
                    const Node& node = mapped.nodes[static_cast<size_t>(local_id)];
                    if (node.parent >= 0 && !(node.flags & (kFlagHidden | kFlagDeleted)) &&
                        (single_char || ContainsFolded(mapped.pool + node.off, node.len, term.name)))
                        ids.Add(id);
                } else if (MatchNodeLocked(id, literal_cq, prefix_node, folders_only, use_attrs)) {
                    ids.Add(id);
                }
            }
        };
        collect_table(*map_, 0);
        if (term.pinyin && pinyin_ready_.load() && pinyin_snapshot_ == map_.get() &&
            pinyin_version_ == kPinyinDataVersion) {
            std::span<const int32_t> candidates = PinyinChineseIdsLocked();
            // Every adjacent Latin pair in a successful match must exist in the
            // pronunciation graph. Pick its smallest posting without expanding paths.
            for (size_t i = 1; i < term.name.size(); ++i) {
                const wchar_t a = FoldChar(term.name[i - 1]), b = FoldChar(term.name[i]);
                if (a < L'a' || a > L'z' || b < L'a' || b > L'z') continue;
                const auto posting = PinyinPairIdsLocked(static_cast<size_t>((a - L'a') * 26 + (b - L'a')));
                if (posting.size() < candidates.size()) candidates = posting;
            }
            size_t checked = 0;
            for (int32_t id : candidates) {
                if ((checked++ & 0x3ff) == 0 && latest && latest->load() != expected) return;
                const auto patch = patches_.find(id);
                if (patch != patches_.end() && patch->second.has_name) continue; // renamed: below
                const auto name = NameOf(id);
                // Literal matches were already visited by the disk postings.
                if (ContainsFolded(name.data(), static_cast<uint32_t>(name.size()), term.name)) continue;
                if (MatchNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
            }
        }
        // The query has one AND group, so every hit also matches the driver
        // term by name; MatchNodeLocked still decides each candidate.
        auto driver_name_matches = [&](int32_t id) {
            const std::wstring_view name = NameOf(id);
            return MatchName(name.data(), static_cast<uint32_t>(name.size()), term);
        };
        for (int32_t id = BaseCount(); id < n; ++id) {
            if (driver_name_matches(id) &&
                MatchNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
        }
        for (const auto& [id, patch] : patches_) {
            // Patches that kept the snapshot name were visited through postings.
            if (!patch.has_name || id < 0 || id >= BaseCount()) continue;
            if (driver_name_matches(id) &&
                MatchNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) ids.Add(id);
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
        if (MatchNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
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
                if (MatchNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
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
        if (MatchNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
            out.Add(i);
    });
}

void Engine::SortIdsLocked(std::vector<int32_t>& ids, ResultSort sort, bool desc) const {
    if (ids.size() <= 1 || sort == ResultSort::Index) {
        if (desc && sort == ResultSort::Index) std::reverse(ids.begin(), ids.end());
        return;
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
    last_search_tick_.store(GetTickCount64(), std::memory_order_relaxed);
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

    MatchSet collected;
    const bool same_scope = cache_path_prefix_ == prefix &&
        cache_folders_only_ == q.folders_only;
    const bool exact_cached_page = cache_epoch_ == filter_epoch_ && same_scope &&
        cache_raw_ == q.needle && cache_set_.universe == LiveCount();
    // Keep the established materialized path for comparison-sorted pages.
    // Borrow only paths whose speedup is supported by the paired benchmarks.
    const bool borrow_cached_page = exact_cached_page &&
        (q.rank || q.sort == ResultSort::Index || cap == 0);
    if (exact_cached_page && !borrow_cached_page) {
        collected = cache_set_;
    } else if (!exact_cached_page) {
        if (cache_epoch_ == filter_epoch_ && same_scope &&
            QueryCanNarrow(cache_raw_, q.needle) && cache_set_.universe == LiveCount()) {
            NarrowMatchesLocked(cq, prefix_node, q.folders_only, use_attrs, cache_set_, collected,
                                latest, expected);
        } else {
            CollectMatchesLocked(cq, prefix_node, q.folders_only, use_attrs, collected, latest, expected);
        }
    }
    if (latest && latest->load() != expected) return out;
    // query_mu_ serializes cache users and mutex_ excludes index mutation for
    // this whole call. Exact pages can borrow the caller-independent matches.
    const MatchSet& candidates = borrow_cached_page ? cache_set_ : collected;
    struct Ranked {
        int32_t id = -1;
        int score = 0;
    };
    auto better = [](const Ranked& a, const Ranked& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.id < b.id;
    };
    const bool rank_single_char = QueryIsSimpleName(cq) &&
        cq.groups[0][0].name_how == NameHow::Substring &&
        cq.groups[0][0].name.size() == 1;
    const wchar_t rank_char = rank_single_char ? cq.groups[0][0].name[0] : 0;
    const bool rank_from_base = map_ && live_.nodes.empty() && patches_.empty() &&
        tombstones_.empty();
    auto score_of = [&](int32_t id) {
        Node n{};
        std::wstring_view nm;
        if (rank_from_base && id >= 0 && id < BaseCount()) {
            n = map_->nodes[static_cast<size_t>(id)];
            nm = { map_->pool + n.off, n.len };
        } else {
            n = NodeAt(id);
            nm = NameOf(id);
        }
        return rank_single_char
            ? RankSingleCharMatch(nm.data(), static_cast<uint32_t>(nm.size()),
                                  (n.flags & kFlagDir) != 0, rank_char)
            : RankName(nm.data(), static_cast<uint32_t>(nm.size()),
                       (n.flags & kFlagDir) != 0, cq);
    };
    // Caller filter (#66). The shared cache keeps the caller-independent set;
    // only this result is narrowed to the folders the caller may list.
    MatchSet visible;
    const bool filtered = visibility && candidates.total;
    if (filtered) {
        visible.universe = candidates.universe;
        std::vector<int32_t> unchecked;
        int32_t last_parent = INT32_MIN;
        int last_state = 0;
        candidates.ForEach([&](int32_t id) {
            // Ids follow the folder layout, so siblings usually arrive together.
            const int32_t parent = NodeAt(id).parent;
            if (parent != last_parent) {
                last_parent = parent;
                last_state = parent < 0 ? 1 : visibility->State(parent);
                if (last_state < 0) unchecked.push_back(parent);
            }
            if (last_state > 0) visible.ids.push_back(id);
        });
        if (!unchecked.empty() && q.rank && cap > 0 && visibility->TopFirst()) {
            // A broad first query can touch thousands of folders, each one a
            // kernel open (a disk seek on a cold hard disk). Rank every name
            // not known to be hidden: only the folders of the best ones decide
            // this page. When those are all known, the page is final and the
            // total counts confirmed names only; the host checks the rest.
            const size_t heap_n = q.offset + cap;
            std::priority_queue<Ranked, std::vector<Ranked>, decltype(better)> top(better);
            candidates.ForEach([&](int32_t id) {
                if (latest && latest->load() != expected) return;
                const int32_t parent = NodeAt(id).parent;
                if (parent >= 0 && visibility->State(parent) == 0) return;
                const Ranked candidate{ id, score_of(id) };
                if (top.size() < heap_n) {
                    top.push(candidate);
                } else if (better(candidate, top.top())) {
                    top.pop();
                    top.push(candidate);
                }
            });
            if (latest && latest->load() != expected) return SearchResult{};
            std::vector<int32_t> deciding;
            for (; !top.empty(); top.pop()) {
                const int32_t parent = NodeAt(top.top().id).parent;
                if (parent >= 0 && visibility->State(parent) < 0) deciding.push_back(parent);
            }
            if (deciding.empty()) out.partial = true;
            unchecked = std::move(deciding);
        }
        if (!unchecked.empty()) {
            std::sort(unchecked.begin(), unchecked.end());
            unchecked.erase(std::unique(unchecked.begin(), unchecked.end()), unchecked.end());
            out.unchecked_dirs.reserve(unchecked.size());
            for (int32_t dir : unchecked) out.unchecked_dirs.emplace_back(dir, BuildPathLocked(dir));
            cache_raw_ = q.needle;
            cache_path_prefix_ = prefix;
            cache_folders_only_ = q.folders_only;
            if (!borrow_cached_page) cache_set_ = std::move(collected);
            cache_epoch_ = filter_epoch_;
            return out;
        }
        visible.total = visible.ids.size();
    }
    const MatchSet& matches = filtered ? visible : candidates;
    out.total = matches.total;
    if (cap == 0 || matches.total == 0) {
        cache_raw_ = q.needle;
        cache_path_prefix_ = prefix;
        cache_folders_only_ = q.folders_only;
        cache_ranked_ = q.rank;
        cache_sort_ = q.sort;
        cache_sort_desc_ = q.sort_desc;
        if (!borrow_cached_page) cache_set_ = candidates;
        cache_epoch_ = filter_epoch_;
        return out;
    }

    cache_raw_ = q.needle;
    cache_path_prefix_ = prefix;
    cache_folders_only_ = q.folders_only;
    cache_ranked_ = q.rank;
    cache_sort_ = q.rank ? ResultSort::Index : q.sort;
    cache_sort_desc_ = q.rank ? false : q.sort_desc;
    if (!borrow_cached_page) cache_set_ = candidates;
    cache_epoch_ = filter_epoch_;

    const size_t start = (std::min)(q.offset, matches.total);
    const size_t want = cap;
    std::vector<int32_t> chosen;
    chosen.reserve((std::min)(want, matches.total));
    if (q.rank) {
        std::priority_queue<Ranked, std::vector<Ranked>, decltype(better)> top(better);
        const size_t heap_n = start + want;
        matches.ForEach([&](int32_t id) {
            if (latest && latest->load() != expected) return;
            Ranked candidate{ id, score_of(id) };
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
    } else if (q.sort == ResultSort::Index) {
        // Index order is already represented by ids / ascending set bits.
        // Skip complete bitmap words and collect only this page, including in
        // reverse order, instead of materializing every match for 48 results.
        const size_t page_size = (std::min)(want, matches.total - start);
        if (!matches.dense) {
            for (size_t k = 0; k < page_size; ++k) {
                const size_t position = q.sort_desc ? matches.ids.size() - 1 - start - k : start + k;
                chosen.push_back(matches.ids[position]);
            }
        } else if (page_size) {
            size_t skip = start;
            auto take_word = [&](size_t word) {
                uint64_t bits = matches.bits[word];
                const size_t count = static_cast<size_t>(std::popcount(bits));
                if (skip >= count) { skip -= count; return; }
                while (bits && chosen.size() < page_size) {
                    const unsigned bit = q.sort_desc
                        ? 63u - static_cast<unsigned>(std::countl_zero(bits))
                        : static_cast<unsigned>(std::countr_zero(bits));
                    if (skip) --skip;
                    else chosen.push_back(static_cast<int32_t>(word * 64 + bit));
                    bits &= ~(uint64_t{1} << bit);
                }
            };
            if (q.sort_desc) {
                for (size_t word = matches.bits.size(); word > 0 && chosen.size() < page_size;)
                    take_word(--word);
            } else {
                for (size_t word = 0; word < matches.bits.size() && chosen.size() < page_size; ++word)
                    take_word(word);
            }
        }
    } else {
        std::vector<int32_t> ids;
        if (!matches.dense) {
            ids = matches.ids;
        } else {
            ids.reserve(matches.total);
            matches.ForEach([&](int32_t id) { ids.push_back(id); });
        }
        PartialSortPage(ids, start, want, q.sort, q.sort_desc);
        const size_t end = (std::min)(ids.size(), start + want);
        for (size_t k = start; k < end; ++k) chosen.push_back(ids[k]);
    }

    out.hits.reserve(chosen.size());
    for (size_t k = 0; k < chosen.size(); ++k) {
        const Node n = NodeAt(chosen[k]);
        const Attr a = AttrAt(chosen[k]);
        Hit h;
        h.path = BuildPathLocked(chosen[k]);
        if (h.path.empty()) continue;
        h.name.assign(NameOf(chosen[k]));
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
    // Read access is required to map the posting region for writing.
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
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
    // Posting tables are counted here and filled straight into a mapped view of
    // the output file below, so the heap never holds both id tables (about 35
    // ids per node) next to the flattened snapshot.
    // Each bucket is reported once per name. Fusing char folding + prefix1 +
    // prefix2 passes avoids redundant scanning and 4x case-folding per node.
    std::vector<uint32_t> seen1(kPrefixBuckets, 0);
    std::vector<uint32_t> seen2(kPrefixBuckets, 0);
    uint32_t epoch = 0;
    std::wstring folded_heap;
    auto fold_into = [&](std::wstring_view name, wchar_t* stack_buf, size_t stack_cap) -> const wchar_t* {
        if (name.size() <= stack_cap) {
            for (size_t k = 0; k < name.size(); ++k) stack_buf[k] = FoldChar(name[k]);
            return stack_buf;
        }
        folded_heap.resize(name.size());
        for (size_t k = 0; k < name.size(); ++k) folded_heap[k] = FoldChar(name[k]);
        return folded_heap.data();
    };
    auto next_epoch = [&] {
        if (++epoch == 0) {
            std::fill(seen1.begin(), seen1.end(), 0u);
            std::fill(seen2.begin(), seen2.end(), 0u);
            epoch = 1;
        }
        return epoch;
    };
    std::vector<uint32_t> counts1(kPrefixBuckets, 0);
    std::vector<uint32_t> counts2(kPrefixBuckets, 0);
    wchar_t stack_buf[256];
    for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i) {
        const std::wstring_view name = name_of(i);
        const wchar_t* folded = fold_into(name, stack_buf, ARRAYSIZE(stack_buf));
        const uint32_t ep = next_epoch();
        const size_t nlen = name.size();
        for (size_t k = 0; k < nlen; ++k) {
            const uint32_t b1 = static_cast<uint16_t>(folded[k]);
            if (seen1[b1] != ep) {
                seen1[b1] = ep;
                ++counts1[b1];
            }
        }
        for (size_t k = 0; k + 1 < nlen; ++k) {
            const uint32_t b2 = (static_cast<uint32_t>(folded[k]) * 131u + folded[k + 1]) & 65535u;
            if (seen2[b2] != ep) {
                seen2[b2] = ep;
                ++counts2[b2];
            }
        }
    }
    auto build_starts = [&](const std::vector<uint32_t>& counts) {
        std::vector<uint32_t> start(kPrefixBuckets + 1, 0);
        uint64_t posting_count = 0;
        for (uint32_t i = 0; i < kPrefixBuckets; ++i) {
            posting_count += counts[i];
            if (posting_count > UINT32_MAX) {
                prefix_overflow = true;
                return std::vector<uint32_t>{};
            }
            start[i + 1] = static_cast<uint32_t>(posting_count);
        }
        return start;
    };
    const auto prefix1_start = build_starts(counts1);
    const auto prefix2_start = prefix_overflow ? std::vector<uint32_t>{} : build_starts(counts2);
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
    off = AlignUp(off + prefix1_start.size() * sizeof(uint32_t) +
                         uint64_t{prefix1_start.back()} * sizeof(int32_t), 16);
    hdr.prefix2_off = off;
    off = AlignUp(off + prefix2_start.size() * sizeof(uint32_t) +
                         uint64_t{prefix2_start.back()} * sizeof(int32_t), 16);
    hdr.vols_off = off;
    off = AlignUp(off + sizeof(DiskVol) * vols.size(), 16);
    hdr.frn_off = off;
    uint64_t frn_count = 0;
    for (const auto& v : vols) frn_count += v.frn_build.size();
    hdr.frn_count = static_cast<uint32_t>(frn_count);

    // Order: (parent, folded name, id); roots (parent -1, validated above) first.
    // A stable bucket pass by parent keeps ids ascending per parent. FlattenLocked
    // already emits siblings in (folded name, id) order, so normally only the
    // root group needs sorting; any other unordered group is sorted as before.
    std::vector<int32_t> child_order(s.nodes.size());
    {
        const size_t count = s.nodes.size();
        auto group_of = [&](size_t id) {
            const int32_t parent = s.nodes[id].parent;
            return parent < 0 ? size_t{0} : static_cast<size_t>(parent) + 1;
        };
        std::vector<uint32_t> group_end(count + 2, 0);
        for (size_t i = 0; i < count; ++i) ++group_end[group_of(i) + 1];
        for (size_t g = 1; g < group_end.size(); ++g) group_end[g] += group_end[g - 1];
        for (size_t i = 0; i < count; ++i)
            child_order[group_end[group_of(i)]++] = static_cast<int32_t>(i);
        auto sibling_less = [&](int32_t a, int32_t b) {
            const int c = CompareFolded(name_of(a), name_of(b));
            return c != 0 ? c < 0 : a < b;
        };
        uint32_t group_begin = 0;
        for (size_t g = 0; g + 1 < group_end.size(); ++g) {
            const auto first = child_order.begin() + group_begin;
            const auto last = child_order.begin() + group_end[g];
            if (last - first > 1 && !std::is_sorted(first, last, sibling_less))
                std::sort(first, last, sibling_less);
            group_begin = group_end[g];
        }
    }

    filename_timing_.Memory().Capture(IndexMemoryPoint::WriteSorted);
    DWORD write_error = ERROR_SUCCESS;
    auto checked = [&](bool result) {
        if (!result) write_error = GetLastError();
        return result;
    };
    // Size the file once, then fill the posting region through a mapped view.
    // Its pages are file-backed, so they add no private commit and the memory
    // manager can write them out under pressure. Cached writes below share the
    // same file cache, and untouched alignment gaps read back as zeros.
    const uint64_t file_size = hdr.frn_off + frn_count * sizeof(DiskFrn);
    auto seek = [&](uint64_t position) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(position);
        return checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
    };
    bool ok = seek(file_size) && checked(SetEndOfFile(h) != FALSE);
    if (ok) {
        SYSTEM_INFO system_info{};
        GetSystemInfo(&system_info);
        const uint64_t granularity = system_info.dwAllocationGranularity;
        const uint64_t view_begin = prefix1_position / granularity * granularity;
        HANDLE section = CreateFileMappingW(h, nullptr, PAGE_READWRITE, 0, 0, nullptr);
        ok = checked(section != nullptr);
        if (ok) {
            auto* view = static_cast<BYTE*>(MapViewOfFile(
                section, FILE_MAP_WRITE, static_cast<DWORD>(view_begin >> 32),
                static_cast<DWORD>(view_begin & 0xFFFFFFFFull),
                static_cast<SIZE_T>(hdr.vols_off - view_begin)));
            ok = checked(view != nullptr);
            if (ok) {
                BYTE* at1 = view + (prefix1_position - view_begin);
                memcpy(at1, prefix1_start.data(), prefix1_start.size() * sizeof(uint32_t));
                int32_t* ids1 = reinterpret_cast<int32_t*>(at1 + prefix1_start.size() * sizeof(uint32_t));
                BYTE* at2 = view + (hdr.prefix2_off - view_begin);
                memcpy(at2, prefix2_start.data(), prefix2_start.size() * sizeof(uint32_t));
                int32_t* ids2 = reinterpret_cast<int32_t*>(at2 + prefix2_start.size() * sizeof(uint32_t));
                std::vector<uint32_t> cursor1(prefix1_start.begin(), prefix1_start.end() - 1);
                std::vector<uint32_t> cursor2(prefix2_start.begin(), prefix2_start.end() - 1);
                for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i) {
                    const std::wstring_view name = name_of(i);
                    const wchar_t* folded = fold_into(name, stack_buf, ARRAYSIZE(stack_buf));
                    const uint32_t ep = next_epoch();
                    const size_t nlen = name.size();
                    for (size_t k = 0; k < nlen; ++k) {
                        const uint32_t b1 = static_cast<uint16_t>(folded[k]);
                        if (seen1[b1] != ep) {
                            seen1[b1] = ep;
                            ids1[cursor1[b1]++] = i;
                        }
                    }
                    for (size_t k = 0; k + 1 < nlen; ++k) {
                        const uint32_t b2 = (static_cast<uint32_t>(folded[k]) * 131u + folded[k + 1]) & 65535u;
                        if (seen2[b2] != ep) {
                            seen2[b2] = ep;
                            ids2[cursor2[b2]++] = i;
                        }
                    }
                }
                filename_timing_.Memory().Capture(IndexMemoryPoint::WritePrefix1);
                filename_timing_.Memory().Capture(IndexMemoryPoint::WritePrefix2);
                ok = checked(UnmapViewOfFile(view) != FALSE);
            }
            CloseHandle(section);
        }
    }
    std::vector<BYTE> pad(128, 0);
    memcpy(pad.data(), &hdr, sizeof(hdr));
    if (ok) ok = seek(0);
    if (ok) ok = checked(WriteAll(h, pad.data(), pad.size()));
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
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.vols_off);
        ok = checked(SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0);
    }
    std::vector<DiskVol> dvols(vols.size());
    uint64_t frn_cur = hdr.frn_off;
    for (size_t i = 0; i < vols.size(); ++i) {
        dvols[i].letter = static_cast<uint16_t>(vols[i].letter);
        dvols[i].kind = PackDiskVolKind(vols[i].kind, vols[i].backend, vols[i].fs_tag);
        dvols[i].item_count = vols[i].item_count;
        if (vols[i].backend == VolumeBackend::DirectoryWalk) {
            dvols[i].journal_id = 0;
            dvols[i].next_usn = static_cast<int64_t>(vols[i].serial);
        } else {
            dvols[i].journal_id = vols[i].journal_id;
            dvols[i].next_usn = vols[i].next_usn;
        }
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
        uint32_t failure_stage = 0;
        if (!PublishShardBase(paths, tmp, built_unix_, 0, published, nullptr, {}, &failure_stage)) {
            const DWORD error = GetLastError();
            diagnostics::runtime::Event("filename_publish_stage_failed", {{"operation", index_operation},
                {"error", error}, {"stage", failure_stage}}, IndexErrorLevel(error));
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
    pinyin_postings_.Reset();
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
            UnpackDiskVolMeta(d.kind, d.journal_id, d.next_usn,
                              v.kind, v.backend, v.fs_tag, v.serial);
            v.item_count = d.item_count;
            v.volume_id = d.volume_id;
            v.journal_id = (v.backend == VolumeBackend::DirectoryWalk) ? 0 : d.journal_id;
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
    InvalidateFilterLocked();
    RequestPinyinBuildLocked();
}

void Engine::PreserveOfflineVolumesLocked(const std::vector<VolumeInfo>& active,
                                          const IndexConfig& config) {
    const int32_t n = LiveCount();
    if (n <= 0) return;
    std::vector<int32_t> remap(static_cast<size_t>(n), -1);
    for (const auto& old : vols_) {
        if (old.kind != VolumeKind::Removable || config.IsExcluded(old.volume_id)) continue;
        const bool online = std::any_of(active.begin(), active.end(), [&](const VolumeInfo& volume) {
            return SameVolumeEntry(volume, old.volume_id, old.backend, old.fs_tag, old.serial);
        });
        if (online || old.root_idx < 0) continue;
        std::fill(remap.begin(), remap.end(), -1);
        VolState kept = old;
        kept.frn_base = nullptr;
        kept.frn_base_n = 0;
        kept.frn_new.clear();
        kept.frn_new_tail.clear();
        kept.frn_new_prev.clear();
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
            if (i == old.root_idx || excluded_parent || ShouldSkipChild(source.parent == old.root_idx, name) ||
                IsExcludedPath(BuildPathLocked(i))) node.flags |= kFlagHidden;
            build_.pool.insert(build_.pool.end(), name.begin(), name.begin() + node.len);
            remap[static_cast<size_t>(i)] = static_cast<int32_t>(build_.nodes.size());
            build_.nodes.push_back(node);
            build_.attrs.push_back(AttrAt(i));
            ++kept.item_count;
        }
        if (old.root_idx >= n || remap[static_cast<size_t>(old.root_idx)] < 0) continue;
        kept.root_idx = remap[static_cast<size_t>(old.root_idx)];
        kept.first_idx = kept.root_idx;
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

    // Reserve the pool exactly: a guessed size made the vector regrow while the
    // old buffer was still alive (about 84 MiB transient on 2.7 M nodes).
    size_t pool_chars = 0;
    std::vector<uint32_t> child_count(static_cast<size_t>(n), 0);
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
        pool_chars += NameOf(i).size();
        const int32_t p = NodeAt(i).parent;
        if (p >= 0 && p < n) ++child_count[static_cast<size_t>(p)];
    }
    out.pool.reserve(pool_chars);
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
    // Everything reachable from a root now has a slot. A live node left over
    // lost its ancestor chain (a tombstoned directory whose child delete never
    // arrived, or an out-of-range/cyclic parent). Emitting it as a parentless
    // root writes a FileRoot that fails hierarchy validation, so every later
    // merge fails as well and the WAL grows until startup replay takes hours.
    // NTFS cannot delete a non-empty directory, so such an orphan is stale:
    // drop it together with its subtree.
    uint64_t orphan_nodes = 0, orphan_roots = 0;
    int32_t first_orphan = -1;
    for (int32_t i = 0; i < n; ++i) {
        if (remap[static_cast<size_t>(i)] >= 0 || IsTomb(i)) continue;
        ++orphan_nodes;
        const int32_t p = NodeAt(i).parent;
        if (p < 0 || p >= n || IsTomb(p)) {
            ++orphan_roots;
            if (first_orphan < 0) first_orphan = i;
        }
    }
    if (orphan_nodes)
        diagnostics::runtime::Event("index_flatten_orphans_dropped", {{"operation", index_operation},
            {"nodes", orphan_nodes}, {"roots", orphan_roots},
            {"first", static_cast<uint32_t>(first_orphan)}}, diagnostics::runtime::Level::Warning);

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
        v.frn_new_tail.clear();
        v.frn_new_prev.clear();
    }
    return true;
}

void Engine::SaveCache() {
    MergeBase(true);
}

DeltaLog* Engine::DeltaForVolume(std::wstring_view volume_id, wchar_t fallback_letter) {
    if (!volume_id.empty()) {
        const std::wstring key = DeltaKeyForId(volume_id);
        if (const auto it = delta_logs_.find(key); it != delta_logs_.end()) {
            return it->second.get();
        }
    }
    if (fallback_letter != 0) {
        const wchar_t upper = static_cast<wchar_t>(towupper(fallback_letter));
        const VolState* fallback_vol = nullptr;
        for (const auto& v : vols_) {
            if (static_cast<wchar_t>(towupper(v.letter)) != upper) continue;
            if (!IsRootInactiveLocked(v.root_idx)) {
                fallback_vol = &v;
                break;
            }
            if (!fallback_vol) fallback_vol = &v;
        }
        if (fallback_vol && !fallback_vol->volume_id.empty()) {
            const std::wstring key = DeltaKeyForId(fallback_vol->volume_id);
            if (const auto it = delta_logs_.find(key); it != delta_logs_.end()) {
                return it->second.get();
            }
        }
        const std::wstring letter_key = std::wstring(L"__LETTER_") + upper;
        if (const auto it = delta_logs_.find(letter_key); it != delta_logs_.end()) {
            return it->second.get();
        }
    }
    if (vols_.empty()) {
        if (const auto it = delta_logs_.find(L""); it != delta_logs_.end()) {
            return it->second.get();
        }
    }
    return nullptr;
}

DeltaLog* Engine::DeltaFor(wchar_t letter) {
    if (letter == 0) {
        const auto it = delta_logs_.find(L"");
        return it == delta_logs_.end() ? nullptr : it->second.get();
    }
    return DeltaForVolume({}, letter);
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
        delta_logs_[L""] = std::move(log);
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
        const wchar_t upper = static_cast<wchar_t>(towupper(v.letter));
        const std::wstring key = !v.volume_id.empty()
            ? DeltaKeyForId(v.volume_id)
            : (upper ? (std::wstring(L"__LETTER_") + upper) : std::wstring(L""));
        delta_logs_[key] = std::move(log);
    }
}

bool Engine::ReplayDeltasLocked() {
    struct Addition {
        size_t volume; // vols_ slot matching the log's id, SIZE_MAX if none
        std::wstring name;
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
    // Resolve each log's volume once. Every record of a log carries the same
    // id; matching it per record normalized two strings per volume each time.
    // record_volume keeps the empty-id rule of record application, add_volume
    // the exact-match rule used for additions.
    std::vector<std::wstring> normalized_ids;
    normalized_ids.reserve(vols_.size());
    for (const auto& volume : vols_) normalized_ids.push_back(NormalizeVolumeId(volume.volume_id));
    std::vector<size_t> record_volume(logs.size(), SIZE_MAX), add_volume(logs.size(), SIZE_MAX);
    for (size_t k = 0; k < logs.size(); ++k) {
        const std::wstring id = NormalizeVolumeId(logs[k].first);
        for (size_t i = 0; i < vols_.size(); ++i) {
            if (record_volume[k] == SIZE_MAX && (logs[k].first.empty() || normalized_ids[i] == id))
                record_volume[k] = i;
            if (add_volume[k] == SIZE_MAX && normalized_ids[i] == id) add_volume[k] = i;
        }
    }
    std::vector<Addition> additions;
    bool valid = true;
    int32_t greatest_reference = -1;
    const auto replay_started = GetTickCount64();
    auto last_progress = replay_started;
    uint64_t records = 0, patch_records = 0, tomb_records = 0;
    // A stalled merge once left a WAL that took 73 minutes to replay with no
    // log output; report progress so a slow load is visible while it runs.
    auto progress = [&](uint8_t pass) {
        ++records;
        if ((records & 0xFFFF) != 0) return;
        const auto now = GetTickCount64();
        if (now - last_progress < 10000) return;
        last_progress = now;
        diagnostics::runtime::Event("index_replay_progress", {{"pass", static_cast<uint64_t>(pass)}, {"records", records},
            {"additions", static_cast<uint64_t>(additions.size())}, {"elapsed_ms", now - replay_started}});
    };
    // IDs are global, while WALs are per volume. Validate every log before
    // changing the snapshot, then restore additions in their original ID order.
    // Version 1 omitted these IDs and cannot safely reconstruct interleaving.
    for (size_t log_index = 0; log_index < logs.size(); ++log_index) {
        const size_t volume = add_volume[log_index];
        if (!DeltaLog::Replay(logs[log_index].second, built_unix_,
            [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                uint64_t, int64_t) {
                progress(1);
                if (op == DeltaOp::Add) {
                    valid &= idx >= LiveCount() && parent >= 0 && parent < idx;
                    additions.push_back({volume, std::wstring(name), idx, parent, flags, mtime, size, frn});
                } else if (op == DeltaOp::Patch || op == DeltaOp::Tomb) {
                    ++(op == DeltaOp::Patch ? patch_records : tomb_records);
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
    const uint64_t pass1_records = records;
    records = 0;
    const int32_t original_count = LiveCount();
    for (size_t i = 0; i < additions.size(); ++i)
        if (additions[i].idx != original_count + static_cast<int32_t>(i)) return false;
    if (greatest_reference >= original_count + static_cast<int32_t>(additions.size())) return false;
    for (const auto& add : additions) {
        VolState* volume = add.volume == SIZE_MAX ? nullptr : &vols_[add.volume];
        AddNodeLocked(live_, add.parent, add.name, add.flags, add.frn, add.size, UnixToFt(add.mtime), true, volume);
    }
    // The child map is rebuilt once when replay finishes. Maintaining it per
    // record is quadratic: every addition is inserted before any tombstone is
    // applied, so a name recreated N times in one folder (atomic saves, temp
    // files) leaves an N-long equal_range that each tombstone has to scan.
    child_map_.clear();
    // Owning volume per node, filled lazily along parent chains. A flat array
    // replaces a hash map that grew by one entry per visited ancestor; the
    // first owner recorded for a node wins, as with emplace before.
    constexpr uint32_t kUnknownOwner = UINT32_MAX;
    std::vector<uint32_t> owners(static_cast<size_t>(LiveCount()), kUnknownOwner);
    auto seed_owner = [&](int32_t idx, size_t owner) {
        if (idx >= 0 && idx < LiveCount() && owners[static_cast<size_t>(idx)] == kUnknownOwner)
            owners[static_cast<size_t>(idx)] = static_cast<uint32_t>(owner);
    };
    for (size_t i = 0; i < vols_.size(); ++i) seed_owner(vols_[i].root_idx, i);
    for (const auto& add : additions)
        if (add.volume != SIZE_MAX) seed_owner(add.idx, add.volume);
    std::vector<int32_t> ancestors;
    auto owner_of = [&](int32_t idx) -> size_t {
        ancestors.clear();
        ParentChainGuard chain(LiveCount());
        while (chain.Visit(idx)) {
            const uint32_t owner = owners[static_cast<size_t>(idx)];
            if (owner != kUnknownOwner) {
                for (int32_t ancestor : ancestors) owners[static_cast<size_t>(ancestor)] = owner;
                return owner;
            }
            ancestors.push_back(idx);
            idx = NodeAt(idx).parent;
        }
        return SIZE_MAX;
    };
    if (!vols_.empty()) for (const auto& add : additions)
        if (owner_of(add.idx) == SIZE_MAX || owner_of(add.parent) != owner_of(add.idx)) return false;
    // vol is matched once per log by stable volume id: a letter reused across
    // sessions must not pull another volume's records into this one.
    auto apply = [&](VolState* vol, DeltaOp op, int32_t idx, int32_t parent,
                     uint8_t flags, uint8_t which, uint32_t mtime, uint64_t size,
                     std::wstring_view name, uint64_t, uint64_t journal_id,
                     int64_t next_usn) {
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
                if (vol && idx == vol->root_idx && !name.empty()) {
                    vol->letter = static_cast<wchar_t>(towupper(name[0]));
                }
            }
        }
    };
    for (size_t log_index = 0; log_index < logs.size(); ++log_index) {
        VolState* volume = record_volume[log_index] == SIZE_MAX ? nullptr : &vols_[record_volume[log_index]];
        if (!DeltaLog::Replay(logs[log_index].second, built_unix_,
                [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                    uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                    uint64_t journal_id, int64_t next_usn) {
                    progress(2);
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
    if (pass1_records)
        diagnostics::runtime::Event("index_replay_end", {{"records", pass1_records},
            {"additions", static_cast<uint64_t>(additions.size())}, {"patches", patch_records},
            {"tombstones", tomb_records}, {"elapsed_ms", GetTickCount64() - replay_started}});
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
    uint64_t previous_built = 0;
    filename_timing_.Memory().Capture(IndexMemoryPoint::MergeBefore);
    std::vector<VolState> vols;
    uint64_t built = 0;
    TraceSearch("filename_merge_begin", revision_.load());
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        previous_built = built_unix_;
        built = (std::max)(static_cast<uint64_t>(std::time(nullptr)), previous_built + 1);
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
    // Serialization is finished. Release the full snapshot before mapping the
    // next base or compacting a failed merge's overlay, not after memory sampling.
    snap = Store{};
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
            // A failed publication does not reclaim names. Run the same bounded
            // local maintenance used during backoff without discarding pending work.
            CompactNamePoolLocked(GetTickCount64());
        }
    }
    if (committed) RemoveLegacyVolumeShards();
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
    _heapmin();
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
    const auto mapped_ms = GetTickCount64() - load_started;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto adopt_started = GetTickCount64();
    AdoptMappedLocked(std::move(mapped));
    const auto adopt_ms = GetTickCount64() - adopt_started;
    const auto replay_started = GetTickCount64();
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
        {"volumes", vols_.size()}, {"elapsed_ms", GetTickCount64() - load_started},
        {"map_ms", mapped_ms}, {"adopt_ms", adopt_ms}, {"replay_ms", GetTickCount64() - replay_started}});
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

namespace {
constexpr uint32_t kNoFrnSlot = UINT32_MAX;
// True when frn_new_tail/frn_new_prev still describe frn_new exactly; any
// future writer that bypasses MapFrnLocked falls back to the linear scan.
bool FrnIndexCurrent(const std::vector<DiskFrn>& rows, const std::vector<uint32_t>& prev) {
    return rows.size() == prev.size();
}
} // namespace

int32_t Engine::FindByFrnLocked(const VolState& v, uint64_t frn) const {
    if (FrnIndexCurrent(v.frn_new, v.frn_new_prev)) {
        const auto tail = v.frn_new_tail.find(frn);
        if (tail != v.frn_new_tail.end() && tail->second < v.frn_new.size() &&
            v.frn_new[tail->second].frn == frn)
            return v.frn_new[tail->second].idx;
    } else {
        for (auto it = v.frn_new.rbegin(); it != v.frn_new.rend(); ++it)
            if (it->frn == frn) return it->idx;
    }
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
    if (FrnIndexCurrent(v.frn_new, v.frn_new_prev)) {
        // Walk the per-FRN chain newest-first, then report oldest-first like
        // the former forward scan (callers prefer links.front()).
        std::vector<int32_t> chain;
        const auto tail = v.frn_new_tail.find(frn);
        for (uint32_t slot = tail == v.frn_new_tail.end() ? kNoFrnSlot : tail->second;
             slot != kNoFrnSlot && slot < v.frn_new.size() && v.frn_new[slot].frn == frn &&
             chain.size() < v.frn_new.size();
             slot = v.frn_new_prev[slot])
            chain.push_back(v.frn_new[slot].idx);
        for (auto it = chain.rbegin(); it != chain.rend(); ++it) add(*it);
    } else {
        for (const auto& entry : v.frn_new)
            if (entry.frn == frn) add(entry.idx);
    }
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
    if (FrnIndexCurrent(v.frn_new, v.frn_new_prev)) {
        const uint32_t slot = static_cast<uint32_t>(v.frn_new.size());
        auto [tail, inserted] = v.frn_new_tail.try_emplace(frn, slot);
        v.frn_new_prev.push_back(inserted ? kNoFrnSlot : tail->second);
        tail->second = slot;
    }
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
    // previous_path and previous_visible describe idx until the record changes
    // its structure; attribute work reuses them instead of rebuilding the path.
    bool path_current = !had_tracking_path && idx >= 0;
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
        const bool same_path = path_current && target == idx;
        event.path = target >= 0 && !same_path ? BuildPathLocked(target) : previous_path;
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
        const bool visible = same_path ? previous_visible :
            target >= 0 && !(NodeAt(target).flags & kFlagHidden) && !IsExcludedPath(event.path);
        if (!visible && !previous_visible) return;
        if (kind == ChangeKind::Renamed && !previous_visible && !had_tracking_path) {
            event.old_path.clear(); event.kind = ChangeKind::Created;
        }
        RecordFeed(event); changes_.Record(std::move(event));
    };
    DeltaLog* delta = DeltaForVolume(v.volume_id, v.letter);
    UsnApply effect = UsnApply::None;

    auto refresh = [&](int32_t i) {
        std::wstring built;
        const std::wstring& path = path_current && i == idx ? previous_path : (built = BuildPathLocked(i));
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
    path_current = false;

    int32_t parent = FindByFrnLocked(v, rec->ParentFileReferenceNumber);
    if (parent < 0) parent = v.root_idx;

    uint8_t flags = is_dir ? kFlagDir : 0;
    // A root fallback is not a real indexed location for an unresolved parent.
    if ((reason & USN_REASON_RENAME_NEW_NAME) && FindByFrnLocked(v, rec->ParentFileReferenceNumber) < 0) flags |= kFlagHidden;
    if (ShouldSkipChild(parent == v.root_idx, name)) flags |= kFlagHidden;
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

std::vector<uint8_t> Engine::SupersededUsnRecords(const BYTE* begin, const BYTE* end) {
    constexpr DWORD kDataReasons = USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION | USN_REASON_DATA_OVERWRITE;
    constexpr DWORD kAttrReasons = kDataReasons | USN_REASON_BASIC_INFO_CHANGE;
    constexpr DWORD kNameReasons = USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
        USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME | USN_REASON_HARD_LINK_CHANGE;
    std::vector<const USN_RECORD_V2*> records;
    for (const BYTE* p = begin; p && p + sizeof(USN_RECORD_COMMON_HEADER) <= end;) {
        const auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(p);
        if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
        records.push_back(hdr->MajorVersion == 2 ? reinterpret_cast<const USN_RECORD_V2*>(p) : nullptr);
        p += hdr->RecordLength;
    }
    // Data reasons force the attribute patch even when the stat is unchanged,
    // so a record carrying them is only superseded by one that carries them too.
    enum : uint8_t { kKeep = 0, kAttrOnly = 1, kAttrData = 2 };
    std::vector<uint8_t> superseded(records.size(), 0);
    std::unordered_map<uint64_t, uint8_t> next;
    next.reserve(records.size());
    for (size_t k = records.size(); k-- > 0;) {
        const USN_RECORD_V2* rec = records[k];
        if (!rec) continue;
        const DWORD reason = rec->Reason;
        const uint8_t kind = (reason & kAttrReasons) == 0 || (reason & kNameReasons) != 0 ? kKeep :
            (reason & kDataReasons) != 0 ? kAttrData : kAttrOnly;
        const auto [it, first] = next.try_emplace(rec->FileReferenceNumber, kind);
        if (!first) {
            if (kind != kKeep && it->second != kKeep && (kind == kAttrOnly || it->second == kAttrData))
                superseded[k] = 1;
            it->second = kind;
        }
    }
    return superseded;
}

Engine::UsnBatch Engine::ApplyUsnBatchLocked(VolState& v, const BYTE* begin, const BYTE* end,
                                             uint64_t& applied, bool& any_struct, bool& any_attr) {
    // Reasons accumulate while a file stays open, so one write session logs
    // several attribute records for the same file. When the next record for
    // that file is attribute-only too, it repeats the stat and patch with the
    // same outcome; the index ends identical and the feed reports one change.
    const std::vector<uint8_t> superseded = SupersededUsnRecords(begin, end);
    size_t k = 0;
    for (const BYTE* p = begin; p && p + sizeof(USN_RECORD_COMMON_HEADER) <= end; ++k) {
        if (!running_) return UsnBatch::Stopped;
        const auto* hdr = reinterpret_cast<const USN_RECORD_COMMON_HEADER*>(p);
        if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
        if (hdr->MajorVersion == 2 && !(k < superseded.size() && superseded[k])) {
            const UsnApply apply = ApplyUsnLocked(v, reinterpret_cast<const USN_RECORD_V2*>(p));
            if (apply != UsnApply::None) ++applied;
            // An invalid parent relation needs a fresh volume snapshot, not a
            // journal cursor advanced past a change we could not apply safely.
            if (v.journal_id == 0) return UsnBatch::InvalidParent;
            if (apply == UsnApply::Structure) any_struct = true;
            else if (apply == UsnApply::Attr) any_attr = true;
        }
        p += hdr->RecordLength;
    }
    return UsnBatch::Done;
}

bool Engine::CatchUpVolume(VolState& v, bool* changed, bool* structural) {
    if (VolumePaused(v.volume_id)) return true;
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
    const BYTE* p = blob.empty() ? nullptr : blob.data();
    bool any_struct = false;
    bool any_attr = false;
    uint64_t applied = 0;
    const UsnBatch batch = ApplyUsnBatchLocked(v, p, p + blob.size(), applied, any_struct, any_attr);
    if (batch == UsnBatch::Stopped) return false;
    if (batch == UsnBatch::InvalidParent) {
        filename_timing_.End(FilenameStage::Journal, timing, applied, ERROR_INVALID_DATA, "invalid_parent", letter);
        return false;
    }
    v.next_usn = last;
    v.folder_size_current = true;
    if (DeltaLog* delta = DeltaForVolume(v.volume_id, v.letter)) delta->QueueUsn(v.journal_id, v.next_usn);
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
    if (EffectiveBackend(volume) == VolumeBackend::DirectoryWalk) return IndexVolumeWalk(volume);
    HANDLE h = OpenVolume(letter);
    if (h == INVALID_HANDLE_VALUE) return fail(GetLastError(), 1);

    VolState vol;
    vol.letter = letter;
    vol.kind = volume.kind;
    vol.backend = VolumeBackend::NtfsMftUsn;
    vol.fs_tag = VolumeFsTag::UnknownOrNtfs;
    vol.serial = volume.serial;
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
    std::unordered_set<uint64_t> seen;  // only for walks deeper than kCheckedHops
    auto build_name = [&](int32_t id) -> std::wstring_view {
        const Node& node = build_.nodes[static_cast<size_t>(id)];
        return std::wstring_view(build_.pool.data() + node.off, node.len);
    };
    auto build_path = [&](int32_t id) {
        return BuildCompleteIndexPath(id, static_cast<int32_t>(build_.nodes.size()),
            [&](int32_t current) { return build_.nodes[static_cast<size_t>(current)].parent; },
            build_name);
    };
    struct DirBuildInfo {
        std::wstring path;
        bool hidden_parent = false;
        bool may_contain_exclusion = false;
    };
    auto may_have_excluded = [&](std::wstring_view dir_path) -> bool {
        if (excluded_paths_.empty() || dir_path.empty()) return false;
        for (const auto& ex : excluded_paths_) {
            if (ex.empty()) continue;
            if (dir_path.size() >= ex.size()) {
                if (CompareStringOrdinal(dir_path.data(), static_cast<int>(ex.size()),
                                         ex.data(), static_cast<int>(ex.size()), TRUE) == CSTR_EQUAL &&
                    (dir_path.size() == ex.size() || dir_path[ex.size()] == L'\\' ||
                     dir_path[ex.size()] == L'/')) {
                    return true;
                }
            } else {
                if (CompareStringOrdinal(ex.data(), static_cast<int>(dir_path.size()),
                                         dir_path.data(), static_cast<int>(dir_path.size()), TRUE) == CSTR_EQUAL &&
                    (dir_path.back() == L'\\' || ex[dir_path.size()] == L'\\' ||
                     ex[dir_path.size()] == L'/')) {
                    return true;
                }
            }
        }
        return false;
    };
    std::unordered_map<int32_t, DirBuildInfo> dir_cache;
    dir_cache.reserve(frn_nodes.size() / 8 + 64);
    auto get_dir_info = [&](int32_t dir_idx) -> const DirBuildInfo* {
        if (auto it = dir_cache.find(dir_idx); it != dir_cache.end()) return &it->second;
        DirBuildInfo info;
        info.path = build_path(dir_idx);
        if (info.path.empty()) return nullptr;
        info.hidden_parent = dir_idx >= 0 && dir_idx != vol.root_idx &&
            (build_.nodes[static_cast<size_t>(dir_idx)].flags & kFlagHidden) != 0;
        info.may_contain_exclusion = !info.hidden_parent && may_have_excluded(info.path);
        auto [it, inserted] = dir_cache.emplace(dir_idx, std::move(info));
        return &it->second;
    };
    std::wstring scratch_path;
    size_t added = 0;
    for (FrnNode& node : frn_nodes) {
        if (!running_ || added >= kIndexCap) break;
        if (node.index >= 0) continue;
        stack.clear();
        if (!seen.empty()) seen.clear();
        uint64_t cur = node.frn;
        int32_t parent_idx = vol.root_idx;
        // Walk to the nearest placed ancestor. Reused MFT slots can give a deep
        // folder a lower FRN than its ancestors, so this is not depth-limited
        // (a limit re-rooted the rest of the chain below the volume root); only
        // a parent cycle in a damaged MFT stops the walk early.
        constexpr size_t kCheckedHops = 64;
        for (;;) {
            if (index_dir_frn_ && cur == index_dir_frn_ && letter == index_dir_letter_) {
                stack.clear();
                break;
            }
            if (cur == root_frn) { parent_idx = vol.root_idx; break; }
            FrnNode* current = find_node(cur);
            if (!current || current->name.empty()) break;
            if (current->index >= 0) { parent_idx = current->index; break; }
            if (stack.size() >= kCheckedHops) {
                if (seen.empty())
                    for (const FrnNode* walked : stack) seen.insert(walked->frn);
                if (!seen.insert(current->frn).second) break;
            }
            stack.push_back(current);
            if (current->parent == cur) break;
            cur = current->parent;
        }
        const DirBuildInfo* parent_info = get_dir_info(parent_idx);
        if (!parent_info) return false;
        if (stack.size() == 1 && !stack[0]->is_dir) {
            FrnNode& n = *stack[0];
            if (!IsIndexArtifactName(n.name)) {
                const size_t sep = (!parent_info->path.empty() && parent_info->path.back() != L'\\') ? 1 : 0;
                if (parent_info->path.size() + sep + n.name.size() > 32766) return false;
                bool hidden = parent_info->hidden_parent || ShouldSkipChild(parent_idx == vol.root_idx, n.name);
                if (!hidden && parent_info->may_contain_exclusion) {
                    scratch_path.assign(parent_info->path);
                    if (sep) scratch_path.push_back(L'\\');
                    scratch_path.append(n.name);
                    hidden = IsExcludedPath(scratch_path);
                }
                const uint8_t flags = hidden ? kFlagHidden : 0;
                const int32_t idx = AddNodeLocked(build_, parent_idx, n.name, flags, n.frn, n.size, n.mtime);
                n.index = idx;
                vol.frn_build.emplace_back(n.frn, idx);
                ++added;
                indexed_.store(build_.nodes.size());
                if ((added & 0x3FFF) == 0) PingNotify();
            }
            continue;
        }
        std::wstring candidate = parent_info->path;
        bool hidden_parent = parent_info->hidden_parent;
        for (auto rit = stack.rbegin(); rit != stack.rend(); ++rit) {
            FrnNode& n = **rit;
            if (IsIndexArtifactName(n.name)) continue;
            uint8_t flags = n.is_dir ? kFlagDir : 0;
            if (!candidate.empty() && candidate.back() != L'\\') candidate += L'\\';
            candidate += n.name;
            if (candidate.size() > 32766) return false;
            if (hidden_parent || ShouldSkipChild(parent_idx == vol.root_idx, n.name) || IsExcludedPath(candidate))
                flags |= kFlagHidden;
            parent_idx = AddNodeLocked(build_, parent_idx, n.name, flags, n.frn, n.size, n.mtime);
            n.index = parent_idx;
            hidden_parent = (flags & kFlagHidden) != 0;
            if (n.is_dir) {
                dir_cache.emplace(parent_idx, DirBuildInfo{
                    candidate, hidden_parent, !hidden_parent && may_have_excluded(candidate)});
            }
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
        const DirBuildInfo* parent_info = get_dir_info(parent_idx);
        if (!parent_info) continue;
        const size_t sep = (!parent_info->path.empty() && parent_info->path.back() != L'\\') ? 1 : 0;
        if (parent_info->path.size() + sep + link.name.size() > 32766) continue;
        bool hidden = parent_info->hidden_parent || ShouldSkipChild(parent_idx == vol.root_idx, link.name);
        if (!hidden && parent_info->may_contain_exclusion) {
            scratch_path.assign(parent_info->path);
            if (sep) scratch_path.push_back(L'\\');
            scratch_path.append(link.name);
            hidden = IsExcludedPath(scratch_path);
        }
        const uint8_t flags = hidden ? kFlagHidden : 0;
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

bool Engine::IndexVolumeForBackend(const VolumeInfo& volume) {
    switch (EffectiveBackend(volume)) {
    case VolumeBackend::NtfsMftUsn:
        return IndexVolumeMft(volume);
    case VolumeBackend::DirectoryWalk:
        return IndexVolumeWalk(volume);
    case VolumeBackend::Unsupported:
    default:
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
}

bool Engine::IndexVolumeWalk(const VolumeInfo& volume) {
    if (volume.mount_point.size() < 2) {
        SetLastError(ERROR_INVALID_NAME);
        return false;
    }
    const wchar_t letter = static_cast<wchar_t>(towupper(volume.mount_point[0]));
    const std::wstring root_path = std::wstring(1, letter) + L":\\";
    const std::wstring root_name = std::wstring(1, letter) + L":";
    if (!running_.load() || VolumePauseRequestedOrActive(volume.id, root_path)) {
        SetLastError(ERROR_OPERATION_ABORTED);
        return false;
    }

    std::vector<std::wstring> local_excluded;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        local_excluded = excluded_paths_;
    }
    auto is_excluded_local = [&](std::wstring_view path) -> bool {
        for (const auto& ex : local_excluded) {
            if (ex.empty() || path.size() < ex.size()) continue;
            if (CompareStringOrdinal(path.data(), static_cast<int>(ex.size()),
                                     ex.data(), static_cast<int>(ex.size()), TRUE) != CSTR_EQUAL) continue;
            if (path.size() == ex.size() || path[ex.size()] == L'\\' || path[ex.size()] == L'/') return true;
        }
        return false;
    };
    auto may_contain_exclusion_local = [&](std::wstring_view dir) -> bool {
        for (const auto& ex : local_excluded) {
            if (ex.empty()) continue;
            if (dir.size() <= ex.size()) {
                if (CompareStringOrdinal(ex.data(), static_cast<int>(dir.size()),
                                         dir.data(), static_cast<int>(dir.size()), TRUE) == CSTR_EQUAL &&
                    (ex.size() == dir.size() || dir.back() == L'\\' || ex[dir.size()] == L'\\' ||
                     ex[dir.size()] == L'/')) {
                    return true;
                }
            } else if (CompareStringOrdinal(dir.data(), static_cast<int>(ex.size()),
                                            ex.data(), static_cast<int>(ex.size()), TRUE) == CSTR_EQUAL &&
                       (dir[ex.size()] == L'\\' || dir[ex.size()] == L'/')) {
                return true;
            }
        }
        return false;
    };

    const size_t start_nodes = build_.nodes.size();
    const size_t start_pool = build_.pool.size();
    if (start_nodes >= kIndexCap) {
        SetLastError(ERROR_FILE_TOO_LARGE);
        return false;
    }

    VolState vol;
    vol.letter = letter;
    vol.kind = volume.kind;
    vol.backend = VolumeBackend::DirectoryWalk;
    vol.fs_tag = FsTagFromName(volume.file_system);
    if (vol.fs_tag == VolumeFsTag::UnknownOrNtfs) vol.fs_tag = VolumeFsTag::Fat32;
    vol.serial = volume.serial;
    vol.volume_id = volume.id;
    vol.journal_id = 0;
    vol.next_usn = static_cast<int64_t>(volume.serial);
    vol.root_idx = AddNodeLocked(build_, -1, root_name, kFlagDir | kFlagHidden);
    vol.first_idx = vol.root_idx;
    indexed_.store(build_.nodes.size());

    SetStatus(L"正在扫描 " + root_path + L"…");
    PingNotify();

    WalkCallbacks cb;
    cb.should_cancel = [&]() {
        return !running_.load() || VolumePauseRequestedOrActive(volume.id, root_path);
    };
    cb.should_skip_name = [this](bool parent_is_root, std::wstring_view name) {
        return ShouldSkipChild(parent_is_root, name) || IsIndexArtifactName(name);
    };
    cb.is_excluded_path = is_excluded_local;
    cb.may_contain_exclusion = may_contain_exclusion_local;
    cb.emit_node = [&](int32_t parent_id, std::wstring_view name, bool is_dir,
                       bool hidden, uint64_t size, uint64_t mtime_ft) -> int32_t {
        const uint8_t flags = static_cast<uint8_t>((is_dir ? kFlagDir : 0) | (hidden ? kFlagHidden : 0));
        const int32_t idx = AddNodeLocked(build_, parent_id, name, flags, 0, size, mtime_ft);
        indexed_.store(build_.nodes.size());
        return idx;
    };
    cb.on_progress = [&](size_t count) {
        SetStatus(StatusDriveProgress(letter, count));
        PingNotify();
    };

    WalkStats stats;
    const size_t cap = kIndexCap > build_.nodes.size() ? kIndexCap - build_.nodes.size() : 0;
    std::wstring walk_root = root_path;
    if (!test_walk_roots_.empty()) {
        const auto it = test_walk_roots_.find(NormalizeVolumeId(volume.id));
        if (it != test_walk_roots_.end() && !it->second.empty()) {
            walk_root = it->second;
        }
    }
    const bool ok = WalkDirectoryTree(walk_root, vol.root_idx, true, cap, cb, &stats);
    if (!ok) {
        build_.nodes.resize(start_nodes);
        build_.attrs.resize(start_nodes);
        build_.pool.resize(start_pool);
        indexed_.store(build_.nodes.size());
        const DWORD err = stats.cancelled ? ERROR_OPERATION_ABORTED :
            (stats.hit_cap ? ERROR_FILE_TOO_LARGE :
             (stats.root_error ? stats.root_error : ERROR_READ_FAULT));
        diagnostics::runtime::Event(
            err == ERROR_OPERATION_ABORTED ? "index_volume_scan_cancelled" : "index_volume_scan_failed",
            {{"operation", index_operation}, {"drive", static_cast<uint64_t>(letter)},
             {"phase", stats.cancelled ? 6ull : 1ull}, {"records", stats.added},
             {"limit", kIndexCap}, {"error", err}},
            IndexErrorLevel(err));
        SetLastError(err);
        return false;
    }
    build_.nodes[static_cast<size_t>(vol.root_idx)].unused =
        static_cast<uint32_t>(build_.nodes.size());
    indexed_.store(build_.nodes.size());
    vol.item_count = static_cast<uint64_t>(stats.added) + 1;
    build_vols_.push_back(std::move(vol));
    return true;
}

bool Engine::RebuildVolumeMft(const VolumeInfo& volume) {
    if (VolumePaused(volume.id)) return true;
    if (!MachineIndexScope() || volume.id.empty()) return false;
    return RebuildVolume(volume);
}

bool Engine::RebuildVolume(const VolumeInfo& volume) {
    if (VolumePauseRequestedOrActive(volume.id, volume.mount_point)) return true;
    if (volume.id.empty()) return false;
    const VolumeBackend backend = EffectiveBackend(volume);
    if (backend == VolumeBackend::Unsupported) {
        SetLastError(ERROR_NOT_SUPPORTED);
        return false;
    }
    if (backend == VolumeBackend::NtfsMftUsn && !IsAdmin()) {
        SetLastError(ERROR_ACCESS_DENIED);
        return false;
    }
    const std::wstring path = CachePath();
    if (path.empty()) return false;

    // Flatten the current live view first so healthy volumes stay intact while
    // only the target volume's span is rebuilt (or appended if newly arrived).
    Store current;
    std::vector<VolState> current_volumes;
    std::vector<std::wstring> inactive_ids;
    uint64_t previous_built = 0;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        previous_built = built_unix_;
        for (const auto& v : vols_) {
            if (!v.volume_id.empty() && IsRootInactiveLocked(v.root_idx)) {
                inactive_ids.push_back(NormalizeVolumeId(v.volume_id));
            }
        }
        if (LiveCount() > 0 && !FlattenLocked(current, current_volumes)) return false;
    }

    const std::wstring target_id = NormalizeVolumeId(volume.id);
    auto target_it = std::find_if(current_volumes.begin(), current_volumes.end(),
        [&](const VolState& state) {
            return NormalizeVolumeId(state.volume_id) == target_id;
        });

    Store replacement;
    std::vector<VolState> healthy;
    bool replacing_existing = false;
    if (target_it != current_volumes.end()) {
        replacing_existing = true;
        const int32_t skip_first = target_it->first_idx;
        const int32_t skip_last = skip_first + static_cast<int32_t>(target_it->item_count);
        if (skip_first < 0 || skip_last <= skip_first ||
            skip_last > static_cast<int32_t>(current.nodes.size())) return false;

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
            state.frn_new_tail.clear();
            state.frn_new_prev.clear();
            state.frn_build.clear();
            for (const auto& entry : source.frn_build) {
                if (entry.second >= 0 && entry.second < static_cast<int32_t>(remap.size()) &&
                    remap[static_cast<size_t>(entry.second)] >= 0)
                    state.frn_build.emplace_back(entry.first, remap[static_cast<size_t>(entry.second)]);
            }
            healthy.push_back(std::move(state));
        }
    } else {
        replacement = std::move(current);
        healthy = std::move(current_volumes);
    }

    if (backend == VolumeBackend::DirectoryWalk && volume.online && volume.enabled) {
        // Arm before scanning so changes occurring during directory walk are queued.
        EnsureVolumeWalkWatch(volume);
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_ = std::move(replacement);
        build_vols_ = std::move(healthy);
        building_ = true;
        indexed_.store(build_.nodes.size());
    }
    const bool walk_reconcile = backend == VolumeBackend::DirectoryWalk && replacing_existing;
    SetStatus(walk_reconcile
        ? (L"正在重新核对 " + volume.mount_point + L" 的索引…")
        : (L"正在重建 " + volume.mount_point + L" 的索引…"));
    if (!IndexVolumeForBackend(volume)) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_vols_.clear();
        building_ = false;
        return false;
    }
    const uint64_t built = (std::max)(static_cast<uint64_t>(std::time(nullptr)), previous_built + 1);
    const bool wrote = WriteIndexFile(path, build_, build_vols_, built);
    if (!wrote) {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_vols_.clear();
        building_ = false;
        return false;
    }

    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const uint64_t saved_built = built_unix_;
        built_unix_ = built;
        if (!CommitMappedFile(path)) {
            built_unix_ = saved_built;
            build_.Clear();
            build_vols_.clear();
            building_ = false;
            return false;
        }
        build_.Clear();
        build_.Shrink();
        build_vols_.clear();
        // Restore offline hidden state for any unrelated offline removable volumes.
        for (const auto& v : vols_) {
            if (v.root_idx < 0 || v.volume_id.empty()) continue;
            const std::wstring norm = NormalizeVolumeId(v.volume_id);
            if (norm == target_id && volume.online) continue;
            if (std::find(inactive_ids.begin(), inactive_ids.end(), norm) != inactive_ids.end()) {
                inactive_volume_roots_.push_back(v.root_idx);
            }
        }
        if (!inactive_volume_roots_.empty()) ++filter_epoch_;
        OpenDeltasLocked();
        ClearVolumeReconcile(volume.id);
        GapFeed();
        building_ = false;
        ready_ = true;
    }
    if (backend == VolumeBackend::DirectoryWalk) PollWalkWatches();
    SetStatus(walk_reconcile
        ? (volume.mount_point + L" 的索引已核对完成")
        : (volume.mount_point + L" 的索引已重建完成"));
    PingNotify(true);
    if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 1, 0);
    return true;
}

void Engine::WalkTree(int32_t parent, const std::wstring& dir, int depth) {
    (void)depth;
    if (!running_ || parent < 0) return;
    std::vector<std::wstring> local_excluded;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        local_excluded = excluded_paths_;
    }
    WalkCallbacks cb;
    cb.should_cancel = [this]() { return !running_.load(); };
    cb.should_skip_name = [this](bool, std::wstring_view name) {
        return ShouldSkipName(name) || IsIndexArtifactName(name);
    };
    cb.is_excluded_path = [&](std::wstring_view path) -> bool {
        for (const auto& ex : local_excluded) {
            if (ex.empty() || path.size() < ex.size()) continue;
            if (CompareStringOrdinal(path.data(), static_cast<int>(ex.size()),
                                     ex.data(), static_cast<int>(ex.size()), TRUE) != CSTR_EQUAL) continue;
            if (path.size() == ex.size() || path[ex.size()] == L'\\' || path[ex.size()] == L'/') return true;
        }
        return false;
    };
    cb.emit_node = [&](int32_t node_parent, std::wstring_view name, bool is_dir,
                       bool hidden, uint64_t size, uint64_t mtime_ft) -> int32_t {
        if (hidden) return -1;
        const int32_t idx = AddNodeLocked(build_, node_parent, name, is_dir ? kFlagDir : 0, 0, size, mtime_ft);
        indexed_.store(build_.nodes.size());
        return idx;
    };
    cb.on_progress = [this](size_t) { PingNotify(); };
    const size_t cap = kIndexCap > build_.nodes.size() ? kIndexCap - build_.nodes.size() : 0;
    WalkDirectoryTree(dir, parent, false, cap, cb, nullptr);
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
    bool used_volumes = false;
    const auto previous_count = indexed_.load();
    if ((IsAdmin() || MachineIndexScope()) && fixture_root_.empty()) {
        const bool has_walk_volumes = std::any_of(configured_volumes.begin(), configured_volumes.end(),
            [](const VolumeInfo& v) { return EffectiveBackend(v) == VolumeBackend::DirectoryWalk; });
        if (has_walk_volumes) SyncWalkWatches(configured_volumes);
        for (const auto& volume : configured_volumes) {
            if (!running_) break;
            const VolumeBackend backend = EffectiveBackend(volume);
            if (backend == VolumeBackend::Unsupported) continue;
            if (backend == VolumeBackend::NtfsMftUsn && !IsAdmin()) {
                if (!incomplete) build_error = ERROR_ACCESS_DENIED;
                incomplete = true;
                continue;
            }
            if (IndexVolumeForBackend(volume)) {
                used_volumes = true;
                if (backend == VolumeBackend::NtfsMftUsn) used_mft = true;
            } else {
                const DWORD error = GetLastError();
                if (!incomplete) build_error = error ? error : ERROR_READ_FAULT;
                incomplete = true;
            }
        }
    }
    if (!used_volumes && !incomplete && running_ && !MachineIndexScope()) {
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
            RemoveLegacyVolumeShards();
        }
        bool any_walk_vol = false;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            build_.Clear();
            build_.Shrink();
            build_vols_.clear();
            UpdateVolumeVisibilityLocked(configured_volumes);
            for (const auto& v : vols_) {
                if (v.backend == VolumeBackend::DirectoryWalk && !IsRootInactiveLocked(v.root_idx)) {
                    any_walk_vol = true;
                    break;
                }
            }
            const bool live_tracked = !vols_.empty() &&
                std::all_of(vols_.begin(), vols_.end(), [&](const VolState& v) {
                    if (IsRootInactiveLocked(v.root_idx)) return true;
                    if (v.journal_id != 0) return true;
                    if (v.backend != VolumeBackend::DirectoryWalk) return false;
                    return std::any_of(watches_.begin(), watches_.end(), [&](const WalkWatch& w) {
                        return !w.volume_id.empty() &&
                            NormalizeVolumeId(w.volume_id) == NormalizeVolumeId(v.volume_id) &&
                            w.dir != INVALID_HANDLE_VALUE;
                    });
                });
            SetStatus(committed ? StatusItemCount(indexed_.load(),
                      live_tracked ? L"实时更新" : L"正在监视")
                      : L"索引可用，但保存失败，请检查索引目录权限及磁盘空间");
            ready_ = true;
            building_ = false;
            folder_size_gap_ = false;
        }
        if (used_volumes) {
            if (any_walk_vol) {
                SyncWalkWatches(configured_volumes);
                PollWalkWatches();
            } else {
                StopWalkWatches();
            }
        } else {
            PollWalkWatches();
        }
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
    _heapmin();
    finish();
}

bool Engine::NeedsSearchRebuildLocked() const {
    return map_ && (map_->hdr->ver < kIndexVer || !map_->prefix1_all_chars);
}

void Engine::StartJournalStreams() {
    journal_streams_.clear();
    for (const auto& volume : vols_) {
        if (!VolumePaused(volume.volume_id) && volume.journal_id && !volume_retry_after_.contains(volume.volume_id)) journal_streams_[volume.volume_id] =
            std::make_unique<UsnStream>(volume.letter, volume.journal_id, volume.next_usn, change_signal_);
    }
}
void Engine::RecoverFailedVolumes(const std::vector<VolumeInfo>& volumes, ULONGLONG now,
                                  const std::function<bool(const VolumeInfo&)>& rebuild) {
    for (const auto& volume : volumes) {
        if (!running_) break;
        if (VolumePaused(volume.id)) continue;
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
    RemoveLegacyVolumeShards();
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
    if (have_cache && !needs_search_rebuild && (IsAdmin() || MachineIndexScope())) {
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
                for (const auto& v : vols_) {
                    if (std::any_of(mounted.begin(), mounted.end(), [&](const VolumeInfo& drive) {
                            return !drive.enabled &&
                                   SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                        })) {
                        fresh = false;
                        break;
                    }
                }
            }
            if (fresh) {
                std::vector<VolumeInfo> startup_failed;
                std::unique_lock<std::shared_mutex> lock(mutex_);
                for (auto& v : vols_) {
                    auto it = std::find_if(drives.begin(), drives.end(), [&](const VolumeInfo& drive) {
                        return SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                    });
                    if (it != drives.end() && it->mount_point.size() >= 2 &&
                        static_cast<wchar_t>(towupper(it->mount_point[0])) !=
                            static_cast<wchar_t>(towupper(v.letter))) {
                        RelocateVolumeLetterLocked(v, it->mount_point[0]);
                    }
                }
                UpdateVolumeVisibilityLocked(drives);
                for (auto& v : vols_) {
                    auto it = std::find_if(drives.begin(), drives.end(), [&](const VolumeInfo& drive) {
                        return SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                    });
                    if (it == drives.end()) continue;
                    if (v.backend == VolumeBackend::DirectoryWalk ||
                        EffectiveBackend(*it) == VolumeBackend::DirectoryWalk) {
                        QueueVolumeReconcile(it->id);
                        GapFeed();
                        continue;
                    }
                    if (!IsAdmin()) {
                        startup_failed.push_back(*it);
                        continue;
                    }
                    lock.unlock();
                    if (!CatchUpVolume(v, nullptr)) startup_failed.push_back(*it);
                    lock.lock();
                }
                for (const auto& drive : drives) {
                    const bool tracked = std::any_of(vols_.begin(), vols_.end(), [&](const VolState& v) {
                        return SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                    });
                    if (!tracked) QueueVolumeReconcile(drive.id);
                }
                lock.unlock();
                if (!startup_failed.empty()) {
                    fresh = true;
                    for (const auto& volume : startup_failed) {
                        if (!RebuildVolume(volume)) {
                            fresh = false;
                            break;
                        }
                    }
                }
            }
            if (fresh) {
                ready_ = true;
                SyncWalkWatches(drives);
                SetStatus(StatusItemCount(indexed_.load(), L"实时更新"));
            }
        }
    }
    // Walk snapshots have no offline journal cursor. Replay provides a useful
    // initial view, but every startup reconciles DirectoryWalk / new volumes.

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
    std::array<ULONGLONG, 2> hotplug_probes{0, 0};
    StartJournalStreams();
    if (running_ && !walk_mode) SyncWalkWatches(online);
    while (running_) {
        ApplyVolumePauses();
        SeedPendingChanges();
        DWORD wait_ms = 15000;
        const ULONGLONG wait_now = GetTickCount64();
        if (topology_dirty_.load() || rebuild_requested_.load()) {
            wait_ms = 0;
        } else {
            const std::vector<std::wstring> queued_reconciles = [this] {
                std::lock_guard<std::mutex> rlock(reconcile_mu_);
                return std::vector<std::wstring>(pending_reconcile_volumes_.begin(),
                                                 pending_reconcile_volumes_.end());
            }();
            for (const auto& norm_id : queued_reconciles) {
                const auto drive_it = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& d) {
                    return NormalizeVolumeId(d.id) == norm_id;
                });
                if (drive_it == online.end() || !drive_it->enabled || !drive_it->online) continue;
                if (VolumePaused(drive_it->id) || PathPaused(drive_it->mount_point)) continue;
                const auto retry_it = volume_retry_after_.find(drive_it->id);
                if (retry_it == volume_retry_after_.end() || wait_now >= retry_it->second) {
                    wait_ms = 0;
                    break;
                }
                const ULONGLONG rem = retry_it->second - wait_now;
                if (rem < wait_ms) wait_ms = static_cast<DWORD>(rem);
            }
            for (ULONGLONG& probe : hotplug_probes) {
                if (!probe) continue;
                if (wait_now >= probe) {
                    wait_ms = 0;
                    break;
                }
                const ULONGLONG rem = probe - wait_now;
                if (rem < wait_ms) wait_ms = static_cast<DWORD>(rem);
            }
        }
        std::vector<HANDLE> events{change_signal_};
        for (auto& watch : watches_) {
            if (events.size() < MAXIMUM_WAIT_OBJECTS && watch.dir != INVALID_HANDLE_VALUE && watch.event) {
                events.push_back(watch.event);
            }
        }
        const auto wait_timing = FilenameTiming::Begin();
        const DWORD wake = WaitForMultipleObjects(static_cast<DWORD>(events.size()), events.data(), FALSE, wait_ms);
        filename_timing_.End(FilenameStage::Wait, wait_timing, wake == WAIT_TIMEOUT ? 0 : 1,
            wake == WAIT_FAILED ? GetLastError() : ERROR_SUCCESS, wake == WAIT_TIMEOUT ? "maintenance" : "event");
        ApplyVolumePauses();
        PollWalkWatches();
        changes_.Flush(false, &filename_timing_.Memory());
        if (!running_) break;
        if (volume_pauses_.empty() && rebuild_requested_.exchange(false)) {
            journal_streams_.clear();
            volume_retry_after_.clear();
            {
                std::lock_guard<std::mutex> rlock(reconcile_mu_);
                pending_reconcile_volumes_.clear();
            }
            FullRebuild();
            StartJournalStreams();
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                walk_mode = vols_.empty() && !MachineIndexScope();
            }
            if (!walk_mode) SyncWalkWatches(online);
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
        const bool dirty_topology = topology_dirty_.exchange(false);
        bool probe_due = false;
        for (ULONGLONG& probe : hotplug_probes) {
            if (probe && loop_tick >= probe) {
                probe = 0;
                probe_due = true;
            }
        }
        if (dirty_topology) {
            hotplug_probes = {loop_tick + 600, loop_tick + 2000};
        }
        const bool refresh_topology = fixture_root_.empty() &&
            (dirty_topology || probe_due || loop_tick - topology_tick >= 30000);
        if (refresh_topology) {
            const auto timing = FilenameTiming::Begin();
            IndexConfig topology_config;
            if (!MachineIndexScope() || LoadMachineConfig(topology_config, nullptr))
                online = ConfiguredVolumes(topology_config);
            topology_tick = loop_tick;
            filename_timing_.End(FilenameStage::Topology, timing, online.size(), ERROR_SUCCESS,
                dirty_topology ? "hotplug" : (probe_due ? "hotplug_followup" : "periodic"));
        }
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!vols_.empty()) {
                if (refresh_topology) {
                    const std::vector<int32_t> prev_inactive = inactive_volume_roots_;
                    for (auto& v : vols_) {
                        auto it = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                            return SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                        });
                        if (it != online.end() && it->mount_point.size() >= 2 &&
                            static_cast<wchar_t>(towupper(it->mount_point[0])) !=
                                static_cast<wchar_t>(towupper(v.letter))) {
                            RelocateVolumeLetterLocked(v, it->mount_point[0]);
                            changed = true;
                        }
                    }
                    UpdateVolumeVisibilityLocked(online);
                    if (prev_inactive != inactive_volume_roots_) changed = true;
                    for (const auto& v : vols_) {
                        if (v.backend != VolumeBackend::DirectoryWalk || IsRootInactiveLocked(v.root_idx)) continue;
                        const bool was_inactive = std::find(prev_inactive.begin(), prev_inactive.end(), v.root_idx) !=
                            prev_inactive.end();
                        if (was_inactive && !v.volume_id.empty()) {
                            QueueVolumeReconcile(v.volume_id);
                            GapFeed();
                        }
                    }
                    for (const auto& drive : online) {
                        const bool tracked = std::any_of(vols_.begin(), vols_.end(), [&](const VolState& v) {
                            return SameVolumeEntry(drive, v.volume_id, v.backend, v.fs_tag, v.serial);
                        });
                        if (!tracked && !drive.id.empty()) {
                            volume_retry_after_.erase(drive.id);
                            QueueVolumeReconcile(drive.id);
                            GapFeed();
                        }
                    }
                } else {
                    UpdateVolumeVisibilityLocked(online);
                }
            } else if (refresh_topology && !walk_mode) {
                for (const auto& drive : online) {
                    if (!drive.id.empty()) {
                        volume_retry_after_.erase(drive.id);
                        QueueVolumeReconcile(drive.id);
                    }
                }
            }
        }
        if (refresh_topology && !walk_mode) {
            SyncWalkWatches(online);
        }
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!vols_.empty()) {
                std::vector<size_t> idx(vols_.size());
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
                lock.unlock();
                for (size_t i : idx) {
                    if (vols_[i].backend == VolumeBackend::DirectoryWalk) continue;
                    if (VolumePaused(vols_[i].volume_id)) continue;
                    const bool present = std::any_of(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                        return SameVolumeEntry(drive, vols_[i].volume_id, vols_[i].backend,
                                               vols_[i].fs_tag, vols_[i].serial);
                    });
                    if (!present) continue;
                    const auto retry = volume_retry_after_.find(vols_[i].volume_id);
                    if (retry != volume_retry_after_.end() && loop_tick < retry->second) continue;
                    if (retry != volume_retry_after_.end() || !CatchUpVolume(vols_[i], &changed, &structural)) {
                        GapFeed();
                        failed = true;
                        auto it = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                            return SameVolumeEntry(drive, vols_[i].volume_id, vols_[i].backend,
                                                   vols_[i].fs_tag, vols_[i].serial);
                        });
                        if (it != online.end()) failed_volumes.push_back(*it);
                    }
                }
                lock.lock();
                if (changed) {
                    const bool live_tracked = std::all_of(vols_.begin(), vols_.end(), [&](const VolState& v) {
                        if (IsRootInactiveLocked(v.root_idx)) return true;
                        if (v.journal_id != 0) return true;
                        if (v.backend != VolumeBackend::DirectoryWalk) return false;
                        return std::any_of(watches_.begin(), watches_.end(), [&](const WalkWatch& w) {
                            return !w.volume_id.empty() &&
                                NormalizeVolumeId(w.volume_id) == NormalizeVolumeId(v.volume_id) &&
                                w.dir != INVALID_HANDLE_VALUE;
                        });
                    });
                    SetStatus(StatusItemCount(indexed_.load(),
                              live_tracked ? L"实时更新" : L"部分磁盘未实时更新"));
                }
            }
        }
        if (failed && running_) {
            RecoverFailedVolumes(failed_volumes, loop_tick,
                [this](const VolumeInfo& volume) { return RebuildVolume(volume); });
        }
        if (running_ && !walk_mode) {
            for (const std::wstring& norm_id : TakePendingReconcileVolumes()) {
                if (!running_) break;
                auto it = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& drive) {
                    return NormalizeVolumeId(drive.id) == norm_id;
                });
                if (it == online.end() || !it->online || !it->enabled) continue;
                if (VolumePauseRequestedOrActive(it->id, it->mount_point)) {
                    QueueVolumeReconcile(norm_id);
                    continue;
                }
                const auto retry = volume_retry_after_.find(it->id);
                if (retry != volume_retry_after_.end() && GetTickCount64() < retry->second) {
                    QueueVolumeReconcile(norm_id);
                    continue;
                }
                if (RebuildVolume(*it)) {
                    volume_retry_after_.erase(it->id);
                    SyncWalkWatches(online);
                    if (EffectiveBackend(*it) == VolumeBackend::NtfsMftUsn) {
                        StartJournalStreams();
                    }
                    changed = true;
                } else {
                    volume_retry_after_[it->id] = GetTickCount64() + 60000;
                    QueueVolumeReconcile(norm_id);
                }
            }
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
        TrimIdleMemory();
        if (filename_timing_.Due()) CaptureMemoryState();
        filename_timing_.Flush();
    }
    journal_streams_.clear();
    FlushDeltas();
    StopWalkWatches();
    filename_timing_.Flush(true);
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

DWORD CALLBACK Engine::WalkWatchRemovalCallback(
    HCMNOTIFICATION, void* context, CM_NOTIFY_ACTION action,
    PCM_NOTIFY_EVENT_DATA, DWORD) {
    auto* io = static_cast<WalkWatchIo*>(context);
    if (!io) return ERROR_SUCCESS;
    if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVE ||
        action == CM_NOTIFY_ACTION_DEVICEREMOVEPENDING ||
        action == CM_NOTIFY_ACTION_DEVICEREMOVECOMPLETE) {
        {
            std::lock_guard<std::mutex> lock(io->mu);
            if (io->dir != INVALID_HANDLE_VALUE) {
                io->eject_closed = true;
                CancelIoEx(io->dir, &io->ov);
                ::CloseHandle(io->dir);
                io->dir = INVALID_HANDLE_VALUE;
                if (io->event) SetEvent(io->event);
            }
        }
        if (io->engine) io->engine->NotifyTopologyChange();
    } else if (action == CM_NOTIFY_ACTION_DEVICEQUERYREMOVEFAILED) {
        io->reopen_needed = true;
        if (io->engine) io->engine->NotifyTopologyChange();
    }
    return ERROR_SUCCESS;
}

bool Engine::OpenWalkWatchHandle(WalkWatch& w) {
    if (w.path.empty()) return false;
    if (!w.io) w.io = std::make_unique<WalkWatchIo>();
    w.io->engine = this;
    w.io->buf.resize(64 * 1024);
    if (!w.event) w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!w.event) return false;
    w.io->event = w.event;
    if (PathPaused(w.path) || (!w.volume_id.empty() && VolumePaused(w.volume_id))) {
        w.dir = INVALID_HANDLE_VALUE;
        return false;
    }

    const std::wstring open_path = ToWin32LongPath(w.path);
    HANDLE h = CreateFileW(open_path.c_str(), FILE_LIST_DIRECTORY,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        w.dir = INVALID_HANDLE_VALUE;
        return false;
    }

    CM_NOTIFY_FILTER filter{};
    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEHANDLE;
    filter.u.DeviceHandle.hTarget = h;
    HCMNOTIFICATION removal = nullptr;
    if (CM_Register_Notification(&filter, w.io.get(), &Engine::WalkWatchRemovalCallback,
                                 &removal) != CR_SUCCESS) {
        removal = nullptr;
    }

    bool armed = false;
    {
        std::lock_guard<std::mutex> lock(w.io->mu);
        w.io->dir = h;
        w.io->removal = removal;
        w.io->eject_closed = false;
        w.io->reopen_needed = false;
        ResetEvent(w.event);
        ZeroMemory(&w.io->ov, sizeof(w.io->ov));
        w.io->ov.hEvent = w.event;
        const BOOL ok = ReadDirectoryChangesW(
            h, w.io->buf.data(), static_cast<DWORD>(w.io->buf.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &w.io->ov, nullptr);
        armed = ok != FALSE || GetLastError() == ERROR_IO_PENDING;
        if (!armed) {
            w.io->dir = INVALID_HANDLE_VALUE;
            w.io->removal = nullptr;
        }
    }
    if (!armed) {
        if (removal) CM_Unregister_Notification(removal);
        ::CloseHandle(h);
        w.dir = INVALID_HANDLE_VALUE;
        return false;
    }
    w.dir = h;
    return true;
}

void Engine::CloseWalkWatchHandle(WalkWatch& w) {
    if (!w.io) {
        if (w.dir != INVALID_HANDLE_VALUE) {
            ::CloseHandle(w.dir);
            w.dir = INVALID_HANDLE_VALUE;
        }
        return;
    }
    HCMNOTIFICATION removal = nullptr;
    {
        std::lock_guard<std::mutex> lock(w.io->mu);
        removal = w.io->removal;
        w.io->removal = nullptr;
    }
    // Never hold io->mu while unregistering CM notifications (callbacks also take io->mu).
    if (removal) CM_Unregister_Notification(removal);
    {
        std::lock_guard<std::mutex> lock(w.io->mu);
        if (w.io->dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(w.io->dir, &w.io->ov);
            DWORD completed = 0;
            GetOverlappedResult(w.io->dir, &w.io->ov, &completed, TRUE);
            ::CloseHandle(w.io->dir);
            w.io->dir = INVALID_HANDLE_VALUE;
        }
        w.io->eject_closed = false;
        w.io->reopen_needed = false;
    }
    w.dir = INVALID_HANDLE_VALUE;
}

void Engine::StopWalkWatches() {
    for (auto& w : watches_) {
        CloseWalkWatchHandle(w);
        if (w.event) {
            if (w.io) w.io->event = nullptr;
            ::CloseHandle(w.event);
            w.event = nullptr;
        }
    }
    watches_.clear();
}

void Engine::StartWalkWatches(const std::vector<std::wstring>& roots) {
    StopWalkWatches();
    watches_.reserve(roots.size());
    for (const auto& path : roots) {
        if (path.empty()) continue;
        watches_.emplace_back();
        auto& w = watches_.back();
        w.path = path;
        w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.io = std::make_unique<WalkWatchIo>();
        w.io->engine = this;
        w.io->event = w.event;
        w.io->buf.resize(64 * 1024);
        if (PathPaused(path)) continue;
        if (!OpenWalkWatchHandle(w)) {
            CloseWalkWatchHandle(w);
            if (w.event) {
                w.io->event = nullptr;
                ::CloseHandle(w.event);
            }
            watches_.pop_back();
        }
    }
}

void Engine::EnsureVolumeWalkWatch(const VolumeInfo& volume) {
    if (!volume.online || !volume.enabled || volume.id.empty() ||
        volume.mount_point.size() < 2 || EffectiveBackend(volume) != VolumeBackend::DirectoryWalk) {
        return;
    }
    const std::wstring norm_id = NormalizeVolumeId(volume.id);
    const std::wstring root_path =
        std::wstring(1, static_cast<wchar_t>(towupper(volume.mount_point[0]))) + L":\\";
    for (auto& w : watches_) {
        if (w.volume_id.empty() || NormalizeVolumeId(w.volume_id) != norm_id) continue;
        if (CompareStringOrdinal(w.path.data(), static_cast<int>(w.path.size()),
                                 root_path.data(), static_cast<int>(root_path.size()), TRUE) != CSTR_EQUAL) {
            CloseWalkWatchHandle(w);
            walk_pending_renames_.erase(w.path);
            w.path = root_path;
        }
        if (w.dir == INVALID_HANDLE_VALUE && !PathPaused(w.path) && !VolumePaused(w.volume_id)) {
            OpenWalkWatchHandle(w);
        }
        return;
    }
    watches_.emplace_back();
    auto& w = watches_.back();
    w.path = root_path;
    w.volume_id = volume.id;
    w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    w.io = std::make_unique<WalkWatchIo>();
    w.io->engine = this;
    w.io->event = w.event;
    w.io->buf.resize(64 * 1024);
    if (!PathPaused(w.path) && !VolumePaused(w.volume_id) && !OpenWalkWatchHandle(w)) {
        CloseWalkWatchHandle(w);
        if (w.event) {
            w.io->event = nullptr;
            ::CloseHandle(w.event);
            w.event = nullptr;
        }
        watches_.pop_back();
    }
}

void Engine::SyncWalkWatches(const std::vector<VolumeInfo>& online) {
    for (auto it = watches_.begin(); it != watches_.end();) {
        if (it->volume_id.empty()) {
            ++it;
            continue;
        }
        const std::wstring norm_id = NormalizeVolumeId(it->volume_id);
        const auto match = std::find_if(online.begin(), online.end(), [&](const VolumeInfo& v) {
            return v.online && v.enabled &&
                EffectiveBackend(v) == VolumeBackend::DirectoryWalk &&
                v.mount_point.size() >= 2 &&
                NormalizeVolumeId(v.id) == norm_id;
        });
        if (match == online.end()) {
            walk_pending_renames_.erase(it->path);
            CloseWalkWatchHandle(*it);
            if (it->event) {
                if (it->io) it->io->event = nullptr;
                ::CloseHandle(it->event);
                it->event = nullptr;
            }
            it = watches_.erase(it);
            continue;
        }
        const std::wstring desired_path =
            std::wstring(1, static_cast<wchar_t>(towupper(match->mount_point[0]))) + L":\\";
        if (CompareStringOrdinal(it->path.data(), static_cast<int>(it->path.size()),
                                 desired_path.data(), static_cast<int>(desired_path.size()), TRUE) != CSTR_EQUAL) {
            walk_pending_renames_.erase(it->path);
            CloseWalkWatchHandle(*it);
            it->path = desired_path;
            OpenWalkWatchHandle(*it);
        } else if (it->io && it->io->reopen_needed.exchange(false)) {
            CloseWalkWatchHandle(*it);
            if (OpenWalkWatchHandle(*it)) {
                QueueVolumeReconcile(it->volume_id);
                GapFeed();
            }
        } else if (it->io) {
            std::lock_guard<std::mutex> lock(it->io->mu);
            if (it->io->dir == INVALID_HANDLE_VALUE) it->dir = INVALID_HANDLE_VALUE;
        }
        ++it;
    }
    for (const auto& volume : online) {
        EnsureVolumeWalkWatch(volume);
    }
}

void Engine::PollWalkWatches() {
    auto is_device_gone_error = [](DWORD err) {
        return err == ERROR_ACCESS_DENIED || err == ERROR_INVALID_HANDLE ||
            err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND ||
            err == ERROR_DEVICE_NOT_CONNECTED || err == ERROR_BAD_COMMAND ||
            err == ERROR_GEN_FAILURE || err == ERROR_NOT_READY;
    };

    for (auto& w : watches_) {
        if (!w.io) continue;
        DWORD n = 0;
        DWORD ov_error = ERROR_SUCCESS;
        std::vector<BYTE> packet;
        bool had_handle = false;
        bool device_gone = false;
        bool eject_closed = false;
        bool overflow = false;

        {
            std::lock_guard<std::mutex> lock(w.io->mu);
            eject_closed = w.io->eject_closed.exchange(false);
            if (w.io->dir == INVALID_HANDLE_VALUE) {
                w.dir = INVALID_HANDLE_VALUE;
                if (eject_closed) {
                    if (w.event) ResetEvent(w.event);
                    walk_pending_renames_.erase(w.path);
                    topology_dirty_ = true;
                }
                continue;
            }
            had_handle = true;
            if (!GetOverlappedResult(w.io->dir, &w.io->ov, &n, FALSE)) {
                ov_error = GetLastError();
                if (ov_error == ERROR_IO_INCOMPLETE) continue;
                n = 0;
            }
            if (n > 0) {
                packet.assign(w.io->buf.data(), w.io->buf.data() + n);
            }
            if (w.event) ResetEvent(w.event);
            ZeroMemory(&w.io->ov, sizeof(w.io->ov));
            w.io->ov.hEvent = w.event;
            const BOOL armed = ReadDirectoryChangesW(
                w.io->dir, w.io->buf.data(), static_cast<DWORD>(w.io->buf.size()), TRUE,
                FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                nullptr, &w.io->ov, nullptr);
            if (!armed && GetLastError() != ERROR_IO_PENDING) {
                const DWORD arm_err = GetLastError();
                if (is_device_gone_error(arm_err) || is_device_gone_error(ov_error)) {
                    device_gone = true;
                    ::CloseHandle(w.io->dir);
                    w.io->dir = INVALID_HANDLE_VALUE;
                    w.dir = INVALID_HANDLE_VALUE;
                } else {
                    overflow = true;
                }
            } else if (n == 0) {
                if (ov_error == ERROR_NOTIFY_ENUM_DIR || ov_error == ERROR_SUCCESS) {
                    overflow = true;
                } else if (is_device_gone_error(ov_error)) {
                    device_gone = true;
                    ::CloseHandle(w.io->dir);
                    w.io->dir = INVALID_HANDLE_VALUE;
                    w.dir = INVALID_HANDLE_VALUE;
                } else if (!eject_closed) {
                    overflow = true;
                }
            }
        }
        if (!had_handle) continue;

        if (n == 0) {
            walk_pending_renames_.erase(w.path);
            if (eject_closed || device_gone) {
                topology_dirty_ = true;
            } else if (overflow) {
                GapFeed();
                if (!w.volume_id.empty()) {
                    QueueVolumeReconcile(w.volume_id);
                } else {
                    rebuild_requested_ = true;
                }
            }
        }
        if (n > 0) {
            std::vector<std::wstring> added_dirs;
            uint64_t applied = 0;
            const auto timing = FilenameTiming::Begin();
            {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                applied = ApplyNotifyLocked(w.path, packet.data(), n, w.volume_id, &added_dirs);
                if (applied) {
                    ++revision_;
                    TraceSearch("filename_commit", revision_.load(), w.path);
                }
            }
            if (applied && notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);
            filename_timing_.End(FilenameStage::Notify, timing, applied, ERROR_SUCCESS, "directory_changes");
            if (!added_dirs.empty()) IndexAddedDirectories(added_dirs, w.volume_id);
        }
    }
}

void Engine::IndexAddedDirectories(const std::vector<std::wstring>& dirs, std::wstring_view volume_id) {
    if (dirs.empty() || !running_.load()) return;
    constexpr size_t kAddedDirCap = 4096;

    std::vector<std::wstring> local_excluded;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        local_excluded = excluded_paths_;
    }
    auto is_excluded_local = [&](std::wstring_view path) -> bool {
        for (const auto& ex : local_excluded) {
            if (ex.empty() || path.size() < ex.size()) continue;
            if (CompareStringOrdinal(path.data(), static_cast<int>(ex.size()),
                                     ex.data(), static_cast<int>(ex.size()), TRUE) != CSTR_EQUAL) continue;
            if (path.size() == ex.size() || path[ex.size()] == L'\\' || path[ex.size()] == L'/') return true;
        }
        return false;
    };

    struct PendingItem {
        int32_t parent_local = -1;
        std::wstring path;
        bool is_dir = false;
        bool hidden = false;
        uint64_t size = 0;
        uint64_t mtime_ft = 0;
    };

    std::vector<PendingItem> collected;
    bool hit_cap = false;
    for (const auto& dir : dirs) {
        if (!running_.load() || collected.size() >= kAddedDirCap) break;
        std::wstring root_clean = dir;
        while (root_clean.size() > 3 && (root_clean.back() == L'\\' || root_clean.back() == L'/')) {
            root_clean.pop_back();
        }
        if (root_clean.empty()) continue;

        std::vector<std::wstring> paths_by_id;
        paths_by_id.push_back(root_clean);

        WalkCallbacks cb;
        cb.should_cancel = [this]() { return !running_.load(); };
        cb.should_skip_name = [this](bool, std::wstring_view name) {
            return ShouldSkipName(name) || IsIndexArtifactName(name);
        };
        cb.is_excluded_path = is_excluded_local;
        cb.emit_node = [&](int32_t local_parent, std::wstring_view name, bool is_dir,
                           bool hidden, uint64_t size, uint64_t mtime_ft) -> int32_t {
            if (local_parent < 0 || static_cast<size_t>(local_parent) >= paths_by_id.size()) return -1;
            std::wstring full = paths_by_id[static_cast<size_t>(local_parent)];
            if (!full.empty() && full.back() != L'\\') full.push_back(L'\\');
            full.append(name);
            const int32_t local_id = static_cast<int32_t>(paths_by_id.size());
            paths_by_id.push_back(full);
            collected.push_back(PendingItem{local_parent, std::move(full), is_dir, hidden, size, mtime_ft});
            return local_id;
        };

        WalkStats stats;
        const size_t remaining = kAddedDirCap - collected.size();
        const bool ok = WalkDirectoryTree(root_clean, 0, false, remaining, cb, &stats);
        if (!ok && stats.hit_cap) {
            hit_cap = true;
            break;
        }
    }

    if (hit_cap) {
        GapFeed();
        if (!volume_id.empty()) QueueVolumeReconcile(volume_id);
        else rebuild_requested_ = true;
        return;
    }
    if (collected.empty()) return;

    bool any_added = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        const int32_t previous_count = LiveCount();
        for (const auto& item : collected) {
            const int32_t existing = ResolvePathLocked(item.path);
            if (existing >= 0 && !IsTomb(existing)) continue;
            const int32_t idx = EnsureChainLocked(live_, item.path, item.is_dir, true);
            if (idx < 0) continue;
            if (item.hidden) {
                const int32_t base = BaseCount();
                if (idx >= base) live_.nodes[static_cast<size_t>(idx - base)].flags |= kFlagHidden;
                else {
                    Patch& pt = patches_[idx];
                    Node n = NodeAt(idx);
                    pt.parent = n.parent;
                    pt.flags = n.flags | kFlagHidden;
                    pt.has_meta = true;
                }
            }
            Attr a;
            a.size = item.is_dir ? 0 : item.size;
            a.mtime = FtToUnix(item.mtime_ft);
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
            if (!item.hidden && !IsExcludedPath(item.path)) {
                ChangeRecord event;
                event.path = item.path;
                event.kind = ChangeKind::Created;
                event.is_dir = item.is_dir;
                event.time = a.mtime;
                RecordFeed(event);
                changes_.Record(std::move(event));
            }
            any_added = true;
        }
        if (LiveCount() > previous_count) {
            const wchar_t hint_letter = !dirs.front().empty() && dirs.front().size() >= 2 && dirs.front()[1] == L':'
                ? dirs.front()[0] : 0;
            DeltaLog* delta = (!volume_id.empty() || !vols_.empty())
                ? DeltaForVolume(volume_id, hint_letter) : DeltaFor(0);
            if (delta) {
                for (int32_t id = previous_count; id < LiveCount(); ++id) {
                    const auto node = NodeAt(id);
                    const auto attr = AttrAt(id);
                    delta->QueueAdd(id, node.parent, node.flags, attr.mtime, attr.size, NameOf(id), 0);
                }
            } else {
                GapFeed();
                if (!volume_id.empty()) QueueVolumeReconcile(volume_id);
                else rebuild_requested_ = true;
            }
            indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
            InvalidateFilterLocked();
            ++struct_changes_;
            last_struct_tick_ = GetTickCount64();
            ++revision_;
        }
    }
    if (any_added && notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);
}

uint64_t Engine::ApplyNotifyLocked(const std::wstring& root, const BYTE* buf, DWORD len,
                                   std::wstring_view volume_id,
                                   std::vector<std::wstring>* added_dirs) {
    uint64_t applied = 0;
    std::wstring& pending_old = walk_pending_renames_[root];
    const wchar_t fallback_letter = (root.size() >= 2 && root[1] == L':') ? root[0] : 0;
    DeltaLog* delta = (!volume_id.empty() || !vols_.empty())
        ? DeltaForVolume(volume_id, fallback_letter) : DeltaFor(0);
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
        const std::wstring long_full = ToWin32LongPath(full);
        if (info->Action == FILE_ACTION_ADDED) {
            event.kind = ChangeKind::Created;
            DWORD attributes = GetFileAttributesW(long_full.c_str());
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
            const bool ok = GetFileAttributesExW(long_full.c_str(), GetFileExInfoStandard, &fad);
            const bool is_dir = ok && (fad.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const bool is_reparse = ok && (fad.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            int32_t idx = EnsureChainLocked(live_, full, is_dir, true);
            bool should_hide = false;
            if (idx >= 0) {
                const auto leaf_slash = full.find_last_of(L"\\/");
                const std::wstring_view leaf = leaf_slash == std::wstring::npos
                    ? std::wstring_view(full) : std::wstring_view(full).substr(leaf_slash + 1);
                const int32_t parent_id = NodeAt(idx).parent;
                const bool parent_is_root = parent_id >= 0 && NodeAt(parent_id).parent < 0;
                const bool hidden_parent = parent_id >= 0 && NodeAt(parent_id).parent >= 0 &&
                    (NodeAt(parent_id).flags & kFlagHidden) != 0;
                should_hide = hidden_parent || ShouldSkipChild(parent_is_root, leaf) || IsExcludedPath(full);
                if (should_hide) {
                    const int32_t base = BaseCount();
                    if (idx >= base) live_.nodes[static_cast<size_t>(idx - base)].flags |= kFlagHidden;
                    else {
                        Patch& pt = patches_[idx];
                        Node n = NodeAt(idx);
                        pt.parent = n.parent;
                        pt.flags = n.flags | kFlagHidden;
                        pt.has_meta = true;
                    }
                }
            }
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
            if (idx >= 0 && is_dir && !is_reparse && !should_hide && added_dirs) {
                added_dirs->push_back(full);
            }
            indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
            InvalidateFilterLocked();
            break;
        }
        case FILE_ACTION_REMOVED: {
            int32_t idx = ResolvePathLocked(full);
            if (idx >= 0 && !IsTomb(idx)) {
                const bool removed_dir = (NodeAt(idx).flags & kFlagDir) != 0;
                std::vector<int32_t> subtree;
                if (removed_dir) CollectLiveSubtreeLocked(idx, subtree);
                else subtree.push_back(idx);
                for (auto it = subtree.rbegin(); it != subtree.rend(); ++it) {
                    const int32_t sub = *it;
                    if (sub < 0 || IsTomb(sub)) continue;
                    if (sub != idx) {
                        ChangeRecord child_ev;
                        child_ev.path = BuildPathLocked(sub);
                        child_ev.kind = ChangeKind::Deleted;
                        child_ev.is_dir = (NodeAt(sub).flags & kFlagDir) != 0;
                        if (!child_ev.path.empty()) {
                            RecordFeed(child_ev);
                            changes_.Record(std::move(child_ev));
                        }
                    }
                    ChildMapRemove(NodeAt(sub).parent, NameOf(sub), sub);
                    tombstones_.insert(sub);
                    ++deleted_;
                    if (delta && sub < previous_count && sub != changed_id) {
                        delta->QueueTomb(sub);
                    }
                }
                indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
                InvalidateFilterLocked();
            }
            break;
        }
        case FILE_ACTION_MODIFIED: {
            int32_t idx = ResolvePathLocked(full);
            if (idx >= 0) {
                WIN32_FILE_ATTRIBUTE_DATA fad{};
                if (GetFileAttributesExW(long_full.c_str(), GetFileExInfoStandard, &fad)) {
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
                const bool parent_is_root = new_parent >= 0 && NodeAt(new_parent).parent < 0;
                const bool hidden_parent = new_parent >= 0 && NodeAt(new_parent).parent >= 0 &&
                    (NodeAt(new_parent).flags & kFlagHidden) != 0;
                uint8_t updated_flags = static_cast<uint8_t>(old.flags & ~kFlagHidden);
                if (hidden_parent || ShouldSkipChild(parent_is_root, new_name) || IsExcludedPath(full)) {
                    updated_flags |= kFlagHidden;
                }
                const int32_t base = BaseCount();
                if (idx >= base) {
                    Node& n = live_.nodes[static_cast<size_t>(idx - base)];
                    n.parent = new_parent;
                    n.flags = updated_flags;
                    pool_waste_ += n.len;
                    n.off = static_cast<uint32_t>(live_.pool.size());
                    n.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + n.len);
                } else {
                    Patch& pt = patches_[idx];
                    pt.parent = new_parent;
                    pt.flags = updated_flags;
                    pt.has_meta = true;
                    if (pt.has_name) pool_waste_ += pt.len;
                    pt.has_name = true;
                    pt.off = static_cast<uint32_t>(live_.pool.size());
                    pt.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + pt.len);
                }
                ChildMapAdd(new_parent, new_name, idx);
                if (updated_flags & kFlagDir) {
                    RefreshSubtreeVisibilityLocked(idx, delta);
                }
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
            if (delta) {
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
            } else if (!volume_id.empty()) {
                GapFeed();
                QueueVolumeReconcile(volume_id);
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

#include "index_volume_pause_impl.h"
