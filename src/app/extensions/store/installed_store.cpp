#include "installed_store.h"
#include <condition_variable>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace pulse::extensions::store {
struct InstalledStoreService::Impl {
    struct Operations {
        std::function<decltype(ReadInstalledStore)> read = ReadInstalledStore;
        std::function<decltype(ReviewStoreChange)> review = ReviewStoreChange;
        std::function<decltype(CommitStoreChange)> commit = CommitStoreChange;
    } operations;
    enum class Command { Refresh, Prepare, Commit };
    struct Request {
        Command command = Command::Refresh;
        uint64_t generation = 0;
        std::filesystem::path root;
        std::string id;
        StoreChangeAction action = StoreChangeAction::Disable;
        StoreChangeReview review;
        StoreChangeConsent consent;
        std::shared_ptr<std::atomic<bool>> cancel = std::make_shared<std::atomic<bool>>(false);
    };
    mutable std::mutex mutex;
    std::condition_variable wake, changed;
    InstalledStoreServiceSnapshot snapshot;
    std::optional<Request> pending, ready;
    std::shared_ptr<std::atomic<bool>> active;
    bool stopping = false;
    std::thread worker;
    explicit Impl(Operations ops) : operations(std::move(ops)) {
        if (!operations.read || !operations.review || !operations.commit) throw std::invalid_argument("installed_operations_missing");
        worker = std::thread(&Impl::Run, this);
    }
    ~Impl() { Stop(); if (worker.joinable()) worker.join(); }
    void CancelWork() {
        if (active) active->store(true);
        if (pending) pending->cancel->store(true);
        if (ready) ready->cancel->store(true);
        pending.reset(); ready.reset();
    }
    void Reset(InstalledStorePhase phase) {
        const auto generation = snapshot.generation + 1;
        const auto restart = snapshot.pending_restart;
        // Keep last immutable rows visible while preparing/committing a command.
        auto installed = snapshot.installed;
        snapshot = {}; snapshot.generation = generation; snapshot.phase = phase;
        snapshot.pending_restart = restart; snapshot.installed = std::move(installed);
    }
    uint64_t Submit(Request request) {
        uint64_t generation;
        {
            std::lock_guard lock(mutex);
            if (stopping) return snapshot.generation;
            CancelWork(); Reset(request.command == Command::Refresh ? InstalledStorePhase::Loading : InstalledStorePhase::Preparing);
            if (request.command == Command::Refresh) snapshot.installed.reset();
            generation = request.generation = snapshot.generation;
            pending = std::move(request);
        }
        wake.notify_one(); changed.notify_all(); return generation;
    }
    uint64_t Cancel() {
        uint64_t generation;
        {
            std::lock_guard lock(mutex);
            if (stopping) return snapshot.generation;
            CancelWork(); Reset(InstalledStorePhase::Cancelled); generation = snapshot.generation;
        }
        changed.notify_all(); return generation;
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true; CancelWork(); Reset(InstalledStorePhase::Cancelled);
        }
        wake.notify_one(); changed.notify_all();
    }
    void Run() {
        for (;;) {
            Request request;
            InstalledStoreServiceSnapshot out;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping) return;
                request = std::move(*pending); pending.reset(); active = request.cancel;
                out = snapshot;
            }
            bool restart = false;
            try {
                if (!request.cancel->load()) {
                    if (request.command == Command::Prepare) {
                        if (operations.review(request.root, request.id, request.action, request.review, out.error)) {
                            out.review = std::make_shared<const StoreChangeReview>(request.review);
                            out.phase = InstalledStorePhase::AwaitingConfirmation;
                        } else out.phase = InstalledStorePhase::Failed;
                    } else {
                        bool read = true;
                        if (request.command == Command::Commit) {
                            StoreChangeResult result;
                            if (operations.commit(request.root, request.review, request.consent, *request.cancel, result, out.error)) {
                                restart = result.restart_required;
                                out.phase = InstalledStorePhase::Changed;
                            } else { out.phase = InstalledStorePhase::Failed; read = false; }
                        } else out.phase = InstalledStorePhase::Ready;
                        if (read && !request.cancel->load()) {
                            InstalledSnapshot installed;
                            if (operations.read(request.root, installed, out.error)) out.installed = std::make_shared<const InstalledSnapshot>(std::move(installed));
                            else out.phase = InstalledStorePhase::Failed;
                        }
                    }
                }
            } catch (const std::exception& e) { out.phase = InstalledStorePhase::Failed; out.error = e.what(); }
            catch (...) { out.phase = InstalledStorePhase::Failed; out.error = "installed_service_exception"; }
            {
                std::lock_guard lock(mutex);
                active.reset();
                // A stale generation can still have crossed the registry commit point.
                snapshot.pending_restart = snapshot.pending_restart || restart;
                if (!stopping && snapshot.generation == request.generation) {
                    out.pending_restart = snapshot.pending_restart;
                    if (request.cancel->load()) { out.phase = InstalledStorePhase::Cancelled; out.error = "cancelled"; }
                    if (out.phase == InstalledStorePhase::AwaitingConfirmation) ready = std::move(request);
                    snapshot = std::move(out);
                }
            }
            changed.notify_all();
        }
    }
};
InstalledStoreService::InstalledStoreService() : impl_(std::make_unique<Impl>(Impl::Operations{})) {}
InstalledStoreService::~InstalledStoreService() = default;
uint64_t InstalledStoreService::Refresh(std::filesystem::path root) {
    Impl::Request request; request.root = std::move(root); return impl_->Submit(std::move(request));
}
uint64_t InstalledStoreService::PrepareChange(std::filesystem::path root, std::string id, StoreChangeAction action) {
    Impl::Request request; request.command = Impl::Command::Prepare; request.root = std::move(root);
    request.id = std::move(id); request.action = action; return impl_->Submit(std::move(request));
}
bool InstalledStoreService::Confirm(uint64_t generation, StoreChangeConsent consent) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->snapshot.generation != generation ||
            impl_->snapshot.phase != InstalledStorePhase::AwaitingConfirmation || !impl_->ready) return false;
        auto request = std::move(*impl_->ready); impl_->ready.reset();
        request.command = Impl::Command::Commit; request.consent = consent;
        impl_->pending = std::move(request); impl_->snapshot.phase = InstalledStorePhase::Committing;
    }
    impl_->wake.notify_one(); impl_->changed.notify_all(); return true;
}
uint64_t InstalledStoreService::Cancel() { return impl_->Cancel(); }
void InstalledStoreService::Stop() { impl_->Stop(); }
InstalledStoreServiceSnapshot InstalledStoreService::GetSnapshot() const { std::lock_guard lock(impl_->mutex); return impl_->snapshot; }
#ifdef PULSE_EXTENSION_INSTALLED_STORE_TESTING
InstalledStoreService::InstalledStoreService(Operations ops) : impl_(std::make_unique<Impl>(Impl::Operations{std::move(ops.read), std::move(ops.review), std::move(ops.commit)})) {}
bool InstalledStoreService::WaitForPhase(uint64_t generation, InstalledStorePhase phase, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] { return impl_->snapshot.generation >= generation &&
        (impl_->snapshot.generation != generation || impl_->snapshot.phase == phase); });
}
bool InstalledStoreService::WaitForPendingRestart(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] { return impl_->snapshot.pending_restart; });
}
#endif
} // namespace pulse::extensions::store
