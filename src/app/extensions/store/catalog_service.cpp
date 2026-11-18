#include "catalog_service.h"
#include <exception>
#include <stdexcept>
#include <utility>

namespace pulse::extensions::store {
CatalogService::CatalogService() : fetch_([](const std::filesystem::path& path,
    const std::atomic<bool>& cancelled, CatalogSnapshot& out, std::string& error) {
    return FetchCatalog(path, cancelled, out, error);
}) {
    worker_ = std::thread(&CatalogService::Run, this);
}
#ifdef PULSE_EXTENSION_CATALOG_SERVICE_TESTING
CatalogService::CatalogService(FetchOperation fetch) : fetch_(std::move(fetch)) {
    if (!fetch_) throw std::invalid_argument("missing catalog fetch operation");
    worker_ = std::thread(&CatalogService::Run, this);
}
bool CatalogService::WaitForGeneration(uint64_t generation, std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] {
        return snapshot_.generation > generation ||
            (snapshot_.generation == generation && snapshot_.state != CatalogServiceState::Loading);
    });
}
#endif
CatalogService::~CatalogService() {
    Stop();
    if (worker_.joinable()) worker_.join();
}
uint64_t CatalogService::Refresh(const std::filesystem::path& cache_file) {
    Request request{cache_file, 0, std::make_shared<std::atomic<bool>>(false)};
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return snapshot_.generation;
        if (active_) active_->store(true);
        if (pending_) pending_->cancelled->store(true);
        generation = snapshot_.generation + 1;
        request.generation = generation;
        pending_ = std::move(request);
        snapshot_ = {CatalogServiceState::Loading, generation, {}, {}};
    }
    wake_.notify_one();
    changed_.notify_all();
    return generation;
}
uint64_t CatalogService::Cancel() {
    uint64_t generation = 0;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return snapshot_.generation;
        if (active_) active_->store(true);
        if (pending_) pending_->cancelled->store(true);
        pending_.reset();
        generation = snapshot_.generation + 1;
        snapshot_ = {CatalogServiceState::Cancelled, generation, {}, "cancelled"};
    }
    changed_.notify_all();
    return generation;
}
CatalogServiceSnapshot CatalogService::GetSnapshot() const {
    std::lock_guard lock(mutex_);
    return snapshot_;
}
void CatalogService::Stop() {
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        if (active_) active_->store(true);
        if (pending_) pending_->cancelled->store(true);
        pending_.reset();
        snapshot_ = {CatalogServiceState::Cancelled, snapshot_.generation + 1, {}, "catalog_service_stopped"};
    }
    wake_.notify_one();
    changed_.notify_all();
}
void CatalogService::Run() {
    for (;;) {
        std::optional<Request> request;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [&] { return stopping_ || pending_.has_value(); });
            if (stopping_) return;
            request = std::move(pending_);
            pending_.reset();
            active_ = request->cancelled;
        }
        CatalogServiceSnapshot completed;
        completed.generation = request->generation;
        try {
            CatalogSnapshot result;
            std::string error;
            const auto status = request->cancelled->load() ? CatalogFetchStatus::Cancelled :
                fetch_(request->cache_file, *request->cancelled, result, error);
            if (request->cancelled->load() || status == CatalogFetchStatus::Cancelled) {
                completed.state = CatalogServiceState::Cancelled;
                completed.error = "cancelled";
            } else if (status == CatalogFetchStatus::Online || status == CatalogFetchStatus::Offline) {
                completed.state = status == CatalogFetchStatus::Online ? CatalogServiceState::Online : CatalogServiceState::Offline;
                completed.result = std::make_shared<const CatalogSnapshot>(std::move(result));
                completed.error = std::move(error);
            } else {
                completed.state = CatalogServiceState::Failed;
                completed.error = error.empty() ? "catalog_fetch_failed" : std::move(error);
            }
        } catch (const std::exception& ex) {
            completed.state = CatalogServiceState::Failed;
            completed.error = std::string("catalog_fetch_exception: ") + ex.what();
        } catch (...) {
            completed.state = CatalogServiceState::Failed;
            completed.error = "catalog_fetch_exception";
        }
        {
            std::lock_guard lock(mutex_);
            active_.reset();
            // A late success (including a non-cooperative operation) never replaces
            // the state of Refresh/Cancel/Stop issued while the request was running.
            if (!stopping_ && snapshot_.generation == request->generation) snapshot_ = std::move(completed);
        }
        changed_.notify_all();
    }
}
} // namespace pulse::extensions::store
