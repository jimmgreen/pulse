#pragma once
#include "manifest.h"
#include "package_revision.h"
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
    // Aligned with packages; empty for declarations without a native runtime.
    std::vector<std::wstring> native_paths;
    explicit operator bool() const { return error.empty(); }
};
// Startup-only, transactionally reads enabled declarations. Disabled versions are never opened.
PackageStoreResult LoadPackageStore(const std::wstring& directory);

}
