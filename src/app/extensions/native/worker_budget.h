#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pulse::extensions::native {

inline constexpr size_t kDefaultGlobalTaskWorkers = 8;
inline constexpr size_t kDefaultGlobalPreviewWorkers = 4;

class WorkerPermitPool;

class WorkerPermit final {
public:
    WorkerPermit() noexcept = default;
    WorkerPermit(const WorkerPermit&) = delete;
    WorkerPermit& operator=(const WorkerPermit&) = delete;

    WorkerPermit(WorkerPermit&& other) noexcept : pool_(other.pool_) {
        other.pool_ = nullptr;
    }

    WorkerPermit& operator=(WorkerPermit&& other) noexcept {
        if (this != &other) {
            Release();
            pool_ = other.pool_;
            other.pool_ = nullptr;
        }
        return *this;
    }

    ~WorkerPermit() noexcept {
        Release();
    }

    explicit operator bool() const noexcept {
        return pool_ != nullptr;
    }

    void Release() noexcept;

private:
    friend class WorkerPermitPool;
    explicit WorkerPermit(WorkerPermitPool* pool) noexcept : pool_(pool) {}

    WorkerPermitPool* pool_ = nullptr;
};

class WorkerPermitPool final {
public:
    explicit WorkerPermitPool(size_t initial_limit) noexcept;
    WorkerPermitPool(const WorkerPermitPool&) = delete;
    WorkerPermitPool& operator=(const WorkerPermitPool&) = delete;
    WorkerPermitPool(WorkerPermitPool&&) = delete;
    WorkerPermitPool& operator=(WorkerPermitPool&&) = delete;

    WorkerPermit TryAcquire() noexcept;

    uint64_t RegisterWaiter(std::function<void()> callback);
    void UnregisterWaiter(uint64_t id) noexcept;

    size_t ActiveCount() const noexcept;
    size_t Limit() const noexcept;
    void SetLimitForTesting(size_t limit) noexcept;

private:
    friend class WorkerPermit;
    struct WaiterSlot;
    struct WaiterEntry {
        uint64_t id = 0;
        std::shared_ptr<WaiterSlot> slot;
    };

    void ReleaseOne() noexcept;
    static void NotifySnapshot(const std::vector<std::shared_ptr<WaiterSlot>>& snapshot) noexcept;

    mutable std::mutex mutex_;
    size_t limit_ = 1;
    size_t active_ = 0;
    uint64_t next_waiter_id_ = 0;
    std::vector<WaiterEntry> waiters_;
};

WorkerPermitPool& GlobalTaskWorkerPool() noexcept;
WorkerPermitPool& GlobalPreviewWorkerPool() noexcept;

} // namespace pulse::extensions::native
