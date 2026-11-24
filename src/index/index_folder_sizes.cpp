#include "index_engine.h"
#include <algorithm>
#include <malloc.h>

namespace pulse::index {
FolderSizeIndex::Item Engine::FolderSizeItem(int32_t id) const {
    if (id < 0 || id >= LiveCount() || IsTomb(id)) return {};
    const auto n = NodeAt(id);
    return {n.parent, AttrAt(id).size, (n.flags & kFlagDir) != 0, true};
}
bool Engine::FolderSizeIdsLocked(const std::vector<std::wstring>& paths, std::vector<int32_t>& ids) const {
    bool covered = false;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto path = NormalizeChangePath(paths[i]);
        const auto id = ResolvePathLocked(path);
        if (id < 0 || IsTomb(id) || !(NodeAt(id).flags & kFlagDir) ||
            (NodeAt(id).flags & kFlagHidden) || IsExcludedPath(path)) continue;
        int32_t root = id;
        int32_t remaining = LiveCount();
        while (root >= 0 && root < LiveCount() && remaining-- > 0 && NodeAt(root).parent >= 0) root = NodeAt(root).parent;
        if (root < 0 || root >= LiveCount() || remaining <= 0 || std::find(inactive_volume_roots_.begin(), inactive_volume_roots_.end(), root) != inactive_volume_roots_.end()) continue;
        for (const auto& volume : vols_) if (volume.root_idx == root && volume.journal_id && volume.folder_size_current) {
            ids[i] = id; covered = true; break;
        }
    }
    return covered;
}
bool Engine::BuildFolderTotals() {
    // One aggregation at a time; other callers answer "unknown" instead of waiting.
    std::unique_lock build(folder_size_build_mutex_, std::try_to_lock);
    if (!build.owns_lock()) return false;
    FolderSizeIndex built;
    uint64_t mutations = 0;
    int32_t nodes = 0;
    bool ok = false;
    const auto started = GetTickCount64();
    {
        // Searches share this lock; only metadata writers wait for the aggregation.
        std::shared_lock lock(mutex_);
        if (!ready_ || building_ || folder_size_gap_) return false;
        if (folder_sizes_.Valid()) return true;
        if (GetTickCount64() < folder_size_retry_after_) return false;
        mutations = folder_size_mutations_;
        nodes = LiveCount();
        ok = built.Build(nodes, [this](int32_t id) { return FolderSizeItem(id); });
    }
    std::unique_lock lock(mutex_);
    if (!ok) {
        folder_sizes_.NoteFailedBuild();
        diagnostics::runtime::Event("index_folder_totals_failed", {{"nodes", static_cast<uint64_t>(nodes)},
            {"elapsed_ms", GetTickCount64() - started}, {"retry_ms", 30000}});
        // Incomplete/cyclic metadata must stay unknown, without rescanning
        // millions of nodes on every client poll. Snapshot replacement
        // resets this delay; ordinary journal edits retry within 30 seconds.
        folder_size_retry_after_ = GetTickCount64() + 30000;
        return false;
    }
    // An edit published between the two locks was not applied to this copy.
    if (mutations != folder_size_mutations_ || folder_sizes_.Valid()) {
        const bool already_valid = folder_sizes_.Valid();
        lock.unlock();
        _heapmin();
        return already_valid;
    }
    folder_sizes_.Adopt(std::move(built));
    diagnostics::runtime::Event("index_folder_totals_ready", {{"nodes", static_cast<uint64_t>(nodes)},
        {"elapsed_ms", GetTickCount64() - started}});
    folder_size_retry_after_ = 0;
    lock.unlock();
    _heapmin();
    return true;
}
std::vector<IndexedFolderSize> Engine::FolderSizes(const std::vector<std::wstring>& paths) {
    if (paths.size() > kFolderSizeBatch) return {};
    std::vector<IndexedFolderSize> result(paths.size());
    for (int attempt = 0; attempt < 2; ++attempt) {
        {
            std::shared_lock lock(mutex_);
            if (!ready_ || building_ || folder_size_gap_) return result;
            std::vector<int32_t> ids(paths.size(), -1);
            if (!FolderSizeIdsLocked(paths, ids)) return result;
            if (folder_sizes_.Valid()) {
                for (size_t i = 0; i < ids.size(); ++i) if (const auto bytes = folder_sizes_.Get(ids[i])) result[i] = {true, *bytes};
                return result;
            }
        }
        if (attempt) return result;
        if (folder_size_background_) {
            // Answer at once; the worker aggregates and the client polls again.
            folder_size_wanted_ = true;
            if (change_signal_) SetEvent(change_signal_);
            return result;
        }
        if (!BuildFolderTotals()) return result;
    }
    return result;
}
}
