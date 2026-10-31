#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <utility>

namespace pulse::ops {

struct TransferSchedulerConfig {
    bool enabled = true;
    unsigned max_active = 8;
    uint64_t max_active_bytes = 256ull << 20;
};
// Read once per task. This switch rolls back scheduling only, not copy semantics.
TransferSchedulerConfig ReadTransferSchedulerConfig();

struct TransferDeviceIdentity {
    // Empty means topology is unknown: the operation must run globally alone.
    std::vector<std::wstring> resources;
    unsigned capacity = 1;
    unsigned tiny_capacity = 1;
};

// Syntax-only server grouping; never resolves aliases or claims physical disk identity.
// Ordinary and extended UNC share names on the same case-folded server share tokens.
TransferDeviceIdentity DirectUncTransferIdentity(const std::wstring& path);

// Task-local, coordinator-thread cache. Resolve performs blocking storage queries.
class TransferDeviceIdentityCache {
public:
    TransferDeviceIdentityCache();
    explicit TransferDeviceIdentityCache(bool remote_budget_enabled) : remote_budget_enabled_(remote_budget_enabled) {}
    TransferDeviceIdentity Resolve(const std::wstring& path);
private:
    const bool remote_budget_enabled_;
    std::unordered_map<std::wstring, TransferDeviceIdentity> volumes_;
};

class TransferDeviceBudget {
    struct State;
public:
    struct Task {};
    using TaskToken = std::shared_ptr<const Task>;
    static TaskToken CreateTask() { return std::make_shared<Task>(); }
    class Waiter {
    public:
        Waiter() = default;
        ~Waiter();
        Waiter(Waiter&& other) noexcept;
        Waiter& operator=(Waiter&& other) noexcept;
        Waiter(const Waiter&) = delete;
        Waiter& operator=(const Waiter&) = delete;
        explicit operator bool() const { return state_ != nullptr; }
        void Reset();
        // Recheck control state after this bounded wait. A release before waiting is remembered.
        void WaitForChange(std::chrono::milliseconds timeout = std::chrono::milliseconds(2));
    private:
        friend class TransferDeviceBudget;
        std::shared_ptr<State> state_;
        uint64_t id_ = 0;
        uint64_t observed_change_ = 0;
    };
    class Lease {
    public:
        Lease() = default;
        ~Lease();
        Lease(Lease&& other) noexcept;
        Lease& operator=(Lease&& other) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        explicit operator bool() const { return state_ != nullptr; }
        void Reset();
    private:
        friend class TransferDeviceBudget;
        std::shared_ptr<State> state_;
        std::vector<std::wstring> resources_;
        uint64_t bytes_ = 0;
        bool unknown_ = false;
    };

    explicit TransferDeviceBudget(TransferSchedulerConfig config = {});
    // Both endpoints are charged once each; identical physical disks are deduplicated.
    // Verification uses this same API/instance with the devices it reads. A failed
    // acquisition has no effects. Unticketed calls cannot bypass conflicting waiters.
    // Registered tasks rotate after each grant; conflicting workers within a task use FIFO.
    // Pause/cancel owners must Reset their waiter before waiting for user control.
    Lease TryAcquire(const TransferDeviceIdentity& source, const TransferDeviceIdentity& destination,
                     uint64_t bytes) const;
    // PULSE_FAIR_TRANSFER_BUDGET is read once per shared State (default on).
    // With it off, Wait returns empty; callers retry ordinary TryAcquire.
    Waiter Wait(const TransferDeviceIdentity& source, const TransferDeviceIdentity& destination,
                uint64_t bytes, TaskToken task) const;
    Lease TryAcquire(Waiter& waiter) const;
    unsigned Active() const;
    size_t Waiting() const;
private:
    Lease TryAcquireLocked(const TransferDeviceIdentity& source, const TransferDeviceIdentity& destination,
                           uint64_t bytes, uint64_t waiter_id) const;
    std::shared_ptr<State> state_;
};

// Cached by the coordinator and shared by queued jobs. Copying a budget shares
// its state; the pair keeps that state alive until every worker has finished.
struct TransferDevicePair {
    TransferDeviceIdentity source;
    TransferDeviceIdentity target;
    TransferDeviceBudget budget;
    TransferDevicePair(TransferDeviceIdentity source_device, TransferDeviceIdentity target_device,
                       TransferDeviceBudget shared_budget)
        : source(std::move(source_device)), target(std::move(target_device)), budget(std::move(shared_budget)) {}
};

// Process-wide default budget: 8 active jobs, 256 MiB; task identity caches remain local.
// Callers bypass this entry point when unified scheduling is disabled.
TransferDeviceBudget& SharedTransferDeviceBudget();

} // namespace pulse::ops
