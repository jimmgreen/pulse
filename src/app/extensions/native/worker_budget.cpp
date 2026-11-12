#include "worker_budget.h"

#include <atomic>
#include <utility>

namespace pulse::extensions::native {

struct WorkerPermitPool::WaiterSlot {
    std::atomic<bool> active{true};
    std::function<void()> callback;

    explicit WaiterSlot(std::function<void()> fn) noexcept : callback(std::move(fn)) {}
};

void WorkerPermit::Release() noexcept {
    if (WorkerPermitPool* pool = std::exchange(pool_, nullptr)) {
        pool->ReleaseOne();
    }
}

WorkerPermitPool::WorkerPermitPool(size_t initial_limit) noexcept
    : limit_(initial_limit == 0 ? 1 : initial_limit) {}

WorkerPermit WorkerPermitPool::TryAcquire() noexcept {
    std::lock_guard lock(mutex_);
    if (active_ >= limit_) {
        return {};
    }
    ++active_;
    return WorkerPermit(this);
}

uint64_t WorkerPermitPool::RegisterWaiter(std::function<void()> callback) {
    if (!callback) {
        return 0;
    }
    auto slot = std::make_shared<WaiterSlot>(std::move(callback));
    std::lock_guard lock(mutex_);
    const uint64_t id = ++next_waiter_id_;
    waiters_.push_back(WaiterEntry{id, std::move(slot)});
    return id;
}

void WorkerPermitPool::UnregisterWaiter(uint64_t id) noexcept {
    if (id == 0) {
        return;
    }
    std::shared_ptr<WaiterSlot> removed;
    {
        std::lock_guard lock(mutex_);
        for (auto it = waiters_.begin(); it != waiters_.end(); ++it) {
            if (it->id == id) {
                if (it->slot) {
                    it->slot->active.store(false, std::memory_order_release);
                    removed = std::move(it->slot);
                }
                waiters_.erase(it);
                break;
            }
        }
    }
    // Destroy the captured functor outside mutex_ so its destructor can safely
    // re-enter pool inspection or other synchronization primitives.
    removed.reset();
}

size_t WorkerPermitPool::ActiveCount() const noexcept {
    std::lock_guard lock(mutex_);
    return active_;
}

size_t WorkerPermitPool::Limit() const noexcept {
    std::lock_guard lock(mutex_);
    return limit_;
}

void WorkerPermitPool::SetLimitForTesting(size_t limit) noexcept {
    const size_t clamped = limit == 0 ? 1 : limit;
    std::vector<std::shared_ptr<WaiterSlot>> snapshot;
    try {
        std::lock_guard lock(mutex_);
        const size_t previous = limit_;
        limit_ = clamped;
        if (clamped > previous && active_ < limit_ && !waiters_.empty()) {
            snapshot.reserve(waiters_.size());
            for (const auto& entry : waiters_) {
                if (entry.slot) {
                    snapshot.push_back(entry.slot);
                }
            }
        }
    } catch (...) {
        return;
    }
    NotifySnapshot(snapshot);
}

void WorkerPermitPool::ReleaseOne() noexcept {
    std::vector<std::shared_ptr<WaiterSlot>> snapshot;
    try {
        std::lock_guard lock(mutex_);
        if (active_ > 0) {
            --active_;
        }
        if (active_ < limit_ && !waiters_.empty()) {
            snapshot.reserve(waiters_.size());
            for (const auto& entry : waiters_) {
                if (entry.slot) {
                    snapshot.push_back(entry.slot);
                }
            }
        }
    } catch (...) {
        return;
    }
    NotifySnapshot(snapshot);
}

void WorkerPermitPool::NotifySnapshot(const std::vector<std::shared_ptr<WaiterSlot>>& snapshot) noexcept {
    for (const auto& slot : snapshot) {
        if (!slot || !slot->active.load(std::memory_order_acquire)) {
            continue;
        }
        try {
            if (slot->callback) {
                slot->callback();
            }
        } catch (...) {}
    }
}

WorkerPermitPool& GlobalTaskWorkerPool() noexcept {
    // Heap-resident singleton: detached worker threads returning permits during
    // process teardown must never race CRT static destructor order.
    static WorkerPermitPool* const pool = new WorkerPermitPool(kDefaultGlobalTaskWorkers);
    return *pool;
}

WorkerPermitPool& GlobalPreviewWorkerPool() noexcept {
    static WorkerPermitPool* const pool = new WorkerPermitPool(kDefaultGlobalPreviewWorkers);
    return *pool;
}

} // namespace pulse::extensions::native
