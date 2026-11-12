#include "package_archive.h"
#include "../package_revision.h"
#include "../package_signature.h"
#include "../native_image_verify.h"
#include "../../../common/json_value.h"
#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <type_traits>
#include <utility>

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
constexpr std::string_view kOfficialCatalogPublicKey =
    "52534131000c000003000000800100000000000000000000010001c3cd365f6d023010f91a712db7787536854a746e74be888a6e03bdb8b9d401edb046493f77464fa40b19edde03f52e9025f7709dbb4eb16c7961f21dd6bbb8637d0d5fb232b028a8b700f9a78934a2c3f2c470798dcef2d4f47445772c766ca6b391efd5fee6e71abdfff48fc01e8da7a53ec30653f0054eefe73dbfd8fed48332803306cc3ffca83306939de5b5f37361a989d46ee1c3bb89f0909e8f77188d6a852015f260d55803b71d9fc9a1d5ceaef5c9c45c5711426727e6f5bebecd3918617847bd138222fb79ca53d3f642493e72d211721740ce8c57f269fa2c68b521578f7def24be9c10e18944efd7d9488ce7e892db0f64a1d0d6a359e22eb744843a815fb8e69f9f55272ad8d04a6394cdb458c86173f53ee5a9487597c20467abe912f92b4772d9d9707d93fbeeebc25ec9f691bdce9e62f46d4021e92cccf9e52dfc48b7503c5ff1e4e44996619f99644a6ba8592a31b8cf377665ca58d52dd69687ac869ccdf1c3ebb0387f87632a790eda7a5c551702c590c2c5a961980d";
struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    FileHandle() = default;
    explicit FileHandle(HANDLE h) noexcept : value(h) {}
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    FileHandle(FileHandle&& other) noexcept : value(std::exchange(other.value, INVALID_HANDLE_VALUE)) {}
    FileHandle& operator=(FileHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = std::exchange(other.value, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
bool ParseThreePartVersion(std::string_view text) {
    size_t pos = 0;
    for (int part = 0; part < 3; ++part) {
        const size_t start = pos;
        uint64_t value = 0;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            const auto digit = static_cast<uint64_t>(text[pos++] - '0');
            if (value > ((std::numeric_limits<uint64_t>::max)() - digit) / 10) return false;
            value = value * 10 + digit;
        }
        if (start == pos || (pos - start > 1 && text[start] == '0')) return false;
        if (part < 2 && (pos == text.size() || text[pos++] != '.')) return false;
    }
    return pos == text.size();
}
bool ValidLocalOrdinaryFilePath(const std::filesystem::path& path) {
    const auto& raw = path.native();
    if (raw.size() < 4 || raw.find(L'\0') != std::wstring::npos) return false;
    if (raw.starts_with(L"\\") || raw.starts_with(L"/")) return false;
    const wchar_t drive = raw[0];
    if (!((drive >= L'A' && drive <= L'Z') || (drive >= L'a' && drive <= L'z'))) return false;
    if (raw[1] != L':' || (raw[2] != L'\\' && raw[2] != L'/')) return false;
    if (raw.find(L':', 2) != std::wstring::npos) return false;
    if (!path.is_absolute() || path.root_name().native().size() != 2 || !path.has_filename()) return false;
    for (const auto& component : path) {
        const auto& part = component.native();
        if (part == L"." || part == L"..") return false;
        if (part.size() > 1 && part != path.root_name().native() &&
            part != L"\\" && part != L"/" && (part.back() == L'.' || part.back() == L' ')) return false;
    }
    const UINT drive_type = GetDriveTypeW(path.root_path().c_str());
    return drive_type == DRIVE_FIXED || drive_type == DRIVE_REMOVABLE || drive_type == DRIVE_RAMDISK;
}
bool PinOrdinaryAncestors(const std::filesystem::path& file_path, std::vector<FileHandle>& pins, std::string& error) {
    std::vector<std::filesystem::path> ancestors;
    for (auto dir = file_path.parent_path(); !dir.empty(); dir = dir.parent_path()) {
        ancestors.push_back(dir);
        if (dir == dir.root_path()) break;
    }
    if (ancestors.empty()) { error = "local_package_path_invalid"; return false; }
    for (auto it = ancestors.rbegin(); it != ancestors.rend(); ++it) {
        FileHandle handle(CreateFileW(it->c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
        if (handle.value == INVALID_HANDLE_VALUE) { error = "local_package_open_failed"; return false; }
        BY_HANDLE_FILE_INFORMATION info{};
        if (!GetFileInformationByHandle(handle.value, &info) || GetFileType(handle.value) != FILE_TYPE_DISK) {
            error = "local_package_not_regular_file";
            return false;
        }
        if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = "local_package_reparse_point"; return false; }
        if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) { error = "local_package_not_regular_file"; return false; }
        pins.push_back(std::move(handle));
    }
    return true;
}
enum class LocalReadStatus { Ok, Missing, Failed };
LocalReadStatus ReadOrdinaryLocalBytes(const std::filesystem::path& path, size_t max_bytes,
    const std::atomic<bool>& cancelled, std::string& out_bytes, std::string& error) {
    if (!ValidLocalOrdinaryFilePath(path)) { error = "local_package_path_invalid"; return LocalReadStatus::Failed; }
    std::vector<FileHandle> ancestor_pins;
    if (!PinOrdinaryAncestors(path, ancestor_pins, error)) return LocalReadStatus::Failed;
    FileHandle probe(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr));
    if (probe.value == INVALID_HANDLE_VALUE) {
        const DWORD code = GetLastError();
        if (code == ERROR_FILE_NOT_FOUND) return LocalReadStatus::Missing;
        error = "local_package_open_failed";
        return LocalReadStatus::Failed;
    }
    BY_HANDLE_FILE_INFORMATION probe_info{};
    if (!GetFileInformationByHandle(probe.value, &probe_info) || GetFileType(probe.value) != FILE_TYPE_DISK) {
        error = "local_package_not_regular_file";
        return LocalReadStatus::Failed;
    }
    if (probe_info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) { error = "local_package_reparse_point"; return LocalReadStatus::Failed; }
    if (probe_info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { error = "local_package_is_directory"; return LocalReadStatus::Failed; }

    FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) { error = "local_package_open_failed"; return LocalReadStatus::Failed; }
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file.value, &info) || GetFileType(file.value) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.dwVolumeSerialNumber != probe_info.dwVolumeSerialNumber ||
        info.nFileIndexHigh != probe_info.nFileIndexHigh ||
        info.nFileIndexLow != probe_info.nFileIndexLow) {
        error = "local_package_not_regular_file";
        return LocalReadStatus::Failed;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart <= 0 || static_cast<uint64_t>(size.QuadPart) > max_bytes) {
        error = "package_asset_size_mismatch";
        return LocalReadStatus::Failed;
    }
    std::string buffer(static_cast<size_t>(size.QuadPart), '\0');
    size_t offset = 0;
    while (offset < buffer.size()) {
        if (cancelled.load()) { error = "cancelled"; return LocalReadStatus::Failed; }
        DWORD read_now = 0;
        const auto chunk = static_cast<DWORD>((std::min)(buffer.size() - offset, size_t{65536}));
        if (!ReadFile(file.value, buffer.data() + offset, chunk, &read_now, nullptr) || read_now != chunk) {
            error = "local_package_read_failed";
            return LocalReadStatus::Failed;
        }
        offset += read_now;
    }
    out_bytes = std::move(buffer);
    return LocalReadStatus::Ok;
}
std::string BytesToLowerHex(std::string_view raw) {
    constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(raw.size() * 2);
    for (unsigned char b : raw) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0f]);
    }
    return out;
}
bool LowerHexToBytes(std::string_view hex, std::string& raw_out) {
    if (hex.empty() || (hex.size() % 2) != 0) return false;
    std::string out;
    out.reserve(hex.size() / 2);
    auto nibble = [](char c, unsigned char& v) {
        if (c >= '0' && c <= '9') { v = static_cast<unsigned char>(c - '0'); return true; }
        if (c >= 'a' && c <= 'f') { v = static_cast<unsigned char>(c - 'a' + 10); return true; }
        if (c >= 'A' && c <= 'F') { v = static_cast<unsigned char>(c - 'A' + 10); return true; }
        return false;
    };
    for (size_t i = 0; i < hex.size(); i += 2) {
        unsigned char hi = 0, lo = 0;
        if (!nibble(hex[i], hi) || !nibble(hex[i + 1], lo)) return false;
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    raw_out = std::move(out);
    return true;
}
bool DecodeBase64Bytes(std::string_view text, std::string& raw_out) {
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    std::string clean;
    for (char c : text) if (c != ' ' && c != '\r' && c != '\n' && c != '\t') clean.push_back(c);
    if (clean.empty() || (clean.size() % 4) != 0) return false;
    std::string out;
    out.reserve((clean.size() / 4) * 3);
    for (size_t i = 0; i < clean.size(); i += 4) {
        const int a = val(clean[i]), b = val(clean[i + 1]);
        const int c = clean[i + 2] == '=' ? -2 : val(clean[i + 2]);
        const int d = clean[i + 3] == '=' ? -2 : val(clean[i + 3]);
        if (a < 0 || b < 0 || c == -1 || d == -1 || (c == -2 && d != -2)) return false;
        if (c == -2 && i + 4 != clean.size()) return false;
        const uint32_t triple = (static_cast<uint32_t>(a) << 18) | (static_cast<uint32_t>(b) << 12) |
            (static_cast<uint32_t>(c < 0 ? 0 : c) << 6) | static_cast<uint32_t>(d < 0 ? 0 : d);
        out.push_back(static_cast<char>((triple >> 16) & 0xff));
        if (c != -2) out.push_back(static_cast<char>((triple >> 8) & 0xff));
        if (d != -2) out.push_back(static_cast<char>(triple & 0xff));
    }
    raw_out = std::move(out);
    return true;
}
bool MatchCandidatePublicKeyText(std::string_view candidate, std::string_view expected_publisher_id, std::string& hex_out) {
    std::string trimmed(candidate);
    while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '\r' || trimmed.back() == '\n' || trimmed.back() == '\t'))
        trimmed.pop_back();
    size_t start = 0;
    while (start < trimmed.size() && (trimmed[start] == ' ' || trimmed[start] == '\r' || trimmed[start] == '\n' || trimmed[start] == '\t'))
        ++start;
    trimmed.erase(0, start);
    std::string raw;
    if (candidate.size() >= 24 && candidate.substr(0, 4) == "RSA1") {
        if (PackageRevision(candidate) == expected_publisher_id) {
            hex_out = BytesToLowerHex(candidate);
            return true;
        }
    }
    if (LowerHexToBytes(trimmed, raw) && PackageRevision(raw) == expected_publisher_id) {
        hex_out = BytesToLowerHex(raw);
        return true;
    }
    if (DecodeBase64Bytes(trimmed, raw) && PackageRevision(raw) == expected_publisher_id) {
        hex_out = BytesToLowerHex(raw);
        return true;
    }
    return false;
}
bool ProbeAdjacentPublicKey(const std::filesystem::path& archive_path, std::string_view publisher_id,
    const std::atomic<bool>& cancelled, std::string& key_hex_out) {
    const auto dir = archive_path.parent_path();
    const std::array<std::filesystem::path, 3> candidates = {
        std::filesystem::path(archive_path.native() + L".pub"),
        dir / (archive_path.stem().native() + L".pub"),
        dir / L"publisher.json",
    };
    for (const auto& candidate : candidates) {
        std::string raw, ignored;
        if (ReadOrdinaryLocalBytes(candidate, 16384, cancelled, raw, ignored) != LocalReadStatus::Ok) continue;
        if (MatchCandidatePublicKeyText(raw, publisher_id, key_hex_out)) return true;
        json::Value doc;
        if (json::Parse(raw, doc, nullptr, {16384, 6}) && doc.is_object()) {
            for (const char* field : {"publicKey", "publicKeyHex"}) {
                if (const auto* v = doc.find(field); v && v->is_string() &&
                    MatchCandidatePublicKeyText(v->as_string(), publisher_id, key_hex_out)) {
                    return true;
                }
            }
        }
    }
    return false;
}
bool VerifyLocalDetachedSignature(const std::filesystem::path& archive_path, const LocalPackageVerifyOptions& options,
    std::string_view official_pin, std::string_view official_public_key,
    std::string_view manifest_bytes, const std::atomic<bool>& cancelled,
    std::string& signature_out, std::string& publisher_id_out, std::string& publisher_key_out, std::string& error) {
    signature_out.clear();
    publisher_id_out.clear();
    publisher_key_out.clear();
    const bool explicit_sig = !options.signature_path.empty();
    const std::filesystem::path sig_path = explicit_sig
        ? options.signature_path
        : std::filesystem::path(archive_path.native() + L".sig.json");
    std::string sig_raw, io_error;
    const auto status = ReadOrdinaryLocalBytes(sig_path, 16384, cancelled, sig_raw, io_error);
    if (status == LocalReadStatus::Missing && !explicit_sig) return true; // Unsigned local package.
    if (status != LocalReadStatus::Ok) {
        error = io_error == "cancelled" ? "cancelled" : "package_signature_invalid";
        return false;
    }
    json::Value sig_doc;
    if (!IntegerTokens(sig_raw) || !json::Parse(sig_raw, sig_doc, nullptr, {16384, 4}) || !sig_doc.is_object()) {
        error = "package_signature_invalid";
        return false;
    }
    for (const auto& key : sig_doc.keys()) {
        if (key != "formatVersion" && key != "algorithm" && key != "publisher" &&
            key != "signature" && key != "publicKey") {
            error = "package_signature_invalid";
            return false;
        }
    }
    const std::string pub_id = sig_doc.get_string("publisher");
    if (!publish_trust_detail::ValidPublisherPin(pub_id)) {
        error = "package_signature_invalid";
        return false;
    }
    std::string embedded_key;
    if (const auto* pk = sig_doc.find("publicKey")) {
        if (!pk->is_string() || pk->as_string().empty()) {
            error = "package_signature_invalid";
            return false;
        }
        embedded_key = Lower(pk->as_string());
    }
    std::string resolved_key;
    if (!official_pin.empty() && pub_id == official_pin) resolved_key = std::string(official_public_key);
    for (const auto& known : options.known_publishers) {
        if (known.id != pub_id) continue;
        const std::string candidate = Lower(known.public_key);
        if (!resolved_key.empty() && resolved_key != candidate) {
            error = "package_signature_invalid";
            return false;
        }
        resolved_key = candidate;
    }
    if (!embedded_key.empty()) {
        if (!resolved_key.empty() && resolved_key != embedded_key) {
            error = "package_signature_invalid";
            return false;
        }
        resolved_key = embedded_key;
    }
    if (resolved_key.empty() && !ProbeAdjacentPublicKey(archive_path, pub_id, cancelled, resolved_key)) {
        if (cancelled.load()) { error = "cancelled"; return false; }
        error = "package_signature_invalid";
        return false;
    }
    std::string canonical_sig = sig_raw;
    if (sig_doc.find("publicKey")) {
        int64_t fmt = 0;
        if (!sig_doc.find("formatVersion") || !sig_doc.find("formatVersion")->get_int(fmt, 1, 1) ||
            !sig_doc.find("algorithm") || !sig_doc.find("algorithm")->is_string() ||
            !sig_doc.find("signature") || !sig_doc.find("signature")->is_string()) {
            error = "package_signature_invalid";
            return false;
        }
        json::Value clean = json::Value::Object();
        clean.set("formatVersion", 1);
        clean.set("algorithm", sig_doc.get_string("algorithm"));
        clean.set("publisher", pub_id);
        clean.set("signature", sig_doc.get_string("signature"));
        canonical_sig = json::Serialize(clean);
    }
    json::Value policy = json::Value::Object(), pins = json::Value::Array(), pin = json::Value::Object();
    pin.set("id", pub_id);
    pin.set("publicKey", resolved_key);
    pins.push(std::move(pin));
    policy.set("formatVersion", 1);
    policy.set("requireSigned", true);
    policy.set("publishers", std::move(pins));
    std::string sig_error;
    if (!VerifyPackageSignature(manifest_bytes, canonical_sig, json::Serialize(policy), sig_error)) {
        error = "package_signature_invalid";
        return false;
    }
    signature_out = std::move(canonical_sig);
    publisher_id_out = pub_id;
    publisher_key_out = std::move(resolved_key);
    return true;
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
        const auto lower_path = Lower(file.path);
        if (lower_path.ends_with(".dll") && !ValidateNativeImageBytes(it->second)) return fail("invalid_native_x64_pe_dll");
        if (lower_path.ends_with(".exe") && !ValidateNativeExecutableBytes(it->second)) return fail("invalid_native_x64_pe_exe");
        data->files.push_back({file.path, std::move(it->second)});
    }
    data->declaration = std::move(*parsed.manifest); data->revision = revision;
    data->manifest = manifest_bytes; data->signature = signature; data->publisher = publisher.id; data->key = publisher.public_key;
    if (cancelled.load()) return fail("cancelled");
    out.data_ = std::move(data);
    return true;
}
bool VerifyLocalPackageInternal(const std::filesystem::path& archive_path,
    const LocalPackageVerifyOptions& options,
    std::string_view official_pin, std::string_view official_public_key,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error) {
    error.clear();
    auto fail = [&](const char* code) { error = code; return false; };
    if (cancelled.load()) return fail("cancelled");
    if (!options.host_version.empty() && !ParseThreePartVersion(options.host_version))
        return fail("install_host_version_invalid");
    if (options.sdk_preview < 0 || (!options.host_version.empty() && options.sdk_preview <= 0))
        return fail("install_sdk_incompatible");

    std::string archive_bytes;
    if (ReadOrdinaryLocalBytes(archive_path, kArchive, cancelled, archive_bytes, error) != LocalReadStatus::Ok) {
        if (error.empty()) error = "local_package_open_failed";
        return false;
    }
    std::vector<Member> members;
    if (!Directory(archive_bytes, members)) return fail("invalid_bounded_zip_directory");
    std::map<std::string, std::string> files;
    if (!Decode(archive_bytes, members, cancelled, files, error)) return false;
    const auto manifest_entry = files.find("manifest.json"), metadata_entry = files.find("package.json");
    if (manifest_entry == files.end() || metadata_entry == files.end()) return fail("missing_required_package_metadata");
    const auto& manifest_bytes = manifest_entry->second;
    const auto revision = PackageRevision(manifest_bytes);
    if (!IntegerTokens(manifest_bytes)) return fail("invalid_manifest_integer");
    auto parsed = ParseManifest(manifest_bytes);
    if (!parsed) { error = "invalid_manifest: " + parsed.error; return false; }
    const auto& manifest = *parsed.manifest;
    if (manifest.api_version != 1) return fail("install_sdk_incompatible");

    std::string signature_bytes, publisher_id, publisher_key;
    if (!VerifyLocalDetachedSignature(archive_path, options, official_pin, official_public_key,
            manifest_bytes, cancelled, signature_bytes, publisher_id, publisher_key, error)) {
        return false;
    }

    json::Value metadata, root;
    const auto& metadata_bytes = metadata_entry->second;
    if (!IntegerTokens(metadata_bytes) || !json::Parse(metadata_bytes, metadata, nullptr, {16384, 8}) ||
        !json::Parse(manifest_bytes, root, nullptr, {kMaxManifestBytes, 40})) {
        return fail("invalid_package_metadata");
    }
    json::Value expected = json::Value::Object();
    expected.set("format", "pulse-extension-local");
    expected.set("formatVersion", manifest.native ? 2 : 1);
    expected.set("apiVersion", manifest.api_version);
    expected.set("id", manifest.id);
    expected.set("manifestSha256", revision);
    std::vector<NativeFile> wanted;
    if (manifest.native) {
        const auto& native = *manifest.native;
        if (native.version != 1 || native.architecture != "windows-x64")
            return fail("install_sdk_incompatible");
        wanted.push_back({native.library, native.sha256});
        wanted.insert(wanted.end(), native.files.begin(), native.files.end());
        json::Value n = json::Value::Object();
        n.set("library", native.library);
        n.set("sha256", native.sha256);
        if (!native.files.empty()) n.set("files", *root.find("native")->find("files"));
        expected.set("native", std::move(n));
    } else if (archive_bytes.size() > kV1) {
        return fail("declarative_archive_size_limit");
    }
    if (!Equal(metadata, expected) || files.size() != wanted.size() + 2)
        return fail("package_metadata_or_member_set_mismatch");

    auto data = std::make_shared<VerifiedPackage::Data>();
    for (const auto& file : wanted) {
        if (cancelled.load()) return fail("cancelled");
        const auto it = files.find(file.path);
        if (it == files.end() || PackageRevision(it->second) != file.sha256)
            return fail("native_file_hash_mismatch");
        const auto lower_path = Lower(file.path);
        if (lower_path.ends_with(".dll") && !ValidateNativeImageBytes(it->second))
            return fail("invalid_native_x64_pe_dll");
        if (lower_path.ends_with(".exe") && !ValidateNativeExecutableBytes(it->second))
            return fail("invalid_native_x64_pe_exe");
        data->files.push_back({file.path, std::move(it->second)});
    }
    data->declaration = std::move(*parsed.manifest);
    data->revision = revision;
    data->manifest = manifest_bytes;
    data->signature = std::move(signature_bytes);
    data->publisher = std::move(publisher_id);
    data->key = std::move(publisher_key);
    if (cancelled.load()) return fail("cancelled");
    out.data_ = std::move(data);
    return true;
}
bool VerifyLocalPackage(const std::filesystem::path& archive_path,
    const LocalPackageVerifyOptions& options,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error) {
    return VerifyLocalPackageInternal(archive_path, options,
        kOfficialCatalogPublisher, kOfficialCatalogPublicKey, cancelled, out, error);
}
bool VerifyLocalPackage(const std::filesystem::path& archive_path,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error) {
    return VerifyLocalPackage(archive_path, LocalPackageVerifyOptions{}, cancelled, out, error);
}
#ifdef PULSE_EXTENSION_LOCAL_PACKAGE_TESTING
namespace testing {
bool VerifyLocalPackageWithOfficialPin(const std::filesystem::path& archive_path,
    const LocalPackageVerifyOptions& options,
    std::string_view official_pin, std::string_view official_public_key,
    const std::atomic<bool>& cancelled, VerifiedPackage& out, std::string& error) {
    return VerifyLocalPackageInternal(archive_path, options, official_pin, official_public_key, cancelled, out, error);
}
} // namespace testing
#endif
} // namespace pulse::extensions::store
