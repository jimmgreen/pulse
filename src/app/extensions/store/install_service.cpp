#include "install_service.h"
#include <array>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace pulse::extensions::store {
namespace {
using Version = std::array<uint64_t, 3>;
bool ParseVersion(std::string_view text, Version& out, bool allow_leading_zero = false) {
    size_t pos = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const size_t start = pos;
        uint64_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            const auto digit = static_cast<uint64_t>(text[pos++] - '0');
            if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (start == pos || (!allow_leading_zero && pos - start > 1 && text[start] == '0')) return false;
        out[i] = value;
        if (i + 1 < out.size() && (pos == text.size() || text[pos++] != '.')) return false;
    }
    return pos == text.size();
}
const char* ReleaseCompatibilityError(const Release& release, std::string_view host_version, int sdk_preview) {
    Version host{}, minimum{};
    if (!ParseVersion(host_version, host)) return "install_host_version_invalid";
    // Catalog v1 minPulse permits zero-padded numeric components.
    if (!ParseVersion(release.min_pulse, minimum, true)) return "install_min_version_invalid";
    if (host < minimum) return "install_host_too_old";
    if (sdk_preview <= 0 || release.sdk_preview != sdk_preview) return "install_sdk_incompatible";
    return nullptr;
}
struct CoreOperations {
    std::function<bool(const Catalog&, std::string_view, std::string_view,
        const std::atomic<bool>&, PackageDownload&, std::string&)> download =
        [](const Catalog& catalog, std::string_view id, std::string_view version,
            const std::atomic<bool>& cancel, PackageDownload& out, std::string& error) {
            return FetchCatalogPackage(catalog, id, version, cancel, out, error);
        };
    decltype(&ReviewPublisherTrust) review_trust = ReviewPublisherTrust;
    decltype(&CommitPublisherTrust) commit_trust = CommitPublisherTrust;
    decltype(&ReviewInstall) review_install = ReviewInstall;
    decltype(&CommitInstall) commit_install = CommitInstall;
};
}
bool IsReleaseCompatible(const Release& release, std::string_view host_version, int sdk_preview) {
    return ReleaseCompatibilityError(release, host_version, sdk_preview) == nullptr;
}
struct InstallService::Impl {
    struct Operations {
        decltype(CoreOperations::download) download;
        std::function<std::remove_pointer_t<decltype(&ReviewPublisherTrust)>> review_trust;
        std::function<std::remove_pointer_t<decltype(&CommitPublisherTrust)>> commit_trust;
        std::function<std::remove_pointer_t<decltype(&ReviewInstall)>> review_install;
        std::function<std::remove_pointer_t<decltype(&CommitInstall)>> commit_install;
        Operations() : download(CoreOperations{}.download), review_trust(ReviewPublisherTrust),
            commit_trust(CommitPublisherTrust), review_install(ReviewInstall), commit_install(CommitInstall) {}
    };
    struct Context {
        uint64_t generation = 0;
        std::shared_ptr<const CatalogSnapshot> catalog;
        std::string id, version;
        std::shared_ptr<std::atomic<bool>> cancelled = std::make_shared<std::atomic<bool>>(false);
        std::shared_ptr<const VerifiedPackage> package;
        PublisherTrustReview trust;
        InstallReview install;
    };
    enum class Command { Prepare, Trust, Install };
    struct Request {
        Command command;
        std::shared_ptr<Context> context;
        bool approved = false;
        InstallConsent consent;
    };
    Config config;
    Operations operations;
    mutable std::mutex mutex;
    std::condition_variable wake, changed;
    InstallServiceSnapshot snapshot;
    std::optional<Request> pending;
    std::shared_ptr<Context> ready;
    std::shared_ptr<std::atomic<bool>> active;
    bool stopping = false;
    std::thread worker;

    explicit Impl(Config value, Operations ops = {}) : config(std::move(value)), operations(std::move(ops)) {
        if (!config.store_root.is_absolute()) throw std::invalid_argument("install_store_root_not_absolute");
        Version host{};
        if (!ParseVersion(config.host_version, host)) throw std::invalid_argument("install_host_version_invalid");
        if (config.sdk_preview <= 0) throw std::invalid_argument("install_sdk_preview_invalid");
        if (!operations.download || !operations.review_trust || !operations.commit_trust ||
            !operations.review_install || !operations.commit_install) throw std::invalid_argument("install_operations_missing");
        worker = std::thread(&Impl::Run, this);
    }
    ~Impl() { Stop(); if (worker.joinable()) worker.join(); }
    void CancelWork() {
        if (active) active->store(true);
        if (pending) pending->context->cancelled->store(true);
        if (ready) ready->cancelled->store(true);
        pending.reset();
        ready.reset();
    }
    void Reset(InstallServicePhase phase, std::string error = {}) {
        const auto generation = snapshot.generation + 1;
        const bool restart = snapshot.pending_restart;
        snapshot = {};
        snapshot.generation = generation;
        snapshot.phase = phase;
        snapshot.error = std::move(error);
        snapshot.pending_restart = restart;
    }
    void Stop() {
        {
            std::lock_guard lock(mutex);
            if (stopping) return;
            stopping = true;
            CancelWork();
            Reset(InstallServicePhase::Cancelled, "install_service_stopped");
        }
        wake.notify_one();
        changed.notify_all();
    }
    bool Compatible(const Context& c, std::string& error) const {
        if (!c.catalog) { error = "install_catalog_missing"; return false; }
        for (const auto& extension : c.catalog->catalog.extensions) {
            if (extension.id != c.id) continue;
            for (const auto& release : extension.versions) {
                if (release.version != c.version) continue;
                if (IsReleaseCompatible(release, config.host_version, config.sdk_preview)) return true;
                error = ReleaseCompatibilityError(release, config.host_version, config.sdk_preview);
                return false;
            }
            error = "install_release_missing";
            return false;
        }
        error = "install_extension_missing";
        return false;
    }
    void Execute(const Request& request, InstallServiceSnapshot& out, bool& restart) {
        auto& c = *request.context;
        auto& cancelled = *c.cancelled;
        auto fail = [&] { out.phase = InstallServicePhase::Failed; if (out.error.empty()) out.error = "install_operation_failed"; };
        if (cancelled.load()) { out.phase = InstallServicePhase::Cancelled; out.error = "cancelled"; return; }
        if (request.command == Command::Prepare) {
            if (!Compatible(c, out.error)) { out.phase = InstallServicePhase::Incompatible; return; }
            PackageDownload download;
            if (!operations.download(c.catalog->catalog, c.id, c.version, cancelled, download, out.error)) { fail(); return; }
            if (cancelled.load()) return;
            if (!download.package) { out.error = "install_package_invalid"; fail(); return; }
            c.package = std::make_shared<const VerifiedPackage>(std::move(download.package));
            out.package = c.package;
            if (!operations.review_trust(config.store_root, *c.package, c.trust, out.error)) { fail(); return; }
            if (cancelled.load()) return;
            out.trust_review = std::make_shared<const PublisherTrustReview>(c.trust);
            if (!c.trust.already_trusted) { out.phase = InstallServicePhase::AwaitingPublisherTrust; return; }
        } else if (request.command == Command::Trust) {
            if (!request.approved) { out.phase = InstallServicePhase::Cancelled; out.error = "publisher_confirmation_declined"; return; }
            PublisherTrustResult result;
            if (!operations.commit_trust(config.store_root, *c.package, c.trust, true, cancelled, result, out.error)) { fail(); return; }
            // A committed policy is intentionally not rolled back after cancellation.
            if (cancelled.load()) return;
        } else {
            InstallResult result;
            if (!operations.commit_install(config.store_root, *c.package, c.install, request.consent, cancelled, result, out.error)) { fail(); return; }
            // Commit may have crossed its atomic rename before cancellation. Preserve
            // this fact even if this generation is now stale or the service stopped.
            restart = result.restart_required;
            out.enabled = result.enabled;
            out.phase = InstallServicePhase::Installed;
            return;
        }
        if (cancelled.load()) return;
        if (!operations.review_install(config.store_root, *c.package, c.install, out.error)) { fail(); return; }
        out.install_review = std::make_shared<const InstallReview>(c.install);
        out.phase = InstallServicePhase::AwaitingInstall;
    }
    void Run() {
        for (;;) {
            std::optional<Request> request;
            {
                std::unique_lock lock(mutex);
                wake.wait(lock, [&] { return stopping || pending.has_value(); });
                if (stopping) return;
                request = std::move(pending);
                pending.reset();
                active = request->context->cancelled;
            }
            const auto& c = *request->context;
            InstallServiceSnapshot out;
            out.generation = c.generation;
            out.id = c.id;
            out.version = c.version;
            out.package = c.package;
            bool restart = false;
            try {
                Execute(*request, out, restart);
            } catch (const std::exception& ex) {
                out.phase = InstallServicePhase::Failed;
                out.error = std::string("install_service_exception: ") + ex.what();
            } catch (...) {
                out.phase = InstallServicePhase::Failed;
                out.error = "install_service_exception";
            }
            {
                std::lock_guard lock(mutex);
                active.reset();
                snapshot.pending_restart = snapshot.pending_restart || restart;
                if (!stopping && snapshot.generation == c.generation) {
                    out.pending_restart = snapshot.pending_restart;
                    if (c.cancelled->load()) { out.phase = InstallServicePhase::Cancelled; out.error = "cancelled"; }
                    if (out.phase == InstallServicePhase::AwaitingInstall || out.phase == InstallServicePhase::AwaitingPublisherTrust)
                        ready = request->context;
                    snapshot = std::move(out);
                }
            }
            changed.notify_all();
        }
    }
};
InstallService::InstallService(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
InstallService::~InstallService() = default;
uint64_t InstallService::Prepare(std::shared_ptr<const CatalogSnapshot> catalog, std::string id, std::string version) {
    auto context = std::make_shared<Impl::Context>();
    context->catalog = std::move(catalog);
    context->id = std::move(id);
    context->version = std::move(version);
    uint64_t generation;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return impl_->snapshot.generation;
        impl_->CancelWork();
        impl_->Reset(InstallServicePhase::Preparing);
        generation = context->generation = impl_->snapshot.generation;
        impl_->snapshot.id = context->id;
        impl_->snapshot.version = context->version;
        impl_->pending = Impl::Request{Impl::Command::Prepare, std::move(context), false, {}};
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return generation;
}
bool InstallService::ConfirmPublisherTrust(uint64_t generation, bool approved) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->snapshot.generation != generation ||
            impl_->snapshot.phase != InstallServicePhase::AwaitingPublisherTrust || !impl_->ready) return false;
        impl_->pending = Impl::Request{Impl::Command::Trust, std::move(impl_->ready), approved, {}};
        impl_->snapshot.phase = InstallServicePhase::Preparing;
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return true;
}
bool InstallService::ConfirmInstall(uint64_t generation, InstallConsent consent) {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || impl_->snapshot.generation != generation ||
            impl_->snapshot.phase != InstallServicePhase::AwaitingInstall || !impl_->ready) return false;
        impl_->pending = Impl::Request{Impl::Command::Install, std::move(impl_->ready), false, consent};
        impl_->snapshot.phase = InstallServicePhase::Committing;
    }
    impl_->wake.notify_one();
    impl_->changed.notify_all();
    return true;
}
uint64_t InstallService::Cancel() {
    uint64_t generation;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return impl_->snapshot.generation;
        impl_->CancelWork();
        impl_->Reset(InstallServicePhase::Cancelled, "cancelled");
        generation = impl_->snapshot.generation;
    }
    impl_->changed.notify_all();
    return generation;
}
InstallServiceSnapshot InstallService::GetSnapshot() const { std::lock_guard lock(impl_->mutex); return impl_->snapshot; }
void InstallService::Stop() { impl_->Stop(); }
#ifdef PULSE_EXTENSION_INSTALL_SERVICE_TESTING
InstallService::InstallService(Config config, Operations ops) {
    Impl::Operations operations;
    operations.download = std::move(ops.download);
    operations.review_trust = std::move(ops.review_trust);
    operations.commit_trust = std::move(ops.commit_trust);
    operations.review_install = std::move(ops.review_install);
    operations.commit_install = std::move(ops.commit_install);
    impl_ = std::make_unique<Impl>(std::move(config), std::move(operations));
}
bool InstallService::WaitForPhase(uint64_t generation, InstallServicePhase phase, std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] {
        return impl_->snapshot.generation > generation ||
            (impl_->snapshot.generation == generation && impl_->snapshot.phase == phase);
    });
}
bool InstallService::WaitForPendingRestart(std::chrono::milliseconds timeout) {
    std::unique_lock lock(impl_->mutex);
    return impl_->changed.wait_for(lock, timeout, [&] { return impl_->snapshot.pending_restart; });
}
#endif
} // namespace pulse::extensions::store
