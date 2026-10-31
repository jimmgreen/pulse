#include "transfer_device_budget.h"
#include "transfer_features.h"
#include "../fs/drive_kind.h"
#include <windows.h>
#include <winioctl.h>
#include <algorithm>
#include <cstddef>
#include <cwchar>
#include <mutex>
#include <condition_variable>
#include <list>
#include <utility>

namespace pulse::ops {
TransferSchedulerConfig ReadTransferSchedulerConfig() {
    TransferSchedulerConfig config;
    wchar_t value[16]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_UNIFIED_SCHEDULER", value, 16);
    if (length == 1 && value[0] == L'0') config.enabled = false;
    return config;
}

TransferDeviceIdentity DirectUncTransferIdentity(const std::wstring& path) {
    size_t server_start = 0;
    if (path.size() >= 8 && _wcsnicmp(path.c_str(), LR"(\\?\UNC\)", 8) == 0) server_start = 8;
    else if (path.starts_with(LR"(\\)") && !path.starts_with(LR"(\\?\)") && !path.starts_with(LR"(\\.\)")) server_start = 2;
    else return {};
    const auto server_end = path.find(L'\\', server_start);
    if (server_end == std::wstring::npos || server_end == server_start || server_end + 1 >= path.size()) return {};
    const auto share_end = path.find(L'\\', server_end + 1);
    const auto share_size = (share_end == std::wstring::npos ? path.size() : share_end) - server_end - 1;
    if (!share_size) return {};
    const auto server = path.substr(server_start, server_end - server_start);
    const auto share = path.substr(server_end + 1, share_size);
    if (server == L"." || server == L".." || share == L"." || share == L"..") return {};
    // Recognizable WebDAV/cloud redirector syntax is not the opt-in SMB policy.
    if (server.find(L'@') != std::wstring::npos || _wcsicmp(share.c_str(), L"DavWWWRoot") == 0) return {};
    // Case folding is local and invariant; hostname aliases intentionally remain distinct.
    const int folded_size = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, server.data(),
        static_cast<int>(server.size()), nullptr, 0, nullptr, nullptr, 0);
    if (folded_size <= 0) return {};
    std::wstring folded(static_cast<size_t>(folded_size), L'\0');
    if (!LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, server.data(), static_cast<int>(server.size()),
        folded.data(), folded_size, nullptr, nullptr, 0)) return {};
    return {{L"unc-server:" + folded}, 1, 4};
}

TransferDeviceIdentityCache::TransferDeviceIdentityCache()
    : remote_budget_enabled_(TransferFeatureEnabled(L"PULSE_REMOTE_COPY_BUDGET")) {}

TransferDeviceIdentity TransferDeviceIdentityCache::Resolve(const std::wstring& path) {
    const bool direct_unc = (path.size() >= 8 && _wcsnicmp(path.c_str(), LR"(\\?\UNC\)", 8) == 0) ||
        (path.starts_with(LR"(\\)") && !path.starts_with(LR"(\\?\)") && !path.starts_with(LR"(\\.\)"));
    if (direct_unc) return remote_budget_enabled_ ? DirectUncTransferIdentity(path) : TransferDeviceIdentity{};
    std::wstring root(32768, L'\0');
    if (!GetVolumePathNameW(path.c_str(), root.data(), static_cast<DWORD>(root.size()))) return {};
    root.resize(wcslen(root.c_str()));
    if (const auto found = volumes_.find(root); found != volumes_.end()) return found->second;
    TransferDeviceIdentity identity;
    if (!pulse::fs::IsRemoteLikeDrive(root) && GetDriveTypeW(root.c_str()) != DRIVE_REMOTE) {
        wchar_t volume[128]{};
        if (GetVolumeNameForVolumeMountPointW(root.c_str(), volume, 128)) {
            std::wstring device(volume);
            if (!device.empty() && device.back() == L'\\') device.pop_back();
            HANDLE handle = CreateFileW(device.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle != INVALID_HANDLE_VALUE) {
                // Bounded topology query. Complicated/spanned volumes fall back conservatively.
                struct Extents { DWORD count; DISK_EXTENT extents[32]; } extents{};
                DWORD returned = 0;
                if (DeviceIoControl(handle, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                    &extents, sizeof(extents), &returned, nullptr) &&
                    extents.count > 0 && extents.count <= 32 &&
                    returned >= offsetof(Extents, extents) + extents.count * sizeof(DISK_EXTENT)) {
                    for (DWORD i = 0; i < extents.count; ++i)
                        identity.resources.push_back(L"disk:" + std::to_wstring(extents.extents[i].DiskNumber));
                    std::sort(identity.resources.begin(), identity.resources.end());
                    identity.resources.erase(std::unique(identity.resources.begin(), identity.resources.end()), identity.resources.end());
                    identity.tiny_capacity = 2;
                    STORAGE_PROPERTY_QUERY query{};
                    query.PropertyId = StorageDeviceSeekPenaltyProperty;
                    query.QueryType = PropertyStandardQuery;
                    DEVICE_SEEK_PENALTY_DESCRIPTOR seek{};
                    const bool solid = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &seek, sizeof(seek), &returned, nullptr) && returned >= sizeof(seek) && !seek.IncursSeekPenalty;
                    query.PropertyId = StorageDeviceProperty;
                    struct DeviceDescriptor { STORAGE_DEVICE_DESCRIPTOR value; BYTE extra[4096]; } descriptor{};
                    const bool local_bus = DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                        &descriptor, sizeof(descriptor), &returned, nullptr) && returned >= sizeof(descriptor.value) &&
                        (descriptor.value.BusType == BusTypeNvme || descriptor.value.BusType == BusTypeSata ||
                         descriptor.value.BusType == BusTypeAta || descriptor.value.BusType == BusTypeSas);
                    if (solid && local_bus && identity.resources.size() == 1 &&
                        GetDriveTypeW(root.c_str()) == DRIVE_FIXED) {
                        identity.capacity = 4;
                        identity.tiny_capacity = 8;
                    }
                }
                CloseHandle(handle);
            }
        }
    }
    volumes_.emplace(root, identity);
    return identity;
}

struct TransferDeviceBudget::State {
    struct Resource { unsigned active = 0; unsigned capacity = 1; };
    explicit State(TransferSchedulerConfig value)
        : fair(TransferFeatureEnabled(L"PULSE_FAIR_TRANSFER_BUDGET")),
          wake(TransferFeatureEnabled(L"PULSE_TRANSFER_BUDGET_WAKE")), config(value) {}
    struct Pending {
        uint64_t id, turn;
        TaskToken task;
        TransferDeviceIdentity source, destination;
        uint64_t bytes;
    };
    const bool fair;
    const bool wake;
    std::condition_variable changed;
    uint64_t change = 0;
    void NotifyWaiters() {
        if (!wake || pending.empty()) return;
        ++change;
        changed.notify_all();
    }
    std::list<Pending> pending;
    uint64_t next_id = 0, next_turn = 0;
    TransferSchedulerConfig config;
    mutable std::mutex mutex;
    std::unordered_map<std::wstring, Resource> resources;
    unsigned active = 0;
    uint64_t bytes = 0;
    bool unknown = false;
};
TransferDeviceBudget::TransferDeviceBudget(TransferSchedulerConfig config)
    : state_(std::make_shared<State>(config)) {}

TransferDeviceBudget::Lease TransferDeviceBudget::TryAcquire(const TransferDeviceIdentity& source,
    const TransferDeviceIdentity& destination, uint64_t bytes) const {
    std::lock_guard lock(state_->mutex);
    return TryAcquireLocked(source, destination, bytes, 0);
}
TransferDeviceBudget::Lease TransferDeviceBudget::TryAcquireLocked(const TransferDeviceIdentity& source,
    const TransferDeviceIdentity& destination, uint64_t bytes, uint64_t waiter_id) const {
    const bool unknown = source.resources.empty() || destination.resources.empty();
    if (state_->unknown || (unknown && state_->active) ||
        state_->active >= (std::max)(1u, state_->config.max_active)) return {};
    if (state_->active && (bytes > state_->config.max_active_bytes ||
        state_->bytes > state_->config.max_active_bytes - bytes)) return {};
    const State::Pending* current = nullptr;
    for (const auto& pending : state_->pending)
        if (pending.id == waiter_id) { current = &pending; break; }
    for (const auto& pending : state_->pending) {
        if (pending.id == waiter_id) continue;
        if (current && (pending.turn > current->turn ||
            (pending.turn == current->turn && pending.id > current->id))) continue;
        bool conflict = unknown || pending.source.resources.empty() || pending.destination.resources.empty();
        for (const auto* a : {&source, &destination})
            for (const auto* b : {&pending.source, &pending.destination})
                for (const auto& key : a->resources)
                    if (std::find(b->resources.begin(), b->resources.end(), key) != b->resources.end()) conflict = true;
        // Reserve room for the older waiter as well as this candidate. This lets
        // disjoint work proceed only when it cannot keep exhausting a global cap.
        const uint64_t available = state_->bytes >= state_->config.max_active_bytes
            ? 0 : state_->config.max_active_bytes - state_->bytes;
        const bool byte_conflict = bytes > available || pending.bytes > available - bytes;
        const bool slot_conflict = state_->active >= (std::max)(1u, state_->config.max_active) - 1;
        if (conflict || byte_conflict || slot_conflict) return {};
    }
    std::unordered_map<std::wstring, unsigned> requested;
    for (const auto* endpoint : {&source, &destination}) {
        for (const auto& key : endpoint->resources) {
            const unsigned capacity = (std::max)(1u, endpoint->capacity);
            auto [item, inserted] = requested.emplace(key, capacity);
            if (!inserted) item->second = (std::min)(item->second, capacity);
        }
    }
    for (const auto& [key, capacity] : requested) {
        const auto found = state_->resources.find(key);
        if (found != state_->resources.end() &&
            found->second.active >= (std::min)(found->second.capacity, capacity)) return {};
    }
    Lease lease;
    lease.resources_.reserve(requested.size());
    for (const auto& [key, capacity] : requested) lease.resources_.push_back(key);
    for (const auto& [key, capacity] : requested) {
        auto [item, inserted] = state_->resources.try_emplace(key, State::Resource{0, capacity});
        if (!inserted) item->second.capacity = item->second.active == 0
            ? capacity : (std::min)(item->second.capacity, capacity);
    }
    // Allocate every resource before changing counts, so allocation failure cannot leak permits.
    for (const auto& key : lease.resources_) ++state_->resources.at(key).active;
    ++state_->active;
    state_->bytes += bytes;
    state_->unknown = unknown;
    lease.state_ = state_;
    lease.bytes_ = bytes;
    lease.unknown_ = unknown;
    return lease;
}
TransferDeviceBudget::Waiter TransferDeviceBudget::Wait(const TransferDeviceIdentity& source,
    const TransferDeviceIdentity& destination, uint64_t bytes, TaskToken task) const {
    if (!state_->fair) return {};
    if (!task) task = CreateTask();
    std::lock_guard lock(state_->mutex);
    uint64_t turn = ++state_->next_turn;
    for (const auto& pending : state_->pending)
        if (pending.task == task) { turn = pending.turn; break; }
    const uint64_t id = ++state_->next_id;
    state_->pending.push_back({id, turn, std::move(task), source, destination, bytes});
    Waiter waiter;
    waiter.state_ = state_; waiter.id_ = id;
    // Force one immediate admission retry after registration.
    waiter.observed_change_ = state_->change - 1;
    return waiter;
}
TransferDeviceBudget::Lease TransferDeviceBudget::TryAcquire(Waiter& waiter) const {
    if (waiter.state_ != state_) return {};
    std::lock_guard lock(state_->mutex);
    const auto found = std::find_if(state_->pending.begin(), state_->pending.end(),
        [&](const auto& pending) { return pending.id == waiter.id_; });
    if (found == state_->pending.end()) return {};
    auto lease = TryAcquireLocked(found->source, found->destination, found->bytes, found->id);
    waiter.observed_change_ = state_->change;
    if (lease) {
        const auto task = found->task;
        state_->pending.erase(found);
        const uint64_t turn = ++state_->next_turn;
        for (auto& pending : state_->pending) if (pending.task == task) pending.turn = turn;
        state_->NotifyWaiters();
        waiter.state_.reset(); waiter.id_ = 0;
    }
    return lease;
}
size_t TransferDeviceBudget::Waiting() const {
    std::lock_guard lock(state_->mutex);
    return state_->pending.size();
}
TransferDeviceBudget::Waiter::~Waiter() { Reset(); }
TransferDeviceBudget::Waiter::Waiter(Waiter&& other) noexcept
    : state_(std::move(other.state_)), id_(std::exchange(other.id_, 0)), observed_change_(other.observed_change_) {}
TransferDeviceBudget::Waiter& TransferDeviceBudget::Waiter::operator=(Waiter&& other) noexcept {
    if (this != &other) { Reset(); state_ = std::move(other.state_); id_ = std::exchange(other.id_, 0); observed_change_ = other.observed_change_; }
    return *this;
}
void TransferDeviceBudget::Waiter::Reset() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        state_->pending.remove_if([&](const auto& pending) { return pending.id == id_; });
        state_->NotifyWaiters();
    }
    state_.reset(); id_ = 0;
}
void TransferDeviceBudget::Waiter::WaitForChange(std::chrono::milliseconds timeout) {
    if (!state_) return;
    std::unique_lock lock(state_->mutex);
    state_->changed.wait_for(lock, timeout, [&] {
        return state_->wake && observed_change_ != state_->change;
    });
}
unsigned TransferDeviceBudget::Active() const {
    std::lock_guard lock(state_->mutex);
    return state_->active;
}
TransferDeviceBudget::Lease::~Lease() { Reset(); }
TransferDeviceBudget::Lease::Lease(Lease&& other) noexcept
    : state_(std::move(other.state_)), resources_(std::move(other.resources_)),
      bytes_(other.bytes_), unknown_(other.unknown_) {}
TransferDeviceBudget::Lease& TransferDeviceBudget::Lease::operator=(Lease&& other) noexcept {
    if (this != &other) {
        Reset();
        state_ = std::move(other.state_);
        resources_ = std::move(other.resources_);
        bytes_ = other.bytes_;
        unknown_ = other.unknown_;
    }
    return *this;
}
void TransferDeviceBudget::Lease::Reset() {
    if (!state_) return;
    {
        std::lock_guard lock(state_->mutex);
        for (const auto& key : resources_) --state_->resources.at(key).active;
        --state_->active;
        state_->bytes -= bytes_;
        if (unknown_) state_->unknown = false;
        state_->NotifyWaiters();
    }
    state_.reset();
    resources_.clear();
}
TransferDeviceBudget& SharedTransferDeviceBudget() {
    static TransferDeviceBudget budget(TransferSchedulerConfig{true, 8, 256ull << 20});
    return budget;
}
} // namespace pulse::ops
