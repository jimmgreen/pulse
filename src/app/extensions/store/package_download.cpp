#include "package_download.h"
#include "../package_revision.h"
#include <algorithm>

namespace pulse::extensions::store {
namespace {
bool Hash(std::string_view value) {
    return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == value.npos;
}
bool AssetUrl(std::string_view url, std::string_view suffix) {
    // Match catalog.cpp AssetValue's public GitHub release-asset grammar, including
    // its no-dot-traversal rule. Never expand the catalog's network authorization.
    constexpr std::string_view prefix = "https://github.com/";
    if (!url.starts_with(prefix) || !url.ends_with(suffix)) return false;
    url.remove_prefix(prefix.size());
    const auto component = [&](size_t maximum, bool owner) {
        const auto slash = url.find('/');
        if (slash == url.npos || slash == 0 || slash > maximum) return false;
        const auto part = url.substr(0, slash);
        if (part == "." || part == "..") return false;
        for (char c : part) {
            const bool alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
            if (!alnum && c != '-' && (owner || (c != '.' && c != '_'))) return false;
        }
        url.remove_prefix(slash + 1);
        return true;
    };
    if (!component(39, true) || !component(100, false) || !url.starts_with("releases/download/")) return false;
    url.remove_prefix(std::string_view("releases/download/").size());
    if (!component(100, false) || url.empty() || url.size() > 128 || url == "." || url == "..") return false;
    return url.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-") == url.npos;
}
bool AssetMetadata(const Asset& asset, uint64_t maximum, std::string_view suffix) {
    return asset.size != 0 && asset.size <= maximum && Hash(asset.sha256) && AssetUrl(asset.url, suffix);
}
bool DownloadAsset(const Asset& asset, app::UpdateSource requested_source,
    const std::atomic<bool>& cancelled, const app::UpdateResponseReader& read,
    std::string& bytes, std::wstring& actual_url, std::string& error) {
    const std::wstring url(asset.url.begin(), asset.url.end()); // Validated ASCII.
    for (const auto& source : app::UpdateSourceUrls(url, requested_source)) {
        if (cancelled.load()) { error = "cancelled"; return false; }
        if (source.empty()) continue;
        // Fresh storage and sticky rejection even if an injected reader ignores
        // consume=false and claims success after an overflow or invalid chunk.
        std::string attempt;
        bool rejected = false, observed_cancelled = false;
        const auto consume = [&](const void* data, DWORD count) {
            if (cancelled.load()) { observed_cancelled = true; return false; }
            if (rejected || observed_cancelled) return false;
            if (count > asset.size - attempt.size() || (count && !data)) {
                rejected = true;
                return false;
            }
            if (count) attempt.append(static_cast<const char*>(data), count);
            if (cancelled.load()) { observed_cancelled = true; return false; }
            return true;
        };
        app::UpdateError category = app::UpdateError::None;
        DWORD code = ERROR_SUCCESS;
        const bool complete = read(source, asset.size, cancelled, consume, category, code);
        if (observed_cancelled || cancelled.load() || code == ERROR_CANCELLED) { error = "cancelled"; return false; }
        if (!complete || rejected || attempt.size() != asset.size) {
            error = rejected ? "package_download_response_rejected" :
                !complete ? "package_download_source_failed: " + std::to_string(code) : "package_download_size_mismatch";
            continue;
        }
        const auto hash = PackageRevision(attempt);
        if (cancelled.load()) { error = "cancelled"; return false; }
        if (hash != asset.sha256) { error = "package_download_hash_mismatch"; continue; }
        bytes = std::move(attempt);
        actual_url = source;
        return true;
    }
    return false;
}
} // namespace

bool FetchCatalogPackage(const Catalog& catalog, std::string_view extension_id,
    std::string_view version, const std::atomic<bool>& cancelled, PackageDownload& out,
    std::string& error, const app::UpdateResponseReader& read) {
    const auto requested_source = app::CurrentUpdateSource();
    error.clear();
    const auto fail = [&](const char* diagnostic) { error = diagnostic; return false; };
    if (cancelled.load()) return fail("cancelled");
    const Extension* extension = nullptr;
    for (const auto& item : catalog.extensions) {
        if (cancelled.load()) return fail("cancelled");
        if (item.id != extension_id) continue;
        if (extension) return fail("ambiguous_catalog_extension");
        extension = &item;
    }
    if (!extension) return fail("catalog_extension_not_found");
    const Release* release = nullptr;
    for (const auto& item : extension->versions) {
        if (cancelled.load()) return fail("cancelled");
        if (item.version != version) continue;
        if (release) return fail("ambiguous_catalog_version");
        release = &item;
    }
    if (!release) return fail("catalog_version_not_found");
    const Publisher* publisher = nullptr;
    for (const auto& item : catalog.publishers) {
        if (cancelled.load()) return fail("cancelled");
        if (item.id != extension->publisher) continue;
        if (publisher) return fail("ambiguous_catalog_publisher");
        publisher = &item;
    }
    if (!publisher) return fail("catalog_publisher_not_found");
    if (!AssetMetadata(release->package, kMaxPackageDownloadBytes, ".pulse-ext") ||
        !AssetMetadata(release->signature, kMaxPackageDownloadSignatureBytes, ".sig.json") ||
        !Hash(release->manifest_sha256)) return fail("invalid_package_download_metadata");
    if (!read) return fail("missing_package_response_reader");
    PackageDownload candidate;
    candidate.requested_source = requested_source;
    std::string archive, signature;
    if (!DownloadAsset(release->package, requested_source, cancelled, read, archive, candidate.package_url, error) ||
        !DownloadAsset(release->signature, requested_source, cancelled, read, signature, candidate.signature_url, error)) return false;
    if (cancelled.load()) return fail("cancelled");
    if (!VerifyCatalogPackage(archive, signature, *extension, *release, *publisher, cancelled, candidate.package, error)) return false;
    if (cancelled.load()) return fail("cancelled");
    out = std::move(candidate);
    error.clear();
    return true;
}
} // namespace pulse::extensions::store
