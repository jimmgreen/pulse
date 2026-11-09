// archive_listing.cpp — see archive_listing.h.
#include "archive_listing.h"
#include "archive_serialization.h"
#include "udf_listing.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <ctime>
#include <memory>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace pulse::preview {
namespace {

// ---- common model ---------------------------------------------------------

using Entry = ArchiveListingEntry;
using archive_serialization::Listing;
using archive_serialization::Serialize;
constexpr size_t kMaxEntries = 100000;
constexpr ULONGLONG kLibarchiveBudgetMs = 1200;

// ---- ZIP central directory -------------------------------------------------

uint16_t U16(const unsigned char* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const unsigned char* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint64_t U64(const unsigned char* p) { return uint64_t(U32(p)) | (uint64_t(U32(p + 4)) << 32); }

bool ReadAt(HANDLE file, uint64_t offset, void* data, DWORD bytes) {
    LARGE_INTEGER at{};
    at.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(file, at, nullptr, FILE_BEGIN)) return false;
    DWORD got = 0;
    return ReadFile(file, data, bytes, &got, nullptr) && got == bytes;
}

std::wstring DecodeName(const unsigned char* data, size_t size, bool utf8_flag) {
    if (!size) return {};
    const int n = static_cast<int>(size);
    bool ascii = true;
    for (size_t i = 0; i < size; ++i) if (data[i] & 0x80) { ascii = false; break; }
    const char* text = reinterpret_cast<const char*>(data);
    auto convert = [&](UINT cp, DWORD flags) -> std::wstring {
        const int chars = MultiByteToWideChar(cp, flags, text, n, nullptr, 0);
        if (chars <= 0) return {};
        std::wstring out(static_cast<size_t>(chars), L'\0');
        MultiByteToWideChar(cp, flags, text, n, out.data(), chars);
        return out;
    };
    if (ascii || utf8_flag) return convert(CP_UTF8, 0);
    // No UTF-8 flag: macOS and newer tools still write UTF-8; Windows
    // archivers write the OEM code page (GBK on Chinese systems).
    if (std::wstring utf8 = convert(CP_UTF8, MB_ERR_INVALID_CHARS); !utf8.empty()) return utf8;
    return convert(CP_OEMCP, 0);
}

bool ListZip(HANDLE file, uint64_t file_size, Listing& listing, uint32_t& bytes_read) {
    listing.format = L"ZIP";
    const DWORD tail_size = static_cast<DWORD>((std::min<uint64_t>)(file_size, 65535 + 22));
    std::vector<unsigned char> tail(tail_size);
    if (tail_size < 22 || !ReadAt(file, file_size - tail_size, tail.data(), tail_size)) return false;
    bytes_read = tail_size;
    uint64_t count = 0, cd_size = 0, cd_offset = 0, cd_end = 0;
    bool found = false;
    // Prefer the enclosing EOCD over signatures embedded in its comment.
    for (size_t i = 0; i + 22 <= tail.size(); ++i) {
        const unsigned char* h = tail.data() + i;
        if (U32(h) != 0x06054b50 || i + 22 + U16(h + 20) != tail.size() ||
            U16(h + 4) != 0 || U16(h + 6) != 0 || U16(h + 8) != U16(h + 10)) continue;
        uint64_t candidate_count = U16(h + 10), candidate_size = U32(h + 12);
        uint64_t candidate_offset = U32(h + 16), candidate_end = file_size - tail_size + i;
        if (candidate_count == 0xFFFF || candidate_size == 0xFFFFFFFFull || candidate_offset == 0xFFFFFFFFull) {
            if (i < 20 || U32(h - 20) != 0x07064b50 || U32(h - 16) != 0 || U32(h - 4) != 1) continue;
            const uint64_t locator_pos = candidate_end - 20;
            uint64_t z64_pos = U64(h - 12);
            unsigned char z64[56]{};
            auto read_zip64 = [&](uint64_t pos) {
                return pos <= locator_pos && locator_pos - pos >= sizeof(z64) &&
                    ReadAt(file, pos, z64, sizeof(z64)) && U32(z64) == 0x06064b50 &&
                    U64(z64 + 4) >= 44 && U64(z64 + 4) == locator_pos - pos - 12;
            };
            if (!read_zip64(z64_pos)) {
                // A prepended SFX stub may leave the locator's offset unadjusted.
                if (locator_pos < sizeof(z64)) continue;
                const uint64_t shifted = locator_pos - sizeof(z64);
                if (shifted < z64_pos || !read_zip64(shifted)) continue;
                z64_pos = shifted;
            }
            if (U32(z64 + 16) != 0 || U32(z64 + 20) != 0 || U64(z64 + 24) != U64(z64 + 32)) continue;
            candidate_count = U64(z64 + 32);
            candidate_size = U64(z64 + 40);
            candidate_offset = U64(z64 + 48);
            candidate_end = z64_pos;
        }
        if (candidate_size > candidate_end || candidate_offset > candidate_end - candidate_size ||
            candidate_count > candidate_size / 46) continue;
        if (candidate_count == 0) {
            if (candidate_size != 0) continue;
            if (candidate_end != 0) {
                unsigned char stub[2]{};
                if (!ReadAt(file, 0, stub, sizeof(stub)) || stub[0] != 'M' || stub[1] != 'Z') continue;
            }
        } else {
            unsigned char signature[4]{};
            if (!ReadAt(file, candidate_end - candidate_size, signature, sizeof(signature)) ||
                U32(signature) != 0x02014b50) continue;
        }
        count = candidate_count;
        cd_size = candidate_size;
        cd_offset = candidate_offset;
        cd_end = candidate_end;
        found = true;
        break;
    }
    if (!found) return false;
    // Self-extracting archives and prepended stubs shift every offset.
    if (cd_size > cd_end) return false;
    const uint64_t cd_start = cd_end - cd_size;
    (void)cd_offset;
    if (cd_size > 64ull * 1024 * 1024) { listing.incomplete = true; cd_size = 64ull * 1024 * 1024; }
    std::vector<unsigned char> cd(static_cast<size_t>(cd_size));
    if (cd_size && !ReadAt(file, cd_start, cd.data(), static_cast<DWORD>(cd_size))) return false;
    bytes_read += static_cast<uint32_t>(cd_size);
    listing.has_packed = true;
    size_t at = 0;
    uint64_t seen = 0;
    while (at + 46 <= cd.size() && U32(&cd[at]) == 0x02014b50) {
        const unsigned char* h = &cd[at];
        const uint16_t flags = U16(h + 8);
        const uint16_t dos_time = U16(h + 12), dos_date = U16(h + 14);
        uint64_t packed = U32(h + 20), size = U32(h + 24);
        const size_t name_len = U16(h + 28), extra_len = U16(h + 30), comment_len = U16(h + 32);
        const uint32_t external = U32(h + 38);
        if (at + 46 + name_len + extra_len + comment_len > cd.size()) break;
        const unsigned char* name = h + 46;
        const unsigned char* extra = name + name_len;
        std::wstring unicode_name;
        for (size_t e = 0; e + 4 <= extra_len;) {
            const uint16_t id = U16(extra + e), len = U16(extra + e + 2);
            if (e + 4 + len > extra_len) break;
            const unsigned char* field = extra + e + 4;
            if (id == 0x0001) {  // ZIP64: only the fields that overflowed are present
                size_t k = 0;
                if (size == 0xFFFFFFFFull && k + 8 <= len) { size = U64(field + k); k += 8; }
                if (packed == 0xFFFFFFFFull && k + 8 <= len) { packed = U64(field + k); k += 8; }
            } else if (id == 0x7075 && len > 5 && field[0] == 1) {  // Info-ZIP Unicode path
                unicode_name = DecodeName(field + 5, len - 5, true);
            }
            e += 4 + len;
        }
        ++seen;
        listing.packed_total += packed;
        if (listing.entries.size() < kMaxEntries) {
            Entry entry;
            entry.path = unicode_name.empty()
                ? DecodeName(name, name_len, (flags & 0x0800) != 0) : unicode_name;
            entry.size = size;
            entry.packed = packed;
            entry.dir = (name_len && name[name_len - 1] == '/') || (external & 0x10);
            entry.encrypted = (flags & 1) != 0;
            if (dos_date) {
                entry.time.wYear = static_cast<WORD>(1980 + (dos_date >> 9));
                entry.time.wMonth = static_cast<WORD>((dos_date >> 5) & 15);
                entry.time.wDay = static_cast<WORD>(dos_date & 31);
                entry.time.wHour = static_cast<WORD>(dos_time >> 11);
                entry.time.wMinute = static_cast<WORD>((dos_time >> 5) & 63);
                entry.has_time = entry.time.wMonth >= 1 && entry.time.wMonth <= 12;
            }
            listing.entries.push_back(std::move(entry));
        } else {
            ++listing.skipped;
            listing.incomplete = true;
        }
        at += 46 + name_len + extra_len + comment_len;
    }
    if (seen != count || at != cd.size()) listing.incomplete = true;
    return seen > 0 || count == 0;
}

// ---- libarchive (Windows' archiveint.dll) ----------------------------------

struct archive;
struct archive_entry;

struct LibArchive {
    HMODULE module = nullptr;
    archive* (*read_new)() = nullptr;
    int (*support_filter_all)(archive*) = nullptr;
    int (*support_format_all)(archive*) = nullptr;
    int (*support_format_raw)(archive*) = nullptr;
    int (*format)(archive*) = nullptr;
    const char* (*format_name)(archive*) = nullptr;
    int (*filter_code)(archive*, int) = nullptr;
    intptr_t (*read_data)(archive*, void*, size_t) = nullptr;
    int (*open_filename_w)(archive*, const wchar_t*, size_t) = nullptr;
    int (*next_header)(archive*, archive_entry**) = nullptr;
    int (*data_skip)(archive*) = nullptr;
    int (*read_free)(archive*) = nullptr;
    const wchar_t* (*pathname_w)(archive_entry*) = nullptr;
    const char* (*pathname_utf8)(archive_entry*) = nullptr;
    int64_t (*entry_size)(archive_entry*) = nullptr;
    int (*size_is_set)(archive_entry*) = nullptr;
    int64_t (*entry_mtime)(archive_entry*) = nullptr;   // time_t is 64-bit here
    unsigned short (*filetype)(archive_entry*) = nullptr;  // __LA_MODE_T on Windows
    int (*is_encrypted)(archive_entry*) = nullptr;
    bool ready = false;

    LibArchive() {
        module = LoadLibraryExW(L"archiveint.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return;
        auto load = [&](auto& fn, const char* name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(GetProcAddress(module, name));
            return fn != nullptr;
        };
        load(support_format_raw, "archive_read_support_format_raw");
        load(format, "archive_format");
        load(format_name, "archive_format_name");
        load(filter_code, "archive_filter_code");
        load(read_data, "archive_read_data");
        ready = load(read_new, "archive_read_new") &&
            load(support_filter_all, "archive_read_support_filter_all") &&
            load(support_format_all, "archive_read_support_format_all") &&
            load(open_filename_w, "archive_read_open_filename_w") &&
            load(next_header, "archive_read_next_header") &&
            load(data_skip, "archive_read_data_skip") &&
            load(read_free, "archive_read_free") &&
            load(pathname_w, "archive_entry_pathname_w") &&
            load(pathname_utf8, "archive_entry_pathname_utf8") &&
            load(entry_size, "archive_entry_size") &&
            load(size_is_set, "archive_entry_size_is_set") &&
            load(entry_mtime, "archive_entry_mtime") &&
            load(filetype, "archive_entry_filetype") &&
            load(is_encrypted, "archive_entry_is_encrypted");
    }
};

const wchar_t* FormatLabel(std::wstring_view ext) {
    if (ext == L".7z") return L"7Z";
    if (ext == L".rar") return L"RAR";
    if (ext == L".tar") return L"TAR";
    if (ext == L".cab") return L"CAB";
    if (ext == L".iso") return L"ISO";
    if (ext == L".tgz" || ext == L".gz") return L"GZIP";
    if (ext == L".txz" || ext == L".xz") return L"XZ";
    if (ext == L".tbz2" || ext == L".bz2") return L"BZIP2";
    return L"Archive";
}

bool ListWithLibarchive(const std::wstring& path, std::wstring_view ext, uint64_t file_size,
                        Listing& listing, uint32_t& bytes_read, std::wstring* error, bool compressed) {
    static LibArchive lib;
    if (!lib.ready) {
        if (error) *error = L"libarchive-unavailable";
        return false;
    }
    listing.format = FormatLabel(ext);
    archive* a = lib.read_new();
    if (!a) return false;
    struct Owner { LibArchive& lib; archive* a; ~Owner() { lib.read_free(a); } } owner{lib, a};
    lib.support_filter_all(a);
    lib.support_format_all(a);
    // Raw is a last-resort bidder and must never turn arbitrary input into an archive.
    if (compressed && lib.support_format_raw && lib.format && lib.filter_code && lib.read_data)
        lib.support_format_raw(a);
    if (lib.open_filename_w(a, path.c_str(), 64 * 1024) != 0) {
        if (error) *error = L"archive-open-failed";
        return false;
    }
    const ULONGLONG started = GetTickCount64();
    size_t seen = 0;
    bool failed = false;
    for (;;) {
        archive_entry* entry = nullptr;
        const int r = lib.next_header(a, &entry);
        if (r == 1) break;                       // ARCHIVE_EOF
        if (r < -20 || !entry) { failed = true; break; }  // FAILED / FATAL
        ++seen;
        const int format = lib.format ? (lib.format(a) & 0xff0000) : 0;
        const int filter = lib.filter_code ? lib.filter_code(a, 0) : 0;
        const wchar_t* compression = filter == 1 ? L"GZIP" : filter == 2 ? L"BZIP2" : filter == 6 ? L"XZ" : L"";
        const char* format_name = lib.format_name ? lib.format_name(a) : nullptr;
        if (format_name && (std::strstr(format_name, "UDF") || std::strstr(format_name, "udf"))) listing.format = L"UDF";
        else if (format == 0x30000) {
            listing.format = L"TAR";
            if (*compression) listing.format += filter == 1 ? L".GZ" : filter == 2 ? L".BZ2" : L".XZ";
        } else if (format == 0x40000) listing.format = L"ISO";
        else if (format == 0x90000) {
            if (!compressed || !*compression) {
                if (error) *error = L"compressed-stream-preview-unavailable";
                return false;
            }
            listing.format = compression;
        }
        if (listing.entries.size() < kMaxEntries) {
            Entry item;
            if (const wchar_t* w = lib.pathname_w(entry)) item.path = w;
            else if (const char* u = lib.pathname_utf8(entry)) {
                const int n = MultiByteToWideChar(CP_UTF8, 0, u, -1, nullptr, 0);
                if (n > 1) {
                    item.path.resize(static_cast<size_t>(n));
                    MultiByteToWideChar(CP_UTF8, 0, u, -1, item.path.data(), n);
                    item.path.resize(static_cast<size_t>(n - 1));
                }
            }
            item.dir = (lib.filetype(entry) & 0170000) == 0040000;
            if (lib.size_is_set(entry)) item.size = static_cast<uint64_t>((std::max<int64_t>)(0, lib.entry_size(entry)));
            item.encrypted = lib.is_encrypted(entry) != 0;
            const int64_t mtime = lib.entry_mtime(entry);
            if (mtime > 0) {
                ULARGE_INTEGER t{};
                t.QuadPart = static_cast<ULONGLONG>(mtime) * 10000000ull + 116444736000000000ull;
                FILETIME utc{t.LowPart, t.HighPart}, local{};
                if (FileTimeToLocalFileTime(&utc, &local) && FileTimeToSystemTime(&local, &item.time))
                    item.has_time = true;
            }
            if (format == 0x90000) {
                const size_t slash = path.find_last_of(L"\\/");
                item.path = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
                const size_t dot = item.path.find_last_of(L'.');
                if (dot != std::wstring::npos) item.path.resize(dot);
                if (item.path.empty()) item.path = L"data";
                item.size = 0;
                char buffer[64 * 1024];
                for (;;) {
                    const intptr_t got = lib.read_data(a, buffer, sizeof(buffer));
                    if (got < 0) {
                        if (error) *error = L"compressed-stream-read-failed";
                        return false;
                    }
                    if (!got) break;
                    item.size += static_cast<uint64_t>(got);
                    if (item.size > 64ull * 1024 * 1024 || GetTickCount64() - started > kLibarchiveBudgetMs) {
                        if (error) *error = L"compressed-stream-preview-limit";
                        return false;
                    }
                }
            }
            listing.entries.push_back(std::move(item));
        } else {
            ++listing.skipped;
        }
        // Solid RAR has to be decompressed to reach the next header: keep the
        // single-threaded host responsive and show what was found so far.
        if (GetTickCount64() - started > kLibarchiveBudgetMs) { listing.incomplete = true; break; }
    }
    if (listing.skipped) listing.incomplete = true;
    bytes_read = static_cast<uint32_t>((std::min<uint64_t>)(file_size, 0xFFFFFFFFull));
    if (failed) {
        if (!seen) {
            if (error) *error = L"archive-read-failed";
            return false;
        }
        listing.incomplete = true;
    }
    return true;
}

std::wstring LowerExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash)) return {};
    std::wstring ext = path.substr(dot);
    for (wchar_t& c : ext) c = static_cast<wchar_t>(towlower(c));
    return ext;
}

} // namespace

bool MakeArchiveListing(const std::wstring& path, std::wstring& text,
                        uint32_t& bytes_read, std::wstring* error) {
    text.clear();
    if (error) error->clear();
    bytes_read = 0;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        if (error) *error = L"archive-open-failed";
        return false;
    }
    LARGE_INTEGER size{};
    unsigned char magic[8]{};
    DWORD got = 0;
    const bool sized = GetFileSizeEx(file, &size) && size.QuadPart > 0;
    const bool sniffed = sized && ReadFile(file, magic, sizeof(magic), &got, nullptr) && got >= 4;
    const std::wstring ext = LowerExtension(path);
    Listing listing;
    bool ok = false;
    // The central directory is at the end, so a ZIP is listed without touching
    // the (possibly many GB of) data in between. Sniff rather than trust the
    // extension: .zip files are sometimes 7z or RAR in disguise and vice versa.
    const bool zip_magic = sniffed && magic[0] == 'P' && magic[1] == 'K';
    if (sized && (zip_magic || (ext == L".zip" && !sniffed) ||
                  (ext == L".zip" && !(magic[0] == '7' && magic[1] == 'z') &&
                   !(magic[0] == 'R' && magic[1] == 'a' && magic[2] == 'r')))) {
        ok = ListZip(file, static_cast<uint64_t>(size.QuadPart), listing, bytes_read);
    }
    // UDF's volume recognition sequence uses NSR02/NSR03 identifiers.
    bool udf = false;
    bool iso9660 = false;
    if (sized && ext == L".iso") {
        unsigned char descriptor[7]{};
        for (uint64_t sector = 16; sector < 256 && sector * 2048 + sizeof(descriptor) <= static_cast<uint64_t>(size.QuadPart); ++sector) {
            if (!ReadAt(file, sector * 2048, descriptor, sizeof(descriptor))) break;
            if (descriptor[0] == 1 && descriptor[6] == 1 && std::memcmp(descriptor + 1, "CD001", 5) == 0)
                iso9660 = true;
            if (descriptor[0] == 0 && descriptor[6] == 1 &&
                (std::memcmp(descriptor + 1, "NSR02", 5) == 0 || std::memcmp(descriptor + 1, "NSR03", 5) == 0)) {
                udf = true;
            }
        }
    }
    const bool compressed = sniffed && ((magic[0] == 0x1f && magic[1] == 0x8b) ||
        (magic[0] == 'B' && magic[1] == 'Z' && magic[2] == 'h') ||
        (got >= 6 && std::memcmp(magic, "\xfd" "7zXZ\0", 6) == 0));
    std::wstring udf_reason;
    if (udf) {
        UdfListing native;
        if (ReadUdfListing(file, static_cast<uint64_t>(size.QuadPart), native)) {
            listing = Listing{};
            listing.format = L"UDF";
            listing.incomplete = native.incomplete;
            for (auto& item : native.entries) {
                Entry entry;
                entry.path = std::move(item.path); entry.size = item.size;
                entry.dir = item.directory; entry.time = item.time; entry.has_time = item.has_time;
                listing.entries.push_back(std::move(entry));
            }
            bytes_read = static_cast<uint32_t>((std::min)(native.bytes_read, uint64_t{UINT32_MAX}));
            if (error) *error = native.reason;
            CloseHandle(file);
            text = Serialize(listing);
            return true;
        }
        udf_reason = native.reason.empty() ? L"udf-invalid" : native.reason;
    }
    CloseHandle(file);
    if (udf_reason == L"udf-cancelled" || udf_reason == L"udf-limit") {
        if (error) *error = udf_reason;
        return false;
    }
    if (!ok && sized) {
        listing = Listing{};
        const bool other_magic = sniffed && ((magic[0] == '7' && magic[1] == 'z') ||
            (magic[0] == 'R' && magic[1] == 'a' && magic[2] == 'r'));
        ok = ListWithLibarchive(path, other_magic && ext == L".zip"
                ? std::wstring_view(magic[0] == '7' ? L".7z" : L".rar") : std::wstring_view(ext),
            static_cast<uint64_t>(size.QuadPart), listing, bytes_read, error, compressed);
    }
    // A UDF-only image starts with zero-filled sectors, which the tar reader
    // may accept as an empty archive. Do not present that as an ISO directory.
    if (udf && !iso9660 && listing.format != L"UDF") ok = false;
    if (!ok) {
        if (udf && error) *error = udf_reason.empty() ? L"udf-preview-unavailable" : udf_reason;
        else if (compressed && error && (*error == L"archive-open-failed" || *error == L"archive-read-failed" || *error == L"libarchive-unavailable"))
            *error = L"compressed-stream-preview-unavailable";
        return false;
    }
    if (udf && listing.format == L"ISO") {
        listing.format = L"ISO/UDF";
        listing.incomplete = true;
        if (error) *error = udf_reason;
    }
    text = Serialize(listing);
    return true;
}

std::wstring SerializeArchiveEntries(std::vector<ArchiveListingEntry> entries,
    const std::wstring& format, bool incomplete) {
    Listing listing;
    listing.entries = std::move(entries);
    listing.format = format;
    listing.incomplete = incomplete;
    return Serialize(listing);
}

} // namespace pulse::preview
