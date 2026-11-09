// places.h — Workspaces, color tags, network pins (plan.md stage 2 / ui.md §7.5–7.7).
#pragma once
#include "../fs/fs_enum.h"
#include "../ui/view_layout.h"
#include <cstdint>
#include <atomic>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <windows.h>

namespace pulse::app {

using TagId = std::wstring;

enum class TagSelectionState { None, Mixed, All };

struct Workspace {
    bool expanded = false;
    uint32_t color_rgb = 0x4CC2FF;
    std::wstring name;
    std::wstring root;
    int layout = 0;
    std::vector<std::wstring> pane_paths;
    std::vector<ui::ViewMode> pane_views;
    std::vector<std::pair<std::wstring, int>> frequent; // path, visit count
};

struct ColorTag {
    TagId id;
    std::wstring name;
    uint32_t rgb = 0xEF4444;
    std::vector<std::wstring> paths;
    // Favorite slot 1..kMaxFavoriteTags (Alt+N toggles it on the selection);
    // 0 = not a favorite. Slots stay put when tags are reordered or removed.
    int favorite = 0;
    std::wstring group;  // TagGroup::id; empty or unknown = ungrouped
};

constexpr int kMaxFavoriteTags = 9;

// Sidebar-only grouping of tags (状态 / 项目 / 类型…); a group never changes
// what a tag matches.
struct TagGroup {
    std::wstring id;
    std::wstring name;
    bool collapsed = false;
};

// How the sidebar lists tags.
enum class TagSidebarMode : int {
    FavoritesAndGroups = 0,  // favorites first, the rest under their groups
    AllByGroup = 1,          // every tag under its group, favorites included
    FavoritesOnly = 2,
};

struct TagAdsRecord {
    TagId id;
    std::wstring name;
    uint32_t rgb = 0;
};

struct NetworkPlace {
    std::wstring name;
    std::wstring unc;
    fs::NetStatus status = fs::NetStatus::Unknown;
    DWORD rtt_ms = 0;
};

enum class PlaceItemKind { Unknown = 0, File, Folder };

struct StarredItem {
    std::wstring path;
    PlaceItemKind kind = PlaceItemKind::Unknown;
    std::wstring badge;
    uint32_t badge_rgb = 0x0078D4;
    std::wstring group;            // StarredGroup::id (folders only); empty = top level
    bool separator_after = false;  // a thin line after it (收藏栏, sidebar)
};

inline constexpr uint32_t kDefaultBadgeRgb = 0x0078D4;

// 收藏夹分组 (toolbar redesign P2): a named run of starred folders shown as one
// entry, a dropdown on the 收藏栏 and a folding row in the sidebar. A group
// without members is not saved.
struct StarredGroup {
    std::wstring id;
    std::wstring name;
    bool collapsed = false;        // the sidebar's fold
    bool separator_after = false;
};

// The starred list as the 收藏栏 and the sidebar show it: each group in place
// of its members, at its first member; separators after the items (or
// groups) that have one. Members inside a group are not rows of their own.
struct StarredRow {
    enum class Kind { Item, Group, Separator } kind = Kind::Item;
    size_t index = 0;              // Item: into the items; Group: its first member
    std::wstring group;            // Group: its id
    std::vector<size_t> members;   // Group: its members, in list order
};
std::vector<StarredRow> MakeStarredRows(const std::vector<StarredItem>& items,
                                        const std::vector<StarredGroup>& groups,
                                        bool folders_only);

// Badge on a non-starred sidebar link (built-in quick access, pinned folder,
// OneDrive). The sidebar model is rebuilt from scratch, so it lives here.
struct QuickAccessBadge {
    std::wstring path;
    std::wstring badge;
    uint32_t badge_rgb = kDefaultBadgeRgb;
};

struct RecentItem {
    std::wstring path;
    PlaceItemKind kind = PlaceItemKind::Unknown;
    uint64_t opened_at = 0; // UTC FILETIME ticks.
};

enum class RecentFilter { All = 0, Folders, Files };

struct TagAdsUpdate {
    std::wstring path;
    std::vector<std::wstring> tag_names;
    std::vector<TagAdsRecord> tags;
};

// Quick-access layout keys: L"builtin:<name>" for a built-in link (see
// BuiltinQuickAccessKey in app_model.h), the folder path for a pinned folder.
bool IsBuiltinQuickAccessKey(std::wstring_view key) noexcept;
// Display order of the quick-access rows. `defaults` lists every row in its
// default place (built-in links, then pins); `order` is the user's arrangement.
// Rows `order` does not mention still get a place: a built-in link follows its
// default predecessor, a folder pinned later goes to the end. Keys of rows that
// no longer exist are dropped. Paths compare case-insensitively.
std::vector<std::wstring> MergeQuickAccessOrder(const std::vector<std::wstring>& order,
                                                const std::vector<std::wstring>& defaults);
// Moves `key` within `keys` to gap `gap` (0..visible.size()) of `visible`, the
// rows on screen (a subsequence of `keys`; hidden links keep their places).
std::vector<std::wstring> MoveQuickAccessKey(std::vector<std::wstring> keys,
                                             const std::vector<std::wstring>& visible,
                                             const std::wstring& key, size_t gap);

class PlacesCatalog {
public:
    PlacesCatalog() = default;
    ~PlacesCatalog();
    std::vector<Workspace> workspaces;
    std::vector<ColorTag> tags;
    std::vector<TagGroup> tag_groups;
    TagSidebarMode tag_sidebar_mode = TagSidebarMode::FavoritesAndGroups;
    std::vector<NetworkPlace> networks;
    std::vector<std::wstring> quick_access_paths;
    // The user's quick-access arrangement, built-in links and pins mixed (see
    // MergeQuickAccessOrder). Empty means the default order.
    std::vector<std::wstring> quick_access_order;
    std::vector<StarredItem> starred_items;
    std::vector<StarredGroup> starred_groups;
    std::vector<QuickAccessBadge> quick_access_badges;
    std::vector<RecentItem> recent_items;
    int active_workspace = -1;
    bool persist = true; // self-test can disable disk writes
    bool load_failed = false;

    void EnsureDefaults();
    bool Load();
    bool Save() const;
    // UI-owned batch: reuse mutation helpers while queueing one final disk snapshot.
    class ScopedAsyncSave {
    public:
        explicit ScopedAsyncSave(PlacesCatalog& owner) : owner_(owner) { ++owner_.async_save_depth_; }
        ~ScopedAsyncSave() {
            if (--owner_.async_save_depth_ == 0 && owner_.async_save_dirty_) {
                owner_.async_save_dirty_ = false;
                owner_.QueuePlacesSave();
            }
        }
        ScopedAsyncSave(const ScopedAsyncSave&) = delete;
        ScopedAsyncSave& operator=(const ScopedAsyncSave&) = delete;
    private:
        PlacesCatalog& owner_;
    };
    bool FlushTagSave() const;
    DWORD TagSaveError() const;

    int FindWorkspace(const std::wstring& root) const;
    int PinWorkspace(const std::wstring& root, const std::wstring& name, int layout,
                     const std::vector<std::wstring>& pane_paths,
                     const std::vector<ui::ViewMode>& pane_views = {});
    bool UnpinWorkspace(int index);
    bool UnpinWorkspace(const std::wstring& root);
    void UpdateWorkspaceSnapshot(int index, int layout, const std::vector<std::wstring>& pane_paths,
                                 const std::vector<ui::ViewMode>& pane_views = {});

    int FindNetwork(const std::wstring& unc) const;
    int PinNetwork(const std::wstring& unc, const std::wstring& name);
    bool UnpinNetwork(const std::wstring& unc);
    // Pulse's display name for a pin; empty goes back to the share's own name.
    bool RenameNetwork(const std::wstring& unc, const std::wstring& name);
    void SetNetworkStatus(const std::wstring& unc, fs::NetStatus status, DWORD rtt_ms);

    void RecordVisit(const std::wstring& path);
    std::vector<std::wstring> FrequentChildren(int workspace_index, int limit = 8) const;

    bool ToggleTag(int tag_index, const std::wstring& path);
    bool SetTagged(int tag_index, const std::wstring& path, bool tagged);
    bool SetTaggedBatch(int tag_index, const std::vector<std::wstring>& paths, bool tagged,
                        std::vector<TagAdsUpdate>* deferred_ads = nullptr);
    bool ToggleTagBatch(int tag_index, const std::vector<std::wstring>& paths,
                        std::vector<TagAdsUpdate>* deferred_ads = nullptr);
    bool PathHasTag(const std::wstring& path, int tag_index) const;
    std::vector<int> TagsForPath(const std::wstring& path) const;
    const std::vector<int>* TagIndicesForPath(const std::wstring& path) const;
    const ColorTag* FindTag(const TagId& id) const;
    int FindTagIndex(const TagId& id) const;
    TagId ResolveTagRef(const std::wstring& ref) const;
    std::vector<std::wstring> PathsForTag(const TagId& id) const;

    TagId CreateTag(const std::wstring& name, uint32_t rgb);
    bool RenameTag(const TagId& id, const std::wstring& name);
    bool SetTagColor(const TagId& id, uint32_t rgb);
    size_t DeleteTag(const TagId& id, std::vector<TagAdsUpdate>* deferred_ads = nullptr);
    bool SetTagsBatch(const TagId& id, const std::vector<std::wstring>& paths, bool tagged,
                      std::vector<TagAdsUpdate>* deferred_ads = nullptr);
    TagSelectionState GetSelectionState(const TagId& id,
                                        const std::vector<std::wstring>& paths) const;
    void RemapPaths(const std::wstring& old_path, const std::wstring& new_path);
    // The same as one call per pair in order, with one save at the end.
    void RemapPaths(const std::vector<std::pair<std::wstring, std::wstring>>& moves);
    void CloneAssignments(const std::wstring& source, const std::wstring& destination);
    void RemoveAssignments(const std::wstring& path, bool include_descendants);
    // The same as one call per path, with one save at the end.
    void RemoveAssignments(const std::vector<std::wstring>& paths, bool include_descendants);

    bool IsQuickAccessPinned(const std::wstring& path) const;
    bool SetQuickAccessPinned(const std::vector<std::wstring>& paths, bool pinned);
    // Moves a pinned folder to a gap in the original pin list (0..size, clamped).
    bool ReorderQuickAccessPinned(const std::wstring& path, size_t position);
    // Stores a whole quick-access layout, `keys` in display order. The pins
    // become exactly the pin keys, in that order; the order list is kept only
    // when it differs from the default (the built-in links first, in
    // `builtin_defaults` order, then the pins), so links added by a later
    // version still land in their default places.
    bool SetQuickAccessLayout(const std::vector<std::wstring>& keys,
                              const std::vector<std::wstring>& builtin_defaults);

    bool IsStarred(const std::wstring& path) const;
    const StarredItem* FindStarred(const std::wstring& path) const;
    StarredItem* FindStarred(const std::wstring& path);
    bool ToggleStarred(const std::wstring& path,
                       PlaceItemKind kind = PlaceItemKind::Unknown);
    bool SetStarredBadge(const std::wstring& path, const std::wstring& text,
                         uint32_t rgb);
    bool SetStarredKind(const std::wstring& path, PlaceItemKind kind);
    const QuickAccessBadge* FindQuickAccessBadge(const std::wstring& path) const;
    // Empty text with the default color removes the badge.
    bool SetQuickAccessBadge(const std::wstring& path, const std::wstring& text, uint32_t rgb);
    bool ReorderStarredFolder(const std::wstring& path, size_t folder_position);
    // 收藏栏 drag: the item at `from` goes to `to` (indices into
    // starred_items). Folders stay ahead of files, so both ends must be of
    // the same kind (a folder, or a file).
    bool MoveStarredItem(size_t from, size_t to);
    std::vector<std::wstring> StarredPaths() const;
    std::vector<std::wstring> StarredFolderPaths() const;
    // 收藏夹分组. Only folders join a group; joining (gather) moves the folder
    // after the group's last member, so a group stays one run.
    const StarredGroup* FindStarredGroup(const std::wstring& id) const;
    std::wstring CreateStarredGroup(const std::wstring& name, const std::wstring& first_path);
    bool SetStarredItemGroup(const std::wstring& path, const std::wstring& group_id, bool gather = true);
    bool RenameStarredGroup(const std::wstring& id, const std::wstring& name);
    bool SetStarredGroupCollapsed(const std::wstring& id, bool collapsed);
    bool UngroupStarred(const std::wstring& id);   // the members go back to the top level
    // A favorite's (or, with a group id, a group's) separator after it.
    bool SetStarredSeparator(const std::wstring& path_or_group, bool on);
    // The 收藏栏 drag: the starred list in this order (a permutation).
    bool ReorderStarred(const std::vector<size_t>& order);
    std::vector<StarredRow> StarredRows(bool folders_only) const {
        return MakeStarredRows(starred_items, starred_groups, folders_only);
    }

    void RecordRecent(const std::wstring& path, PlaceItemKind kind);
    const RecentItem* FindRecent(const std::wstring& path) const;
    bool SetRecentKind(const std::wstring& path, PlaceItemKind kind);
    bool RemoveRecent(const std::wstring& path);
    bool ClearRecent();
    std::vector<RecentItem> RecentItems(RecentFilter filter = RecentFilter::All) const;
    std::vector<std::wstring> RecentFolderPaths(size_t limit = 16) const;
    bool FlushPendingSave(bool force = false) const;

    void MergeAdsRecords(const std::wstring& path,
                         const std::vector<TagAdsRecord>& records,
                         const std::vector<std::wstring>& legacy_names = {});
    void ReadAdsIntoCatalog(const std::wstring& path);
    uint64_t TagRevision() const noexcept { return tag_revision_; }
    void TagsReordered();
    // Favorite tags: slot 1..kMaxFavoriteTags -> tag index, or -1 when free.
    int FavoriteTagIndex(int slot) const;
    // Adding takes the lowest free slot (false when all are taken).
    bool SetTagFavorite(const TagId& id, bool favorite);
    // Gives the favorites, in this order, the slots they hold now (sorted), so
    // a sidebar drag can reorder them without touching the other slots.
    void ReorderFavoriteTags(const std::vector<TagId>& order);

    const TagGroup* FindTagGroup(const std::wstring& id) const;
    // A tag's group as the sidebar shows it: empty when ungrouped or unknown.
    std::wstring TagGroupOf(const ColorTag& tag) const;
    std::wstring CreateTagGroup(const std::wstring& name);  // empty on a bad name
    bool RenameTagGroup(const std::wstring& id, const std::wstring& name);
    bool DeleteTagGroup(const std::wstring& id);  // its tags become ungrouped
    bool SetTagGroup(const TagId& id, const std::wstring& group_id);
    bool SetTagGroupCollapsed(const std::wstring& id, bool collapsed);
    void SetTagSidebarMode(TagSidebarMode mode);

private:
    friend struct PlacesPersistenceAudit;
#ifdef PULSE_PLACES_SAVE_TEST
    friend struct PlacesSaveTestAccess;
#endif
    struct SaveSnapshot {
        std::vector<Workspace> workspaces;
        std::vector<ColorTag> tags;
        std::vector<TagGroup> tag_groups;
        TagSidebarMode tag_sidebar_mode = TagSidebarMode::FavoritesAndGroups;
        std::vector<NetworkPlace> networks;
        std::vector<std::wstring> quick_access_paths;
        std::vector<std::wstring> quick_access_order;
        std::vector<StarredItem> starred_items;
        std::vector<StarredGroup> starred_groups;
        std::vector<QuickAccessBadge> quick_access_badges;
        std::vector<RecentItem> recent_items;
        int active_workspace = -1;
        uint64_t tags_generation = 0;
        bool persist = true;
    };

    void RebuildTagIndex();
    int FreeFavoriteSlot() const;  // 0 when every slot is taken
    void NormalizeFavoriteSlots();
    void RebuildStarIndex();
    void CommitTagChanges(const std::vector<std::wstring>& paths,
                          std::vector<TagAdsUpdate>* deferred_ads);
    void QueueTagSave() const;
    static bool SaveTagFile(const std::vector<ColorTag>& tags, uint64_t generation);
    void StopTagWriter();
    SaveSnapshot CaptureSaveSnapshot() const;
    static bool SaveSnapshotFile(const SaveSnapshot& snapshot);
    void QueuePlacesSave() const;
    unsigned async_save_depth_ = 0;
    mutable bool async_save_dirty_ = false;
    void StopPlacesWriter();
    void MarkPlacesDirty() const;

    std::unordered_map<std::wstring, std::vector<int>> tag_index_;
    std::unordered_set<std::wstring> starred_index_;
    mutable ULONGLONG places_save_due_ = 0;
    uint64_t tag_revision_ = 1;
    mutable std::mutex tag_save_mutex_;
    mutable std::condition_variable tag_save_cv_;
    mutable std::optional<std::pair<std::vector<ColorTag>, uint64_t>> pending_tag_save_;
    mutable uint64_t tags_generation_ = 0;
    mutable DWORD tag_save_error_ = ERROR_SUCCESS;
    mutable std::mutex tag_save_io_mutex_;
    mutable std::thread tag_save_thread_;
    mutable bool tag_save_stop_ = false;
    mutable std::mutex places_save_mutex_;
    mutable std::condition_variable places_save_cv_;
    mutable std::optional<std::pair<SaveSnapshot, uint64_t>> pending_places_save_;
    mutable std::thread places_save_thread_;
    mutable bool places_save_stop_ = false;
    mutable std::mutex places_save_io_mutex_;
    mutable uint64_t places_save_revision_ = 0;
};

inline std::wstring MakeStarredPath() { return L"pulse:starred"; }
inline std::wstring MakeRecentPath() { return L"pulse:recent"; }
inline std::wstring MakeRecyclePath() { return L"pulse:recycle"; }
inline std::wstring MakeTagPath(const TagId& id) { return L"pulse:tag:" + id; }
inline std::wstring MakeTagPath(int i) { return L"pulse:tag:" + std::to_wstring(i); }
// Sidebar-only rows of the 标签 section: a group header and 所有标签 ›.
inline std::wstring MakeTagGroupPath(const std::wstring& id) { return L"pulse:tag-group:" + id; }
inline bool IsTagGroupPath(const std::wstring& path) { return path.starts_with(L"pulse:tag-group:"); }
inline std::wstring TagGroupIdFromPath(const std::wstring& path) {
    return IsTagGroupPath(path) ? path.substr(16) : std::wstring{};
}
inline std::wstring MakeTagManagerPath() { return L"pulse:tag-manager"; }
// A 收藏夹分组 entry (收藏栏 button, sidebar row); not a place.
inline std::wstring MakeStarredGroupPath(const std::wstring& id) { return L"pulse:star-group:" + id; }
inline bool IsStarredGroupPath(const std::wstring& path) { return path.starts_with(L"pulse:star-group:"); }
inline std::wstring StarredGroupIdFromPath(const std::wstring& path) {
    return IsStarredGroupPath(path) ? path.substr(17) : std::wstring{};
}
inline std::wstring MakeSearchPath(const std::wstring& q) { return L"pulse:search:" + q; }
inline std::wstring MakeWorkspacePath(int i) { return L"pulse:workspace:" + std::to_wstring(i); }
inline std::wstring MakeSettingsPath(std::wstring_view page = L"general") {
    return L"pulse:settings:" + std::wstring(page);
}
bool ParsePulsePath(const std::wstring& path, std::wstring* kind, std::wstring* rest);

bool WriteTagAdsV2(const std::wstring& path, const std::vector<TagAdsRecord>& tags);
std::vector<std::wstring> ReadTagAds(const std::wstring& path);
std::vector<TagAdsRecord> ReadTagAdsV2(const std::wstring& path);

} // namespace pulse::app
