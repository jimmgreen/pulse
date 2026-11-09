#pragma once
#include <algorithm>
#include <cstdint>
#include <unordered_set>
#include <vector>

namespace pulse::app {
struct MarqueeContext {
    const void* pane = nullptr;
    const void* tab = nullptr;
    const void* snapshot = nullptr;
    const void* tree = nullptr;
    uint64_t generation = 0;
    bool operator==(const MarqueeContext&) const = default;
};

// Keep small gesture scratch for reuse; never retain an unbounded directory-
// sized allocation after release/Esc. Limits are capacities, not last sizes.
inline constexpr size_t kMarqueeRetainedHitCapacity = 1024;
inline constexpr size_t kMarqueeRetainedBucketCount = 2048;
inline void ClearMarqueeScratch(std::vector<int>& values) {
    if (values.capacity() > kMarqueeRetainedHitCapacity) std::vector<int>{}.swap(values);
    else values.clear();
}
inline void ClearMarqueeScratch(std::unordered_set<int>& values) {
    if (values.bucket_count() > kMarqueeRetainedBucketCount) std::unordered_set<int>{}.swap(values);
    else values.clear();
}

// The base is immutable for one gesture. Only the symmetric difference of
// successive hit lists can change its result. Keep the expanded all-selection
// off the tab so its public compact representation remains unchanged.
class MarqueeSelection {
public:
    void Reset() {
        ClearMarqueeScratch(previous_);
        ClearMarqueeScratch(expanded_);
        initialized_ = false;
    }

    size_t RetainedHitCapacity() const { return previous_.capacity(); }
    size_t RetainedBucketCount() const { return expanded_.bucket_count(); }

    bool Apply(std::unordered_set<int>& selected, bool& all_selected,
        const std::unordered_set<int>& base, bool additive, bool toggle,
        int count, std::vector<int>& hits, bool compact_all = true) {
        std::sort(hits.begin(), hits.end());
        hits.erase(std::unique(hits.begin(), hits.end()), hits.end());
        bool changed = false;
        if (!initialized_) {
            std::unordered_set<int> result;
            if (additive) result = base;
            for (int index : hits) {
                if (toggle && base.contains(index)) result.erase(index);
                else result.insert(index);
            }
            changed = all_selected ? result.size() != static_cast<size_t>(count) : selected != result;
            if (changed || all_selected) selected.swap(result);
            all_selected = false;
            initialized_ = true;
            previous_.assign(hits.begin(), hits.end());
        } else if (all_selected) {
            selected.swap(expanded_);
        }
        all_selected = false;
        auto assign = [&](int index, bool hit) {
            const bool in_base = additive && base.contains(index);
            const bool wanted = toggle ? in_base != hit : in_base || hit;
            if (wanted) changed = selected.insert(index).second || changed;
            else changed = (selected.erase(index) != 0) || changed;
        };
        size_t old = 0, next = 0;
        while (old < previous_.size() || next < hits.size()) {
            if (next == hits.size() || (old < previous_.size() && previous_[old] < hits[next]))
                assign(previous_[old++], false);
            else if (old == previous_.size() || hits[next] < previous_[old])
                assign(hits[next++], true);
            else { ++old; ++next; }
        }
        previous_.assign(hits.begin(), hits.end());
        if (compact_all && count > 0 && selected.size() == static_cast<size_t>(count)) {
            all_selected = true;
            selected.swap(expanded_);
        }
        return changed;
    }
private:
    std::vector<int> previous_;
    std::unordered_set<int> expanded_;
    bool initialized_ = false;
};
} // namespace pulse::app
