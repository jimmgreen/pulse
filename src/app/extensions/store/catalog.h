#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::extensions::store {
inline constexpr size_t kMaxCatalogBytes = 4u * 1024u * 1024u;
inline constexpr size_t kMaxCatalogSignatureBytes = 16384;
inline constexpr size_t kMaxCatalogPublishers = 512;
inline constexpr size_t kMaxCatalogExtensions = 2048;
inline constexpr size_t kMaxCatalogReleases = 8192;
inline constexpr std::string_view kOfficialCatalogPublisher =
    "86f9a10e981d16ac677eb11b9c45e82bf38a7e00c0d3d271e84ef9e0d7a4762b";

// All text is validated UTF-8. An absent optional string is empty.
struct LocalizedText { std::string zh, en, tw; };
struct Asset { std::string url; uint64_t size = 0; std::string sha256; };
struct Release {
    std::string version, released, min_pulse, manifest_sha256;
    int sdk_preview = 0;
    std::vector<std::string> resources;
    Asset package, signature;
    LocalizedText notes;
};
struct Publisher {
    std::string id, name, public_key, url;
    bool official = false;
};
struct Extension {
    std::string id, publisher;
    LocalizedText name, summary, description;
    std::string category, icon_glyph, icon_color, license, homepage, source;
    std::vector<std::string> tags;
    std::vector<Release> versions; // Strictly newest first, numeric version order.
};
struct Catalog {
    std::string generated;
    std::vector<Publisher> publishers;
    std::vector<Extension> extensions;
};

// No I/O. Verifies the EXACT index bytes with the built-in official key, then
// validates catalog.py's v1 schema. Unknown fields are rejected. On failure out
// is unchanged and error contains a diagnostic (not localized UI text).
// This authenticates metadata only, not downloaded assets. No freshness/rollback
// guarantee: callers must enforce any cache-age policy independently.
bool LoadVerifiedCatalog(std::string_view index, std::string_view signature,
    Catalog& out, std::string& error);

#ifdef PULSE_EXTENSION_CATALOG_TESTING
namespace testing {
// Only compiled into isolated tests; production trust cannot be supplied by data.
bool LoadVerifiedCatalogWithKey(std::string_view index, std::string_view signature,
    std::string_view trusted_pin, std::string_view trusted_public_key,
    Catalog& out, std::string& error);
}
#endif
} // namespace pulse::extensions::store
