#pragma once

#include "../common/path_utils.h"
#include "../fs/fs_snapshot.h"
#include <algorithm>
#include <unordered_map>

namespace pulse::app {

// Unknown (no applicable listing) is not missing; release validates the
// destination asynchronously. A cached file of the same name isn't a folder.
inline bool TrayFolderKnownMissing(const fs::SnapshotPtr& parent, const std::wstring& name) {
    if (!parent || name.empty()) return false;
    return std::none_of(parent->begin(), parent->end(), [&](const auto& entry) {
        return entry.is_dir && path::EqualInsensitive(entry.name, name);
    });
}

inline std::vector<uint64_t> TraySnapshotConflicts(const fs::SnapshotPtr& snapshot,
    std::unordered_multimap<std::wstring, uint64_t> candidates) {
    std::vector<uint64_t> found;
    if (!snapshot || candidates.empty()) return found;
    for (const auto& entry : *snapshot) {
        if (entry.change_record_only) continue;
        const auto [first, last] = candidates.equal_range(path::FoldedKey(entry.name));
        for (auto it = first; it != last; ++it) found.push_back(it->second);
        candidates.erase(first, last);
        if (candidates.empty()) break;
    }
    return found;
}

} // namespace pulse::app
