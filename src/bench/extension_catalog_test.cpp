#include "../app/extensions/store/catalog.h"
#include "../app/extensions/package_revision.h"
#include "../common/json_value.h"
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {
using pulse::json::Value;
using namespace pulse::extensions::store;
void Require(bool condition) { if (!condition) throw std::runtime_error("fixture CNG operation failed"); }
std::string Hex(const std::vector<UCHAR>& bytes) {
    constexpr char digits[] = "0123456789abcdef";
    std::string text;
    for (auto b : bytes) { text += digits[b >> 4]; text += digits[b & 15]; }
    return text;
}
struct FixtureKey {
    BCRYPT_ALG_HANDLE algorithm{};
    BCRYPT_KEY_HANDLE key{};
    std::string public_key, pin;
    FixtureKey() {
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0);
        Require(BCryptGenerateKeyPair(algorithm, &key, 2048, 0) >= 0);
        Require(BCryptFinalizeKeyPair(key, 0) >= 0);
        ULONG size = 0;
        Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &size, 0) >= 0);
        std::vector<UCHAR> blob(size);
        Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, blob.data(), size, &size, 0) >= 0);
        public_key = Hex(blob);
        pin = pulse::extensions::PackageRevision(std::string_view(reinterpret_cast<const char*>(blob.data()), blob.size()));
    }
    ~FixtureKey() { if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    FixtureKey(const FixtureKey&) = delete;
    FixtureKey& operator=(const FixtureKey&) = delete;
    std::string Sign(std::string_view bytes, bool manifest_prefix = false) const {
        constexpr char catalog[] = "Pulse.Extension.Catalog.Signature\0v1\0";
        constexpr char manifest[] = "Pulse.Extension.Manifest.Signature\0v1\0";
        std::string payload = manifest_prefix ? std::string(manifest, sizeof(manifest) - 1) : std::string(catalog, sizeof(catalog) - 1);
        payload.append(bytes);
        const auto hash = pulse::extensions::PackageRevision(payload);
        std::vector<UCHAR> digest;
        for (size_t i = 0; i < hash.size(); i += 2) digest.push_back(static_cast<UCHAR>(std::stoul(hash.substr(i, 2), nullptr, 16)));
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM};
        ULONG size = 0;
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), nullptr, 0, &size, BCRYPT_PAD_PKCS1) >= 0);
        std::vector<UCHAR> signature(size);
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), signature.data(), size, &size, BCRYPT_PAD_PKCS1) >= 0);
        Value result = Value::Object();
        result.set("formatVersion", 1); result.set("algorithm", "RSA-PKCS1-SHA256");
        result.set("publisher", pin); result.set("signature", Hex(signature));
        return pulse::json::Serialize(result);
    }
};
Value Localized() {
    Value v = Value::Object();
    v.set("zh", "压缩与解压"); v.set("en", "7-Zip"); v.set("tw", "壓縮與解壓");
    return v;
}
Value Fixture(const FixtureKey& key) {
    Value root = Value::Object(), publisher = Value::Object(), extension = Value::Object();
    root.set("formatVersion", 1); root.set("generated", "2026-11-09T04:15:38Z");
    publisher.set("id", key.pin); publisher.set("name", "Fixture Publisher"); publisher.set("publicKey", key.public_key);
    publisher.set("official", true); publisher.set("url", "https://github.com/example/pulse");
    Value publishers = Value::Array(); publishers.push(publisher); root.set("publishers", publishers);
    extension.set("formatVersion", 1); extension.set("id", "pulse.7zip"); extension.set("publisher", key.pin);
    extension.set("name", Localized()); extension.set("summary", Localized()); extension.set("description", Localized());
    extension.set("category", "fileops"); extension.set("license", "LGPL-2.1-or-later (7-Zip) with unRAR restriction");
    extension.set("homepage", "https://www.7-zip.org/"); extension.set("source", "https://github.com/example/pulse/tree/main/extensions/7zip");
    Value icon = Value::Object(); icon.set("glyph", "archive"); icon.set("color", "#A16207"); extension.set("icon", icon);
    Value tags = Value::Array(); tags.push("7z"); tags.push("压缩"); extension.set("tags", tags);
    Value release = Value::Object();
    release.set("version", "1.0.0"); release.set("released", "2026-11-09"); release.set("minPulse", "1.0.60");
    release.set("sdkPreview", 3); release.set("manifestSha256", std::string(64, 'a')); release.set("notes", Localized());
    Value resources = Value::Array(); resources.push("settings.own"); release.set("resources", resources);
    for (const auto* name : {"package", "signature"}) {
        Value asset = Value::Object();
        asset.set("url", std::string("https://github.com/example/pulse-extensions/releases/download/pulse.7zip-1.0.0/pulse.7zip-1.0.0") +
            (std::string_view(name) == "package" ? ".pulse-ext" : ".sig.json"));
        asset.set("size", std::string_view(name) == "package" ? 1058405 : 934); asset.set("sha256", std::string(64, 'b'));
        release.set(name, asset);
    }
    Value versions = Value::Array(); versions.push(release); extension.set("versions", versions);
    Value extensions = Value::Array(); extensions.push(extension); root.set("extensions", extensions);
    return root;
}
using Edit = std::function<void(Value&)>;
void EditPublisher(Value& root, const Edit& edit) {
    Value publisher = root.find("publishers")->at(0); edit(publisher);
    Value list = Value::Array(); list.push(publisher); root.set("publishers", list);
}
void EditExtension(Value& root, const Edit& edit) {
    Value extension = root.find("extensions")->at(0); edit(extension);
    Value list = Value::Array(); list.push(extension); root.set("extensions", list);
}
void EditRelease(Value& root, const Edit& edit) {
    EditExtension(root, [&](Value& extension) {
        Value release = extension.find("versions")->at(0); edit(release);
        Value list = Value::Array(); list.push(release); extension.set("versions", list);
    });
}
void EditAsset(Value& root, const Edit& edit) {
    EditRelease(root, [&](Value& release) { Value asset = *release.find("package"); edit(asset); release.set("package", asset); });
}
int passed = 0, failed = 0;
void Check(const char* name, bool ok) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    ok ? ++passed : ++failed;
}
} // namespace
int main() {
    try {
        FixtureKey key, other;
        const Value fixture = Fixture(key);
        auto load = [&](std::string_view bytes, std::string_view sig, Catalog& result, std::string& error) {
            return testing::LoadVerifiedCatalogWithKey(bytes, sig, key.pin, key.public_key, result, error);
        };
        const auto bytes = pulse::json::Serialize(fixture), sig = key.Sign(bytes);
        Catalog result;
        std::string error;
        Check("signed catalog with pulse.7zip and all metadata", load(bytes, sig, result, error) &&
            result.publishers.size() == 1 && result.publishers[0].official && result.extensions.size() == 1 &&
            result.extensions[0].id == "pulse.7zip" && result.extensions[0].name.tw == "壓縮與解壓" &&
            result.extensions[0].versions[0].package.size == 1058405 && result.extensions[0].versions[0].sdk_preview == 3 &&
            result.extensions[0].versions[0].resources == std::vector<std::string>{"settings.own"});
        Check("production refuses injected test signer", !LoadVerifiedCatalog(bytes, sig, result, error) && error == "unknown_catalog_signer");
        Check("exact bytes: appended whitespace invalidates signature", !load(bytes + "\n", sig, result, error));
        Check("wrong signature key", !load(bytes, other.Sign(bytes), result, error));
        Check("manifest domain cannot sign catalog", !load(bytes, key.Sign(bytes, true), result, error));
        Check("missing signature", !load(bytes, "", result, error));
        Check("oversize index", !load(std::string(kMaxCatalogBytes + 1, ' '), sig, result, error));
        Check("oversize signature", !load(bytes, std::string(kMaxCatalogSignatureBytes + 1, ' '), result, error));
        auto reject_raw = [&](const char* name, const std::string& text) {
            Catalog unchanged; unchanged.generated = "sentinel";
            std::string diagnostic;
            Check(name, !load(text, key.Sign(text), unchanged, diagnostic) && !diagnostic.empty() && unchanged.generated == "sentinel");
        };
        auto reject = [&](const char* name, const Edit& edit) {
            auto root = fixture; edit(root); reject_raw(name, pulse::json::Serialize(root));
        };
        reject_raw("duplicate root key", "{\"formatVersion\":1," + bytes.substr(1));
        reject_raw("escaped duplicate key", "{\"format\\u0056ersion\":1," + bytes.substr(1));
        reject_raw("invalid UTF-8", bytes.substr(0, 1) + "\"\xff\":1," + bytes.substr(1));
        reject_raw("trailing garbage", bytes + "x");
        reject_raw("fractional integer token", "{\"formatVersion\":1.0," + bytes.substr(bytes.find(',') + 1));
        reject_raw("rounded fractional integer", "{\"formatVersion\":1.00000000000000001," + bytes.substr(bytes.find(',') + 1));
        reject_raw("exponent integer token", "{\"formatVersion\":1e0," + bytes.substr(bytes.find(',') + 1));
        reject_raw("excess depth", std::string(12, '[') + "0" + std::string(12, ']'));
        reject("unknown root field", [](Value& v) { v.set("extra", 1); });
        reject("boolean formatVersion", [](Value& v) { v.set("formatVersion", true); });
        reject("invalid generated date", [](Value& v) { v.set("generated", "2026-02-29T00:00:00Z"); });
        reject("invalid generated time", [](Value& v) { v.set("generated", "2026-11-09T24:00:00Z"); });
        reject("publishers wrong type", [](Value& v) { v.set("publishers", Value::Object()); });
        reject("too many publishers", [&](Value& v) { Value a = Value::Array(); for (size_t i = 0; i <= kMaxCatalogPublishers; ++i) a.push(fixture.find("publishers")->at(0)); v.set("publishers", a); });
        reject("too many extensions", [&](Value& v) { Value a = Value::Array(); for (size_t i = 0; i <= kMaxCatalogExtensions; ++i) a.push(fixture.find("extensions")->at(0)); v.set("extensions", a); });
        reject("duplicate publisher", [](Value& v) { auto a = *v.find("publishers"); a.push(a.at(0)); v.set("publishers", a); });
        reject("duplicate extension", [](Value& v) { auto a = *v.find("extensions"); a.push(a.at(0)); v.set("extensions", a); });
        reject("publisher fingerprint mismatch", [](Value& v) { EditPublisher(v, [](Value& p) { p.set("id", std::string(64, 'c')); }); });
        reject("malformed publisher key", [](Value& v) { EditPublisher(v, [](Value& p) { p.set("publicKey", "52534131"); }); });
        reject("uppercase publisher hex", [](Value& v) { EditPublisher(v, [](Value& p) { auto s = p.get_string("publicKey"); s[0] = 'A'; p.set("publicKey", s); }); });
        reject("official flag wrong type", [](Value& v) { EditPublisher(v, [](Value& p) { p.set("official", 1); }); });
        reject("unknown official publisher", [&](Value& v) { EditPublisher(v, [&](Value& p) { p.set("id", other.pin); p.set("publicKey", other.public_key); }); });
        reject("unknown extension publisher", [](Value& v) { EditExtension(v, [](Value& e) { e.set("publisher", std::string(64, 'c')); }); });
        reject("reserved pulse namespace", [](Value& v) { EditPublisher(v, [](Value& p) { p.set("official", false); }); });
        reject("invalid extension id", [](Value& v) { EditExtension(v, [](Value& e) { e.set("id", "Pulse.7zip"); }); });
        reject("localized text wrong type", [](Value& v) { EditExtension(v, [](Value& e) { e.set("name", "7-Zip"); }); });
        reject("localized text too long", [](Value& v) { EditExtension(v, [](Value& e) { auto n = Localized(); n.set("zh", std::string(41, 'x')); e.set("name", n); }); });
        reject("localized unknown locale", [](Value& v) { EditExtension(v, [](Value& e) { auto n = Localized(); n.set("fr", "test"); e.set("name", n); }); });
        reject("localized control character", [](Value& v) { EditExtension(v, [](Value& e) { auto n = Localized(); n.set("en", "x\tx"); e.set("name", n); }); });
        reject("unknown category", [](Value& v) { EditExtension(v, [](Value& e) { e.set("category", "other"); }); });
        reject("unknown resource", [](Value& v) { EditRelease(v, [](Value& r) { Value a = Value::Array(); a.push("filesystem.all"); r.set("resources", a); }); });
        reject("duplicate resource", [](Value& v) { EditRelease(v, [](Value& r) { auto a = *r.find("resources"); a.push("settings.own"); r.set("resources", a); }); });
        reject("sdk boolean", [](Value& v) { EditRelease(v, [](Value& r) { r.set("sdkPreview", true); }); });
        reject("sdk overflow", [](Value& v) { EditRelease(v, [](Value& r) { r.set("sdkPreview", 100); }); });
        reject("invalid release date", [](Value& v) { EditRelease(v, [](Value& r) { r.set("released", "2026-04-31"); }); });
        reject("version leading zero", [](Value& v) { EditRelease(v, [](Value& r) { r.set("version", "01.0.0"); }); });
        reject("invalid manifest hash", [](Value& v) { EditRelease(v, [](Value& r) { r.set("manifestSha256", std::string(64, 'A')); }); });
        reject("empty versions", [](Value& v) { EditExtension(v, [](Value& e) { e.set("versions", Value::Array()); }); });
        reject("too many versions", [](Value& v) { EditExtension(v, [](Value& e) { Value a = Value::Array(); for (int i = 0; i < 21; ++i) a.push(e.find("versions")->at(0)); e.set("versions", a); }); });
        reject("duplicate versions", [](Value& v) { EditExtension(v, [](Value& e) { auto a = *e.find("versions"); a.push(a.at(0)); e.set("versions", a); }); });
        reject("ascending versions", [](Value& v) { EditExtension(v, [](Value& e) { auto a = *e.find("versions"); auto newer = a.at(0); newer.set("version", "2.0.0"); a.push(newer); e.set("versions", a); }); });
        reject("size zero", [](Value& v) { EditAsset(v, [](Value& a) { a.set("size", 0); }); });
        reject("size negative", [](Value& v) { EditAsset(v, [](Value& a) { a.set("size", -1); }); });
        reject("size wrong type", [](Value& v) { EditAsset(v, [](Value& a) { a.set("size", "123"); }); });
        reject("package size overflow", [](Value& v) { EditAsset(v, [](Value& a) { a.set("size", 256 * 1024 * 1024 + 256 * 1024 + 1); }); });
        reject("signature size overflow", [](Value& v) { EditRelease(v, [](Value& r) { auto a = *r.find("signature"); a.set("size", 16385); r.set("signature", a); }); });
        reject("invalid asset hash", [](Value& v) { EditAsset(v, [](Value& a) { a.set("sha256", std::string(63, 'a')); }); });
        for (const auto* url : {"http://github.com/a/b/releases/download/v/a.pulse-ext", "https://evil.com/a.pulse-ext",
            "https://github.com@evil.com/a/b/releases/download/v/a.pulse-ext", "https://github.com/a/b/releases/download/../a.pulse-ext",
            "https://github.com/a/b/releases/download/v/a.pulse-ext?x=1", "https://github.com/a/b/releases/download/v/a.zip",
            "https://github.com/a/b/releases/download/v/a%2fpulse-ext"}) {
            reject(url, [&](Value& v) { EditAsset(v, [&](Value& a) { a.set("url", url); }); });
        }
        for (const auto* url : {"file:///tmp/x", "https://localhost/x", "https://127.0.0.1/x", "https://a..com/x", "https://example.com/%0a", "https://example.com/%2e%2e/x", "https://example.com/%xx", "https://example.com\\evil"}) {
            reject(url, [&](Value& v) { EditExtension(v, [&](Value& e) { e.set("homepage", url); }); });
        }
        auto valid = [&](const char* name, const Edit& edit) { auto root = fixture; edit(root); const auto text = pulse::json::Serialize(root); Check(name, load(text, key.Sign(text), result, error)); };
        valid("empty signed catalog", [](Value& v) { v.set("publishers", Value::Array()); v.set("extensions", Value::Array()); });
        valid("nonofficial community publisher", [](Value& v) { EditPublisher(v, [](Value& p) { p.set("official", false); }); EditExtension(v, [](Value& e) { e.set("id", "example.7zip"); }); });
        valid("numeric version sorting 10 before 2", [](Value& v) { EditExtension(v, [](Value& e) { auto r = e.find("versions")->at(0); Value a = Value::Array(); r.set("version", "10.0.0"); a.push(r); r.set("version", "2.0.0"); a.push(r); e.set("versions", a); }); });
        valid("leap date", [](Value& v) { EditRelease(v, [](Value& r) { r.set("released", "2024-02-29"); }); });
        valid("Unicode scalar length not byte length", [](Value& v) { EditExtension(v, [](Value& e) { auto n = Localized(); std::string zh; for (int i = 0; i < 40; ++i) zh += "压"; n.set("zh", zh); e.set("name", n); }); });
        Value bad_sig; Require(pulse::json::Parse(sig, bad_sig));
        bad_sig.set("algorithm", "RSA-PSS-SHA256");
        Check("unsupported signature algorithm", !load(bytes, pulse::json::Serialize(bad_sig), result, error));
        Check("signature duplicate field", !load(bytes, "{\"formatVersion\":1," + sig.substr(1), result, error));
        Require(pulse::json::Parse(sig, bad_sig)); bad_sig.set("signature", std::string(512, '0'));
        Check("corrupted signature", !load(bytes, pulse::json::Serialize(bad_sig), result, error));
        bad_sig.set("publisher", kOfficialCatalogPublisher);
        Check("built-in official key imports and fingerprint matches", !LoadVerifiedCatalog(bytes, pulse::json::Serialize(bad_sig), result, error) && error == "catalog_signature_invalid");
        Require(pulse::json::Parse(sig, bad_sig)); bad_sig.set("signature", std::string(511, 'a'));
        Check("odd-length signature hex", !load(bytes, pulse::json::Serialize(bad_sig), result, error));
        Require(pulse::json::Parse(sig, bad_sig)); bad_sig.set("signature", std::string(512, 'A'));
        Check("noncanonical signature hex", !load(bytes, pulse::json::Serialize(bad_sig), result, error));
        auto duplicate_nested = bytes;
        duplicate_nested.insert(duplicate_nested.find("\"name\":{") + 8, "\"en\":\"duplicate\",");
        reject_raw("duplicate nested localized key", duplicate_nested);
        reject("Unicode whitespace-only text", [](Value& v) { EditExtension(v, [](Value& e) { auto n = Localized(); n.set("en", "　 "); e.set("name", n); }); });
        reject("unknown nested asset field", [](Value& v) { EditAsset(v, [](Value& a) { a.set("extra", false); }); });
        reject("invalid minPulse", [](Value& v) { EditRelease(v, [](Value& r) { r.set("minPulse", "1.0.60-beta"); }); });
        reject("tags wrong type", [](Value& v) { EditExtension(v, [](Value& e) { e.set("tags", "7z"); }); });
        reject("too many tags", [](Value& v) { EditExtension(v, [](Value& e) { Value tags = Value::Array(); for (int i = 0; i < 9; ++i) tags.push("tag"); e.set("tags", tags); }); });
        valid("optional traditional language absent", [](Value& v) { EditExtension(v, [](Value& e) { Value n = Value::Object(); n.set("zh", "测试"); n.set("en", "test"); e.set("description", n); }); });
        valid("empty resource list", [](Value& v) { EditRelease(v, [](Value& r) { r.set("resources", Value::Array()); }); });
        valid("inclusive asset size and SDK limits", [](Value& v) { EditRelease(v, [](Value& r) { r.set("sdkPreview", 99); auto a = *r.find("package"); a.set("size", 256 * 1024 * 1024 + 256 * 1024); r.set("package", a); a = *r.find("signature"); a.set("size", 16384); r.set("signature", a); }); });
        valid("maximum semantic version components", [](Value& v) { EditRelease(v, [](Value& r) { r.set("version", "99999.99999.99999"); }); });
        std::printf("Catalog tests: %d passed, %d failed\n", passed, failed);
        return failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::printf("[FAIL] fixture: %s\n", error.what());
        return 1;
    }
}
