#pragma once
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <utility>

namespace pulse::preview {
// Estimates cover task-owned codec buffers, output and IPC copies, not total
// process commit. The owner process holds leases until mapping ACK or host exit.
class DecodeMemoryBudget {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        Lease(Lease&& other) noexcept { *this = std::move(other); }
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) { Reset(); owner_ = std::exchange(other.owner_, nullptr);
                bytes_ = std::exchange(other.bytes_, 0); unknown_ = std::exchange(other.unknown_, false); }
            return *this;
        }
        ~Lease() { Reset(); }
        void Reset() {
            if (!owner_) return;
            std::lock_guard lock(owner_->mutex_);
            owner_->used_ -= bytes_;
            if (unknown_) --owner_->unknown_;
            owner_ = nullptr; bytes_ = 0; unknown_ = false;
        }
        uint64_t Bytes() const { return bytes_; }
    private:
        friend class DecodeMemoryBudget;
        DecodeMemoryBudget* owner_ = nullptr;
        uint64_t bytes_ = 0;
        bool unknown_ = false;
    };
    explicit DecodeMemoryBudget(uint64_t limit) : limit_(limit) {}
    bool Reserve(Lease& lease, uint64_t bytes, bool unknown) {
        std::lock_guard lock(mutex_);
        if (lease.owner_ && lease.owner_ != this) return false;
        bytes = (std::max)(bytes, lease.bytes_);
        unknown = unknown || lease.unknown_;
        const uint64_t extra = bytes - lease.bytes_;
        if (extra > limit_ - used_ || (unknown && !lease.unknown_ && unknown_)) return false;
        used_ += extra;
        if (unknown && !lease.unknown_) ++unknown_;
        lease.owner_ = this; lease.bytes_ = bytes; lease.unknown_ = unknown;
        return true;
    }
    uint64_t Used() { std::lock_guard lock(mutex_); return used_; }
    uint64_t Limit() const { return limit_; }
private:
    std::mutex mutex_;
    uint64_t limit_, used_ = 0;
    unsigned unknown_ = 0;
};
inline DecodeMemoryBudget& SharedDecodeMemoryBudget() {
    static DecodeMemoryBudget budget([] {
        wchar_t value[32]{};
        const auto n = GetEnvironmentVariableW(L"PULSE_GRID_DECODE_BUDGET_MIB", value, 32);
        wchar_t* end = nullptr;
        const auto mib = n && n < 32 ? wcstoul(value, &end, 10) : 0;
        return uint64_t(mib >= 32 && mib <= 4096 && end && !*end ? mib : 1024) << 20;
    }());
    return budget;
}
}
