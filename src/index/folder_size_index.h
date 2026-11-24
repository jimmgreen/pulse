#pragma once
#include <algorithm>
#include <cstdint>
#include <limits>
#include <optional>
#include <unordered_map>
#include <vector>

namespace pulse::index {
// Retain totals for directories only. File metadata stays in the existing MFT
// index; an edit touches just the old and new ancestor chains.
// A build stores totals in two arrays sorted by id (12 bytes per directory
// instead of a hash node and bucket each); directories created afterwards
// live in a small map until the next build.
class FolderSizeIndex {
public:
    struct Item { int32_t parent = -1; uint64_t bytes = 0; bool directory = false, alive = false; };
    void Reset() {
        valid_ = false; live_ = 0;
        std::vector<int32_t>().swap(ids_);
        std::vector<uint64_t>().swap(totals_);
        extra_.clear();
    }
    bool Valid() const { return valid_; }
    uint64_t Builds() const { return builds_; }
    // Background builds fill a private copy that is published under the engine's
    // write lock; both outcomes count as one aggregation attempt.
    void Adopt(FolderSizeIndex&& built) {
        ids_ = std::move(built.ids_); totals_ = std::move(built.totals_); extra_ = std::move(built.extra_);
        live_ = built.live_; valid_ = built.valid_; ++builds_;
    }
    void NoteFailedBuild() { Reset(); ++builds_; }
    std::optional<uint64_t> Get(int32_t id) const {
        const uint64_t* total = valid_ ? Find(id) : nullptr;
        return total ? std::optional(*total) : std::nullopt;
    }
    template<class Read> bool Build(int32_t count, Read read) {
        Reset(); ++builds_;
        if (count < 0) return false;
        if (count == 0) { live_ = 0; valid_ = true; return true; }

        // Pass 1: validate parent invariants and collect only live directory IDs
        // (~10% of nodes), eliminating count-sized sums/children/queue vectors.
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive) continue;
            if (n.parent >= count || n.parent == i) { Reset(); return false; }
            if (n.parent >= 0) {
                const auto p = read(n.parent);
                if (p.alive && !p.directory) { Reset(); return false; }
            }
            if (n.directory) ids_.push_back(i);
        }

        const size_t dir_count = ids_.size();
        if (dir_count == 0) { live_ = 0; valid_ = true; return true; }

        constexpr int kBlockShift = 8;
        constexpr size_t kInvalidSlot = (std::numeric_limits<size_t>::max)();
        const size_t num_blocks = static_cast<size_t>((count - 1) >> kBlockShift) + 1;
        std::vector<uint32_t> block_start(num_blocks + 1, 0);
        {
            size_t cursor = 0;
            for (size_t b = 0; b < num_blocks; ++b) {
                block_start[b] = static_cast<uint32_t>(cursor);
                const int64_t limit = static_cast<int64_t>(b + 1) << kBlockShift;
                while (cursor < dir_count && static_cast<int64_t>(ids_[cursor]) < limit) ++cursor;
            }
            block_start[num_blocks] = static_cast<uint32_t>(cursor);
        }

        auto find_dir_slot = [&](int32_t id) noexcept -> size_t {
            if (id < 0 || id >= count) return kInvalidSlot;
            const size_t b = static_cast<size_t>(id >> kBlockShift);
            const auto begin = ids_.begin() + block_start[b];
            const auto end = ids_.begin() + block_start[b + 1];
            const auto it = std::lower_bound(begin, end, id);
            return (it != end && *it == id) ? static_cast<size_t>(it - ids_.begin()) : kInvalidSlot;
        };

        totals_.assign(dir_count, 0);
        std::vector<uint32_t> dir_children(dir_count, 0);

        // Pass 2: fold leaf file sizes straight into their live parent directory
        // and count directory-to-directory edges only.
        for (int32_t i = 0; i < count; ++i) {
            const auto n = read(i);
            if (!n.alive || n.parent < 0) continue;
            const size_t p_slot = find_dir_slot(n.parent);
            if (p_slot == kInvalidSlot) continue;
            if (n.directory) {
                ++dir_children[p_slot];
            } else {
                if (UINT64_MAX - totals_[p_slot] < n.bytes) { Reset(); return false; }
                totals_[p_slot] += n.bytes;
            }
        }

        // Pass 3: topological aggregation over directories only.
        std::vector<uint32_t> queue;
        queue.reserve(dir_count);
        for (size_t d = 0; d < dir_count; ++d) {
            if (!dir_children[d]) queue.push_back(static_cast<uint32_t>(d));
        }
        for (size_t at = 0; at < queue.size(); ++at) {
            const uint32_t d = queue[at];
            const auto n = read(ids_[d]);
            if (n.parent >= 0) {
                const size_t p_slot = find_dir_slot(n.parent);
                if (p_slot != kInvalidSlot) {
                    if (UINT64_MAX - totals_[p_slot] < totals_[d]) { Reset(); return false; }
                    totals_[p_slot] += totals_[d];
                    if (--dir_children[p_slot] == 0) queue.push_back(static_cast<uint32_t>(p_slot));
                }
            }
        }
        if (queue.size() != dir_count) { Reset(); return false; }
        for (uint64_t total : totals_) {
            if (total == kRemoved) { Reset(); return false; }
        }
        live_ = ids_.size();
        valid_ = true;
        return true;
    }
    template<class Read> void Replace(int32_t id, Item before, Item after, Read read) {
        if (!valid_) return;
        if (before.alive && after.alive && before.directory != after.directory) { Reset(); return; }
        const uint64_t old_size = before.directory ? Get(id).value_or(0) : before.bytes;
        const uint64_t new_size = after.directory ? (before.alive ? old_size : 0) : after.bytes;
        auto adjust = [&](int32_t parent, uint64_t bytes, bool add) {
            // Malformed chains must never hang the index service.
            for (size_t depth = 0; parent >= 0; ++depth) {
                if (depth > live_) return false;
                const auto n = read(parent);
                if (!n.alive) return true;
                uint64_t* total = Find(parent);
                if (!total) return false;
                // kRemoved marks an erased slot, so a total stays below it.
                if (add ? kRemoved - 1 - *total < bytes : *total < bytes) return false;
                if (add) *total += bytes; else *total -= bytes;
                parent = n.parent;
            }
            return true;
        };
        if ((before.alive && !adjust(before.parent, old_size, false)) ||
            (after.alive && !adjust(after.parent, new_size, true))) { Reset(); return; }
        if (after.alive && after.directory) Set(id, new_size);
        else Erase(id);
    }
private:
    static constexpr uint64_t kRemoved = UINT64_MAX;
    uint64_t* Slot(int32_t id) {
        const auto it = std::lower_bound(ids_.begin(), ids_.end(), id);
        return it != ids_.end() && *it == id ? &totals_[static_cast<size_t>(it - ids_.begin())] : nullptr;
    }
    uint64_t* Find(int32_t id) {
        if (uint64_t* slot = Slot(id)) return *slot == kRemoved ? nullptr : slot;
        const auto it = extra_.find(id);
        return it == extra_.end() ? nullptr : &it->second;
    }
    const uint64_t* Find(int32_t id) const { return const_cast<FolderSizeIndex*>(this)->Find(id); }
    void Set(int32_t id, uint64_t total) {
        if (uint64_t* slot = Slot(id)) {
            if (*slot == kRemoved) ++live_;
            *slot = total;
        } else if (extra_.insert_or_assign(id, total).second) {
            ++live_;
        }
    }
    void Erase(int32_t id) {
        if (uint64_t* slot = Slot(id)) {
            if (*slot != kRemoved) { *slot = kRemoved; --live_; }
        } else if (extra_.erase(id)) {
            --live_;
        }
    }
    bool valid_ = false;
    uint64_t builds_ = 0;
    size_t live_ = 0;
    std::vector<int32_t> ids_;
    std::vector<uint64_t> totals_;
    std::unordered_map<int32_t, uint64_t> extra_;
};
}
