// index_engine.h — Filename index: mmap base + heap delta (优化.md R1).
//
// V11 uses the V9 aggregate snapshot layout with corrected attribute replay. The base is read-only;
// USN/RDCW mutations append to a small heap delta. Search takes a shared lock
// and never waits on Status()/Count(). V7-V10 snapshots remain readable and
// trigger a background rebuild to repair visibility and disconnected parent links.
#pragma once
#include "usn_stream.h"
#include "filename_timing.h"
#include "index_feed.h"
#include "folder_size_index.h"
#include "folder_size_protocol.h"
#include "index_config.h"
#include "index_query.h"
#include "index_delta.h"
#include "change_tracking.h"
#include "change_feed_history.h"
#include <atomic>
#include <bit>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <map>
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
// Consecutive base-merge failures back off 5 s, 10 s, 20 s ... capped at ten
// minutes, so a deterministic failure cannot rewrite the full base every 5 s.
constexpr ULONGLONG MergeRetryDelayMs(uint32_t consecutive_failures) noexcept {
    constexpr ULONGLONG base = 5000, cap = 10ull * 60ull * 1000ull;
    if (consecutive_failures == 0) return 0;
    const uint32_t shift = consecutive_failures - 1 < 7 ? consecutive_failures - 1 : 7;
    const ULONGLONG delay = base << shift;
    return delay < cap ? delay : cap;
}
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
    uint64_t session_id = 0;
    bool subscribe = false;
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
    uint64_t revision = 0;
    std::vector<Hit> hits;
    size_t total = 0;
    DWORD error = 0;
    // Filtered searches: parent folders DirVisibility could not answer yet.
    // No hits are produced until every one of them has been checked.
    std::vector<std::pair<int32_t, std::wstring>> unchecked_dirs;
    uint64_t layout = 0;  // Engine::LayoutEpoch() the node ids above belong to
};

// Optional per-caller filter for Engine::Search. A match is returned only
// when the caller may list its parent folder, since that listing is what
// would show the name to the caller in Explorer. Node ids passed to State()
// are stable for one Engine::LayoutEpoch().
class DirVisibility {
public:
    virtual ~DirVisibility() = default;
    virtual int State(int32_t dir) const = 0;  // 1 listable, 0 not, -1 unknown
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
    uint64_t child_order_off;
    uint64_t name_order_off;   // 0 = absent (v8+)
    uint64_t size_order_off;   // 0 = absent
    uint64_t mtime_order_off;  // 0 = absent
    uint64_t prefix1_off;      // 65537 u32 starts + posting_count i32 ids
    uint64_t prefix2_off;
};
struct DiskVol {
    uint16_t letter = 0;
    uint16_t kind = 0;
    uint32_t frn_count = 0;
    uint64_t item_count = 0;
    uint64_t journal_id = 0;
    int64_t next_usn = 0;
    int32_t root_idx = -1;
    int32_t first_idx = 0; // v9: inclusive start of this volume's DFS span
    uint64_t frn_off = 0;
    wchar_t volume_id[64]{};
};
struct DiskFrn {
    uint64_t frn = 0;
    int32_t idx = -1;
    int32_t pad = 0;
};
#pragma pack(pop)

static_assert(sizeof(Node) == 16, "v6 node is 16 bytes");
static_assert(sizeof(Attr) == 12, "v6 attr is 12 bytes");
static_assert(sizeof(DiskHeader) == 128, "v8 header fills the 128-byte prefix");

class Engine {
public:
    Engine() = default;
    ~Engine() { Stop(); }
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void Start(HWND notify, UINT msg);
    void StartFixture(HWND notify, UINT msg, std::wstring root) { fixture_root_ = std::move(root); Start(notify,msg); }
    void RequestStop() { running_ = false; if (change_signal_) SetEvent(change_signal_); }
    void Stop();

    SearchResult Search(const Query& q, const std::atomic<uint32_t>* latest = nullptr,
                        uint32_t expected = 0,
                        const DirVisibility* visibility = nullptr) const;
    size_t Count() const { return indexed_.load(); }
    bool Ready() const { return ready_.load(); }
    bool PinyinReady() const { return pinyin_ready_.load(); }
    uint64_t Revision() const { return revision_.load(); }
    uint64_t LayoutEpoch() const { return layout_epoch_.load(); }
    FileFeedPage ReadFeed(bool changes, const std::wstring& root, uint64_t epoch, uint64_t cursor) const;
    std::vector<IndexedFolderSize> FolderSizes(const std::vector<std::wstring>& paths);
    std::wstring Status() const;
    std::vector<VolumeInfo> Volumes() const;
    void RequestRebuild();
    ChangeState ChangeCoverage(const std::wstring& path) const;
    void SetChangeLease(const std::wstring& owner, bool enabled);
    ChangeTracker& Changes() { return changes_; }
    void AddForTest(std::wstring path, std::wstring name, bool is_dir,
                    uint64_t size = 0, uint64_t mtime = 0);

private:
    FolderSizeIndex folder_sizes_;
    ULONGLONG folder_size_retry_after_ = 0;
    bool folder_size_usn_update_ = false;
    std::atomic<bool> folder_size_gap_{false};
    // Totals are aggregated under a shared lock so searches keep running; edits
    // published in between are detected by this counter (guarded by mutex_).
    uint64_t folder_size_mutations_ = 0;
    std::mutex folder_size_build_mutex_;
    // A running service aggregates on its worker and answers clients at once.
    bool folder_size_background_ = false;
    std::atomic<bool> folder_size_wanted_{false};
    FolderSizeIndex::Item FolderSizeItem(int32_t id) const;
    bool FolderSizeIdsLocked(const std::vector<std::wstring>& paths, std::vector<int32_t>& ids) const;
    bool BuildFolderTotals();
    ChangeTracker changes_;
    std::mutex change_seed_mutex_;
    std::unordered_set<std::wstring> change_seed_owners_;
    void SeedChanges(const std::wstring& owner);
    void SeedPendingChanges();
    std::unordered_map<std::wstring, std::wstring> walk_pending_renames_;
    friend struct EngineTestAccess;

    struct FrnNode {
        uint64_t frn = 0;
        uint64_t parent = 0;
        uint64_t size = 0;
        uint64_t mtime = 0;
        std::wstring name;
        int32_t index = -1;
        bool is_dir = false;
        uint8_t name_type = 0xFF;
        // An additional hard link name of `frn` (see MftFile::links). The
        // build keeps one node per link; only the primary resolves parents.
        bool extra_link = false;
    };
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
        const int32_t* child_order = nullptr;
        const int32_t* name_order = nullptr;
        const int32_t* size_order = nullptr;
        const int32_t* mtime_order = nullptr;
        const uint32_t* prefix1_start = nullptr; // 65537 entries
        const int32_t* prefix1_ids = nullptr;
        bool prefix1_all_chars = false;
        const uint32_t* prefix2_start = nullptr;
        const int32_t* prefix2_ids = nullptr;
        void Close();
        ~MappedFile() { Close(); }
        MappedFile() = default;
        MappedFile(const MappedFile&) = delete;
        MappedFile& operator=(const MappedFile&) = delete;
    };

    struct QueryShard {
        std::wstring volume_id;
        int32_t first = 0;
        int32_t last = 0;
        std::unique_ptr<MappedFile> mapped;
    };

    struct VolState {
        std::unordered_map<uint64_t, std::wstring> tracking_paths;
        wchar_t letter = 0;
        VolumeKind kind = VolumeKind::Other;
        std::wstring volume_id;
        uint64_t item_count = 0;
        uint64_t journal_id = 0;
        bool folder_size_current = false;
        int64_t next_usn = 0;
        int32_t root_idx = -1;
        int32_t first_idx = 0;
        const DiskFrn* frn_base = nullptr;
        uint32_t frn_base_n = 0;
        std::vector<DiskFrn> frn_new;
        std::vector<std::pair<uint64_t, int32_t>> frn_build; // MFT rebuild only
    };

    bool BuildMftTree(VolState vol, uint64_t root_frn, std::vector<FrnNode> nodes);
    bool NeedsSearchRebuildLocked() const;

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

    struct MatchSet {
        std::vector<int32_t> ids;
        std::vector<uint64_t> bits;
        size_t total = 0;
        int32_t universe = 0;
        bool dense = false;
        void Clear();
        void Begin(int32_t n);
        void Add(int32_t i);
        template <class Fn>
        void ForEach(Fn fn) const {
            if (!dense) {
                for (int32_t id : ids) fn(id);
                return;
            }
            for (size_t w = 0; w < bits.size(); ++w) {
                uint64_t x = bits[w];
                while (x) {
                    const unsigned b = static_cast<unsigned>(std::countr_zero(x));
                    fn(static_cast<int32_t>(w * 64 + b));
                    x &= x - 1;
                }
            }
        }
    };

    void Worker();
    std::wstring fixture_root_;
    void StartJournalStreams();
    std::map<std::wstring, std::unique_ptr<UsnStream>> journal_streams_;
    HANDLE change_signal_ = nullptr;
    std::atomic<uint64_t> revision_{1};
    std::atomic<uint64_t> layout_epoch_{1};  // bumped whenever node ids are renumbered
    std::atomic<uint64_t> feed_epoch_{(GetTickCount64() << 20) ^ GetCurrentProcessId()};
    uint64_t feed_sequence_ = 0;
    ChangeFeedHistory feed_changes_;
    void RecordFeed(const ChangeRecord& record);
    void CaptureMemoryState();
    void GapFeed() { changes_.Gap(); ++feed_epoch_; folder_size_gap_ = true; }
    void PinyinWorker();
    void RequestPinyinBuildLocked();
    void StopPinyinWorker();
    bool TryLoadCache();
    void SaveCache();
    void MergeBase(bool force, const char* reason = "forced");
    const char* MaintenanceMergeReason(ULONGLONG now, uint64_t delta_bytes) const;
    bool CompactNamePoolLocked(ULONGLONG now);
    void RecoverFailedVolumes(const std::vector<VolumeInfo>& volumes, ULONGLONG now,
                              const std::function<bool(const VolumeInfo&)>& rebuild);
    void FlushDeltas();
    void OpenDeltasLocked();
    void CloseDeltas();
    bool ReplayDeltasLocked();
    DeltaLog* DeltaFor(wchar_t letter);
    void FullRebuild(const char* reason = "requested_or_watch_gap");
    void PreserveOfflineVolumesLocked(const std::vector<VolumeInfo>& active,
                                      const IndexConfig& config);
    bool IndexVolumeMft(const VolumeInfo& volume);
    bool RebuildVolumeMft(const VolumeInfo& volume);
    void WalkTree(int32_t parent, const std::wstring& dir, int depth);
    void CompactLocked();
    bool FlattenLocked(Store& out, std::vector<VolState>& vols_out) const;
    bool WriteIndexFile(const std::wstring& path, const Store& s,
                        const std::vector<VolState>& vols, uint64_t built_unix) const;
    void WriteVolumeShards(const Store& aggregate, const std::vector<VolState>& vols,
                           uint64_t built_unix) const;
    bool MapIndexFile(const std::wstring& path, std::unique_ptr<MappedFile>& out) const;
    void AdoptMappedLocked(std::unique_ptr<MappedFile> mapped);
    void RefreshQueryShardsLocked();
    const QueryShard* QueryShardForLocked(int32_t id) const;
    Node QueryNodeAtLocked(int32_t id) const;
    Attr QueryAttrAtLocked(int32_t id) const;
    std::wstring_view QueryNameOfLocked(int32_t id) const;
    std::wstring BuildQueryPathLocked(int32_t id) const;
    bool MatchQueryNodeLocked(int32_t id, const CompiledQuery& query, int32_t prefix_node,
                              bool folders_only, bool use_attrs) const;
    void RebuildChildMapLocked();
    bool CommitMappedFile(const std::wstring& path);

    void CollectMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                              bool folders_only, bool use_attrs,
                              MatchSet& out, const std::atomic<uint32_t>* latest,
                              uint32_t expected) const;
    void NarrowMatchesLocked(const CompiledQuery& cq, int32_t prefix_node,
                             bool folders_only, bool use_attrs,
                             const MatchSet& prev, MatchSet& out,
                             const std::atomic<uint32_t>* latest, uint32_t expected) const;
    void SortIdsLocked(std::vector<int32_t>& ids, ResultSort sort, bool desc) const;
    void PartialSortPage(std::vector<int32_t>& ids, size_t offset, size_t limit,
                         ResultSort sort, bool desc) const;

    void StartWalkWatches(const std::vector<std::wstring>& roots);
    void StopWalkWatches();
    void PollWalkWatches();
    uint64_t ApplyNotifyLocked(const std::wstring& root, const BYTE* buf, DWORD len);

    bool CatchUpVolume(VolState& v, bool* changed, bool* structural = nullptr);
    enum class UsnApply : uint8_t { None, Attr, Structure };
    UsnApply ApplyUsnLocked(VolState& v, const USN_RECORD_V2* rec);
    bool IsIndexNoiseLocked(const VolState& v, const USN_RECORD_V2* rec) const;
    void ResolveIndexDirFrn();
    bool InSubtreeLocked(int32_t node, int32_t ancestor) const;
    int32_t SubtreeEndLocked(int32_t node) const;
    bool VolumeSpan(const VolState& v, int32_t& lo, int32_t& hi) const;
    int32_t FindByFrnLocked(const VolState& v, uint64_t frn) const;
    // Every live (non-tombstoned) node mapped to `frn`: one per hard link.
    void CollectFrnLocked(const VolState& v, uint64_t frn, std::vector<int32_t>& out) const;
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
    void UpdateVolumeVisibilityLocked(const std::vector<VolumeInfo>& active, bool only_hide = false);
    void InvalidateFilterLocked() {
        ++filter_epoch_;
        ++folder_size_mutations_;
        if (!folder_size_usn_update_) {
            folder_sizes_.Reset();
            folder_size_retry_after_ = 0;
        }
    }
    void SetStatus(std::wstring s);
    bool IsExcludedPath(std::wstring_view path) const;
    void PublishExcludedPaths(const IndexConfig& config,
                              const std::function<void()>& locked_action = {});
    bool ShouldSkipName(std::wstring_view name) const;
    void RefreshSubtreeVisibilityLocked(int32_t root, DeltaLog* delta);
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
    std::atomic<bool> rebuild_requested_{false};
    std::atomic<size_t> indexed_{0};
    std::atomic<ULONGLONG> last_notify_{0};

    mutable std::shared_mutex mutex_;
    mutable std::mutex query_mu_;
    mutable std::mutex status_mu_;
    std::wstring status_;

    std::unique_ptr<MappedFile> map_;
    std::vector<QueryShard> query_shards_;
    bool query_shards_ready_ = false;
    Store live_;  // tests: full store; after mmap: delta only
    Store build_;
    std::vector<VolState> vols_;
    std::vector<VolState> build_vols_;
    std::vector<int32_t> inactive_volume_roots_;
    std::unordered_set<int32_t> tombstones_;
    std::unordered_map<int32_t, Patch> patches_;
    std::unordered_multimap<uint64_t, int32_t> child_map_;
    std::unordered_map<wchar_t, std::unique_ptr<DeltaLog>> delta_logs_;
    size_t deleted_ = 0;
    size_t pool_waste_ = 0;
    ULONGLONG name_pool_retry_after_tick_ = 0;
    uint64_t built_unix_ = 0;
    uint64_t index_dir_frn_ = 0;
    std::wstring index_directory_;
    wchar_t index_dir_letter_ = 0;
    std::atomic<bool> merging_{false};
    size_t struct_changes_ = 0;
    bool subtree_intervals_valid_ = true;
    ULONGLONG last_merge_tick_ = 0;
    ULONGLONG merge_retry_after_tick_ = 0;
    uint32_t merge_failures_ = 0;
    ULONGLONG last_struct_tick_ = 0;
    ULONGLONG last_delta_flush_tick_ = 0;
    std::unordered_map<std::wstring, ULONGLONG> volume_retry_after_;
    std::unordered_map<std::wstring, ULONGLONG> volume_recovered_after_;
    FilenameTiming filename_timing_;

    struct WalkWatch {
        std::wstring path;
        HANDLE dir = INVALID_HANDLE_VALUE;
        HANDLE event = nullptr;
        OVERLAPPED ov{};
        std::vector<BYTE> buf;
    };
    std::vector<WalkWatch> watches_;
    std::vector<std::wstring> walk_roots_;
    std::vector<std::wstring> excluded_paths_;
    // System group "node_modules": dependency folders stay out of search.
    bool hide_node_modules_ = true;

    // Auxiliary IDs are tied to one immutable filename snapshot, never persisted
    // in the filename format. Mutable names always use the current overlay.
    std::thread pinyin_thread_;
    std::mutex pinyin_build_mutex_;
    std::condition_variable pinyin_build_cv_;
    std::atomic<uint64_t> pinyin_request_{0};
    std::atomic<bool> pinyin_stopping_{false};
    std::atomic<bool> pinyin_ready_{false};
    mutable const MappedFile* pinyin_snapshot_ = nullptr;
    mutable uint32_t pinyin_version_ = 0;
    mutable std::vector<int32_t> pinyin_chinese_ids_;
    mutable std::vector<std::vector<int32_t>> pinyin_pair_ids_;
    mutable uint64_t filter_epoch_ = 1;
    mutable uint64_t cache_epoch_ = 0;
    mutable std::wstring cache_raw_;
    mutable std::wstring cache_path_prefix_;
    mutable bool cache_folders_only_ = false;
    mutable bool cache_ranked_ = false;
    mutable ResultSort cache_sort_ = ResultSort::Index;
    mutable bool cache_sort_desc_ = false;
    mutable MatchSet cache_set_;
};

} // namespace pulse::index
