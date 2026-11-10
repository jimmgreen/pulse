#pragma once
#include "catalog.h"
#include "../../update_transport.h"
#include <atomic>
#include <filesystem>
#include <functional>

namespace pulse::extensions::store {
inline constexpr std::wstring_view kCatalogIndexUrl =
    L"https://github.com/jimmgreen/pulse-extensions/releases/download/catalog/index.json";
inline constexpr std::wstring_view kCatalogSignatureUrl =
    L"https://github.com/jimmgreen/pulse-extensions/releases/download/catalog/index.sig.json";
enum class CatalogFetchStatus { Online, Offline, Failed, Cancelled };
struct CatalogSnapshot {
    Catalog catalog;
    std::string index_bytes, signature_bytes;
    bool offline = false;
    bool cache_persisted = false;
    app::UpdateSource requested_source = app::UpdateSource::Automatic;
    std::wstring source; // Actual index URL online; empty for offline cache.
    std::string diagnostic; // Failure/warning details, not localized UI text.
};
// Synchronous, blocking WORKER-ONLY API. Captures CurrentUpdateSource once and
// attempts index/signature as a pair from each source; never mixes responses.
// Empty cache_file disables caching. Otherwise it must be an absolute file path
// in an existing caller-owned directory (no directories are created). Callers
// must serialize updates sharing a cache path; this is not a cross-process store.
// Cache reads always reverify exact bytes with the built-in official key.
// Failed/Cancelled leave out unchanged; cancellation never falls back offline.
// Cancellation is checked at I/O boundaries and immediately before the atomic
// cache commit. Once the commit starts it is a completed operation, not cancelled.
// generated ordering prevents rollback against the verified local baseline only;
// it is NOT trustworthy wall-clock freshness and cannot resist cache deletion.
CatalogFetchStatus FetchCatalog(const std::filesystem::path& cache_file,
    const std::atomic<bool>& cancelled, CatalogSnapshot& out, std::string& error,
    const app::UpdateResponseReader& read = app::ReadUpdateResponse);

#ifdef PULSE_EXTENSION_CATALOG_CLIENT_TESTING
namespace testing {
using CatalogValidator = std::function<bool(std::string_view, std::string_view, Catalog&, std::string&)>;
enum class CatalogCachePoint { BeforeRead, AfterRead, BeforeWrite, AfterWrite, BeforeCommit };
using CatalogCacheHook = std::function<void(CatalogCachePoint)>;
// Test-only trust and cancellation checkpoints; neither exists in production.
CatalogFetchStatus FetchCatalogWithValidator(const std::filesystem::path& cache_file,
    const std::atomic<bool>& cancelled, CatalogSnapshot& out, std::string& error,
    const app::UpdateResponseReader& read, const CatalogValidator& validate,
    const CatalogCacheHook& hook = {});
}
#endif
} // namespace pulse::extensions::store
