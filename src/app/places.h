// places.h — Workspaces, color tags, network pins (plan.md stage 2 / ui.md §7.5–7.7).
#pragma once
#include "../fs/fs_enum.h"
#include "../ui/view_layout.h"
#include <cstdint>
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

struct TagAdsUpdate {
    std::wstring path;
    std::vector<std::wstring> tag_names;
    std::vector<TagAdsRecord> tags;
};

class PlacesCatalog {
public:
    PlacesCatalog() = default;
    ~PlacesCatalog();
    std::vector<Workspace> workspaces;
    std::vector<ColorTag> tags;
    std::vector<NetworkPlace> networks;
    std::vector<std::wstring> starred;
    int active_workspace = -1;
    bool persist = true; // self-test can disable disk writes

    void EnsureDefaults();
    bool Load();
    bool Save() const;

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
    void CloneAssignments(const std::wstring& source, const std::wstring& destination);
    void RemoveAssignments(const std::wstring& path, bool include_descendants);

    bool IsStarred(const std::wstring& path) const;
    bool ToggleStarred(const std::wstring& path);

    void MergeAdsRecords(const std::wstring& path,
                         const std::vector<TagAdsRecord>& records,
                         const std::vector<std::wstring>& legacy_names = {});
    void ReadAdsIntoCatalog(const std::wstring& path);
    uint64_t TagRevision() const noexcept { return tag_revision_; }
    void TagsReordered();

private:
    void RebuildTagIndex();
    void RebuildStarIndex();
    void CommitTagChanges(const std::vector<std::wstring>& paths,
                          std::vector<TagAdsUpdate>* deferred_ads);
    void QueueTagSave() const;
    static bool SaveTagFile(const std::vector<ColorTag>& tags);
    void StopTagWriter();

    std::unordered_map<std::wstring, std::vector<int>> tag_index_;
    std::unordered_set<std::wstring> starred_index_;
    uint64_t tag_revision_ = 1;
    mutable std::mutex tag_save_mutex_;
    mutable std::condition_variable tag_save_cv_;
    mutable std::optional<std::vector<ColorTag>> pending_tag_save_;
    mutable std::thread tag_save_thread_;
    mutable bool tag_save_stop_ = false;
};

inline std::wstring MakeStarredPath() { return L"pulse:starred"; }
inline std::wstring MakeTagPath(const TagId& id) { return L"pulse:tag:" + id; }
inline std::wstring MakeTagPath(int i) { return L"pulse:tag:" + std::to_wstring(i); }
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
