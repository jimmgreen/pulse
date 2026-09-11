#pragma once
#include <cstdint>

namespace pulse::index {

// Brent's cycle detector bounds parent walks without allocating per USN record.
class ParentChainGuard {
public:
    explicit ParentChainGuard(int32_t count) : count_(count) {}
    bool Visit(int32_t node) {
        if (node < 0 || node >= count_ || node == checkpoint_) return false;
        if (distance_ == power_) {
            checkpoint_ = node;
            power_ *= 2;
            distance_ = 0;
        }
        ++distance_;
        return true;
    }
private:
    int32_t count_;
    int32_t checkpoint_ = -1;
    uint64_t power_ = 1;
    uint64_t distance_ = 1;
};

} // namespace pulse::index
