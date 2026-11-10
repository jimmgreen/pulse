#pragma once
#include "catalog_client.h"
#include "install_transaction.h"
#include "package_download.h"
#include "publisher_trust.h"
#include <chrono>
#include <memory>

namespace pulse::extensions::store {
// Shared UI/worker gate. Invalid versions/SDK values return false; no I/O or throws.
// Host is canonical three-part numeric; catalog minPulse permits zero padding.
bool IsReleaseCompatible(const Release& release, std::string_view host_version, int sdk_preview);

enum class InstallServicePhase {
    Idle, Preparing, AwaitingPublisherTrust, AwaitingInstall, Committing,
    Installed, Failed, Cancelled, Incompatible
};
struct InstallServiceConfig {
    std::filesystem::path store_root; // Absolute; never created by Prepare.
    std::string host_version; // Strict three-part numeric version, from host constants.
    int sdk_preview = 0;
};
struct InstallServiceSnapshot {
    InstallServicePhase phase = InstallServicePhase::Idle;
    uint64_t generation = 0;
    std::string id, version, error;
    std::shared_ptr<const VerifiedPackage> package;
    // Display-only copies. Worker retains distinct originals for commit authority.
    std::shared_ptr<const PublisherTrustReview> trust_review;
    std::shared_ptr<const InstallReview> install_review;
    bool pending_restart = false; // Sticky for this service's lifetime, even after Cancel.
    bool enabled = false; // Result of the current generation's successful installation.
};

// One serial worker; no callbacks, HWNDs, network/disk I/O under the state mutex.
// Catalog inputs must be immutable results of the verified catalog service.
// Explicit publisher consent and install/native consent are separate commands.
class InstallService {
public:
    using Config = InstallServiceConfig;
    explicit InstallService(Config config);
    // Stop only signals. Destruction joins and MUST occur off the active UI thread.
    // Do not destroy on the worker or race destruction against API calls.
    ~InstallService();
    InstallService(const InstallService&) = delete;
    InstallService& operator=(const InstallService&) = delete;

    // Immediate new Preparing generation, clears prior display data, cancels active
    // work, and replaces pending work. A stopped service cannot restart.
    uint64_t Prepare(std::shared_ptr<const CatalogSnapshot> catalog,
        std::string id, std::string version);
    // Accepted only once for the current generation in the corresponding awaiting
    // phase; acceptance immediately consumes that phase, preventing double clicks.
    bool ConfirmPublisherTrust(uint64_t expected_generation, bool approved);
    bool ConfirmInstall(uint64_t expected_generation, InstallConsent consent);
    uint64_t Cancel();
    InstallServiceSnapshot GetSnapshot() const;
    // Non-joining/idempotent. Cancels active and pending work; advances generation.
    void Stop();

#ifdef PULSE_EXTENSION_INSTALL_SERVICE_TESTING
    struct Operations {
        std::function<bool(const Catalog&, std::string_view, std::string_view,
            const std::atomic<bool>&, PackageDownload&, std::string&)> download;
        std::function<bool(const std::filesystem::path&, const VerifiedPackage&,
            PublisherTrustReview&, std::string&)> review_trust;
        std::function<bool(const std::filesystem::path&, const VerifiedPackage&,
            const PublisherTrustReview&, bool, const std::atomic<bool>&,
            PublisherTrustResult&, std::string&)> commit_trust;
        std::function<bool(const std::filesystem::path&, const VerifiedPackage&,
            InstallReview&, std::string&)> review_install;
        std::function<bool(const std::filesystem::path&, const VerifiedPackage&,
            const InstallReview&, const InstallConsent&, const std::atomic<bool>&,
            InstallResult&, std::string&)> commit_install;
    };
    InstallService(Config config, Operations operations);
    bool WaitForPhase(uint64_t generation, InstallServicePhase phase,
        std::chrono::milliseconds timeout);
    bool WaitForPendingRestart(std::chrono::milliseconds timeout);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace pulse::extensions::store
