#pragma once
#include "package_archive.h"
#include "../../update_transport.h"

namespace pulse::extensions::store {
inline constexpr uint64_t kMaxPackageDownloadBytes = 256ull * 1024 * 1024 + 256ull * 1024;
inline constexpr uint64_t kMaxPackageDownloadSignatureBytes = 16ull * 1024;
struct PackageDownload {
    VerifiedPackage package;
    std::wstring package_url, signature_url; // Actual successful source URLs.
    app::UpdateSource requested_source = app::UpdateSource::Automatic;
};

// Synchronous, blocking WORKER-ONLY API. catalog MUST be an unmodified result of
// LoadVerifiedCatalog; this function cannot authenticate a caller-built Catalog.
// Selects extension/release/publisher from that same catalog by exact id/version.
// Captures CurrentUpdateSource once. Each asset independently falls back in source
// order, discarding partial/invalid responses. Both exact signed sizes/hashes must
// pass before strict in-memory VerifyCatalogPackage; never writes disk, changes
// publisher policy, or executes DLLs. Failure/cancellation leaves out unchanged.
// Callers must not mutate inputs or share out/error concurrently during the call.
// Errors are diagnostics, not localized UI text. Cancellation is checked at stream
// boundaries and before publication; it never triggers another source attempt.
bool FetchCatalogPackage(const Catalog& catalog, std::string_view extension_id,
    std::string_view version, const std::atomic<bool>& cancelled, PackageDownload& out,
    std::string& error, const app::UpdateResponseReader& read = app::ReadUpdateResponse);
} // namespace pulse::extensions::store
