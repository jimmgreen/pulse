#pragma once
#include "catalog_client.h"
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace pulse::extensions::store {
enum class CatalogServiceState { Idle, Loading, Online, Offline, Failed, Cancelled };
struct CatalogServiceSnapshot {
    CatalogServiceState state = CatalogServiceState::Idle;
    uint64_t generation = 0;
    std::shared_ptr<const CatalogSnapshot> result; // Only Online/Offline has a result.
    std::string error; // Diagnostic, not localized UI text.
};

// Single serial worker, latest-generation wins. Refresh/Cancel/GetSnapshot/Stop
// take only a short state mutex; they never wait for network or perform file I/O.
// UI pulls immutable snapshots; there are no callbacks capturing a UI owner.
// Production always calls FetchCatalog with its fixed production trust. No cache
// parent directory creation or preference changes. Callers must not share one
// cache path with a separate writer/service without external serialization.
class CatalogService {
public:
    CatalogService();
    // Stop only signals. Destruction Stop()+joins and MUST happen off the UI thread:
    // cooperative WinHTTP cancellation may wait for the existing transport timeout.
    // Never destroy this service from its worker or race destruction with API calls.
    ~CatalogService();
    CatalogService(const CatalogService&) = delete;
    CatalogService& operator=(const CatalogService&) = delete;

    // Advances generation, immediately publishes Loading with no retained result,
    // cancels active work, and replaces (never appends to) the pending request.
    uint64_t Refresh(const std::filesystem::path& cache_file);
    // Advances generation, clears pending, cancels active, publishes Cancelled.
    uint64_t Cancel();
    CatalogServiceSnapshot GetSnapshot() const;
    // Idempotent, non-joining. First call advances generation and publishes
    // Cancelled / catalog_service_stopped. Future Refresh/Cancel return that same
    // generation without scheduling work; a stopped service cannot restart.
    void Stop();

#ifdef PULSE_EXTENSION_CATALOG_SERVICE_TESTING
    using FetchOperation = std::function<CatalogFetchStatus(const std::filesystem::path&,
        const std::atomic<bool>&, CatalogSnapshot&, std::string&)>;
    explicit CatalogService(FetchOperation fetch);
    // Test-only: waits for terminal target state OR supersession (generation >
    // target), with a finite deadline. Does not perform network/file I/O.
    bool WaitForGeneration(uint64_t generation, std::chrono::milliseconds timeout);
#endif
private:
    using Operation = std::function<CatalogFetchStatus(const std::filesystem::path&,
        const std::atomic<bool>&, CatalogSnapshot&, std::string&)>;
    struct Request {
        std::filesystem::path cache_file;
        uint64_t generation;
        std::shared_ptr<std::atomic<bool>> cancelled;
    };
    void Run();
    mutable std::mutex mutex_;
    std::condition_variable wake_, changed_;
    CatalogServiceSnapshot snapshot_;
    std::optional<Request> pending_;
    std::shared_ptr<std::atomic<bool>> active_;
    bool stopping_ = false;
    Operation fetch_;
    std::thread worker_;
};
} // namespace pulse::extensions::store
