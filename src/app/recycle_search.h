#pragma once
#include "../fs/fs_recycle.h"
#include "../index/index_query.h"
#include "../index/index_types.h"
#include <functional>

namespace pulse::app {

// Produces unpaged candidates. The caller merges and sorts all providers before
// applying the page limit; the original path is not the payload's identity.
inline index::SearchResult SearchRecycleEntries(const index::Query& query,
    const std::vector<fs::DirEntry>& entries,
    const std::function<bool()>& cancelled = {}) {
    index::SearchResult result;
    if (cancelled && cancelled()) {
        result.error = ERROR_CANCELLED;
        return result;
    }
    auto compiled = index::ParseQuery(query.needle);
    if (compiled.content.present()) return result;
    const auto& scope = query.path_prefix.empty() ? compiled.path_prefix : query.path_prefix;
    if (!scope.empty() && !fs::IsRecycleViewPath(scope)) return result;
    for (const auto& entry : entries) {
        if (cancelled && cancelled()) {
            result = {};
            result.error = ERROR_CANCELLED;
            return result;
        }
        if (entry.recycle_path.empty() || (query.folders_only && !entry.is_dir)) continue;
        const uint64_t modified = (static_cast<uint64_t>(entry.mtime.dwHighDateTime) << 32) |
            entry.mtime.dwLowDateTime;
        bool matched = compiled.groups.empty();
        for (const auto& group : compiled.groups) {
            bool all = true;
            for (const auto& term : group) {
                if (!index::MatchTerm(entry.full_path, entry.name, entry.is_dir,
                    entry.size, modified, term)) { all = false; break; }
            }
            if (all) { matched = true; break; }
        }
        if (!matched) continue;
        index::Hit hit;
        hit.path = entry.full_path;
        hit.name = entry.name;
        hit.is_dir = entry.is_dir;
        hit.size = entry.size;
        hit.mtime = modified;
        hit.recycle_path = entry.recycle_path;
        result.hits.push_back(std::move(hit));
    }
    result.total = result.hits.size();
    return result;
}

// Must run on a worker: EnumerateRecycleBin reads only the current user's SID.
inline index::SearchResult SearchRecycleBin(const index::Query& query,
    const std::function<bool()>& cancelled = {}) {
    if (cancelled && cancelled()) {
        index::SearchResult result;
        result.error = ERROR_CANCELLED;
        return result;
    }
    const auto compiled = index::ParseQuery(query.needle);
    const auto& scope = query.path_prefix.empty() ? compiled.path_prefix : query.path_prefix;
    if (compiled.content.present() || (!scope.empty() && !fs::IsRecycleViewPath(scope))) return {};
    std::vector<fs::DirEntry> entries;
    fs::RecycleBinInfo info;
    try {
        fs::EnumerateRecycleBin(entries, &info, cancelled);
    } catch (...) {
        index::SearchResult result;
        result.error = cancelled && cancelled() ? ERROR_CANCELLED : ERROR_READ_FAULT;
        return result;
    }
    auto result = SearchRecycleEntries(query, entries, cancelled);
    if (!info.valid && !result.error) result.error = ERROR_READ_FAULT;
    return result;
}
} // namespace pulse::app
