#pragma once
#include "package_archive.h"
#include <atomic>
#include <filesystem>
#include <functional>

namespace pulse::extensions::store {
struct InstallReview {
    std::wstring root;
    std::string id, revision, active_revision, publisher_id;
    std::vector<std::string> added_resources;
    bool native_code = false;
    bool currently_enabled = false;
};
struct InstallConsent {
    bool approved = false;
    bool native_execution = false;
    bool added_resources = false;
    bool enable_new = false;
};
struct InstallResult {
    bool changed = false;
    bool restart_required = false;
    std::string id, revision, previous_revision;
    bool enabled = false;
};
// Worker-only. Root and ancestors must already exist on a local drive, without
// reparse points. Shares Python's .registry.lock byte-range protocol. Publisher
// trust must be explicitly provisioned by the caller; these functions NEVER
// silently add publishers or weaken signature-policy.json.
// Review does not grant authority. Commit rechecks the active revision under
// the lock, refusing stale authorization and requiring explicit native consent.
bool ReviewInstall(const std::filesystem::path& root, const VerifiedPackage& package,
    InstallReview& out, std::string& error);
// Registry rename is the commit point. Failure/cancellation leaves registry
// untouched; verified immutable revisions may remain for retry/rollback. Output
// is unchanged on failure. Same-revision installs verify existing disk bytes.
bool CommitInstall(const std::filesystem::path& root, const VerifiedPackage& package,
    const InstallReview& review, const InstallConsent& consent,
    const std::atomic<bool>& cancelled, InstallResult& out, std::string& error);
#ifdef PULSE_EXTENSION_INSTALL_TRANSACTION_TESTING
namespace testing {
using BeforeCommit = std::function<void()>;
struct InstallHooks {
    BeforeCommit before_commit;
    std::function<void(const std::filesystem::path&)> before_payload_publish;
    std::function<void(const std::filesystem::path&, unsigned long)> rename_retry;
};
bool CommitInstallWithHooks(const std::filesystem::path& root, const VerifiedPackage& package,
    const InstallReview& review, const InstallConsent& consent,
    const std::atomic<bool>& cancelled, InstallResult& out, std::string& error,
    const InstallHooks& hooks);
bool CommitInstallWithHook(const std::filesystem::path& root, const VerifiedPackage& package,
    const InstallReview& review, const InstallConsent& consent,
    const std::atomic<bool>& cancelled, InstallResult& out, std::string& error,
    const BeforeCommit& before_commit);
}
#endif
}
