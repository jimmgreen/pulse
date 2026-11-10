#pragma once
#include "../app/extensions/store/package_archive.h"
#include "../app/extensions/package_revision.h"
#include "../common/json_value.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <functional>
#include <stdexcept>

namespace package_fixture {
using namespace pulse::extensions::store;
using pulse::json::Value;
using pulse::extensions::PackageRevision;
inline void Require(bool ok) { if (!ok) throw std::runtime_error("fixture CNG failure"); }
inline std::string Hex(const std::vector<UCHAR>& b) {
    std::string s; constexpr char h[] = "0123456789abcdef";
    for (auto c : b) { s += h[c >> 4]; s += h[c & 15]; } return s;
}
struct Key {
    BCRYPT_ALG_HANDLE algorithm{}; BCRYPT_KEY_HANDLE key{};
    Publisher publisher;
    Key() {
        Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_RSA_ALGORITHM, nullptr, 0) >= 0);
        Require(BCryptGenerateKeyPair(algorithm, &key, 2048, 0) >= 0 && BCryptFinalizeKeyPair(key, 0) >= 0);
        ULONG n = 0; Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, nullptr, 0, &n, 0) >= 0);
        std::vector<UCHAR> b(n); Require(BCryptExportKey(key, nullptr, BCRYPT_RSAPUBLIC_BLOB, b.data(), n, &n, 0) >= 0);
        publisher.public_key = Hex(b); publisher.id = PackageRevision(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
    }
    ~Key() { if (key) BCryptDestroyKey(key); if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0); }
    std::string Sign(std::string_view bytes) const {
        constexpr char prefix[] = "Pulse.Extension.Manifest.Signature\0v1\0";
        std::string payload(prefix, sizeof(prefix) - 1); payload += bytes;
        const auto hex = PackageRevision(payload); std::vector<UCHAR> digest;
        for (size_t i = 0; i < hex.size(); i += 2) digest.push_back(static_cast<UCHAR>(std::stoul(hex.substr(i, 2), nullptr, 16)));
        BCRYPT_PKCS1_PADDING_INFO padding{BCRYPT_SHA256_ALGORITHM}; ULONG n = 0;
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), nullptr, 0, &n, BCRYPT_PAD_PKCS1) >= 0);
        std::vector<UCHAR> sig(n);
        Require(BCryptSignHash(key, &padding, digest.data(), static_cast<ULONG>(digest.size()), sig.data(), n, &n, BCRYPT_PAD_PKCS1) >= 0);
        Value v = Value::Object(); v.set("formatVersion", 1); v.set("algorithm", "RSA-PKCS1-SHA256"); v.set("publisher", publisher.id); v.set("signature", Hex(sig));
        return pulse::json::Serialize(v);
    }
};
inline void W16(std::string& s, size_t p, uint16_t n) { s[p] = static_cast<char>(n); s[p + 1] = static_cast<char>(n >> 8); }
inline void W32(std::string& s, size_t p, uint32_t n) { W16(s, p, static_cast<uint16_t>(n)); W16(s, p + 2, static_cast<uint16_t>(n >> 16)); }
inline uint32_t Crc(std::string_view s) {
    uint32_t c = UINT32_MAX;
    for (unsigned char b : s) { c ^= b; for (int i = 0; i < 8; ++i) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0); }
    return c ^ UINT32_MAX;
}
// Raw DEFLATE stored blocks: no zlib dependency or executable archive tools.
inline std::string Deflate(std::string_view bytes) {
    std::string out;
    do {
        const auto n = static_cast<uint16_t>((std::min)(bytes.size(), size_t{65535}));
        std::string header(5, '\0'); header[0] = bytes.size() <= 65535 ? 1 : 0;
        W16(header, 1, n); W16(header, 3, static_cast<uint16_t>(~n));
        out += header; out.append(bytes.substr(0, n)); bytes.remove_prefix(n);
    } while (!bytes.empty());
    return out;
}
struct ZipFile { std::string name, bytes; uint32_t attr = 0100644u << 16; uint16_t flags = 0, method = 0; std::string extra; };
struct Zip { std::string bytes; std::vector<size_t> local, central; size_t end = 0; };
inline Zip MakeZip(const std::vector<ZipFile>& files) {
    Zip z;
    for (const auto& f : files) {
        z.local.push_back(z.bytes.size()); std::string h(30, '\0');
        W32(h, 0, 0x04034b50); W16(h, 4, 20); W16(h, 6, f.flags); W16(h, 8, f.method);
        const auto packed = f.method == 8 ? Deflate(f.bytes) : f.bytes;
        W32(h, 14, Crc(f.bytes)); W32(h, 18, static_cast<uint32_t>(packed.size())); W32(h, 22, static_cast<uint32_t>(f.bytes.size())); W16(h, 26, static_cast<uint16_t>(f.name.size())); W16(h, 28, static_cast<uint16_t>(f.extra.size()));
        z.bytes += h; z.bytes += f.name; z.bytes += f.extra; z.bytes += packed;
    }
    const size_t start = z.bytes.size();
    for (size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i]; z.central.push_back(z.bytes.size()); std::string h(46, '\0');
        W32(h, 0, 0x02014b50); W16(h, 4, 0x314); W16(h, 6, 20); W16(h, 8, f.flags); W16(h, 10, f.method);
        const auto packed = f.method == 8 ? Deflate(f.bytes) : f.bytes;
        W32(h, 16, Crc(f.bytes)); W32(h, 20, static_cast<uint32_t>(packed.size())); W32(h, 24, static_cast<uint32_t>(f.bytes.size()));
        W16(h, 28, static_cast<uint16_t>(f.name.size())); W16(h, 30, static_cast<uint16_t>(f.extra.size())); W32(h, 38, f.attr); W32(h, 42, static_cast<uint32_t>(z.local[i]));
        z.bytes += h; z.bytes += f.name; z.bytes += f.extra;
    }
    z.end = z.bytes.size(); std::string h(22, '\0'); W32(h, 0, 0x06054b50);
    W16(h, 8, static_cast<uint16_t>(files.size())); W16(h, 10, static_cast<uint16_t>(files.size()));
    W32(h, 12, static_cast<uint32_t>(z.end - start)); W32(h, 16, static_cast<uint32_t>(start)); z.bytes += h; return z;
}
inline std::string Pe() {
    // Complete headers and one empty section; structurally valid, never loaded.
    std::string s(512, '\0'); s[0] = 'M'; s[1] = 'Z'; W32(s, 60, 64); W32(s, 64, 0x4550);
    W16(s, 68, IMAGE_FILE_MACHINE_AMD64); W16(s, 70, 1);
    W16(s, 84, sizeof(IMAGE_OPTIONAL_HEADER64)); W16(s, 86, IMAGE_FILE_DLL | IMAGE_FILE_EXECUTABLE_IMAGE);
    W16(s, 88, IMAGE_NT_OPTIONAL_HDR64_MAGIC); W32(s, 88 + 56, 8192); W32(s, 88 + 60, 512);
    const size_t section = 88 + sizeof(IMAGE_OPTIONAL_HEADER64);
    W32(s, section + 12, 4096);
    return s;
}
struct Fixture {
    std::string manifest, signature; Extension extension; Release release; Publisher publisher; std::vector<ZipFile> files;
    Fixture(const Key& key, int count = 0, std::string dll = Pe(), std::string id = "test.archive", std::string name = "安全包 測試") : publisher(key.publisher) {
        extension.id = std::move(id); extension.publisher = publisher.id;
        extension.name = {"测试", "Test", "測試"};
        Value m = Value::Object(); m.set("apiVersion", 1); m.set("id", extension.id); m.set("name", std::move(name));
        Value resources = Value::Array(); resources.push("settings.own"); m.set("resources", resources); release.resources = {"settings.own"};
        Value metadata = Value::Object(); metadata.set("format", "pulse-extension-local"); metadata.set("formatVersion", count ? 2 : 1);
        metadata.set("apiVersion", 1); metadata.set("id", extension.id);
        if (count) {
            Value native = Value::Object(); native.set("version", 1); native.set("architecture", "windows-x64"); native.set("library", "test.dll"); native.set("sha256", PackageRevision(dll));
            Value n = Value::Object(); n.set("library", "test.dll"); n.set("sha256", PackageRevision(dll)); files.push_back({"test.dll", dll});
            if (count > 1) {
                Value sidecars = Value::Array();
                for (int i = 1; i < count; ++i) {
                    ZipFile f{"data" + std::to_string(i) + ".txt", "中文 data " + std::to_string(i)};
                    Value v = Value::Object(); v.set("path", f.name); v.set("sha256", PackageRevision(f.bytes)); sidecars.push(v); files.push_back(std::move(f));
                }
                native.set("files", sidecars); n.set("files", sidecars);
            }
            m.set("native", native); metadata.set("native", n);
        }
        manifest = pulse::json::Serialize(m); signature = key.Sign(manifest); release.manifest_sha256 = PackageRevision(manifest);
        metadata.set("manifestSha256", release.manifest_sha256);
        files.insert(files.begin(), {"package.json", pulse::json::Serialize(metadata)}); files.insert(files.begin(), {"manifest.json", manifest});
    }
    void Assets(std::string_view bytes) {
        release.package.size = bytes.size(); release.package.sha256 = PackageRevision(bytes);
        release.signature.size = signature.size(); release.signature.sha256 = PackageRevision(signature);
    }
};
inline std::string Policy(const Publisher& publisher) {
    Value policy = Value::Object(), pins = Value::Array(), pin = Value::Object();
    pin.set("id", publisher.id); pin.set("publicKey", publisher.public_key); pins.push(pin);
    policy.set("formatVersion", 1); policy.set("requireSigned", true); policy.set("publishers", pins);
    return pulse::json::Serialize(policy);
}
inline bool Verify(Fixture& fixture, VerifiedPackage& out, std::string& error) {
    auto zip = MakeZip(fixture.files); fixture.Assets(zip.bytes);
    const std::atomic<bool> cancelled = false;
    return VerifyCatalogPackage(zip.bytes, fixture.signature, fixture.extension, fixture.release, fixture.publisher, cancelled, out, error);
}
} // namespace package_fixture
