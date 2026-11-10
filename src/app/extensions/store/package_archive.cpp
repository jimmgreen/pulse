#include "package_archive.h"
#include "../package_revision.h"
#include "../package_signature.h"
#include "../native_image_verify.h"
#include "../../../common/json_value.h"
#include <algorithm>
#include <array>
#include <map>
#include <set>
#include <type_traits>

namespace pulse::extensions::store {
struct VerifiedPackage::Data {
    Manifest declaration;
    std::string revision, manifest, signature, publisher, key;
    std::vector<PackageFile> files;
};
const Manifest& VerifiedPackage::Declaration() const { return data_->declaration; }
const std::string& VerifiedPackage::Revision() const { return data_->revision; }
const std::string& VerifiedPackage::ManifestBytes() const { return data_->manifest; }
const std::string& VerifiedPackage::SignatureBytes() const { return data_->signature; }
const std::string& VerifiedPackage::PublisherId() const { return data_->publisher; }
const std::string& VerifiedPackage::PublisherKey() const { return data_->key; }
const std::vector<PackageFile>& VerifiedPackage::Files() const { return data_->files; }
namespace {
constexpr size_t kFile = 64u * 1024 * 1024, kTotal = 256u * 1024 * 1024;
constexpr size_t kV1 = 256u * 1024, kArchive = kTotal + kV1;
uint16_t U16(std::string_view s, size_t p) {
    return static_cast<uint16_t>(static_cast<unsigned char>(s[p]) | (static_cast<unsigned char>(s[p + 1]) << 8));
}
uint32_t U32(std::string_view s, size_t p) { return U16(s, p) | (static_cast<uint32_t>(U16(s, p + 2)) << 16); }
std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}
bool SafeName(std::string_view s) {
    if (s.empty() || s.size() > 128 || s.back() == '.') return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); };
    if (!alnum(s.front())) return false;
    for (char c : s) if (!alnum(c) && c != '.' && c != '_' && c != '-') return false;
    const auto stem = Lower(std::string(s.substr(0, s.find('.'))));
    return stem != "con" && stem != "prn" && stem != "aux" && stem != "nul" &&
        !(stem.size() == 4 && (stem.starts_with("com") || stem.starts_with("lpt")) && stem[3] >= '1' && stem[3] <= '9');
}
bool Extras(std::string_view s) {
    for (size_t p = 0; p < s.size();) {
        if (s.size() - p < 4) return false;
        const auto tag = U16(s, p), len = U16(s, p + 2);
        p += 4;
        // Unix extra fields can encode link targets independently of the central
        // regular-file mode. Never let the reader reinterpret those as payloads.
        if (len > s.size() - p || tag == 1 || (tag == 0x000d && len > 12) || tag == 0x756e) return false;
        p += len;
    }
    return true;
}
struct Member { std::string name; uint32_t crc, compressed, size, offset; uint16_t flags, method; };
bool Directory(std::string_view s, std::vector<Member>& members) {
    if (s.size() < 22 || U32(s, 0) != 0x04034b50) return false;
    const size_t low = s.size() > 65557 ? s.size() - 65557 : 0;
    size_t end = s.size() - 22;
    for (;;) {
        if (U32(s, end) == 0x06054b50) break;
        if (end == low) return false;
        --end;
    }
    const size_t count = U16(s, end + 10), size = U32(s, end + 12), offset = U32(s, end + 16);
    if (U16(s, end + 4) || U16(s, end + 6) || count < 2 || count > 66 ||
        U16(s, end + 8) != count || size > 16384 || offset > end || size != end - offset ||
        U16(s, end + 20) != s.size() - end - 22) return false;
    size_t p = offset;
    std::set<std::string> seen;
    uint64_t total = 0;
    for (size_t i = 0; i < count; ++i) {
        if (p > end || end - p < 46 || U32(s, p) != 0x02014b50) return false;
        const size_t n = U16(s, p + 28), x = U16(s, p + 30), c = U16(s, p + 32);
        if (46 + n + x + c > end - p) return false;
        Member m{std::string(s.substr(p + 46, n)), U32(s, p + 16), U32(s, p + 20), U32(s, p + 24), U32(s, p + 42), U16(s, p + 8), U16(s, p + 10)};
        const auto attr = U32(s, p + 38), kind = (attr >> 16) & 0170000;
        if (!SafeName(m.name) || !seen.insert(Lower(m.name)).second || U16(s, p + 34) ||
            (attr & 0x10) || (kind != 0 && kind != 0100000) || (m.flags & ~uint16_t{0x080e}) ||
            (m.method != 0 && m.method != 8) || (m.method == 0 && (m.flags & 6)) ||
            m.size == UINT32_MAX || m.compressed > kArchive || m.offset >= offset ||
            !Extras(s.substr(p + 46 + n, x))) return false;
        const size_t limit = m.name == "manifest.json" ? kMaxManifestBytes : m.name == "package.json" ? 16384 : kFile;
        if (m.size > limit) return false;
        if (m.name != "manifest.json" && m.name != "package.json") total += m.size;
        if (total > kTotal) return false;
        members.push_back(std::move(m));
        p += 46 + n + x + c;
    }
    // Case folding is only for collision detection; required JSON names are exact.
    const auto has_exact = [&](std::string_view name) {
        return std::any_of(members.begin(), members.end(), [&](const auto& member) { return member.name == name; });
    };
    if (p != end || !has_exact("manifest.json") || !has_exact("package.json")) return false;
    std::sort(members.begin(), members.end(), [](const auto& a, const auto& b) { return a.offset < b.offset; });
    if (members.front().offset != 0) return false;
    for (size_t i = 0; i < members.size(); ++i) {
        const auto& m = members[i];
        p = m.offset;
        const size_t next = i + 1 == members.size() ? offset : members[i + 1].offset;
        if (p > next || next - p < 30 || U32(s, p) != 0x04034b50 || U16(s, p + 6) != m.flags || U16(s, p + 8) != m.method) return false;
        const size_t n = U16(s, p + 26), x = U16(s, p + 28);
        if (30 + n + x > next - p || s.substr(p + 30, n) != m.name || !Extras(s.substr(p + 30 + n, x))) return false;
        if (!(m.flags & 8) && (U32(s, p + 14) != m.crc || U32(s, p + 18) != m.compressed || U32(s, p + 22) != m.size)) return false;
        p += 30 + n + x;
        if (m.compressed > next - p) return false;
        p += m.compressed;
        if (m.flags & 8) {
            if (next - p == 16 && U32(s, p) == 0x08074b50) p += 4;
            if (next - p != 12 || U32(s, p) != m.crc || U32(s, p + 4) != m.compressed || U32(s, p + 8) != m.size) return false;
            p += 12;
        }
        if (p != next) return false;
    }
    return true;
}
struct archive;
struct archive_entry;
struct ReaderApi {
    HMODULE module{};
    archive* (*create)(){};
    int (*format)(archive*){};
    int (*open)(archive*, const void*, size_t){};
    int (*next)(archive*, archive_entry**){};
    int64_t (*read)(archive*, void*, size_t){};
    int (*free)(archive*){};
    const char* (*name)(archive_entry*){};
    int64_t (*size)(archive_entry*){};
    unsigned (*type)(archive_entry*){};
    const char* (*symlink)(archive_entry*){};
    const char* (*hardlink)(archive_entry*){};
    bool ready = false;
    ReaderApi() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* symbol) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, symbol));
            return fn != nullptr;
        };
        ready = load(create, "archive_read_new") && load(format, "archive_read_support_format_zip") &&
            load(open, "archive_read_open_memory") && load(next, "archive_read_next_header") && load(read, "archive_read_data") &&
            load(free, "archive_read_free") && load(name, "archive_entry_pathname") && load(size, "archive_entry_size") &&
            load(type, "archive_entry_filetype") && load(symlink, "archive_entry_symlink") && load(hardlink, "archive_entry_hardlink");
    }
    ~ReaderApi() { if (module) FreeLibrary(module); }
};
uint32_t Crc(uint32_t crc, std::string_view s) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            t[i] = c;
        }
        return t;
    }();
    for (unsigned char c : s) crc = table[(crc ^ c) & 255] ^ (crc >> 8);
    return crc;
}
bool Decode(std::string_view bytes, const std::vector<Member>& members, const std::atomic<bool>& cancel,
    std::map<std::string, std::string>& files, std::string& error) {
    static const ReaderApi api;
    auto fail = [&](const char* code) { error = code; return false; };
    if (!api.ready) return fail("unsupported_system_archive_reader (archiveint.dll unavailable; Windows 8.1 unsupported)");
    struct Handle { const ReaderApi& api; archive* value; ~Handle() { if (value) api.free(value); } } h{api, api.create()};
    if (!h.value || api.format(h.value) != 0 || api.open(h.value, bytes.data(), bytes.size()) != 0) return fail("invalid_zip_stream");
    std::array<char, 65536> buffer{};
    for (size_t i = 0; i < members.size(); ++i) {
        if (cancel.load()) return fail("cancelled");
        archive_entry* entry = nullptr;
        if (api.next(h.value, &entry) != 0 || !entry) return fail("invalid_zip_header");
        const char* name = api.name(entry);
        const auto it = std::find_if(members.begin(), members.end(), [&](const auto& m) { return name && m.name == name; });
        if (it == members.end() || files.contains(it->name) || api.size(entry) != it->size ||
            api.type(entry) != 0100000 || api.symlink(entry) || api.hardlink(entry)) return fail("invalid_zip_member");
        std::string data;
        uint32_t crc = UINT32_MAX;
        for (;;) {
            if (cancel.load()) return fail("cancelled");
            const int64_t n = api.read(h.value, buffer.data(), buffer.size());
            if (n < 0) return fail("zip_decompression_or_crc_failed");
            if (!n) break;
            if (static_cast<uint64_t>(n) > buffer.size() || static_cast<uint64_t>(n) > it->size - data.size()) return fail("zip_decompressed_size_limit");
            const std::string_view chunk(buffer.data(), static_cast<size_t>(n));
            crc = Crc(crc, chunk);
            data.append(chunk);
        }
        if (data.size() != it->size || (crc ^ UINT32_MAX) != it->crc) return fail("zip_size_or_crc_mismatch");
        files.emplace(it->name, std::move(data));
    }
    if (cancel.load()) return fail("cancelled");
    archive_entry* extra = nullptr;
    if (api.next(h.value, &extra) != 1) return fail("unexpected_zip_member");
    return true;
}
bool IntegerTokens(std::string_view s) {
    bool quoted = false;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (quoted) { if (c == '\\') ++i; else if (c == '"') quoted = false; }
        else if (c == '"') quoted = true;
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
bool Equal(const json::Value& a, const json::Value& b) {
    if (a.type() != b.type() || a.size() != b.size()) return false;
    if (a.is_object()) {
        for (const auto& k : a.keys()) { const auto* v = b.find(k); if (!v || !Equal(*a.find(k), *v)) return false; }
        return true;
    }
    if (a.is_array()) { for (size_t i = 0; i < a.size(); ++i) if (!Equal(a.at(i), b.at(i))) return false; return true; }
    return json::Serialize(a) == json::Serialize(b);
}
} // namespace
bool VerifyCatalogPackage(std::string_view archive_bytes, std::string_view signature,
    const Extension& extension, const Release& release, const Publisher& publisher,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error) {
    error.clear();
    auto fail = [&](const char* code) { error = code; return false; };
    if (cancelled.load()) return fail("cancelled");
    if (archive_bytes.size() > kArchive || signature.empty() || signature.size() > 16384 ||
        archive_bytes.size() != release.package.size || signature.size() != release.signature.size) return fail("package_asset_size_mismatch");
    if (PackageRevision(archive_bytes) != release.package.sha256 || PackageRevision(signature) != release.signature.sha256) return fail("package_asset_hash_mismatch");
    if (extension.publisher != publisher.id) return fail("package_publisher_mismatch");
    std::vector<Member> members;
    if (!Directory(archive_bytes, members)) return fail("invalid_bounded_zip_directory");
    std::map<std::string, std::string> files;
    if (!Decode(archive_bytes, members, cancelled, files, error)) return false;
    const auto manifest_entry = files.find("manifest.json"), metadata_entry = files.find("package.json");
    if (manifest_entry == files.end() || metadata_entry == files.end()) return fail("missing_required_package_metadata");
    const auto& manifest_bytes = manifest_entry->second;
    const auto revision = PackageRevision(manifest_bytes);
    if (revision != release.manifest_sha256) return fail("manifest_hash_mismatch");
    if (!IntegerTokens(manifest_bytes)) return fail("invalid_manifest_integer");
    auto parsed = ParseManifest(manifest_bytes);
    if (!parsed) { error = "invalid_manifest: " + parsed.error; return false; }
    const auto& manifest = *parsed.manifest;
    auto resources = manifest.resources, expected_resources = release.resources;
    std::sort(resources.begin(), resources.end()); std::sort(expected_resources.begin(), expected_resources.end());
    if (manifest.id != extension.id || resources != expected_resources) return fail("manifest_catalog_identity_mismatch");
    json::Value policy = json::Value::Object(), pins = json::Value::Array(), pin = json::Value::Object();
    pin.set("id", publisher.id); pin.set("publicKey", publisher.public_key); pins.push(std::move(pin));
    policy.set("formatVersion", 1); policy.set("requireSigned", true); policy.set("publishers", std::move(pins));
    if (!VerifyPackageSignature(manifest_bytes, signature, json::Serialize(policy), error)) return false;
    json::Value metadata, root;
    const auto& metadata_bytes = metadata_entry->second;
    if (!IntegerTokens(metadata_bytes) || !json::Parse(metadata_bytes, metadata, nullptr, {16384, 8}) ||
        !json::Parse(manifest_bytes, root, nullptr, {kMaxManifestBytes, 40})) return fail("invalid_package_metadata");
    json::Value expected = json::Value::Object();
    expected.set("format", "pulse-extension-local"); expected.set("formatVersion", manifest.native ? 2 : 1);
    expected.set("apiVersion", manifest.api_version); expected.set("id", manifest.id); expected.set("manifestSha256", revision);
    std::vector<NativeFile> wanted;
    if (manifest.native) {
        const auto& native = *manifest.native;
        wanted.push_back({native.library, native.sha256});
        wanted.insert(wanted.end(), native.files.begin(), native.files.end());
        json::Value n = json::Value::Object(); n.set("library", native.library); n.set("sha256", native.sha256);
        if (!native.files.empty()) n.set("files", *root.find("native")->find("files"));
        expected.set("native", std::move(n));
    } else if (archive_bytes.size() > kV1) return fail("declarative_archive_size_limit");
    if (!Equal(metadata, expected) || files.size() != wanted.size() + 2) return fail("package_metadata_or_member_set_mismatch");
    auto data = std::make_shared<VerifiedPackage::Data>();
    for (const auto& file : wanted) {
        if (cancelled.load()) return fail("cancelled");
        const auto it = files.find(file.path);
        if (it == files.end() || PackageRevision(it->second) != file.sha256) return fail("native_file_hash_mismatch");
        if (Lower(file.path).ends_with(".dll") && !ValidateNativeImageBytes(it->second)) return fail("invalid_native_x64_pe_dll");
        data->files.push_back({file.path, std::move(it->second)});
    }
    data->declaration = std::move(*parsed.manifest); data->revision = revision;
    data->manifest = manifest_bytes; data->signature = signature; data->publisher = publisher.id; data->key = publisher.public_key;
    if (cancelled.load()) return fail("cancelled");
    out.data_ = std::move(data);
    return true;
}
} // namespace pulse::extensions::store
