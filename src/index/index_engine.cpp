// index_engine.cpp — mmap v6 base + heap delta (优化.md).
#include "index_engine.h"
#include "index_config.h"
#include "index_query.h"
#include "index_mft.h"
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
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>

namespace pulse::index {

namespace {

constexpr size_t kIndexCap = 4000000;
constexpr size_t kFrnMergeThreshold = 4096;
constexpr ULONGLONG kMinMergeIntervalMs = 10ull * 60ull * 1000ull;
constexpr ULONGLONG kIdleMergeQuietMs = 10ull * 60ull * 1000ull;
constexpr ULONGLONG kDeltaFlushMs = 15ull * 1000ull;
constexpr size_t kMergeStructChanges = 100000;
constexpr uint64_t kMergeDeltaBytes = 64ull * 1024ull * 1024ull;
constexpr uint64_t kCacheFreshSecs = 24ull * 60 * 60;
constexpr uint32_t kIndexVer = kIndexSnapshotVersion;
constexpr uint32_t kIndexVerMin = 7;
constexpr uint64_t kUnixFtEpoch = 116444736000000000ull;
constexpr ULONGLONG kNotifyMinMs = 500;
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

std::vector<VolumeInfo> ConfiguredVolumes() {
    IndexConfig config;
    if (MachineIndexScope()) LoadMachineConfig(config, nullptr);
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
    const BYTE* b = static_cast<const BYTE*>(p);
    while (n) {
        DWORD w = 0;
        DWORD chunk = n > 1u << 20 ? (1u << 20) : static_cast<DWORD>(n);
        if (!WriteFile(h, b, chunk, &w, nullptr) || w == 0) return false;
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
    const auto active = ConfiguredVolumes();
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        UpdateVolumeVisibilityLocked(active, true);
    }
    rebuild_requested_.store(true);
    SetStatus(L"索引配置已更改，准备重建…");
    PingNotify(true);
}

std::vector<VolumeInfo> Engine::Volumes() const {
    IndexConfig config;
    if (MachineIndexScope()) LoadMachineConfig(config, nullptr);
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
    thread_ = std::thread(&Engine::Worker, this);
}

void Engine::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    changes_.Flush();
    StopWalkWatches();
    FlushDeltas();
    CloseDeltas();
    std::unique_lock<std::shared_mutex> lock(mutex_);
    query_shards_.clear();
    query_shards_ready_ = false;
    if (map_) map_->Close();
    map_.reset();
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
    int32_t chain[64];
    int depth = 0;
    for (int32_t i = node; i >= 0 && depth < 64; i = NodeAt(i).parent)
        chain[depth++] = i;
    std::wstring out;
    for (int k = depth - 1; k >= 0; --k) {
        std::wstring_view seg = NameOf(chain[k]);
        if (!out.empty() && out.back() != L'\\') out += L'\\';
        out.append(seg.data(), seg.size());
        if (k == depth - 1 && seg.size() == 2 && seg[1] == L':') out += L'\\';
    }
    return out;
}

bool Engine::IsUnderLocked(int32_t node, int32_t ancestor) const {
    return InSubtreeLocked(node, ancestor);
}

int32_t Engine::SubtreeEndLocked(int32_t node) const {
    if (node < 0) return -1;
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
    for (int32_t i = node; i >= 0;) {
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
                    for (int32_t j = i; j >= 0; j = NodeAt(j).parent) {
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
    if (only_hide) {
        for (int32_t root : inactive)
            if (std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), root) ==
                inactive_volume_roots_.end()) inactive_volume_roots_.push_back(root);
    } else {
        inactive_volume_roots_ = std::move(inactive);
    }
    InvalidateFilterLocked();
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
        const bool single_char = term.name.size() == 1;
        const uint32_t bucket = single_char ? PrefixChar(term.name) : PairBucket(term.name);
        const bool direct_single_char = single_char &&
            term.name_how == NameHow::Substring && prefix_node < 0 && !folders_only &&
            live_.nodes.empty() && patches_.empty() && tombstones_.empty() &&
            inactive_volume_roots_.empty();
        auto collect_table = [&](const MappedFile& mapped, int32_t global_first) {
            const uint32_t* starts = single_char ? mapped.prefix1_start : mapped.prefix2_start;
            const int32_t* postings = single_char ? mapped.prefix1_ids : mapped.prefix2_ids;
            if (!starts || !postings || (single_char && !mapped.prefix1_all_chars)) return;
            const uint32_t first = starts[bucket];
            const uint32_t last = starts[bucket + 1];
            for (uint32_t p = first; p < last; ++p) {
                if ((p & 0x3ffu) == 0 && latest && latest->load() != expected) return;
                const int32_t local_id = postings[p];
                const int32_t id = global_first + local_id;
                if (id < 0 || id >= BaseCount()) continue;
                if (direct_single_char) {
                    if (local_id < 0 || local_id >= static_cast<int32_t>(mapped.n)) continue;
                    const Node& node = mapped.nodes[static_cast<size_t>(local_id)];
                    if (node.parent >= 0 && !(node.flags & (kFlagHidden | kFlagDeleted)))
                        ids.Add(id);
                } else if (MatchQueryNodeLocked(id, cq, prefix_node, folders_only, use_attrs)) {
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
    if (!scoped && map_ && map_->hdr && map_->hdr->ver >= 8 && !vols_.empty()) {
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
                            uint32_t expected) const {
    SearchResult out;
    std::lock_guard<std::mutex> query_guard(query_mu_);
    if (latest && latest->load() != expected) return out;
    CompiledQuery cq = ParseQuery(q.needle);
    std::shared_lock<std::shared_mutex> lock(mutex_);
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
    out.total = matches.total;
    if (cap == 0 || matches.total == 0) {
        cache_raw_ = q.needle;
        cache_path_prefix_ = prefix;
        cache_folders_only_ = q.folders_only;
        cache_ranked_ = q.rank;
        cache_sort_ = q.sort;
        cache_sort_desc_ = q.sort_desc;
        cache_set_ = matches;
        cache_epoch_ = filter_epoch_;
        return out;
    }

    cache_raw_ = q.needle;
    cache_path_prefix_ = prefix;
    cache_folders_only_ = q.folders_only;
    cache_ranked_ = q.rank;
    cache_sort_ = q.rank ? ResultSort::Index : q.sort;
    cache_sort_desc_ = q.rank ? false : q.sort_desc;
    cache_set_ = matches;
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
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    auto name_of = [&](int32_t id) -> std::wstring_view {
        const Node& node = s.nodes[static_cast<size_t>(id)];
        return { s.pool.data() + node.off, node.len };
    };
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
        for (uint32_t i = 0; i < kPrefixBuckets; ++i)
            start[i + 1] = start[i] + counts[i];
        std::vector<uint32_t> cursor(start.begin(), start.end() - 1);
        std::vector<int32_t> ids(start.back(), -1);
        for (int32_t i = 0; i < static_cast<int32_t>(s.nodes.size()); ++i)
            visit(name_of(i), [&](uint32_t bucket) { ids[cursor[bucket]++] = i; });
        return std::make_pair(std::move(start), std::move(ids));
    };
    const auto prefix1 = build_prefix(false);
    const auto prefix2 = build_prefix(true);

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
    bool ok = WriteAll(h, pad.data(), pad.size());
    if (ok && !s.nodes.empty())
        ok = WriteAll(h, s.nodes.data(), s.nodes.size() * sizeof(Node));
    if (ok) {
        const uint64_t at = hdr.nodes_off + s.nodes.size() * sizeof(Node);
        if (hdr.attrs_off > at) {
            std::vector<BYTE> z(static_cast<size_t>(hdr.attrs_off - at), 0);
            ok = WriteAll(h, z.data(), z.size());
        }
    }
    if (ok && !s.attrs.empty())
        ok = WriteAll(h, s.attrs.data(), s.attrs.size() * sizeof(Attr));
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.pool_off);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
    }
    if (ok && !s.pool.empty())
        ok = WriteAll(h, s.pool.data(), s.pool.size() * sizeof(wchar_t));
    auto write_order = [&](uint64_t position, const std::vector<int32_t>& order) {
        if (!ok) return;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(position);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
        if (ok && !order.empty()) ok = WriteAll(h, order.data(), order.size() * sizeof(int32_t));
    };
    write_order(hdr.child_order_off, child_order);
    auto write_prefix = [&](uint64_t position, const std::vector<uint32_t>& start,
                            const std::vector<int32_t>& ids) {
        if (!ok) return;
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(position);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
        if (ok) ok = WriteAll(h, start.data(), start.size() * sizeof(uint32_t));
        if (ok && !ids.empty()) ok = WriteAll(h, ids.data(), ids.size() * sizeof(int32_t));
    };
    write_prefix(prefix1_position, prefix1.first, prefix1.second);
    write_prefix(hdr.prefix2_off, prefix2.first, prefix2.second);
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.vols_off);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
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
        ok = WriteAll(h, dvols.data(), dvols.size() * sizeof(DiskVol));
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.frn_off);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
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
            if (!WriteAll(h, rows.data(), rows.size() * sizeof(DiskFrn))) { ok = false; break; }
        }
    }
    CloseHandle(h);
    if (!ok) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool Engine::CommitMappedFile(const std::wstring& path) {
    const std::wstring tmp = path + L".tmp";
    std::wstring published_path = path;
    if (MachineIndexScope()) {
        const ShardPaths paths = AggregateShardPaths();
        ShardManifest published;
        if (!PublishShardBase(paths, tmp, built_unix_, 0, published, nullptr)) return false;
        published_path = published.active_slot == 0 ? paths.base_a : paths.base_b;
    } else {
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
    }
    if (map_) {
        map_->Close();
        map_.reset();
    }
    std::unique_ptr<MappedFile> mapped;
    if (!MapIndexFile(published_path, mapped)) return false;
    AdoptMappedLocked(std::move(mapped));
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
        PublishShardBase(paths, temp, built_unix, 0, published, nullptr, source.volume_id);
    }
}

bool Engine::MapIndexFile(const std::wstring& path, std::unique_ptr<MappedFile>& out) const {
    auto m = std::make_unique<MappedFile>();
    m->file = CreateFileW(path.c_str(), GENERIC_READ,
                          FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                          nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (m->file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(m->file, &sz) || sz.QuadPart < static_cast<LONGLONG>(sizeof(DiskHeader)))
        return false;
    m->size = static_cast<size_t>(sz.QuadPart);
    m->mapping = CreateFileMappingW(m->file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!m->mapping) return false;
    m->view = static_cast<const uint8_t*>(MapViewOfFile(m->mapping, FILE_MAP_READ, 0, 0, 0));
    if (!m->view) return false;
    m->hdr = reinterpret_cast<const DiskHeader*>(m->view);
    if (memcmp(m->hdr->magic, "PIDX", 4) != 0 ||
        m->hdr->ver < kIndexVerMin || m->hdr->ver > kIndexVer) return false;
    if (m->hdr->node_count > kIndexCap + 64) return false;
    auto in_range = [&](uint64_t o, uint64_t n) {
        return o <= m->size && n <= m->size && o + n <= m->size;
    };
    if (!in_range(m->hdr->nodes_off, sizeof(Node) * m->hdr->node_count)) return false;
    if (!in_range(m->hdr->attrs_off, sizeof(Attr) * m->hdr->node_count)) return false;
    if (!in_range(m->hdr->pool_off, sizeof(wchar_t) * m->hdr->pool_chars)) return false;
    if (!in_range(m->hdr->child_order_off, sizeof(int32_t) * m->hdr->node_count)) return false;
    if (m->hdr->name_order_off &&
        !in_range(m->hdr->name_order_off, sizeof(int32_t) * m->hdr->node_count)) return false;
    if (m->hdr->size_order_off &&
        !in_range(m->hdr->size_order_off, sizeof(int32_t) * m->hdr->node_count)) return false;
    if (m->hdr->mtime_order_off &&
        !in_range(m->hdr->mtime_order_off, sizeof(int32_t) * m->hdr->node_count)) return false;
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
        return true;
    };
    if (!prefix_in_range(m->hdr->prefix1_off, true, m->prefix1_start, m->prefix1_ids,
                         m->prefix1_all_chars)) return false;
    bool unused_prefix_flag = false;
    if (!prefix_in_range(m->hdr->prefix2_off, false, m->prefix2_start, m->prefix2_ids,
                         unused_prefix_flag)) return false;
    if (!in_range(m->hdr->vols_off, sizeof(DiskVol) * m->hdr->vol_count)) return false;
    if (!in_range(m->hdr->frn_off, sizeof(DiskFrn) * m->hdr->frn_count)) return false;
    m->nodes = reinterpret_cast<const Node*>(m->view + m->hdr->nodes_off);
    m->attrs = reinterpret_cast<const Attr*>(m->view + m->hdr->attrs_off);
    m->pool = reinterpret_cast<const wchar_t*>(m->view + m->hdr->pool_off);
    m->n = m->hdr->node_count;
    for (uint32_t i = 0; i < m->n; ++i) {
        const Node& node = m->nodes[i];
        if (static_cast<uint64_t>(node.off) + node.len > m->hdr->pool_chars) return false;
    }
    m->vols = m->hdr->vol_count
        ? reinterpret_cast<const DiskVol*>(m->view + m->hdr->vols_off) : nullptr;
    m->nvol = m->hdr->vol_count;
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
        if (m->child_order[i] < 0 || m->child_order[i] >= static_cast<int32_t>(m->n)) return false;
        if (m->name_order && (m->name_order[i] < 0 || m->name_order[i] >= static_cast<int32_t>(m->n)))
            return false;
        if (m->size_order && (m->size_order[i] < 0 || m->size_order[i] >= static_cast<int32_t>(m->n)))
            return false;
        if (m->mtime_order && (m->mtime_order[i] < 0 || m->mtime_order[i] >= static_cast<int32_t>(m->n)))
            return false;
    }
    out = std::move(m);
    return true;
}

void Engine::AdoptMappedLocked(std::unique_ptr<MappedFile> mapped) {
    query_shards_.clear();
    query_shards_ready_ = false;
    map_ = std::move(mapped);
    live_.Clear();
    live_.Shrink();
    tombstones_.clear();
    patches_.clear();
    vols_.clear();
    inactive_volume_roots_.clear();
    deleted_ = 0;
    pool_waste_ = 0;
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
    std::vector<std::wstring_view> parts;
    for (int32_t current = id; current >= 0 && parts.size() < 256;) {
        parts.push_back(QueryNameOfLocked(current));
        current = QueryNodeAtLocked(current).parent;
    }
    std::wstring path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        if (!path.empty() && path.back() != L'\\') path += L'\\';
        path.append(it->data(), it->size());
        if (it == parts.rbegin() && it->size() == 2 && (*it)[1] == L':') path += L'\\';
    }
    return path;
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
                    for (int32_t current = id; current >= 0;
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
    bool saved = true;
    for (auto& [k, log] : delta_logs_)
        if (log && !log->Flush()) saved = false;
    if (!saved) SetStatus(L"索引更新暂时无法保存，正在等待重试；请检查磁盘空间和权限");
    last_delta_flush_tick_ = GetTickCount64();
}

void Engine::OpenDeltasLocked() {
    CloseDeltas();
    if (vols_.empty()) {
        auto log = std::make_unique<DeltaLog>();
        const std::wstring path = DeltaFilePath(0);
        if (!path.empty() && log->Open(path, built_unix_))
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

void Engine::ReplayDeltasLocked() {
    auto apply = [&](const std::wstring& vid, DeltaOp op, int32_t idx, int32_t parent,
                     uint8_t flags, uint8_t which, uint32_t mtime, uint64_t size,
                     std::wstring_view name, uint64_t frn, uint64_t journal_id,
                     int64_t next_usn) {
        // Match by stable volume id: a letter reused across sessions must not
        // pull another volume's records into this one.
        VolState* vol = nullptr;
        for (auto& v : vols_)
            if (vid.empty() || NormalizeVolumeId(v.volume_id) == NormalizeVolumeId(vid)) {
                vol = &v;
                break;
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
            if (static_cast<size_t>(LiveCount()) >= kIndexCap + 64) return;
            if (!name.data() && !name.empty()) return;
            AddNodeLocked(live_, parent, name, flags, frn, size, UnixToFt(mtime), true, vol);
            return;
        }
        if (op == DeltaOp::Patch && idx >= 0) {
            if (idx >= LiveCount()) return;
            Patch& p = patches_[idx];
            if (which & static_cast<uint8_t>(PatchBits::Meta)) {
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
                p.off = static_cast<uint32_t>(live_.pool.size());
                p.len = static_cast<uint16_t>(name.size());
                live_.pool.insert(live_.pool.end(), name.begin(), name.end());
                p.has_name = true;
            }
        }
    };
    if (vols_.empty()) {
        DeltaLog::Replay(DeltaFilePath(0), built_unix_,
            [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                uint64_t journal_id, int64_t next_usn) {
                apply(L"", op, idx, parent, flags, which, mtime, size, name, frn,
                      journal_id, next_usn);
            });
        return;
    }
    for (const auto& v : vols_) {
        std::wstring path = DeltaFilePathForVolume(v.volume_id);
        if (path.empty()) path = DeltaFilePath(v.letter);
        if (!DeltaLog::Replay(path, built_unix_,
                [&](DeltaOp op, int32_t idx, int32_t parent, uint8_t flags, uint8_t which,
                    uint32_t mtime, uint64_t size, std::wstring_view name, uint64_t frn,
                    uint64_t journal_id, int64_t next_usn) {
                    apply(v.volume_id, op, idx, parent, flags, which, mtime, size, name, frn,
                          journal_id, next_usn);
                })) {
            DeleteFileW(path.c_str());
        }
    }
    RebuildChildMapLocked();
    indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
    InvalidateFilterLocked();
}

void Engine::MergeBase(bool force) {
    if (merging_.exchange(true)) return;
    const ULONGLONG now = GetTickCount64();
    if (!force && last_merge_tick_ && now - last_merge_tick_ < kMinMergeIntervalMs) {
        merging_ = false;
        return;
    }
    const std::wstring path = CachePath();
    if (path.empty()) {
        merging_ = false;
        return;
    }
    Store snap;
    std::vector<VolState> vols;
    uint64_t built = static_cast<uint64_t>(std::time(nullptr));
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (LiveCount() == 0) {
            merging_ = false;
            return;
        }
        FlattenLocked(snap, vols);
        built_unix_ = built;
    }
    const bool wrote = WriteIndexFile(path, snap, vols, built);
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
        }
    }
    if (committed && MachineIndexScope()) {
        WriteVolumeShards(snap, vols, built);
        std::unique_lock<std::shared_mutex> lock(mutex_);
        RefreshQueryShardsLocked();
    }
    merging_ = false;
}

bool Engine::TryLoadCache() {
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
    ReplayDeltasLocked();
    OpenDeltasLocked();
    SetStatus(L"已加载 " + std::to_wstring(indexed_.load()) + L" 项");
    ready_ = true;
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

void Engine::MapFrnLocked(VolState& v, uint64_t frn, int32_t idx) {
    v.frn_new.push_back(DiskFrn{ frn, idx, 0 });
    if (v.frn_build.size() || (!v.frn_base && v.frn_new.size() < kFrnMergeThreshold)) {
        v.frn_build.emplace_back(frn, idx);
    }
}

void Engine::ResolveIndexDirFrn() {
    const std::wstring dir = DataDir();
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
    if (IsIndexNoiseLocked(v, rec)) return UsnApply::None;
    std::wstring_view name(
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset),
        rec->FileNameLength / sizeof(WCHAR));
    if (name.empty()) return UsnApply::None;
    const DWORD reason = rec->Reason;
    const bool structural_reason = (reason & (USN_REASON_FILE_CREATE | USN_REASON_FILE_DELETE |
                                              USN_REASON_RENAME_NEW_NAME)) != 0;
    const bool attr_reason = (reason & (USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
                                        USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE)) != 0;
    if (!structural_reason && !attr_reason) return UsnApply::None;

    const uint64_t frn = rec->FileReferenceNumber;
    const bool is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int32_t idx = FindByFrnLocked(v, frn);
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
            changes_.Gap(); return;
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
                    v.tracking_paths.erase(v.tracking_paths.begin()); changes_.Gap();
                }
                v.tracking_paths[frn] = event.path;
            } else v.tracking_paths.erase(frn);
        }
        if (kind != ChangeKind::Renamed && had_tracking_path) event.path = previous_path;
        const bool visible = target >= 0 && !(NodeAt(target).flags & kFlagHidden) && !IsExcludedPath(event.path);
        if (!visible && !previous_visible) return;
        if (kind == ChangeKind::Renamed && !previous_visible && !had_tracking_path) {
            event.old_path.clear(); event.kind = ChangeKind::Created;
        }
        changes_.Record(std::move(event));
    };
    DeltaLog* delta = DeltaFor(v.letter);
    UsnApply effect = UsnApply::None;

    auto refresh = [&](int32_t i) {
        std::wstring path = BuildPathLocked(i);
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
        if (effect == UsnApply::None) effect = UsnApply::Attr;
    };

    if (reason & USN_REASON_FILE_DELETE) {
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
    if (idx >= 0 && idx < LiveCount() && attr_reason) refresh(idx);
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
    for (int32_t a = parent; a > v.root_idx && a >= 0;) {
        const Node an = NodeAt(a);
        if (an.flags & kFlagHidden) { flags |= kFlagHidden; break; }
        a = an.parent;
    }

    if (idx >= 0 && idx < LiveCount()) {
        const Node old = NodeAt(idx);
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
        delta->QueueAdd(parent, flags, a.mtime, a.size, name, frn);
    }
    track(ChangeKind::Created, idx);
    return UsnApply::Structure;
}

bool Engine::CatchUpVolume(VolState& v, bool* changed, bool* structural) {
    uint64_t journal_id = 0;
    int64_t start_usn = 0;
    wchar_t letter = 0;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        if (v.journal_id == 0) return false;
        journal_id = v.journal_id;
        start_usn = v.next_usn;
        letter = v.letter;
    }
    HANDLE h = OpenVolume(letter);
    if (h == INVALID_HANDLE_VALUE) return false;

    READ_USN_JOURNAL_DATA_V0 rud{};
    rud.StartUsn = start_usn;
    rud.ReasonMask = 0xFFFFFFFF;
    rud.UsnJournalID = journal_id;
    std::vector<BYTE> blob;
    blob.reserve(256 * 1024);
    std::vector<BYTE> buf(256 * 1024);
    bool ok = true;
    USN last = start_usn;
    for (;;) {
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
            blob.insert(blob.end(), p, p + hdr->RecordLength);
            ++nrec;
            p += hdr->RecordLength;
        }
        rud.StartUsn = next;
        last = next;
        if (nrec == 0) break;
    }
    CloseHandle(h);
    if (!ok || !running_) return false;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    BYTE* p = blob.empty() ? nullptr : blob.data();
    BYTE* end = p + blob.size();
    bool any_struct = false;
    bool any_attr = false;
    while (p && p + sizeof(USN_RECORD_COMMON_HEADER) <= end) {
        if (!running_) return false;
        auto* hdr = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(p);
        if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
        if (hdr->MajorVersion == 2) {
            const UsnApply apply = ApplyUsnLocked(v, reinterpret_cast<USN_RECORD_V2*>(p));
            if (apply == UsnApply::Structure) any_struct = true;
            else if (apply == UsnApply::Attr) any_attr = true;
        }
        p += hdr->RecordLength;
    }
    v.next_usn = last;
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
    return true;
}

bool Engine::IndexVolumeMft(const VolumeInfo& volume) {
    if (volume.mount_point.size() < 2) return false;
    const wchar_t letter = towupper(volume.mount_point[0]);
    HANDLE h = OpenVolume(letter);
    if (h == INVALID_HANDLE_VALUE) return false;

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
    bool from_mft = EnumerateMft(h, &running_, progress, [&](MftFile&& f) {
        if (frn_nodes.size() >= kIndexCap) return false;
        FrnNode node;
        node.frn = f.frn;
        node.parent = f.parent;
        node.size = f.size;
        node.mtime = f.mtime;
        node.name = std::move(f.name);
        node.is_dir = f.is_dir;
        node.name_type = f.name_type;
        frn_nodes.push_back(std::move(node));
        return true;
    });

    if (!from_mft) {
        MFT_ENUM_DATA_V0 med{};
        med.StartFileReferenceNumber = 0;
        med.LowUsn = 0;
        med.HighUsn = MAXLONGLONG;
        std::vector<BYTE> buffer(1024 * 1024);
        DWORD br = 0;
        while (running_) {
            if (!DeviceIoControl(h, FSCTL_ENUM_USN_DATA, &med, sizeof(med),
                                 buffer.data(), static_cast<DWORD>(buffer.size()), &br, nullptr)) {
                DWORD err = GetLastError();
                if (err != ERROR_HANDLE_EOF) {
                    CloseHandle(h);
                    return false;
                }
                break;
            }
            if (br <= sizeof(USN)) break;
            med.StartFileReferenceNumber = *reinterpret_cast<USN*>(buffer.data());
            auto* rec = reinterpret_cast<PUSN_RECORD_V2>(buffer.data() + sizeof(USN));
            BYTE* end = buffer.data() + br;
            while (reinterpret_cast<BYTE*>(rec) + sizeof(USN_RECORD_V2) <= end) {
                FrnNode n;
                n.frn = rec->FileReferenceNumber;
                n.parent = rec->ParentFileReferenceNumber;
                n.name.assign(rec->FileName, rec->FileNameLength / sizeof(WCHAR));
                n.is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                frn_nodes.push_back(std::move(n));
                if (rec->RecordLength == 0) break;
                rec = reinterpret_cast<PUSN_RECORD_V2>(reinterpret_cast<BYTE*>(rec) + rec->RecordLength);
            }
            if (frn_nodes.size() >= kIndexCap) break;
            SetStatus(StatusDriveProgress(letter, frn_nodes.size()));
            PingNotify();
        }
    }
    CloseHandle(h);
    if (!running_ || frn_nodes.empty()) return !frn_nodes.empty();

    return BuildMftTree(std::move(vol), RootFrn(letter), std::move(frn_nodes));
}

bool Engine::BuildMftTree(VolState vol, uint64_t root_frn, std::vector<FrnNode> frn_nodes) {
    const wchar_t letter = vol.letter;
    auto rank = [](uint8_t t) { return (t == 1 || t == 3) ? 0 : (t == 0 ? 1 : 2); };
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
        int32_t chain[64];
        int depth = 0;
        for (int32_t current = id; current >= 0 && depth < 64;
             current = build_.nodes[static_cast<size_t>(current)].parent)
            chain[depth++] = current;
        std::wstring path;
        for (int k = depth - 1; k >= 0; --k) {
            const std::wstring_view part = build_name(chain[k]);
            if (!path.empty() && path.back() != L'\\') path += L'\\';
            path.append(part.data(), part.size());
            if (k == depth - 1 && part.size() == 2 && part[1] == L':') path += L'\\';
        }
        return path;
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
        bool hidden_parent = parent_idx >= 0 && parent_idx != vol.root_idx &&
            (build_.nodes[static_cast<size_t>(parent_idx)].flags & kFlagHidden) != 0;
        for (auto rit = stack.rbegin(); rit != stack.rend(); ++rit) {
            FrnNode& n = **rit;
            if (IsIndexArtifactName(n.name)) continue;
            uint8_t flags = n.is_dir ? kFlagDir : 0;
            if (!candidate.empty() && candidate.back() != L'\\') candidate += L'\\';
            candidate += n.name;
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

void Engine::FullRebuild() {
    const auto configured_volumes = ConfiguredVolumes();
    IndexConfig config;
    if (MachineIndexScope()) LoadMachineConfig(config, nullptr);
    excluded_paths_ = config.excluded_paths;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_.pool.reserve(1u << 20);
        build_.nodes.reserve(1u << 16);
        building_ = true;
        build_vols_.clear();
        if (MachineIndexScope()) PreserveOfflineVolumesLocked(configured_volumes, config);
    }
    if (indexed_.load() == 0) SetStatus(L"正在建立索引…");
    else SetStatus(L"已索引 " + std::to_wstring(indexed_.load()) + L" 项，正在重建…");
    PingNotify(true);

    bool used_mft = false;
    if (IsAdmin()) {
        for (const auto& volume : configured_volumes) {
            if (!running_) break;
            if (IndexVolumeMft(volume)) used_mft = true;
        }
    }
    if (!used_mft && running_ && !MachineIndexScope()) {
        walk_roots_.clear();
        auto walk_root = [this](const std::wstring& path) {
            walk_roots_.push_back(path);
            int32_t parent = EnsureChainLocked(build_, path, true, false);
            if (parent >= 0) WalkTree(parent, path, 0);
        };
        wchar_t profile[MAX_PATH] = {};
        if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PROFILE, nullptr, 0, profile)))
            walk_root(Display(fs::NormalizePath(profile)));
        // R2: non-admin default is the user profile only (RDCW can actually watch it).
    }

    if (running_) {
        const uint64_t built = static_cast<uint64_t>(std::time(nullptr));
        const std::wstring path = CachePath();
        const bool wrote = !path.empty() && WriteIndexFile(path, build_, build_vols_, built);
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
                if (map_) { map_->Close(); map_.reset(); }
                live_ = std::move(build_);
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
        }
        if (used_mft) StopWalkWatches();
        else StartWalkWatches(walk_roots_);
        PingNotify(true);
        if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 1, 0);
    } else {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_.Shrink();
        build_vols_.clear();
        building_ = false;
    }
}

bool Engine::NeedsSearchRebuildLocked() const {
    return map_ && (map_->hdr->ver < kIndexVer || !map_->prefix1_all_chars);
}

void Engine::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    ResolveIndexDirFrn();
    IndexConfig startup_config;
    if (MachineIndexScope()) LoadMachineConfig(startup_config, nullptr);
    excluded_paths_ = startup_config.excluded_paths;
    const bool have_cache = TryLoadCache();
    const auto initial_drives = ConfiguredVolumes();
    bool needs_search_rebuild = false;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        UpdateVolumeVisibilityLocked(initial_drives);
        needs_search_rebuild = NeedsSearchRebuildLocked();
    }
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
            IndexConfig configured;
            if (MachineIndexScope()) LoadMachineConfig(configured, nullptr);
            const auto mounted = EnumerateLocalVolumes(configured);
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
    } else if (have_cache && !needs_search_rebuild && !IsAdmin()) {
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        if (built_unix_ && now >= built_unix_ && now - built_unix_ < kCacheFreshSecs)
            fresh = true;
    }
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
        FullRebuild();
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
    bool struct_dirty = false;
    while (running_) {
        SeedPendingChanges();
        for (int i = 0; i < 10 && running_; ++i) {
            Sleep(100);
            PollWalkWatches();
        }
        changes_.Flush();
        if (!running_) break;
        if (rebuild_requested_.exchange(false)) {
            FullRebuild();
            last_merge_tick_ = GetTickCount64();
            struct_dirty = false;
            continue;
        }
        bool changed = false;
        bool structural = false;
        bool failed = false;
        bool need_compact = false;
        std::vector<VolumeInfo> failed_volumes;
        const auto online = ConfiguredVolumes();
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
                    if (!CatchUpVolume(vols_[i], &changed, &structural)) {
                        changes_.Gap();
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
            const size_t n = static_cast<size_t>(LiveCount());
            // live_.pool only contains names introduced by the delta layer;
            // comparing waste against it made any single rename look like a
            // full-store fragmentation event. Compare against the mapped
            // base pool and require a meaningful absolute amount of waste.
            const uint64_t base_pool = map_ && map_->hdr ? map_->hdr->pool_chars : 0;
            if (n > 0 && (deleted_ * 10 > n ||
                          pool_waste_ >= (1ull << 20) ||
                          (base_pool && pool_waste_ * 10 > base_pool)))
                need_compact = true;
        }
        if (failed && running_) {
            bool recovered = !failed_volumes.empty();
            for (const auto& volume : failed_volumes) {
                if (!running_ || !RebuildVolumeMft(volume)) {
                    recovered = false;
                    break;
                }
            }
            if (!recovered) {
                SetStatus(L"变更跟踪失效，正在恢复索引…");
                FullRebuild();
            }
            last_merge_tick_ = GetTickCount64();
            struct_dirty = false;
            continue;
        }
        if (structural) struct_dirty = true;
        if (changed && notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);

        const ULONGLONG now = GetTickCount64();
        if (now - last_delta_flush_tick_ >= kDeltaFlushMs)
            FlushDeltas();

        uint64_t delta_bytes = 0;
        size_t struct_n = 0;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            struct_n = struct_changes_;
            for (const auto& [k, log] : delta_logs_)
                if (log) delta_bytes += log->BytesOnDisk() + (log->HasPending() ? 1 : 0);
        }
        // Merge triggers must measure unmerged work only. (A live-node count
        // here forced a full rewrite every loop on any machine over the old
        // 500k threshold, e.g. the 2.6M-item dev box.)
        const bool over_delta = struct_n >= kMergeStructChanges ||
            delta_bytes >= kMergeDeltaBytes;
        const bool idle_merge = struct_dirty && last_struct_tick_ &&
            now - last_struct_tick_ >= kIdleMergeQuietMs &&
            now - last_merge_tick_ >= kMinMergeIntervalMs;
        if (need_compact || over_delta || idle_merge) {
            MergeBase(over_delta || need_compact);
        }
    }
    FlushDeltas();
    StopWalkWatches();
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_END);
}

void Engine::StopWalkWatches() {
    for (auto& w : watches_) {
        if (w.dir != INVALID_HANDLE_VALUE) {
            CancelIoEx(w.dir, &w.ov);
            CloseHandle(w.dir);
            w.dir = INVALID_HANDLE_VALUE;
        }
        if (w.event) { CloseHandle(w.event); w.event = nullptr; }
    }
    watches_.clear();
}

void Engine::StartWalkWatches(const std::vector<std::wstring>& roots) {
    StopWalkWatches();
    for (const auto& path : roots) {
        if (path.empty()) continue;
        WalkWatch w;
        w.path = path;
        w.buf.resize(64 * 1024);
        w.event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        w.dir = CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            nullptr, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
        if (w.dir == INVALID_HANDLE_VALUE) {
            if (w.event) CloseHandle(w.event);
            continue;
        }
        w.ov.hEvent = w.event;
        const BOOL subtree = path.size() > 3;
        ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size()), subtree,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &w.ov, nullptr);
        watches_.push_back(std::move(w));
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
        if (n == 0) { changes_.Gap(); walk_pending_renames_.erase(w.path); }
        if (n > 0) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            ApplyNotifyLocked(w.path, w.buf.data(), n);
        }
        ResetEvent(w.event);
        ZeroMemory(&w.ov, sizeof(w.ov));
        w.ov.hEvent = w.event;
        const BOOL subtree = w.path.size() > 3;
        ReadDirectoryChangesW(w.dir, w.buf.data(), static_cast<DWORD>(w.buf.size()), subtree,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
            nullptr, &w.ov, nullptr);
    }
}

void Engine::ApplyNotifyLocked(const std::wstring& root, const BYTE* buf, DWORD len) {
    std::wstring& pending_old = walk_pending_renames_[root];
    const BYTE* p = buf;
    const BYTE* end = buf + len;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        const auto name_offset = offsetof(FILE_NOTIFY_INFORMATION, FileName);
        if (info->FileNameLength % sizeof(WCHAR) || info->FileNameLength > static_cast<size_t>(end - p) - name_offset) {
            changes_.Gap(); break;
        }
        std::wstring rel(info->FileName, info->FileNameLength / sizeof(WCHAR));
        std::wstring full = root;
        if (!full.empty() && full.back() != L'\\' && !rel.empty()) full += L'\\';
        full += rel;
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
            else changes_.Gap(); }
        if (info->Action != FILE_ACTION_RENAMED_OLD_NAME) changes_.Record(std::move(event));
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
        if (info->NextEntryOffset == 0) break;
        if (info->NextEntryOffset < sizeof(FILE_NOTIFY_INFORMATION) || info->NextEntryOffset > static_cast<size_t>(end - p)) { changes_.Gap(); break; }
        p += info->NextEntryOffset;
    }
}

} // namespace pulse::index
