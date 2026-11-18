// fs_snapshot.h — Path -> read-only snapshot cache with LRU and generation tracking.
#pragma once
#include "fs_enum.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pulse::fs {

using SnapshotPtr = std::shared_ptr<const std::vector<DirEntry>>;

// Stable identity of a directory object. A path can point to a different
// object after an uninstall/reinstall or a rename/recreate cycle.
struct DirectoryIdentity {
    DWORD volume_serial = 0;
    DWORD file_index_high = 0;
    DWORD file_index_low = 0;

    bool valid() const noexcept {
        return file_index_high != 0 || file_index_low != 0;
    }
};

bool QueryDirectoryIdentity(const std::wstring& path, DirectoryIdentity& out);
bool SameDirectoryIdentity(const DirectoryIdentity& a,
                           const DirectoryIdentity& b) noexcept;

class SnapshotStore {
public:
    // Keep navigation snapshots bounded. Cold folders are re-enumerated when
    // revisited instead of pinning a large portion of the working set.
    explicit SnapshotStore(size_t capacity = 16,
                           size_t byte_capacity = 24ull * 1024ull * 1024ull);

    // Get existing snapshot if fresh, else nullptr. Does not bump generation;
    // WorkerPool::Refresh owns the generation that later Update() compares.
    SnapshotPtr GetOrStart(const std::wstring& path, uint64_t& out_generation,
                           DirectoryIdentity* out_identity = nullptr);

    DirectoryIdentity Identity(const std::wstring& path) const;
    bool IsDirty(const std::wstring& path) const;

    // Last snapshot even if dirty (for UNC cache-first display).
    SnapshotPtr Peek(const std::wstring& path) const;

    // Atomically update snapshot for path if generation is not older.
    // listed_at: WatchClock when its listing began (0: unknown).
    void Update(const std::wstring& path, uint64_t generation, SnapshotPtr snapshot,
                DirectoryIdentity identity = {}, uint64_t listed_at = 0);

    // Always-write a live listing (incremental watch apply). Keeps the last
    // worker generation so a later Refresh Update() is not discarded.
    uint64_t Put(const std::wstring& path, SnapshotPtr snapshot);

    // Mark dirty (e.g. from DirWatch).
    void MarkDirty(const std::wstring& path);
    // A fresh enumeration of path is being queued, as GetOrStart does when it
    // starts one: earlier dirty marks are covered by it, and only changes
    // marked after this call ask for another pass when its result arrives.
    void BeginRefresh(const std::wstring& path);

    // A newly armed watch (fs::ArmedAt). ListedSince: the clean snapshot was
    // listed after `armed_at`, so it misses nothing. NoteArm: a listing still
    // runs; TakeArmMissed, when it lands, tells whether it began too early.
    bool ListedSince(const std::wstring& path, uint64_t armed_at) const;
    void NoteArm(const std::wstring& path, uint64_t armed_at);
    bool TakeArmMissed(const std::wstring& path, uint64_t listed_at);

    size_t EntryCount() const;
    // Snapshot payload allocations, excluding allocator bookkeeping and map nodes.
    size_t ResidentBytes() const;

private:
    struct Entry {
        SnapshotPtr snapshot;
        DirectoryIdentity identity;
        uint64_t generation = 0;
        size_t bytes = 0;
        bool dirty = true;
        uint64_t listed_at = 0;    // WatchClock when the snapshot's listing began
        uint64_t arm_pending = 0;  // NoteArm awaiting a listing
        std::list<std::wstring>::iterator lru_it;
    };

    mutable std::mutex mutex_;
    size_t capacity_;
    size_t byte_capacity_;
    size_t resident_bytes_ = 0;
    std::unordered_map<std::wstring, Entry> map_;
    std::list<std::wstring> lru_;
    // Changed folders with no entry: never listed here, or evicted while a
    // change was pending (#58-12). A big folder can outgrow the byte budget;
    // dropping its listing must not drop the mark that asks a listing in
    // flight to run once more, or the files copied meanwhile never show.
    std::unordered_set<std::wstring> stale_;
    static constexpr size_t kStaleLimit = 1024;

    size_t SnapshotBytes(const std::wstring& key, const SnapshotPtr& snapshot) const;
    void EvictToBudget(const std::wstring* protected_key = nullptr);
    // Removes one entry (map and LRU); a dirty one leaves its mark in stale_.
    void Evict(std::unordered_map<std::wstring, Entry>::iterator it);
    void EvictOldest();
    void NoteStale(const std::wstring& key);
};

} // namespace pulse::fs