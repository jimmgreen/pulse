#pragma once
#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace pulse::ops {
// Contains only admitted jobs, so storage is bounded by the pool capacity.
class TransferPoolProgress {
public:
    void Admit(size_t index) { bytes_.try_emplace(index, 0); }
    bool Update(size_t index, uint64_t bytes) {
        const auto found = bytes_.find(index);
        if (found == bytes_.end()) return false;
        total_ -= found->second;
        found->second = bytes;
        total_ += bytes;
        return true;
    }
    void Retire(size_t index) {
        const auto found = bytes_.find(index);
        if (found == bytes_.end()) return;
        total_ -= found->second;
        bytes_.erase(found);
    }
    uint64_t Total() const { return total_; }
private:
    std::unordered_map<size_t, uint64_t> bytes_;
    uint64_t total_ = 0;
};
}
