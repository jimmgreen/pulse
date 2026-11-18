#pragma once
#include <map>
#include <mutex>
#include <string>
#include <utility>

namespace pulse::ui {
// Admission never waits for a provider. A lease covers teardown as well as open,
// so a blocked retired worker continues to consume its process-wide slot.
class PreviewResourceBudget {
public:
    class Lease {
    public:
        Lease() = default;
        Lease(Lease&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)), key_(std::move(other.key_)) {}
        Lease& operator=(Lease&& other) noexcept {
            if (this != &other) { Reset(); owner_ = std::exchange(other.owner_, nullptr); key_ = std::move(other.key_); }
            return *this;
        }
        ~Lease() { Reset(); }
        explicit operator bool() const { return owner_ != nullptr; }
        void Reset() {
            if (!owner_) return;
            std::lock_guard lock(owner_->mutex_);
            --owner_->active_;
            if (!key_.empty()) {
                auto it = owner_->keys_.find(key_);
                if (--it->second == 0) owner_->keys_.erase(it);
            }
            owner_ = nullptr;
        }
    private:
        friend class PreviewResourceBudget;
        PreviewResourceBudget* owner_ = nullptr;
        std::wstring key_;
    };
    PreviewResourceBudget(unsigned limit, unsigned per_key = 0) : limit_(limit), per_key_(per_key) {}
    Lease TryAcquire(std::wstring key = {}) {
        Lease lease;
        std::lock_guard lock(mutex_);
        if (active_ >= limit_) return lease;
        if (!key.empty()) {
            auto it = keys_.find(key);
            if (it != keys_.end() && it->second >= per_key_) return lease;
            ++keys_[key];
        }
        ++active_;
        lease.owner_ = this;
        lease.key_ = std::move(key);
        return lease;
    }
private:
    std::mutex mutex_;
    std::map<std::wstring, unsigned> keys_;
    unsigned active_ = 0;
    const unsigned limit_, per_key_;
};

// Detached provider threads can outlive static destruction at process shutdown.
inline PreviewResourceBudget& PreviewApartmentBudget() {
    static auto* budget = new PreviewResourceBudget(8);
    return *budget;
}
inline PreviewResourceBudget& PreviewProviderBudget() {
    static auto* budget = new PreviewResourceBudget(8, 2);
    return *budget;
}
inline PreviewResourceBudget& VideoWorkerBudget() {
    static auto* budget = new PreviewResourceBudget(4);
    return *budget;
}
} // namespace pulse::ui
