#pragma once
#include "manifest.h"
#include "package_revision.h"
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions {
inline constexpr size_t kMaxStorePackages = 32;
inline constexpr size_t kMaxStoreRegistryBytes = 64 * 1024;
struct PackageStoreResult {
    std::vector<Manifest> packages;
    std::string error;
    std::vector<std::string> revisions;
    // Verified startup bytes; trust refresh must not load a newer registry revision.
    std::vector<std::string> manifest_bytes;
    std::vector<std::string> signatures;
    // Aligned with packages: lowercase SHA-256 publisher fingerprint if signed, empty if unsigned.
    std::vector<std::string> publisher_ids;
    // Aligned with packages; empty for declarations without a native runtime.
    std::vector<std::wstring> native_paths;
    // Enabled unsigned packages left out because the policy requires signatures and
    // developer mode is off. Skipped per package; the store itself still loads.
    std::vector<std::string> skipped_unsigned;
    explicit operator bool() const { return error.empty(); }
};
struct PackageStoreOptions {
    // Developer mode: an unsigned package is accepted where the policy would answer
    // signature_required. Every other check (hashes, native image, ids) still applies,
    // and the signature policy file is never written.
    bool allow_unsigned = false;
};
// Startup-only, transactionally reads enabled declarations. Disabled versions are never opened.
PackageStoreResult LoadPackageStore(const std::wstring& directory);
PackageStoreResult LoadPackageStore(const std::wstring& directory, const PackageStoreOptions& options);

// Developer-mode folder packages (devmode::ReadRegistry shadow copies). Never fails as a
// whole: each package either loads or reports an error, aligned with the registry order.
struct DevelopmentPackage {
    std::string id;
    std::string revision;
    std::string error;          // empty when loaded
    std::optional<Manifest> manifest;
    std::wstring native_path;
};
std::vector<DevelopmentPackage> LoadDevelopmentPackages(const std::wstring& store_root,
    const std::vector<std::string>& taken_ids);

}