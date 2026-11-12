#pragma once
#include "catalog.h"
#include "../manifest.h"
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::store {
struct PackageFile { std::string name, bytes; };

struct LocalPackageVerifyOptions {
    std::filesystem::path signature_path;
    std::vector<Publisher> known_publishers;
    std::string host_version;
    int sdk_preview = 0;
};

// Immutable, memory-only verification result. Default constructed values are invalid.
// Accessors require a valid value. Copies share immutable storage and are thread safe.
class VerifiedPackage {
public:
    VerifiedPackage() = default;
    explicit operator bool() const noexcept { return static_cast<bool>(data_); }
    const pulse::extensions::Manifest& Declaration() const;
    const std::string& Revision() const;
    const std::string& ManifestBytes() const;
    const std::string& SignatureBytes() const;
    const std::string& PublisherId() const;
    const std::string& PublisherKey() const;
    const std::vector<PackageFile>& Files() const;
private:
    struct Data;
    std::shared_ptr<const Data> data_;
    friend bool VerifyCatalogPackage(std::string_view, std::string_view, const Extension&,
        const Release&, const Publisher&, const std::atomic<bool>&, VerifiedPackage&, std::string&);
    friend bool VerifyLocalPackageInternal(const std::filesystem::path&, const LocalPackageVerifyOptions&,
        std::string_view, std::string_view, const std::atomic<bool>&, VerifiedPackage&, std::string&);
};

// Worker-only; performs no disk extraction and never loads package DLLs. Metadata
// MUST come from the same LoadVerifiedCatalog result (extension/release/publisher
// association is the caller's responsibility). No trust is derived from ZIP data.
// Concurrent calls are independent; caller must not mutate inputs or share out/error
// during a call. Failure/cancellation leaves out unchanged. Win8.1 without the system
// archiveint.dll reader returns an explicit unsupported error. Files excludes JSON.
bool VerifyCatalogPackage(std::string_view archive, std::string_view signature,
    const Extension& extension, const Release& release, const Publisher& publisher,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error);

// Worker-only local .pulse-ext verifier. Rejects relative, UNC, device, directory,
// and reparse-point paths. Probes <archive_path>.sig.json when signature_path is
// empty: if present, verifies cryptographically and fails closed on any error; if
// absent, produces an unsigned VerifiedPackage (PublisherId().empty()).
bool VerifyLocalPackage(const std::filesystem::path& archive_path,
    const LocalPackageVerifyOptions& options,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error);
bool VerifyLocalPackage(const std::filesystem::path& archive_path,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error);

#ifdef PULSE_EXTENSION_LOCAL_PACKAGE_TESTING
namespace testing {
// Test-only seam mirroring LoadVerifiedCatalogWithKey; never compiled into production binaries.
bool VerifyLocalPackageWithOfficialPin(const std::filesystem::path& archive_path,
    const LocalPackageVerifyOptions& options,
    std::string_view official_pin, std::string_view official_public_key,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error);
} // namespace testing
#endif
} // namespace pulse::extensions::store
