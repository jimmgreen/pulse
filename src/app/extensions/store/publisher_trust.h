#pragma once
#include "package_archive.h"
#include <atomic>
#include <filesystem>

namespace pulse::extensions::store {
struct PublisherTrustReview {
    std::wstring root;
    std::string package_revision, publisher_id, policy_revision;
    bool store_exists = false;
    bool already_trusted = false;
};
struct PublisherTrustResult {
    bool changed = false;
    bool root_created = false;
};
// Worker-only. Does not grant trust or create a missing root. Existing stores
// use the common registry lock (which may initialize .registry.lock). The UI
// must display this verified package's publisher fingerprint and request explicit
// publisher trust. Catalog authentication is NOT this consent.
bool ReviewPublisherTrust(const std::filesystem::path& root, const VerifiedPackage& package,
    PublisherTrustReview& out, std::string& error);
// Explicit trust action, separate from installation/native execution consent.
// Creates missing root directories only after approval; verifies the exact review
// and preserves every existing policy setting/pin. Never overwrites malformed or
// full policy, never removes a publisher, never enables/installs a package. The
// atomic policy rename is the commit point. Failure/cancellation preserves policy
// and output; explicitly created empty directories/lock may remain for retry.
bool CommitPublisherTrust(const std::filesystem::path& root, const VerifiedPackage& package,
    const PublisherTrustReview& review, bool approved, const std::atomic<bool>& cancelled,
    PublisherTrustResult& out, std::string& error);
}
