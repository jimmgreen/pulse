// index_engine.cpp — mmap v6 base + heap delta (优化.md).
#include "index_engine.h"
#include "index_query.h"
#include "index_mft.h"
#include "index_paths.h"
#include "../fs/fs_enum.h"
#include "../common/path_utils.h"
#include <shlobj.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <string>
#include <thread>
#include <unordered_map>

namespace pulse::index {

namespace {

constexpr size_t kIndexCap = 4000000;
constexpr size_t kFrnMergeThreshold = 4096;
constexpr ULONGLONG kCacheSaveIntervalMs = 5 * 60 * 1000;
constexpr uint64_t kCacheFreshSecs = 24ull * 60 * 60;
constexpr uint32_t kIndexVer = 6;
constexpr uint64_t kUnixFtEpoch = 116444736000000000ull;
constexpr ULONGLONG kNotifyMinMs = 500;

uint32_t FtToUnix(uint64_t ft) {
    if (ft < kUnixFtEpoch) return 0;
    const uint64_t s = (ft - kUnixFtEpoch) / 10000000ull;
    return s > 0xffffffffull ? 0xffffffffu : static_cast<uint32_t>(s);
}

uint64_t UnixToFt(uint32_t u) {
    if (u == 0) return 0;
    return static_cast<uint64_t>(u) * 10000000ull + kUnixFtEpoch;
}

std::wstring Display(std::wstring p) {
    return pulse::path::StripExtendedPathPrefix(p);
}

bool ShouldSkipName(std::wstring_view name) {
    return name == L"." || name == L".." ||
           name == L"$Recycle.Bin" || name == L"System Volume Information" ||
           name == L"WinSxS" || name == L"servicing" ||
           (name.size() == 12 && _wcsnicmp(name.data(), L"node_modules", 12) == 0);
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
    for (wchar_t c : name) {
        h ^= FoldChar(c);
        h *= 16777619u;
    }
    return h;
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

std::vector<wchar_t> FixedDriveLetters() {
    std::vector<wchar_t> out;
    DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1u << i))) continue;
        wchar_t root[] = { static_cast<wchar_t>(L'A' + i), L':', L'\\', 0 };
        if (GetDriveTypeW(root) == DRIVE_FIXED) out.push_back(static_cast<wchar_t>(L'A' + i));
    }
    return out;
}

std::wstring CachePath() { return CacheFilePath(); }

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
    n = nvol = nfrn = 0;
    size = 0;
}

void Engine::SetStatus(std::wstring s) {
    std::lock_guard<std::mutex> lock(status_mu_);
    status_ = std::move(s);
}

std::wstring Engine::Status() const {
    std::lock_guard<std::mutex> lock(status_mu_);
    return status_;
}

void Engine::PingNotify(bool force) {
    if (!notify_ || !notify_msg_) return;
    const ULONGLONG t = GetTickCount64();
    const ULONGLONG prev = last_notify_.load();
    if (!force && t - prev < kNotifyMinMs) return;
    last_notify_.store(t);
    PostMessageW(notify_, notify_msg_, 0, 0);
}

void Engine::Start(HWND notify, UINT msg) {
    Stop();
    notify_ = notify;
    notify_msg_ = msg;
    running_ = true;
    ready_ = false;
    thread_ = std::thread(&Engine::Worker, this);
}

void Engine::Stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    StopWalkWatches();
    std::unique_lock<std::shared_mutex> lock(mutex_);
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
    const int32_t base = BaseCount();
    if (i < base) n = map_->nodes[static_cast<size_t>(i)];
    else n = live_.nodes[static_cast<size_t>(i - base)];
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
    auto it = patches_.find(i);
    if (it != patches_.end() && it->second.has_attr) return it->second.attr;
    const int32_t base = BaseCount();
    if (i < base) return map_->attrs[static_cast<size_t>(i)];
    return live_.attrs[static_cast<size_t>(i - base)];
}

std::wstring_view Engine::NameOf(int32_t i) const {
    auto it = patches_.find(i);
    if (it != patches_.end() && it->second.has_name)
        return { live_.pool.data() + it->second.off, it->second.len };
    const int32_t base = BaseCount();
    if (i < base) {
        const Node& n = map_->nodes[static_cast<size_t>(i)];
        return { map_->pool + n.off, n.len };
    }
    const Node& n = live_.nodes[static_cast<size_t>(i - base)];
    return { live_.pool.data() + n.off, n.len };
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
    const int32_t n = LiveCount();
    child_map_.reserve(static_cast<size_t>(n));
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
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
    for (int32_t i = node; i >= 0;) {
        if (i == ancestor) return true;
        i = NodeAt(i).parent;
    }
    return false;
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

void Engine::CollectMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                                  bool folders_only, bool use_attrs,
                                  std::vector<int32_t>& ids) const {
    const int32_t n = LiveCount();
    unsigned hw = std::thread::hardware_concurrency();
    unsigned T = hw < 2 ? 1u : (std::min)(hw, 8u);
    if (n < 250000) T = 1;
    if (T == 1) {
        ids.reserve(256);
        for (int32_t i = 0; i < n; ++i) {
            if (MatchNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
                ids.push_back(i);
        }
        return;
    }
    std::vector<std::vector<int32_t>> parts(T);
    std::vector<std::thread> threads;
    threads.reserve(T);
    for (unsigned t = 0; t < T; ++t) {
        threads.emplace_back([&, t] {
            const int32_t a = static_cast<int32_t>(static_cast<uint64_t>(n) * t / T);
            const int32_t b = static_cast<int32_t>(static_cast<uint64_t>(n) * (t + 1) / T);
            auto& part = parts[t];
            part.reserve(64);
            for (int32_t i = a; i < b; ++i) {
                if (MatchNodeLocked(i, cq, prefix_node, folders_only, use_attrs))
                    part.push_back(i);
            }
        });
    }
    for (auto& th : threads) th.join();
    size_t total = 0;
    for (const auto& p : parts) total += p.size();
    ids.reserve(total);
    for (auto& p : parts) ids.insert(ids.end(), p.begin(), p.end());
}

void Engine::SortIdsLocked(std::vector<int32_t>& ids, ResultSort sort, bool desc) const {
    if (ids.size() <= 1 || sort == ResultSort::Index) {
        if (desc && sort == ResultSort::Index) std::reverse(ids.begin(), ids.end());
        return;
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

SearchResult Engine::Search(const Query& q) const {
    SearchResult out;
    CompiledQuery cq = ParseQuery(q.needle);
    std::shared_lock<std::shared_mutex> lock(mutex_);
    int32_t prefix_node = -1;
    if (!q.path_prefix.empty()) {
        prefix_node = ResolvePathLocked(q.path_prefix);
        if (prefix_node < 0) return out;
    }
    bool use_attrs = false;
    if (QueryUsesAttrs(cq)) {
        use_attrs = true;
    }
    const size_t cap = q.limit;

    std::vector<int32_t> ids;
    const bool same_scope = cache_path_prefix_ == q.path_prefix &&
        cache_folders_only_ == q.folders_only;
    const bool exact_cached_page = cache_epoch_ == filter_epoch_ && same_scope &&
        cache_raw_ == q.needle && !q.rank && !cache_ranked_ &&
        cache_sort_ == q.sort && cache_sort_desc_ == q.sort_desc;
    if (exact_cached_page) {
        ids = cache_ids_;
    } else if (cache_epoch_ == filter_epoch_ && same_scope &&
               QueryCanNarrow(cache_raw_, q.needle)) {
        ids.reserve(cache_ids_.size());
        for (int32_t i : cache_ids_) {
            if (i < 0 || i >= LiveCount()) continue;
            if (MatchNodeLocked(i, cq, prefix_node, q.folders_only, use_attrs))
                ids.push_back(i);
        }
    } else {
        CollectMatchesLocked(cq, prefix_node, q.folders_only, use_attrs, ids);
    }
    out.total = ids.size();
    if (cap == 0 || ids.empty()) return out;

    if (!exact_cached_page && !q.rank && q.sort != ResultSort::Index)
        SortIdsLocked(ids, q.sort, q.sort_desc);

    if (!q.rank) {
        cache_raw_ = q.needle;
        cache_path_prefix_ = q.path_prefix;
        cache_folders_only_ = q.folders_only;
        cache_ranked_ = false;
        cache_sort_ = q.sort;
        cache_sort_desc_ = q.sort_desc;
        cache_ids_ = ids;
        cache_epoch_ = filter_epoch_;
    } else {
        cache_raw_ = q.needle;
        cache_path_prefix_ = q.path_prefix;
        cache_folders_only_ = q.folders_only;
        cache_ranked_ = true;
        cache_sort_ = ResultSort::Index;
        cache_sort_desc_ = false;
        cache_ids_ = ids;
        cache_epoch_ = filter_epoch_;
    }

    const size_t start = (std::min)(q.offset, ids.size());
    const size_t end = (std::min)(ids.size(), start + cap);
    std::vector<int32_t> chosen;
    chosen.reserve(end - start);
    if (q.rank) {
        std::vector<int> score(ids.size());
        for (size_t k = 0; k < ids.size(); ++k) {
            const Node n = NodeAt(ids[k]);
            std::wstring_view nm = NameOf(ids[k]);
            score[k] = RankName(nm.data(), static_cast<uint32_t>(nm.size()), (n.flags & kFlagDir) != 0, cq);
        }
        std::vector<size_t> order(ids.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::partial_sort(order.begin(), order.begin() + static_cast<std::ptrdiff_t>(end), order.end(),
                          [&](size_t a, size_t b) {
                              if (score[a] != score[b]) return score[a] > score[b];
                              return ids[a] < ids[b];
                          });
        for (size_t k = start; k < end; ++k) chosen.push_back(ids[order[k]]);
    } else {
        for (size_t k = start; k < end; ++k) chosen.push_back(ids[k]);
    }

    out.hits.reserve(chosen.size());
    for (size_t k = 0; k < chosen.size(); ++k) {
        const Node n = NodeAt(chosen[k]);
        const Attr a = AttrAt(chosen[k]);
        Hit h;
        h.path = BuildPathLocked(chosen[k]);
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
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

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
    hdr.vols_off = off;
    off = AlignUp(off + sizeof(DiskVol) * vols.size(), 16);
    hdr.frn_off = off;
    uint64_t frn_count = 0;
    for (const auto& v : vols) frn_count += v.frn_build.size();
    hdr.frn_count = static_cast<uint32_t>(frn_count);

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
    if (ok) {
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(hdr.vols_off);
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
    }
    std::vector<DiskVol> dvols(vols.size());
    uint64_t frn_cur = hdr.frn_off;
    for (size_t i = 0; i < vols.size(); ++i) {
        dvols[i].letter = static_cast<uint16_t>(vols[i].letter);
        dvols[i].journal_id = vols[i].journal_id;
        dvols[i].next_usn = vols[i].next_usn;
        dvols[i].root_idx = vols[i].root_idx;
        dvols[i].frn_count = static_cast<uint32_t>(vols[i].frn_build.size());
        dvols[i].frn_off = frn_cur;
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
    if (map_) {
        map_->Close();
        map_.reset();
    }
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    std::unique_ptr<MappedFile> mapped;
    if (!MapIndexFile(path, mapped)) return false;
    AdoptMappedLocked(std::move(mapped));
    return true;
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
    if (memcmp(m->hdr->magic, "PIDX", 4) != 0 || m->hdr->ver != kIndexVer) return false;
    if (m->hdr->node_count > kIndexCap + 64) return false;
    auto in_range = [&](uint64_t o, uint64_t n) {
        return o <= m->size && n <= m->size && o + n <= m->size;
    };
    if (!in_range(m->hdr->nodes_off, sizeof(Node) * m->hdr->node_count)) return false;
    if (!in_range(m->hdr->attrs_off, sizeof(Attr) * m->hdr->node_count)) return false;
    if (!in_range(m->hdr->pool_off, sizeof(wchar_t) * m->hdr->pool_chars)) return false;
    if (!in_range(m->hdr->vols_off, sizeof(DiskVol) * m->hdr->vol_count)) return false;
    if (!in_range(m->hdr->frn_off, sizeof(DiskFrn) * m->hdr->frn_count)) return false;
    m->nodes = reinterpret_cast<const Node*>(m->view + m->hdr->nodes_off);
    m->attrs = reinterpret_cast<const Attr*>(m->view + m->hdr->attrs_off);
    m->pool = reinterpret_cast<const wchar_t*>(m->view + m->hdr->pool_off);
    m->n = m->hdr->node_count;
    m->vols = m->hdr->vol_count
        ? reinterpret_cast<const DiskVol*>(m->view + m->hdr->vols_off) : nullptr;
    m->nvol = m->hdr->vol_count;
    m->frns = m->hdr->frn_count
        ? reinterpret_cast<const DiskFrn*>(m->view + m->hdr->frn_off) : nullptr;
    m->nfrn = static_cast<uint32_t>(m->hdr->frn_count);
    out = std::move(m);
    return true;
}

void Engine::AdoptMappedLocked(std::unique_ptr<MappedFile> mapped) {
    map_ = std::move(mapped);
    live_.Clear();
    live_.Shrink();
    tombstones_.clear();
    patches_.clear();
    vols_.clear();
    deleted_ = 0;
    pool_waste_ = 0;
    built_unix_ = map_ && map_->hdr ? map_->hdr->built_unix : 0;
    if (map_) {
        vols_.resize(map_->nvol);
        for (uint32_t i = 0; i < map_->nvol; ++i) {
            const DiskVol& d = map_->vols[i];
            VolState& v = vols_[i];
            v.letter = static_cast<wchar_t>(d.letter);
            v.journal_id = d.journal_id;
            v.next_usn = d.next_usn;
            v.root_idx = d.root_idx;
            if (d.frn_count && d.frn_off + d.frn_count * sizeof(DiskFrn) <= map_->size) {
                v.frn_base = reinterpret_cast<const DiskFrn*>(map_->view + d.frn_off);
                v.frn_base_n = d.frn_count;
            }
        }
    }
    RebuildChildMapLocked();
    indexed_.store(static_cast<size_t>(LiveCount()));
    InvalidateFilterLocked();
}

bool Engine::FlattenLocked(Store& out, std::vector<VolState>& vols_out) const {
    const int32_t n = LiveCount();
    std::vector<int32_t> remap(static_cast<size_t>(n), -1);
    out.Clear();
    out.nodes.reserve(static_cast<size_t>(n));
    out.attrs.reserve(static_cast<size_t>(n));
    out.pool.reserve(static_cast<size_t>(n) * 12);
    for (int32_t i = 0; i < n; ++i) {
        if (IsTomb(i)) continue;
        const Node node = NodeAt(i);
        if (node.parent >= 0 && (node.parent >= n || remap[static_cast<size_t>(node.parent)] < 0))
            continue;
        const std::wstring_view name = NameOf(i);
        Node nn = node;
        nn.parent = node.parent < 0 ? -1 : remap[static_cast<size_t>(node.parent)];
        nn.off = static_cast<uint32_t>(out.pool.size());
        nn.len = static_cast<uint16_t>(name.size());
        nn.pad = 0;
        nn.unused = 0;
        out.pool.insert(out.pool.end(), name.begin(), name.end());
        remap[static_cast<size_t>(i)] = static_cast<int32_t>(out.nodes.size());
        out.nodes.push_back(nn);
        out.attrs.push_back(AttrAt(i));
    }
    vols_out = vols_;
    for (auto& v : vols_out) {
        if (v.root_idx >= 0 && v.root_idx < n)
            v.root_idx = remap[static_cast<size_t>(v.root_idx)];
        v.frn_build.clear();
        auto push = [&](uint64_t frn, int32_t idx) {
            if (idx >= 0 && idx < n && remap[static_cast<size_t>(idx)] >= 0)
                v.frn_build.emplace_back(frn, remap[static_cast<size_t>(idx)]);
        };
        for (uint32_t k = 0; k < v.frn_base_n; ++k)
            push(v.frn_base[k].frn, v.frn_base[k].idx);
        for (const auto& e : v.frn_new) push(e.frn, e.idx);
        v.frn_base = nullptr;
        v.frn_base_n = 0;
        v.frn_new.clear();
    }
    return true;
}

void Engine::SaveCache() {
    const std::wstring path = CachePath();
    if (path.empty()) return;
    Store snap;
    std::vector<VolState> vols;
    uint64_t built = static_cast<uint64_t>(std::time(nullptr));
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (LiveCount() == 0) return;
        FlattenLocked(snap, vols);
        built_unix_ = built;
    }
    if (!WriteIndexFile(path, snap, vols, built)) return;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    CommitMappedFile(path);
}

bool Engine::TryLoadCache() {
    const std::wstring path = CachePath();
    if (path.empty()) return false;
    std::unique_ptr<MappedFile> mapped;
    if (!MapIndexFile(path, mapped)) {
        DeleteFileW(path.c_str());
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    AdoptMappedLocked(std::move(mapped));
    SetStatus(L"缓存 " + std::to_wstring(indexed_.load()) + L" 项");
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

void Engine::ApplyUsnLocked(VolState& v, const USN_RECORD_V2* rec) {
    std::wstring_view name(
        reinterpret_cast<const wchar_t*>(reinterpret_cast<const BYTE*>(rec) + rec->FileNameOffset),
        rec->FileNameLength / sizeof(WCHAR));
    if (name.empty()) return;
    const uint64_t frn = rec->FileReferenceNumber;
    const bool is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    int32_t idx = FindByFrnLocked(v, frn);

    auto refresh = [&](int32_t i) {
        std::wstring path = BuildPathLocked(i);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad)) return;
        Attr a;
        a.size = (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
        a.mtime = FtToUnix((static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
                           fad.ftLastWriteTime.dwLowDateTime);
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
    };

    if (rec->Reason & USN_REASON_FILE_DELETE) {
        if (idx >= 0 && !IsTomb(idx)) {
            ChildMapRemove(NodeAt(idx).parent, NameOf(idx), idx);
            tombstones_.insert(idx);
            ++deleted_;
            InvalidateFilterLocked();
        }
        return;
    }
    if (idx >= 0 && (rec->Reason & (USN_REASON_DATA_EXTEND | USN_REASON_DATA_TRUNCATION |
                                    USN_REASON_DATA_OVERWRITE | USN_REASON_BASIC_INFO_CHANGE))) {
        refresh(idx);
    }
    if (!(rec->Reason & (USN_REASON_FILE_CREATE | USN_REASON_RENAME_NEW_NAME))) return;

    int32_t parent = FindByFrnLocked(v, rec->ParentFileReferenceNumber);
    if (parent < 0) parent = v.root_idx;

    uint8_t flags = is_dir ? kFlagDir : 0;
    if (ShouldSkipName(name)) flags |= kFlagHidden;
    for (int32_t a = parent; a > v.root_idx && a >= 0;) {
        const Node an = NodeAt(a);
        if (an.flags & kFlagHidden) { flags |= kFlagHidden; break; }
        a = an.parent;
    }

    if (idx >= 0) {
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
        refresh(idx);
        InvalidateFilterLocked();
    } else {
        if (static_cast<size_t>(LiveCount()) >= kIndexCap + 64) return;
        idx = AddNodeLocked(live_, parent, name, flags, frn, 0, 0, true, &v);
        refresh(idx);
        InvalidateFilterLocked();
    }
}

bool Engine::CatchUpVolume(VolState& v, bool* changed) {
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
        DWORD br = 0;
        if (!DeviceIoControl(h, FSCTL_READ_USN_JOURNAL, &rud, sizeof(rud),
                             buf.data(), static_cast<DWORD>(buf.size()), &br, nullptr)) {
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
    if (!ok) return false;

    std::unique_lock<std::shared_mutex> lock(mutex_);
    BYTE* p = blob.empty() ? nullptr : blob.data();
    BYTE* end = p + blob.size();
    while (p && p + sizeof(USN_RECORD_COMMON_HEADER) <= end) {
        auto* hdr = reinterpret_cast<USN_RECORD_COMMON_HEADER*>(p);
        if (hdr->RecordLength == 0 || p + hdr->RecordLength > end) break;
        if (hdr->MajorVersion == 2) {
            ApplyUsnLocked(v, reinterpret_cast<USN_RECORD_V2*>(p));
            if (changed) *changed = true;
        }
        p += hdr->RecordLength;
    }
    v.next_usn = last;
    indexed_.store(static_cast<size_t>(LiveCount()) > deleted_ ? LiveCount() - deleted_ : 0);
    return true;
}

bool Engine::IndexVolumeMft(wchar_t letter) {
    HANDLE h = OpenVolume(letter);
    if (h == INVALID_HANDLE_VALUE) return false;

    VolState vol;
    vol.letter = letter;
    if (!QueryJournal(h, vol.journal_id, vol.next_usn)) {
        vol.journal_id = 0;
        vol.next_usn = 0;
    }

    struct FrnNode {
        uint64_t parent = 0;
        uint64_t size = 0;
        uint64_t mtime = 0;
        std::wstring name;
        bool is_dir = false;
        uint8_t name_type = 0xFF;
    };
    std::unordered_map<uint64_t, FrnNode> frn_nodes;
    frn_nodes.reserve(256000);

    auto progress = [&](size_t n) {
        SetStatus(std::wstring(L"MFT ") + letter + L": " + std::to_wstring(n));
        PingNotify();
    };
    auto rank = [](uint8_t t) { return (t == 1 || t == 3) ? 0 : (t == 0 ? 1 : 2); };
    bool from_mft = EnumerateMft(h, &running_, progress, [&](MftFile&& f) {
        if (frn_nodes.size() >= kIndexCap) return false;
        auto& slot = frn_nodes[f.frn];
        if (slot.name.empty() || rank(f.name_type) < rank(slot.name_type)) {
            slot.parent = f.parent;
            slot.size = f.size;
            slot.mtime = f.mtime;
            slot.name = std::move(f.name);
            slot.is_dir = f.is_dir;
            slot.name_type = f.name_type;
        } else {
            if (f.size) slot.size = f.size;
            if (f.mtime) slot.mtime = f.mtime;
            if (f.is_dir) slot.is_dir = true;
        }
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
                n.parent = rec->ParentFileReferenceNumber;
                n.name.assign(rec->FileName, rec->FileNameLength / sizeof(WCHAR));
                n.is_dir = (rec->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                frn_nodes[rec->FileReferenceNumber] = std::move(n);
                if (rec->RecordLength == 0) break;
                rec = reinterpret_cast<PUSN_RECORD_V2>(reinterpret_cast<BYTE*>(rec) + rec->RecordLength);
            }
            if (frn_nodes.size() >= kIndexCap) break;
            SetStatus(std::wstring(L"MFT ") + letter + L": " + std::to_wstring(frn_nodes.size()));
            PingNotify();
        }
    }
    CloseHandle(h);
    if (!running_ || frn_nodes.empty()) return !frn_nodes.empty();

    const wchar_t root_name[3] = { letter, L':', 0 };
    const uint64_t root_frn = RootFrn(letter);
    std::unordered_map<uint64_t, int32_t> frn_to_idx;
    frn_to_idx.reserve(frn_nodes.size() + 1);
    vol.root_idx = AddNodeLocked(build_, -1, root_name, kFlagDir | kFlagHidden, root_frn);
    if (root_frn) {
        frn_to_idx[root_frn] = vol.root_idx;
        vol.frn_build.emplace_back(root_frn, vol.root_idx);
    }

    std::vector<uint64_t> stack;
    size_t added = 0;
    for (const auto& [frn, node] : frn_nodes) {
        if (!running_ || added >= kIndexCap) break;
        if (frn_to_idx.contains(frn)) continue;
        stack.clear();
        uint64_t cur = frn;
        int32_t parent_idx = vol.root_idx;
        for (int hop = 0; hop < 48; ++hop) {
            auto done = frn_to_idx.find(cur);
            if (done != frn_to_idx.end()) { parent_idx = done->second; break; }
            auto it = frn_nodes.find(cur);
            if (it == frn_nodes.end() || it->second.name.empty()) break;
            stack.push_back(cur);
            if (it->second.parent == cur) break;
            cur = it->second.parent;
        }
        for (auto rit = stack.rbegin(); rit != stack.rend(); ++rit) {
            const FrnNode& n = frn_nodes[*rit];
            uint8_t flags = n.is_dir ? kFlagDir : 0;
            if (ShouldSkipName(n.name)) flags |= kFlagHidden;
            parent_idx = AddNodeLocked(build_, parent_idx, n.name, flags, *rit, n.size, n.mtime);
            frn_to_idx[*rit] = parent_idx;
            vol.frn_build.emplace_back(*rit, parent_idx);
            ++added;
        }
        indexed_.store(build_.nodes.size());
        if ((added & 0x3FFF) == 0) PingNotify();
    }
    if (added == 0) return false;
    build_vols_.push_back(std::move(vol));
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
        if (ShouldSkipName(e.name)) continue;
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
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        build_.Clear();
        build_.pool.reserve(1u << 20);
        build_.nodes.reserve(1u << 16);
        building_ = true;
    }
    if (indexed_.load() == 0) SetStatus(L"正在建索引…");
    else SetStatus(L"索引 " + std::to_wstring(indexed_.load()) + L" 项，正在重建…");
    build_vols_.clear();
    PingNotify(true);

    bool used_mft = false;
    if (IsAdmin()) {
        for (wchar_t letter : FixedDriveLetters()) {
            if (!running_) break;
            if (IndexVolumeMft(letter)) used_mft = true;
        }
    }
    if (!used_mft && running_) {
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
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (wrote && CommitMappedFile(path)) {
            build_.Clear();
        } else {
            if (map_) { map_->Close(); map_.reset(); }
            live_ = std::move(build_);
            vols_ = std::move(build_vols_);
            tombstones_.clear();
            patches_.clear();
            deleted_ = 0;
            RebuildChildMapLocked();
            indexed_.store(live_.nodes.size());
            built_unix_ = built;
            InvalidateFilterLocked();
        }
        build_.Clear();
        build_.Shrink();
        build_vols_.clear();
        const bool live_tracked = !vols_.empty() &&
            std::all_of(vols_.begin(), vols_.end(), [](const VolState& v) { return v.journal_id != 0; });
        SetStatus((used_mft ? L"MFT 索引 " : L"已索引 ") + std::to_wstring(indexed_.load()) +
                  (live_tracked ? L" 项（USN 实时）" : L" 项（监听）"));
        ready_ = true;
        building_ = false;
        lock.unlock();
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

void Engine::Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    const bool have_cache = TryLoadCache();
    PingNotify(true);

    bool fresh = false;
    if (have_cache && IsAdmin()) {
        bool have_vols = false;
        {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            have_vols = !vols_.empty();
        }
        if (have_vols) {
            auto drives = FixedDriveLetters();
            fresh = true;
            {
                std::shared_lock<std::shared_mutex> lock(mutex_);
                if (vols_.size() != drives.size()) fresh = false;
                for (const auto& v : vols_)
                    if (std::find(drives.begin(), drives.end(), v.letter) == drives.end())
                        fresh = false;
            }
            if (fresh) {
                std::unique_lock<std::shared_mutex> lock(mutex_);
                for (auto& v : vols_) {
                    lock.unlock();
                    if (!CatchUpVolume(v, nullptr)) { fresh = false; break; }
                    lock.lock();
                }
            }
            if (fresh)
                SetStatus(L"MFT 索引 " + std::to_wstring(indexed_.load()) + L" 项（USN 实时）");
        }
    } else if (have_cache && !IsAdmin()) {
        const uint64_t now = static_cast<uint64_t>(std::time(nullptr));
        if (built_unix_ && now >= built_unix_ && now - built_unix_ < kCacheFreshSecs)
            fresh = true;
    }
    if (fresh) {
        if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 1, 0);
    } else if (running_) {
        FullRebuild();
    }
    bool walk_mode = false;
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        walk_mode = vols_.empty();
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

    ULONGLONG last_save = GetTickCount64();
    bool dirty = false;
    while (running_) {
        for (int i = 0; i < 10 && running_; ++i) {
            Sleep(100);
            PollWalkWatches();
        }
        if (!running_) break;
        bool changed = false;
        bool failed = false;
        bool need_compact = false;
        {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            if (!vols_.empty()) {
                std::vector<size_t> idx(vols_.size());
                for (size_t i = 0; i < idx.size(); ++i) idx[i] = i;
                lock.unlock();
                for (size_t i : idx) {
                    if (!CatchUpVolume(vols_[i], &changed)) failed = true;
                }
                lock.lock();
                if (changed) {
                    const bool live_tracked = std::all_of(vols_.begin(), vols_.end(),
                        [](const VolState& v) { return v.journal_id != 0; });
                    SetStatus(L"MFT 索引 " + std::to_wstring(indexed_.load()) +
                              (live_tracked ? L" 项（USN 实时）" : L" 项（部分非实时）"));
                }
            }
            const size_t n = static_cast<size_t>(LiveCount());
            if (n > 0 && (deleted_ * 10 > n || (pool_waste_ > 0 && pool_waste_ * 10 > live_.pool.size())))
                need_compact = true;
        }
        if (failed && running_) {
            FullRebuild();
            last_save = GetTickCount64();
            dirty = false;
            continue;
        }
        if (need_compact) {
            std::unique_lock<std::shared_mutex> lock(mutex_);
            CompactLocked();
            dirty = true;
        }
        if (changed) {
            dirty = true;
            if (notify_ && notify_msg_) PostMessageW(notify_, notify_msg_, 2, 0);
        }
        const bool delta_big = [&] {
            std::shared_lock<std::shared_mutex> lock(mutex_);
            return live_.nodes.size() > 100000 || patches_.size() > 10000;
        }();
        if ((dirty && GetTickCount64() - last_save >= kCacheSaveIntervalMs) || delta_big) {
            SaveCache();
            last_save = GetTickCount64();
            dirty = false;
        }
    }
    if (dirty) SaveCache();
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
    std::wstring pending_old;
    const BYTE* p = buf;
    const BYTE* end = buf + len;
    while (p + sizeof(FILE_NOTIFY_INFORMATION) <= end) {
        auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
        std::wstring rel(info->FileName, info->FileNameLength / sizeof(WCHAR));
        std::wstring full = root;
        if (!full.empty() && full.back() != L'\\' && !rel.empty()) full += L'\\';
        full += rel;
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
                ChildMapRemove(old.parent, NameOf(idx), idx);
                const int32_t base = BaseCount();
                if (idx >= base) {
                    Node& n = live_.nodes[static_cast<size_t>(idx - base)];
                    pool_waste_ += n.len;
                    n.off = static_cast<uint32_t>(live_.pool.size());
                    n.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + n.len);
                } else {
                    Patch& pt = patches_[idx];
                    pt.parent = old.parent;
                    pt.flags = old.flags;
                    pt.has_meta = true;
                    pt.has_name = true;
                    pt.off = static_cast<uint32_t>(live_.pool.size());
                    pt.len = static_cast<uint16_t>((std::min)(new_name.size(), static_cast<size_t>(65535)));
                    live_.pool.insert(live_.pool.end(), new_name.begin(), new_name.begin() + pt.len);
                }
                ChildMapAdd(old.parent, new_name, idx);
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
        p += info->NextEntryOffset;
    }
}

} // namespace pulse::index
