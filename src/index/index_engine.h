// index_engine.h — Filename index: mmap base + heap delta (优化.md R1).
//
// Disk format v6 is the in-memory layout. The process maps pulse-index.bin
// read-only; USN/RDCW mutations append to a small heap delta. Search takes a
// shared lock and never waits on Status()/Count().
#pragma once
#include "index_query.h"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <windows.h>
#include <winioctl.h>

namespace pulse::index {

inline constexpr size_t kSearchPageCap = 100000;
inline constexpr size_t kSearchUiPageSize = 2048;

struct Hit {
    std::wstring path;
    std::wstring name;
    bool is_dir = false;
    uint64_t size = 0;
    uint64_t mtime = 0; // FILETIME as u64
};

enum class ResultSort : uint8_t { Index, Name, Size, Mtime };

struct Query {
    std::wstring needle;
    std::wstring path_prefix;
    bool folders_only = false;
    size_t offset = 0;
    size_t limit = 48;
    bool rank = true;
    ResultSort sort = ResultSort::Index;
    bool sort_desc = false;
};

struct SearchResult {
    std::vector<Hit> hits;
    size_t total = 0;
};

#pragma pack(push, 1)
struct Node {
    int32_t parent = -1;
    uint32_t off = 0;
    uint16_t len = 0;
    uint8_t flags = 0;
    uint8_t pad = 0;
    uint32_t unused = 0;
};
struct Attr {
    uint32_t mtime = 0; // Unix seconds; 0 = unknown
    uint64_t size = 0;
};
struct DiskHeader {
    char magic[4];
    uint32_t ver;
    uint32_t node_count;
    uint32_t vol_count;
    uint64_t built_unix;
    uint64_t nodes_off;
    uint64_t attrs_off;
    uint64_t pool_off;
    uint64_t pool_chars;
    uint64_t vols_off;
    uint64_t frn_off;
    uint64_t frn_count;
};
struct DiskVol {
    uint16_t letter = 0;
    uint16_t pad = 0;
    uint32_t frn_count = 0;
    uint64_t journal_id = 0;
    int64_t next_usn = 0;
    int32_t root_idx = -1;
    int32_t pad2 = 0;
    uint64_t frn_off = 0;
};
struct DiskFrn {
    uint64_t frn = 0;
    int32_t idx = -1;
    int32_t pad = 0;
};
#pragma pack(pop)

static_assert(sizeof(Node) == 16, "v6 node is 16 bytes");
static_assert(sizeof(Attr) == 12, "v6 attr is 12 bytes");

class Engine {
public:
    Engine() = default;
    ~Engine() { Stop(); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Start(HWND notify, UINT msg);
    void Stop();

    SearchResult Search(const Query& q) const;
    size_t Count() const { return indexed_.load(); }
    bool Ready() const { return ready_.load(); }
    std::wstring Status() const;
    void AddForTest(std::wstring path, std::wstring name, bool is_dir,
                    uint64_t size = 0, uint64_t mtime = 0);

private:
    static constexpr uint8_t kFlagDir = 1;
    static constexpr uint8_t kFlagHidden = 2;
    static constexpr uint8_t kFlagDeleted = 4;

    struct Store {
        std::vector<wchar_t> pool;
        std::vector<Node> nodes;
        std::vector<Attr> attrs;
        void Clear() { pool.clear(); nodes.clear(); attrs.clear(); }
        void Shrink() { pool.shrink_to_fit(); nodes.shrink_to_fit(); attrs.shrink_to_fit(); }
    };

    struct MappedFile {
        HANDLE file = INVALID_HANDLE_VALUE;
        HANDLE mapping = nullptr;
        const uint8_t* view = nullptr;
        size_t size = 0;
        const DiskHeader* hdr = nullptr;
        const Node* nodes = nullptr;
        const Attr* attrs = nullptr;
        const wchar_t* pool = nullptr;
        uint32_t n = 0;
        const DiskVol* vols = nullptr;
        uint32_t nvol = 0;
        const DiskFrn* frns = nullptr;
        uint32_t nfrn = 0;
        void Close();
        ~MappedFile() { Close(); }
        MappedFile() = default;
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;
    };

    struct VolState {
        wchar_t letter = 0;
        uint64_t journal_id = 0;
        int64_t next_usn = 0;
        int32_t root_idx = -1;
        const DiskFrn* frn_base = nullptr;
        uint32_t frn_base_n = 0;
        std::vector<DiskFrn> frn_new;
        std::vector<std::pair<uint64_t, int32_t>> frn_build; // MFT rebuild only
    };

    struct Patch {
        int32_t parent = -1;
        uint32_t off = 0;
        uint16_t len = 0;
        uint8_t flags = 0;
        bool has_meta = false;
        bool has_name = false;
        bool has_attr = false;
        Attr attr{};
    };

    void Worker();
    bool TryLoadCache();
    void SaveCache();
    void FullRebuild();
    bool IndexVolumeMft(wchar_t letter);
    void WalkTree(int32_t parent, const std::wstring& dir, int depth);
    void CompactLocked();
    bool FlattenLocked(Store& out, std::vector<VolState>& vols_out) const;
    bool WriteIndexFile(const std::wstring& path, const Store& s,
                        const std::vector<VolState>& vols, uint64_t built_unix) const;
    bool MapIndexFile(const std::wstring& path, std::unique_ptr<MappedFile>& out) const;
    void AdoptMappedLocked(std::unique_ptr<MappedFile> mapped);
    void RebuildChildMapLocked();
    bool CommitMappedFile(const std::wstring& path);

    void CollectMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                              bool folders_only, bool use_attrs,
                              std::vector<int32_t>& ids) const;
    void SortIdsLocked(std::vector<int32_t>& ids, ResultSort sort, bool desc) const;

    void StartWalkWatches(const std::vector<std::wstring>& roots);
    void StopWalkWatches();
    void PollWalkWatches();
    void ApplyNotifyLocked(const std::wstring& root, const BYTE* buf, DWORD len);

    bool CatchUpVolume(VolState& v, bool* changed);
    void ApplyUsnLocked(VolState& v, const USN_RECORD_V2* rec);
    int32_t FindByFrnLocked(const VolState& v, uint64_t frn) const;
    static void MapFrnLocked(VolState& v, uint64_t frn, int32_t idx);

    int32_t AddNodeLocked(Store& s, int32_t parent, std::wstring_view name, uint8_t flags,
                          uint64_t frn = 0, uint64_t size = 0, uint64_t mtime_ft = 0,
                          bool index_live = false, VolState* vol = nullptr);
    int32_t FindChildLiveLocked(int32_t parent, std::wstring_view name) const;
    static int32_t FindChildInStore(const Store& s, int32_t parent, std::wstring_view name);
    int32_t EnsureChainLocked(Store& s, const std::wstring& path, bool leaf_is_dir,
                              bool index_live);
    std::wstring BuildPathLocked(int32_t node) const;
    bool IsUnderLocked(int32_t node, int32_t ancestor) const;
    int32_t ResolvePathLocked(const std::wstring& path) const;
    bool MatchNodeLocked(int32_t i, const CompiledQuery& q, int32_t prefix_node,
                         bool folders_only, bool use_attrs) const;
    void InvalidateFilterLocked() { ++filter_epoch_; }
    void SetStatus(std::wstring s);
    void PingNotify(bool force = false);

    int32_t BaseCount() const { return map_ ? static_cast<int32_t>(map_->n) : 0; }
    int32_t LiveCount() const { return BaseCount() + static_cast<int32_t>(live_.nodes.size()); }
    bool IsTomb(int32_t i) const;
    Node NodeAt(int32_t i) const;
    Attr AttrAt(int32_t i) const;
    std::wstring_view NameOf(int32_t i) const;
    void ChildMapAdd(int32_t parent, std::wstring_view name, int32_t idx);
    void ChildMapRemove(int32_t parent, std::wstring_view name, int32_t idx);

    HWND notify_ = nullptr;
    UINT notify_msg_ = 0;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> ready_{false};
    std::atomic<bool> building_{false};
    std::atomic<size_t> indexed_{0};
    std::atomic<ULONGLONG> last_notify_{0};

    mutable std::shared_mutex mutex_;
    mutable std::mutex status_mu_;
    std::wstring status_;

    std::unique_ptr<MappedFile> map_;
    Store live_;  // tests: full store; after mmap: delta only
    Store build_;
    std::vector<VolState> vols_;
    std::vector<VolState> build_vols_;
    std::unordered_set<int32_t> tombstones_;
    std::unordered_map<int32_t, Patch> patches_;
    std::unordered_multimap<uint64_t, int32_t> child_map_;
    size_t deleted_ = 0;
    size_t pool_waste_ = 0;
    uint64_t built_unix_ = 0;

    struct WalkWatch {
        std::wstring path;
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED ov{};
        std::vector<BYTE> buf;
    };
    std::vector<WalkWatch> watches_;
    std::vector<std::wstring> walk_roots_;

    mutable uint64_t filter_epoch_ = 1;
    mutable uint64_t cache_epoch_ = 0;
    mutable std::wstring cache_raw_;
    mutable std::wstring cache_path_prefix_;
    mutable bool cache_folders_only_ = false;
    mutable bool cache_ranked_ = false;
    mutable ResultSort cache_sort_ = ResultSort::Index;
    mutable bool cache_sort_desc_ = false;
    mutable std::vector<int32_t> cache_ids_;
};

} // namespace pulse::index
