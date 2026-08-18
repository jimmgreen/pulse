// fs_snapshot.cpp
#include "fs_snapshot.h"
#include "fs_enum.h"

namespace pulse::fs {

SnapshotStore::SnapshotStore(size_t capacity, size_t byte_capacity)
    : capacity_((std::max<size_t>)(1, capacity)), byte_capacity_(byte_capacity) {
}

size_t SnapshotStore::SnapshotBytes(const SnapshotPtr& snapshot) {
    if (!snapshot) return 0;
    size_t bytes = sizeof(std::vector<DirEntry>) + snapshot->capacity() * sizeof(DirEntry);
    for (const auto& entry : *snapshot) {
        bytes += entry.name.capacity() * sizeof(wchar_t);
        bytes += entry.full_path.capacity() * sizeof(wchar_t);
        bytes += entry.link_target.capacity() * sizeof(wchar_t);
    }
    return bytes;
}

void SnapshotStore::EvictToBudget(const std::wstring* protected_key) {
    while (byte_capacity_ && resident_bytes_ > byte_capacity_ && !lru_.empty()) {
        auto oldest = std::prev(lru_.end());
        if (protected_key && *oldest == *protected_key) {
            if (oldest == lru_.begin()) break;
            --oldest;
        }
        auto it = map_.find(*oldest);
        if (it != map_.end()) {
            resident_bytes_ -= (std::min)(resident_bytes_, it->second.bytes);
            map_.erase(it);
        }
        lru_.erase(oldest);
    }
}

SnapshotPtr SnapshotStore::GetOrStart(const std::wstring& path, uint64_t& out_generation) {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) {
        // Move to front of LRU.
        lru_.erase(it->second.lru_it);
        lru_.push_front(key);
        it->second.lru_it = lru_.begin();
        if (!it->second.dirty && it->second.snapshot) {
            out_generation = it->second.generation;
            return it->second.snapshot;
        }
        // Need refresh: bump generation.
        it->second.generation = ++global_gen_;
        it->second.dirty = false;
        out_generation = it->second.generation;
        return nullptr;
    }

    // Evict if needed.
    while (map_.size() >= capacity_ && !lru_.empty()) {
        const std::wstring& oldest = lru_.back();
        auto old = map_.find(oldest);
        if (old != map_.end())
            resident_bytes_ -= (std::min)(resident_bytes_, old->second.bytes);
        map_.erase(oldest);
        lru_.pop_back();
    }

    uint64_t gen = ++global_gen_;
    Entry e;
    e.generation = gen;
    e.dirty = false;
    lru_.push_front(key);
    e.lru_it = lru_.begin();
    map_[key] = std::move(e);
    out_generation = gen;
    return nullptr;
}

SnapshotPtr SnapshotStore::Peek(const std::wstring& path) const {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) return nullptr;
    return it->second.snapshot;
}

void SnapshotStore::Update(const std::wstring& path, uint64_t generation, SnapshotPtr snapshot) {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it == map_.end()) {
        while (map_.size() >= capacity_ && !lru_.empty()) {
            auto old = map_.find(lru_.back());
            if (old != map_.end())
                resident_bytes_ -= (std::min)(resident_bytes_, old->second.bytes);
            map_.erase(lru_.back());
            lru_.pop_back();
        }
        Entry e;
        e.generation = generation;
        e.dirty = false;
        e.snapshot = std::move(snapshot);
        e.bytes = SnapshotBytes(e.snapshot);
        resident_bytes_ += e.bytes;
        lru_.push_front(key);
        e.lru_it = lru_.begin();
        map_[key] = std::move(e);
        EvictToBudget(&key);
        return;
    }
    if (generation < it->second.generation && it->second.snapshot) return;
    resident_bytes_ -= (std::min)(resident_bytes_, it->second.bytes);
    it->second.snapshot = std::move(snapshot);
    it->second.bytes = SnapshotBytes(it->second.snapshot);
    resident_bytes_ += it->second.bytes;
    it->second.generation = generation;
    it->second.dirty = false;
    lru_.erase(it->second.lru_it);
    lru_.push_front(key);
    it->second.lru_it = lru_.begin();
    EvictToBudget(&key);
}

void SnapshotStore::MarkDirty(const std::wstring& path) {
    std::wstring key = NormalizePath(path);
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = map_.find(key);
    if (it != map_.end()) it->second.dirty = true;
}

size_t SnapshotStore::EntryCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return map_.size();
}

size_t SnapshotStore::ResidentBytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return resident_bytes_;
}

} // namespace pulse::fs
