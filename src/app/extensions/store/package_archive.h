#pragma once
#include "catalog.h"
#include "../manifest.h"
#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::store {
struct PackageFile { std::string name, bytes; };

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
} // namespace pulse::extensions::store
