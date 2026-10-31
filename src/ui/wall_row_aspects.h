#pragma once
#include <cstdint>
#include <utility>
#include <vector>

namespace pulse::ui {
// One projection per snapshot/order/layout cache entry. A source revision is
// unique even when a source is evicted and later recreated at the same store version.
class WallRowAspects {
public:
    template<class Build>
    bool Refresh(uint64_t revision, Build&& build) {
        if (ready_ && revision_ == revision) return false;
        auto next = build();
        const bool changed = !ready_ || values_ != next;
        if (changed) values_ = std::move(next);
        revision_ = revision;
        ready_ = true;
        return changed;
    }
    const std::vector<float>& Values() const noexcept { return values_; }
private:
    uint64_t revision_ = 0;
    bool ready_ = false;
    std::vector<float> values_;
};
} // namespace pulse::ui
