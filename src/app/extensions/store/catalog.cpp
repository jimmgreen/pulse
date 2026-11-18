#include "catalog.h"
#include "../package_revision.h"
#include "../../../common/json_value.h"
#include <algorithm>
#include <array>
#include <cstring>
#include <initializer_list>
#include <regex>
#include <set>
#include <utility>

namespace pulse::extensions::store {
namespace {
using json::Value;
constexpr std::string_view kOfficialKey =
    "52534131000c000003000000800100000000000000000000010001c3cd365f6d023010f91a712db7787536854a746e74be888a6e03bdb8b9d401edb046493f77464fa40b19edde03f52e9025f7709dbb4eb16c7961f21dd6bbb8637d0d5fb232b028a8b700f9a78934a2c3f2c470798dcef2d4f47445772c766ca6b391efd5fee6e71abdfff48fc01e8da7a53ec30653f0054eefe73dbfd8fed48332803306cc3ffca83306939de5b5f37361a989d46ee1c3bb89f0909e8f77188d6a852015f260d55803b71d9fc9a1d5ceaef5c9c45c5711426727e6f5bebecd3918617847bd138222fb79ca53d3f642493e72d211721740ce8c57f269fa2c68b521578f7def24be9c10e18944efd7d9488ce7e892db0f64a1d0d6a359e22eb744843a815fb8e69f9f55272ad8d04a6394cdb458c86173f53ee5a9487597c20467abe912f92b4772d9d9707d93fbeeebc25ec9f691bdce9e62f46d4021e92cccf9e52dfc48b7503c5ff1e4e44996619f99644a6ba8592a31b8cf377665ca58d52dd69687ac869ccdf1c3ebb0387f87632a790eda7a5c551702c590c2c5a961980d";
struct Algorithm {
    BCRYPT_ALG_HANDLE value{};
    ~Algorithm() { if (value) BCryptCloseAlgorithmProvider(value, 0); }
};
struct Key {
    BCRYPT_KEY_HANDLE value{};
    ~Key() { if (value) BCryptDestroyKey(value); }
};
bool Fields(const Value& v, std::initializer_list<std::string_view> required,
    std::initializer_list<std::string_view> optional = {}) {
    if (!v.is_object()) return false;
    for (auto key : required) if (!v.find(key)) return false;
    for (const auto& key : v.keys()) {
        if (std::find(required.begin(), required.end(), key) == required.end() &&
            std::find(optional.begin(), optional.end(), key) == optional.end()) return false;
    }
    return true;
}
const Value& Member(const Value& v, std::string_view key) {
    static const Value missing;
    const auto* found = v.find(key);
    return found ? *found : missing;
}
bool Integer(const Value& v, int64_t low, int64_t high, int64_t* result = nullptr) {
    int64_t n = 0;
    if (!v.get_int(n, low, high)) return false;
    if (result) *result = n;
    return true;
}
// The shared DOM stores numbers as doubles. All numeric catalog fields are
// JSON integers: do not accidentally accept 1.0, 1e0 or rounded fractions.
bool IntegerTokens(std::string_view s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) {
            if (c == '\\') ++i;
            else if (c == '"') quoted = false;
        } else if (c == '"') quoted = true;
        else if (c == '-' || (c >= '0' && c <= '9')) {
            while (i + 1 < s.size()) {
                const char next = s[i + 1];
                if (next == '.' || next == 'e' || next == 'E') return false;
                if (next < '0' || next > '9') break;
                ++i;
            }
        }
    }
    return true;
}
bool Parse(std::string_view bytes, Value& v, size_t max_bytes, size_t depth) {
    return bytes.size() <= max_bytes && IntegerTokens(bytes) &&
        json::Parse(bytes, v, nullptr, {max_bytes, depth});
}
bool Match(std::string_view text, const char* pattern) {
    return std::regex_match(text.begin(), text.end(), std::regex(pattern));
}
bool Hash(std::string_view text) {
    return text.size() == 64 && std::all_of(text.begin(), text.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}
bool Hex(std::string_view text, std::vector<UCHAR>& bytes) {
    if (text.empty() || text.size() > 8192 || text.size() % 2) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        const int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0) return false;
        bytes.push_back(static_cast<UCHAR>((a << 4) | b));
    }
    return true;
}
bool PublicKey(std::string_view text, std::vector<UCHAR>& bytes, std::string& pin) {
    if (!Hex(text, bytes) || bytes.size() < sizeof(BCRYPT_RSAKEY_BLOB)) return false;
    BCRYPT_RSAKEY_BLOB h{};
    std::memcpy(&h, bytes.data(), sizeof(h));
    if (h.Magic != BCRYPT_RSAPUBLIC_MAGIC || h.BitLength < 2048 || h.BitLength > 4096 ||
        h.BitLength % 8 || h.cbModulus != h.BitLength / 8 || h.cbPublicExp < 1 || h.cbPublicExp > 8 ||
        h.cbPrime1 || h.cbPrime2 || bytes.size() != sizeof(h) + h.cbPublicExp + h.cbModulus) return false;
    Algorithm algorithm;
    Key key;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
        BCryptImportKeyPair(algorithm.value, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key.value,
            bytes.data(), static_cast<ULONG>(bytes.size()), 0) < 0) return false;
    pin = PackageRevision(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
    return Hash(pin);
}
bool Text(const Value& value, size_t limit, std::string& out) {
    if (!value.is_string()) return false;
    const auto& s = value.as_string();
    size_t count = 0;
    bool nonspace = false;
    for (size_t i = 0; i < s.size();) {
        const auto first = static_cast<unsigned char>(s[i++]);
        uint32_t cp = first;
        size_t continuation = 0;
        if (first >= 0xf0) { cp &= 7; continuation = 3; }
        else if (first >= 0xe0) { cp &= 15; continuation = 2; }
        else if (first >= 0xc0) { cp &= 31; continuation = 1; }
        // Parse already validated UTF-8; count Unicode scalars, like Python len.
        for (size_t j = 0; j < continuation; ++j) cp = (cp << 6) | (static_cast<unsigned char>(s[i++]) & 63);
        ++count;
        if (cp < 32 && cp != '\n') return false;
        const bool whitespace = cp == ' ' || cp == '\n' || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
            (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 || cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (!whitespace) nonspace = true;
    }
    if (!nonspace || count > limit) return false;
    out = s;
    return true;
}
bool Localized(const Value& v, size_t limit, LocalizedText& out) {
    return Fields(v, {"zh", "en"}, {"tw"}) && Text(Member(v, "zh"), limit, out.zh) &&
        Text(Member(v, "en"), limit, out.en) && (!v.find("tw") || Text(Member(v, "tw"), limit, out.tw));
}
bool Https(std::string_view s) {
    if (s.size() > 2048 || !Match(s, R"(https://[A-Za-z0-9.-]+(/[A-Za-z0-9._~%/+-]*)?)")) return false;
    // Tighten the tools' broad HTTPS regex: no malformed hosts, local names,
    // dot traversal or encoded separators/controls when handed to a URL opener.
    const auto host_end = s.find('/', 8);
    const auto host = s.substr(8, host_end == s.npos ? s.size() - 8 : host_end - 8);
    if (host.size() > 253 || host.find('.') == host.npos) return false;
    size_t start = 0;
    bool numeric = true;
    while (start < host.size()) {
        const auto end = host.find('.', start);
        const auto label = host.substr(start, end == host.npos ? host.size() - start : end - start);
        if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') return false;
        for (char c : label) if (c < '0' || c > '9') numeric = false;
        if (end == host.npos) break;
        start = end + 1;
        if (start == host.size()) return false;
    }
    std::string lowercase_host(host);
    for (char& c : lowercase_host) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (numeric || lowercase_host.ends_with(".localhost") || lowercase_host.ends_with(".local")) return false;
    if (host_end == s.npos) return true;
    const auto path = s.substr(host_end);
    if (path.find("/../") != path.npos || path.find("/./") != path.npos || path.ends_with("/..") || path.ends_with("/.")) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] != '%') continue;
        if (i + 2 >= path.size()) return false;
        const auto hex = path.substr(i + 1, 2);
        if (!Match(hex, "[0-9A-Fa-f]{2}")) return false;
        const auto code = static_cast<unsigned int>(std::stoul(std::string(hex), nullptr, 16));
        if (code <= 32 || code == 127 || code == '/' || code == '\\' || code == '.' || code == '%' || code == ':' || code == '@') return false;
        i += 2;
    }
    return true;
}
bool OptionalUrl(const Value& v, std::string_view key, std::string& out) {
    if (!v.find(key)) return true;
    if (!Member(v, key).is_string() || !Https(v.get_string(key))) return false;
    out = v.get_string(key);
    return true;
}
bool Date(std::string_view s) {
    if (!Match(s, "[0-9]{4}-[0-9]{2}-[0-9]{2}")) return false;
    const int year = std::stoi(std::string(s.substr(0, 4)));
    const int month = std::stoi(std::string(s.substr(5, 2)));
    const int day = std::stoi(std::string(s.substr(8, 2)));
    if (!year || month < 1 || month > 12) return false;
    constexpr int days[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    const bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    return day >= 1 && day <= days[month - 1] + (month == 2 && leap ? 1 : 0);
}
bool Generated(std::string_view s) {
    return s.size() == 20 && Date(s.substr(0, 10)) && s[10] == 'T' &&
        Match(s.substr(11), "([01][0-9]|2[0-3]):[0-5][0-9]:[0-5][0-9]Z");
}
bool Version(std::string_view s, std::array<int, 3>& parts) {
    if (!Match(s, R"((0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4})\.(0|[1-9][0-9]{0,4}))")) return false;
    size_t start = 0;
    for (size_t i = 0; i < parts.size(); ++i) {
        const auto end = s.find('.', start);
        parts[i] = std::stoi(std::string(s.substr(start, end == s.npos ? s.size() - start : end - start)));
        start = end + 1;
    }
    return true;
}
bool AssetValue(const Value& v, int64_t limit, std::string_view suffix, Asset& out) {
    int64_t size = 0;
    const auto url = v.get_string("url");
    if (!Fields(v, {"url", "size", "sha256"}) ||
        !Integer(Member(v, "size"), 1, limit, &size) || !Hash(v.get_string("sha256")) ||
        !Match(url, R"(https://github\.com/[A-Za-z0-9-]{1,39}/[A-Za-z0-9._-]{1,100}/releases/download/[A-Za-z0-9._-]{1,100}/[A-Za-z0-9._-]{1,128})") ||
        !Https(url) || !url.ends_with(suffix)) return false;
    out = {url, static_cast<uint64_t>(size), v.get_string("sha256")};
    return true;
}
bool ReleaseValue(const Value& v, Release& out, std::array<int, 3>& version) {
    int64_t sdk = 0;
    if (!Fields(v, {"version", "released", "minPulse", "sdkPreview", "manifestSha256", "resources", "package", "signature", "notes"}) ||
        !Version(v.get_string("version"), version) || !Date(v.get_string("released")) ||
        !Match(v.get_string("minPulse"), R"([0-9]{1,3}\.[0-9]{1,3}\.[0-9]{1,4})") ||
        !Integer(Member(v, "sdkPreview"), 1, 99, &sdk) || !Hash(v.get_string("manifestSha256")) ||
        !Member(v, "resources").is_array() || Member(v, "resources").size() > 1 ||
        !AssetValue(Member(v, "package"), 256 * 1024 * 1024 + 256 * 1024, ".pulse-ext", out.package) ||
        !AssetValue(Member(v, "signature"), 16384, ".sig.json", out.signature) ||
        !Localized(Member(v, "notes"), 600, out.notes)) return false;
    for (const auto& resource : Member(v, "resources").items()) {
        if (!resource.is_string() || resource.as_string() != "settings.own") return false;
        out.resources.push_back(resource.as_string());
    }
    out.version = v.get_string("version"); out.released = v.get_string("released");
    out.min_pulse = v.get_string("minPulse"); out.manifest_sha256 = v.get_string("manifestSha256");
    out.sdk_preview = static_cast<int>(sdk);
    return true;
}
bool ExtensionValue(const Value& v, Extension& out, size_t& releases) {
    if (!Fields(v, {"formatVersion", "id", "publisher", "name", "summary", "description", "category", "icon", "license", "versions"}, {"homepage", "source", "tags"}) ||
        !Integer(Member(v, "formatVersion"), 1, 1) ||
        !Match(v.get_string("id"), R"([a-z0-9][a-z0-9-]{0,30}(\.[a-z0-9][a-z0-9-]{0,30}){1,3})") ||
        !Hash(v.get_string("publisher")) || !Localized(Member(v, "name"), 40, out.name) ||
        !Localized(Member(v, "summary"), 120, out.summary) || !Localized(Member(v, "description"), 1200, out.description) ||
        !Match(v.get_string("category"), "fileops|preview|search|panel|tools") ||
        !Fields(Member(v, "icon"), {"glyph", "color"}) ||
        !Match(Member(v, "icon").get_string("glyph"), "archive|film|image|doc|find|ocr|git|rename|duplicate|compress|cloud|puzzle") ||
        !Match(Member(v, "icon").get_string("color"), "#[0-9A-F]{6}") ||
        !Text(Member(v, "license"), 80, out.license) || !OptionalUrl(v, "homepage", out.homepage) || !OptionalUrl(v, "source", out.source)) return false;
    if (v.find("tags")) {
        const auto& tags = Member(v, "tags");
        if (!tags.is_array() || tags.size() > 8) return false;
        for (const auto& tag : tags.items()) {
            std::string text;
            if (!Text(tag, 20, text)) return false;
            out.tags.push_back(std::move(text));
        }
    }
    const auto& versions = Member(v, "versions");
    if (!versions.is_array() || versions.size() < 1 || versions.size() > 20 || releases + versions.size() > kMaxCatalogReleases) return false;
    releases += versions.size();
    std::array<int, 3> previous{100000, 0, 0};
    for (const auto& value : versions.items()) {
        Release release;
        std::array<int, 3> version{};
        if (!ReleaseValue(value, release, version) || version >= previous) return false;
        previous = version;
        out.versions.push_back(std::move(release));
    }
    out.id = v.get_string("id"); out.publisher = v.get_string("publisher"); out.category = v.get_string("category");
    out.icon_glyph = Member(v, "icon").get_string("glyph"); out.icon_color = Member(v, "icon").get_string("color");
    return true;
}
bool Load(std::string_view index, std::string_view signature, std::string_view trusted_pin,
    std::string_view trusted_key, Catalog& out, std::string& error) {
    error.clear();
    auto fail = [&](const char* message) { error = message; return false; };
    if (index.size() > kMaxCatalogBytes || signature.size() > kMaxCatalogSignatureBytes) return fail("catalog_size_limit");
    Value sig;
    if (!Parse(signature, sig, kMaxCatalogSignatureBytes, 4) ||
        !Fields(sig, {"formatVersion", "algorithm", "publisher", "signature"}) ||
        !Integer(Member(sig, "formatVersion"), 1, 1) || sig.get_string("algorithm") != "RSA-PKCS1-SHA256" ||
        !Hash(sig.get_string("publisher"))) return fail("invalid_catalog_signature_format");
    if (sig.get_string("publisher") != trusted_pin) return fail("unknown_catalog_signer");
    std::vector<UCHAR> public_key, signed_bytes;
    std::string pin;
    if (!PublicKey(trusted_key, public_key, pin) || pin != trusted_pin) return fail("invalid_catalog_trust_key");
    if (!Hex(sig.get_string("signature"), signed_bytes) || signed_bytes.size() < 256 || signed_bytes.size() > 512) return fail("invalid_catalog_signature_format");
    constexpr char prefix[] = "Pulse.Extension.Catalog.Signature\0v1\0";
    std::string payload(prefix, sizeof(prefix) - 1);
    payload.append(index);
    std::vector<UCHAR> digest;
    if (!Hex(PackageRevision(payload), digest) || digest.size() != 32) return fail("catalog_hash_failed");
    Algorithm algorithm;
    Key key;
    if (BCryptOpenAlgorithmProvider(&algorithm.value, BCRYPT_RSA_ALGORITHM, nullptr, 0) < 0 ||
        BCryptImportKeyPair(algorithm.value, nullptr, BCRYPT_RSAPUBLIC_BLOB, &key.value,
            public_key.data(), static_cast<ULONG>(public_key.size()), 0) < 0) return fail("invalid_catalog_trust_key");
    BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
    if (BCryptVerifySignature(key.value, &padding, digest.data(), static_cast<ULONG>(digest.size()),
        signed_bytes.data(), static_cast<ULONG>(signed_bytes.size()), BCRYPT_PAD_PKCS1) < 0) return fail("catalog_signature_invalid");
    Value root;
    if (!Parse(index, root, kMaxCatalogBytes, 10) ||
        !Fields(root, {"formatVersion", "generated", "publishers", "extensions"}) ||
        !Integer(Member(root, "formatVersion"), 1, 1) || !Generated(root.get_string("generated"))) return fail("invalid_catalog_format");
    const auto& publishers = Member(root, "publishers");
    const auto& extensions = Member(root, "extensions");
    if (!publishers.is_array() || publishers.size() > kMaxCatalogPublishers ||
        !extensions.is_array() || extensions.size() > kMaxCatalogExtensions) return fail("catalog_entry_limit_or_type");
    Catalog result;
    result.generated = root.get_string("generated");
    std::set<std::string> pins, ids;
    for (const auto& value : publishers.items()) {
        Publisher publisher;
        std::vector<UCHAR> blob;
        std::string fingerprint;
        if (!Fields(value, {"id", "name", "publicKey", "official"}, {"url"}) ||
            !Text(Member(value, "name"), 60, publisher.name) || !OptionalUrl(value, "url", publisher.url) ||
            !Member(value, "official").is_bool() || !PublicKey(value.get_string("publicKey"), blob, fingerprint) ||
            value.get_string("id") != fingerprint || !pins.insert(fingerprint).second) return fail("invalid_catalog_publisher");
        publisher.id = fingerprint; publisher.public_key = value.get_string("publicKey");
        publisher.official = Member(value, "official").as_bool();
        if (publisher.official && (publisher.id != trusted_pin || publisher.public_key != trusted_key)) return fail("unknown_official_publisher");
        result.publishers.push_back(std::move(publisher));
    }
    size_t releases = 0;
    for (const auto& value : extensions.items()) {
        Extension extension;
        if (!ExtensionValue(value, extension, releases) || !ids.insert(extension.id).second) return fail("invalid_catalog_extension");
        const auto publisher = std::find_if(result.publishers.begin(), result.publishers.end(), [&](const Publisher& p) { return p.id == extension.publisher; });
        if (publisher == result.publishers.end()) return fail("unknown_extension_publisher");
        if (extension.id.starts_with("pulse.") && !publisher->official) return fail("reserved_extension_namespace");
        result.extensions.push_back(std::move(extension));
    }
    out = std::move(result);
    return true;
}
} // namespace
bool LoadVerifiedCatalog(std::string_view index, std::string_view signature, Catalog& out, std::string& error) {
    return Load(index, signature, kOfficialCatalogPublisher, kOfficialKey, out, error);
}
#ifdef PULSE_EXTENSION_CATALOG_TESTING
namespace testing {
bool LoadVerifiedCatalogWithKey(std::string_view index, std::string_view signature,
    std::string_view trusted_pin, std::string_view trusted_public_key, Catalog& out, std::string& error) {
    return Load(index, signature, trusted_pin, trusted_public_key, out, error);
}
}
#endif
} // namespace pulse::extensions::store
