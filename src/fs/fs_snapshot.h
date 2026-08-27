// fs_snapshot.h — Path -> read-only snapshot cache with LRU and generation tracking.
#pragma once
#include "fs_enum.h"
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
    void Update(const std::wstring& path, uint64_t generation, SnapshotPtr snapshot,
                DirectoryIdentity identity = {});

    // Always-write a live listing (incremental watch apply). Keeps the last
    // worker generation so a later Refresh Update() is not discarded.
    uint64_t Put(const std::wstring& path, SnapshotPtr snapshot);

    // Mark dirty (e.g. from DirWatch).
    void MarkDirty(const std::wstring& path);

    size_t EntryCount() const;
    size_t ResidentBytes() const;

private:
    struct Entry {
        SnapshotPtr snapshot;
        DirectoryIdentity identity;
        uint64_t generation = 0;
        size_t bytes = 0;
        bool dirty = true;
        std::list<std::wstring>::iterator lru_it;
    };

    mutable std::mutex mutex_;
    size_t capacity_;
    size_t byte_capacity_;
    size_t resident_bytes_ = 0;
    std::unordered_map<std::wstring, Entry> map_;
    std::list<std::wstring> lru_;

    static size_t SnapshotBytes(const SnapshotPtr& snapshot);
    void EvictToBudget(const std::wstring* protected_key = nullptr);
};

} // namespace pulse::fs
