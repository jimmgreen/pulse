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

class SnapshotStore {
public:
    explicit SnapshotStore(size_t capacity = 32,
                           size_t byte_capacity = 48ull * 1024ull * 1024ull);

    // Get existing snapshot if fresh, else nullptr. Increments generation.
    SnapshotPtr GetOrStart(const std::wstring& path, uint64_t& out_generation);

    // Last snapshot even if dirty (for UNC cache-first display).
    SnapshotPtr Peek(const std::wstring& path) const;

    // Atomically update snapshot for path if generation is not older.
    void Update(const std::wstring& path, uint64_t generation, SnapshotPtr snapshot);

    // Mark dirty (e.g. from DirWatch).
    void MarkDirty(const std::wstring& path);

    size_t EntryCount() const;
    size_t ResidentBytes() const;

private:
    struct Entry {
        SnapshotPtr snapshot;
        uint64_t generation = 0;
        size_t bytes = 0;
        bool dirty = true;
        std::list<std::wstring>::iterator lru_it;
    };

    mutable std::mutex mutex_;
    size_t capacity_;
    size_t byte_capacity_;
    size_t resident_bytes_ = 0;
    uint64_t global_gen_ = 0;
    std::unordered_map<std::wstring, Entry> map_;
    std::list<std::wstring> lru_;

    static size_t SnapshotBytes(const SnapshotPtr& snapshot);
    void EvictToBudget(const std::wstring* protected_key = nullptr);
};

} // namespace pulse::fs
