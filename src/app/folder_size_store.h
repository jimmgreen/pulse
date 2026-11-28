#pragma once
#include "folder_sizes.h"
#include <list>
#include <map>
#include <set>
#include <utility>
#include <vector>

namespace pulse::app::folder_size {
inline constexpr size_t kCacheLimit = 4096;
std::wstring Key(std::wstring path);
bool Within(const std::wstring& path, const std::wstring& parent);
uint64_t NowUtcMs();
using SavedValues = std::vector<std::pair<std::wstring, FolderSizeValue>>;
SavedValues ReadValues(const std::wstring& file);
bool WriteValues(const std::wstring& file, const SavedValues& values);

// The coordinator holds its mutex while accessing this bounded memory store.
class Store {
public:
    struct Entry {
        FolderSizeValue value; // Published result only; never an in-flight subtotal.
        FolderSizeWork work;
        bool auto_deferred = false;
        uint64_t next_index_at = 0;
        // The service's latest answer for this path was an available total.
        // Cleared by unavailable answers (journal gap, totals building) and
        // failed connections; never persisted.
        bool index_current = false;
        uint64_t completed = 0, not_before = 0;
        uint64_t revision = 0, request_epoch = 0, watch_generation = 0;
        std::list<std::wstring>::iterator position;
    };
    Entry* Ensure(const std::wstring& key, const std::set<std::wstring>& protected_paths);
    void Touch(Entry& entry);
    // `restart` also supersedes a walk in progress; otherwise that walk keeps
    // going and its result is published as a snapshot of the older revision.
    void MarkStale(Entry& entry, uint64_t now, uint64_t delay, bool restart = false);
    // `known_only` leaves idle entries without any result untouched: a newly
    // armed watch has nothing to reconcile for them, and marking them stale
    // would discard an index answer already on its way.
    // `restart` supersedes ancestor walks in progress (an explicit request for a
    // folder below them must not wait for them to finish).
    bool InvalidateAncestors(const std::wstring& path, uint64_t now, uint64_t delay, bool known_only = false,
                             bool restart = false);
    bool Invalidate(const std::wstring& path, uint64_t now, uint64_t delay, bool known_only = false);
    SavedValues Snapshot() const;
    std::map<std::wstring, Entry> entries;
private:
    std::list<std::wstring> lru_;
    uint64_t next_revision_ = 0;
};
} // namespace pulse::app::folder_size