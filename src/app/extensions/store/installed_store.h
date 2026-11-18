#pragma once
#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pulse::extensions::store {
struct InstalledEntry {
    std::string id, revision, previous_revision, publisher_id, name;
    std::vector<std::string> resources;
    bool native_code = false, enabled = false;
    std::string error; // Metadata/signature failure; row is still removable.
};
struct InstalledSnapshot {
    std::filesystem::path root;
    std::string registry_revision; // SHA-256 of exact registry bytes; empty if absent.
    std::vector<InstalledEntry> entries;
    bool missing = false; // Root absent; reading never creates it.
};
enum class StoreChangeAction { Enable, Disable, Uninstall, Rollback };
struct StoreChangeReview {
    std::filesystem::path root;
    std::string id, registry_revision, active_revision, previous_revision, publisher_id;
    std::string store_identity, policy_revision; // Directory identity + exact trust-policy digest.
    StoreChangeAction action = StoreChangeAction::Disable;
    bool enabled = false, native_code = false;
    std::vector<std::string> added_resources;
    bool operator==(const StoreChangeReview&) const = default;
};
struct StoreChangeConsent { bool approved = false, native_execution = false, added_resources = false; };
struct StoreChangeResult { bool changed = false, restart_required = false; };
// Worker-only. Uses install transaction's pinned Session and .registry.lock.
// Snapshot verifies metadata only; Enable/Rollback verify all target payload/PE bytes.
// Failure leaves out unchanged. Uninstall retains immutable files and trust policy.
bool ReadInstalledStore(const std::filesystem::path& root, InstalledSnapshot& out, std::string& error);
bool ReviewStoreChange(const std::filesystem::path& root, const std::string& id,
    StoreChangeAction action, StoreChangeReview& out, std::string& error);
bool CommitStoreChange(const std::filesystem::path& root, const StoreChangeReview& review,
    const StoreChangeConsent& consent, const std::atomic<bool>& cancelled,
    StoreChangeResult& out, std::string& error);

enum class InstalledStorePhase { Idle, Loading, Ready, Preparing, AwaitingConfirmation, Committing, Changed, Failed, Cancelled };
struct InstalledStoreServiceSnapshot {
    uint64_t generation = 0;
    InstalledStorePhase phase = InstalledStorePhase::Idle;
    std::shared_ptr<const InstalledSnapshot> installed;
    std::shared_ptr<const StoreChangeReview> review;
    std::string error;
    bool pending_restart = false;
};
// One serial worker, pull-only model. Stop signals; destructor joins and must run
// off the active UI thread. Each Refresh/Prepare cancels and supersedes older work.
class InstalledStoreService {
public:
    InstalledStoreService();
    ~InstalledStoreService();
    InstalledStoreService(const InstalledStoreService&) = delete;
    InstalledStoreService& operator=(const InstalledStoreService&) = delete;
    uint64_t Refresh(std::filesystem::path root);
    uint64_t PrepareChange(std::filesystem::path root, std::string id, StoreChangeAction action);
    bool Confirm(uint64_t generation, StoreChangeConsent consent);
    uint64_t Cancel();
    void Stop();
    InstalledStoreServiceSnapshot GetSnapshot() const;
#ifdef PULSE_EXTENSION_INSTALLED_STORE_TESTING
    struct Operations {
        std::function<decltype(ReadInstalledStore)> read = ReadInstalledStore;
        std::function<decltype(ReviewStoreChange)> review = ReviewStoreChange;
        std::function<decltype(CommitStoreChange)> commit = CommitStoreChange;
    };
    explicit InstalledStoreService(Operations operations);
    bool WaitForPhase(uint64_t generation, InstalledStorePhase phase, std::chrono::milliseconds timeout);
    bool WaitForPendingRestart(std::chrono::milliseconds timeout);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
#ifdef PULSE_EXTENSION_INSTALLED_STORE_TESTING
namespace testing {
bool CommitStoreChangeWithHook(const std::filesystem::path&, const StoreChangeReview&,
    const StoreChangeConsent&, const std::atomic<bool>&, StoreChangeResult&, std::string&,
    const std::function<void()>& before_commit);
}
#endif
} // namespace pulse::extensions::store
