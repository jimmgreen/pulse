#include "catalog_client.h"
#include "../package_revision.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <utility>
#include <vector>

namespace pulse::extensions::store {
namespace {
using Validator = std::function<bool(std::string_view, std::string_view, Catalog&, std::string&)>;
enum class Point { BeforeRead, AfterRead, BeforeWrite, AfterWrite, BeforeCommit };
using Hook = std::function<void(Point)>;
constexpr size_t kHeaderSize = 20;
constexpr size_t kAnchorSize = 104;
constexpr size_t kMaximumCacheSize = kHeaderSize + kMaxCatalogBytes + kMaxCatalogSignatureBytes;
struct Watermark {
    std::string generated;
    uint64_t sequence = 0;
    bool valid = false;
};
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Temporary {
    std::filesystem::path path;
    Handle file;
    bool committed = false;
    ~Temporary() {
        if (!committed && file.value != INVALID_HANDLE_VALUE) {
            FILE_DISPOSITION_INFO disposition{TRUE};
            SetFileInformationByHandle(file.value, FileDispositionInfo, &disposition, sizeof(disposition));
        }
    }
};
void Notify(const Hook& hook, Point point) { if (hook) hook(point); }
std::string IoError(const char* operation) { return std::string(operation) + ":" + std::to_string(GetLastError()); }
bool CachePath(const std::filesystem::path& path) {
    if (!path.is_absolute() || path.filename().empty() || path.native().size() > 32700) return false;
    const auto name = path.filename().wstring();
    return name != L"." && name != L".." && name.find_first_of(L":*?\"<>|") == name.npos &&
        name.back() != L'.' && name.back() != L' ' && path.native().find(L'\0') == path.native().npos;
}
bool RegularFile(HANDLE file, BY_HANDLE_FILE_INFORMATION* out_info = nullptr) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(file, &info)) return false;
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) {
        SetLastError(ERROR_CANT_ACCESS_FILE);
        return false;
    }
    if (GetFileType(file) != FILE_TYPE_DISK) return false;
    if (out_info) *out_info = info;
    return true;
}
uint32_t Number(std::string_view bytes, size_t offset) {
    uint32_t n = 0;
    for (size_t i = 0; i < 4; ++i) n |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return n;
}
uint64_t Number64(std::string_view bytes, size_t offset) {
    uint64_t n = 0;
    for (size_t i = 0; i < 8; ++i) n |= static_cast<uint64_t>(static_cast<unsigned char>(bytes[offset + i])) << (8 * i);
    return n;
}
void AppendNumber(std::string& bytes, uint32_t n) {
    for (unsigned int i = 0; i < 4; ++i) bytes += static_cast<char>((n >> (8 * i)) & 255);
}
void AppendNumber64(std::string& bytes, uint64_t n) {
    for (unsigned int i = 0; i < 8; ++i) bytes += static_cast<char>((n >> (8 * i)) & 255);
}
uint64_t CacheAgeHours(const FILETIME& last_write) {
    const uint64_t written = (static_cast<uint64_t>(last_write.dwHighDateTime) << 32) | last_write.dwLowDateTime;
    FILETIME now_ft{};
    GetSystemTimeAsFileTime(&now_ft);
    const uint64_t now = (static_cast<uint64_t>(now_ft.dwHighDateTime) << 32) | now_ft.dwLowDateTime;
    constexpr uint64_t kTicksPerHour = 3600ull * 10000000ull;
    return now > written ? (now - written) / kTicksPerHour : 0;
}
bool IsCatalogExpired(const Catalog& catalog) {
    if (catalog.expires.empty()) return false;
    SYSTEMTIME utc{};
    GetSystemTime(&utc);
    char now[24]{};
    std::snprintf(now, sizeof(now), "%04u-%02u-%02uT%02u:%02u:%02uZ",
        static_cast<unsigned>(utc.wYear), static_cast<unsigned>(utc.wMonth), static_cast<unsigned>(utc.wDay),
        static_cast<unsigned>(utc.wHour), static_cast<unsigned>(utc.wMinute), static_cast<unsigned>(utc.wSecond));
    const std::string_view now_view(now, 20);
    if (catalog.expires.size() == 10) return now_view.substr(0, 10) > catalog.expires;
    return now_view > catalog.expires;
}
void AppendDiagnostic(std::string& dst, std::string_view part) {
    if (part.empty()) return;
    if (!dst.empty()) dst += ";";
    dst.append(part);
}
std::string OfflineDiagnostic(const CatalogSnapshot& cached, std::string_view failures) {
    std::string diag(failures);
    AppendDiagnostic(diag, "cache_age_hours=" + std::to_string(cached.cache_age_hours));
    if (cached.stale_offline) AppendDiagnostic(diag, "stale_offline_cache");
    if (cached.expired) AppendDiagnostic(diag, "catalog_expired:" + cached.catalog.expires);
    return diag;
}
std::filesystem::path AnchorPath(const std::filesystem::path& path) {
    auto anchor = path;
    anchor += L".anchor";
    return anchor;
}
bool ValidAnchorGenerated(std::string_view s) {
    if (s.size() != 20 || s[4] != '-' || s[7] != '-' || s[10] != 'T' || s[13] != ':' || s[16] != ':' || s[19] != 'Z') return false;
    for (size_t i : {size_t{0}, size_t{1}, size_t{2}, size_t{3}, size_t{5}, size_t{6}, size_t{8}, size_t{9},
                     size_t{11}, size_t{12}, size_t{14}, size_t{15}, size_t{17}, size_t{18}}) {
        if (s[i] < '0' || s[i] > '9') return false;
    }
    return true;
}
bool ReadAnchor(const std::filesystem::path& anchor_path, Watermark& out) {
    Handle file;
    file.value = CreateFileW(anchor_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length{};
    if (!RegularFile(file.value) || !GetFileSizeEx(file.value, &length) || length.QuadPart != static_cast<LONGLONG>(kAnchorSize)) return false;
    std::string bytes(kAnchorSize, '\0');
    DWORD received = 0;
    if (!ReadFile(file.value, bytes.data(), static_cast<DWORD>(kAnchorSize), &received, nullptr) || received != kAnchorSize) return false;
    const uint64_t seq = Number64(bytes, 12);
    const std::string_view gen = std::string_view(bytes).substr(20, 20);
    if (bytes.compare(0, 8, "PLSCAN01") != 0 || Number(bytes, 8) != 1 || seq > kMaxCatalogSequence ||
        !ValidAnchorGenerated(gen) || PackageRevision(std::string_view(bytes).substr(0, 40)) != std::string_view(bytes).substr(40, 64)) {
        return false;
    }
    out = {std::string(gen), seq, true};
    return true;
}
bool WriteAnchor(const std::filesystem::path& anchor_path, const Watermark& wm) {
    if (!wm.valid || wm.generated.size() != 20 || wm.sequence > kMaxCatalogSequence) return false;
    std::string bytes("PLSCAN01", 8);
    AppendNumber(bytes, 1);
    AppendNumber64(bytes, wm.sequence);
    bytes += wm.generated;
    const auto digest = PackageRevision(bytes);
    if (digest.size() != 64) return false;
    bytes += digest;
    static std::atomic<uint64_t> anchor_seq{0};
    Temporary temporary;
    for (int attempt = 0; attempt < 16; ++attempt) {
        temporary.path = anchor_path;
        temporary.path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(anchor_seq.fetch_add(1, std::memory_order_relaxed));
        temporary.file.value = CreateFileW(temporary.path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (temporary.file.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    }
    if (temporary.file.value == INVALID_HANDLE_VALUE || !RegularFile(temporary.file.value)) return false;
    DWORD written = 0;
    if (!WriteFile(temporary.file.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
        written != bytes.size() || !FlushFileBuffers(temporary.file.value)) return false;
    Handle destination;
    destination.value = CreateFileW(anchor_path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (destination.value != INVALID_HANDLE_VALUE) {
        if (!RegularFile(destination.value)) return false;
        CloseHandle(destination.value);
        destination.value = INVALID_HANDLE_VALUE;
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) return false;
    const auto target = anchor_path.native();
    const size_t name_bytes = target.size() * sizeof(wchar_t);
    const size_t rename_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    std::vector<unsigned char> storage(rename_size);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = TRUE;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, target.data(), name_bytes);
    if (!SetFileInformationByHandle(temporary.file.value, FileRenameInfo, rename, static_cast<DWORD>(rename_size))) return false;
    temporary.committed = true;
    return true;
}
bool ReadCache(const std::filesystem::path& path, const std::atomic<bool>& cancelled,
    CatalogSnapshot& snapshot, std::string& error, const Validator& validate, const Hook& hook) {
    Notify(hook, Point::BeforeRead);
    if (cancelled) return false;
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) { error = IoError("cache_open"); return false; }
    LARGE_INTEGER length{};
    BY_HANDLE_FILE_INFORMATION file_info{};
    if (!RegularFile(file.value, &file_info) || !GetFileSizeEx(file.value, &length)) { error = IoError("cache_stat"); return false; }
    if (length.QuadPart < static_cast<LONGLONG>(kHeaderSize) || length.QuadPart > static_cast<LONGLONG>(kMaximumCacheSize)) {
        error = "cache_size_limit"; return false;
    }
    std::string bytes(static_cast<size_t>(length.QuadPart), '\0');
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancelled) return false;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        DWORD received = 0;
        if (!ReadFile(file.value, bytes.data() + offset, chunk, &received, nullptr) || received != chunk) {
            error = "cache_short_read"; return false;
        }
        offset += received;
    }
    Notify(hook, Point::AfterRead);
    if (cancelled) return false;
    const uint32_t index_size = Number(bytes, 12), signature_size = Number(bytes, 16);
    if (bytes.compare(0, 8, "PLSCAT01") != 0 || Number(bytes, 8) != 1 ||
        !index_size || index_size > kMaxCatalogBytes || !signature_size || signature_size > kMaxCatalogSignatureBytes ||
        kHeaderSize + static_cast<size_t>(index_size) + signature_size != bytes.size()) {
        error = "cache_invalid_envelope"; return false;
    }
    snapshot.index_bytes = bytes.substr(kHeaderSize, index_size);
    snapshot.signature_bytes = bytes.substr(kHeaderSize + index_size, signature_size);
    if (!validate(snapshot.index_bytes, snapshot.signature_bytes, snapshot.catalog, error)) {
        error = "cache_verification:" + error; return false;
    }
    snapshot.cache_age_hours = CacheAgeHours(file_info.ftLastWriteTime);
    snapshot.stale_offline = snapshot.cache_age_hours > kStaleOfflineCacheHours;
    snapshot.expired = IsCatalogExpired(snapshot.catalog);
    return !cancelled;
}
bool WriteCache(const std::filesystem::path& path, const std::atomic<bool>& cancelled,
    const CatalogSnapshot& snapshot, std::string& error, const Hook& hook) {
    Notify(hook, Point::BeforeWrite);
    if (cancelled) return false;
    std::string bytes("PLSCAT01", 8);
    AppendNumber(bytes, 1);
    AppendNumber(bytes, static_cast<uint32_t>(snapshot.index_bytes.size()));
    AppendNumber(bytes, static_cast<uint32_t>(snapshot.signature_bytes.size()));
    bytes += snapshot.index_bytes; bytes += snapshot.signature_bytes;
    static std::atomic<uint64_t> sequence{0};
    Temporary temporary;
    for (int attempt = 0; attempt < 16; ++attempt) {
        if (cancelled) return false;
        temporary.path = path;
        temporary.path += L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1, std::memory_order_relaxed));
        temporary.file.value = CreateFileW(temporary.path.c_str(), GENERIC_WRITE | DELETE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (temporary.file.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) {
            error = IoError("cache_create_temporary"); return false;
        }
    }
    if (temporary.file.value == INVALID_HANDLE_VALUE) { error = "cache_temporary_collision"; return false; }
    if (!RegularFile(temporary.file.value)) { error = IoError("cache_temporary_stat"); return false; }
    size_t offset = 0;
    while (offset < bytes.size()) {
        if (cancelled) return false;
        const DWORD chunk = static_cast<DWORD>((std::min)(bytes.size() - offset, size_t{65536}));
        DWORD written = 0;
        if (!WriteFile(temporary.file.value, bytes.data() + offset, chunk, &written, nullptr) || written != chunk) {
            error = IoError("cache_write"); return false;
        }
        offset += written;
    }
    Notify(hook, Point::AfterWrite);
    if (cancelled) return false;
    if (!FlushFileBuffers(temporary.file.value)) { error = IoError("cache_flush"); return false; }
    if (cancelled) return false;
    // Open the destination as the leaf itself, never follow a symlink/junction.
    Handle destination;
    destination.value = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (destination.value != INVALID_HANDLE_VALUE) {
        if (!RegularFile(destination.value)) { error = IoError("cache_destination"); return false; }
    } else if (GetLastError() != ERROR_FILE_NOT_FOUND) { error = IoError("cache_destination_open"); return false; }
    // Legacy FileRenameInfo cannot replace an open destination, even when that
    // handle shares DELETE. Rename replaces the leaf itself, never its target.
    if (destination.value != INVALID_HANDLE_VALUE) {
        CloseHandle(destination.value);
        destination.value = INVALID_HANDLE_VALUE;
    }
    const auto target = path.native();
    const size_t name_bytes = target.size() * sizeof(wchar_t);
    const size_t rename_size = sizeof(FILE_RENAME_INFO) + name_bytes;
    std::vector<unsigned char> storage(rename_size);
    auto* rename = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    rename->ReplaceIfExists = TRUE;
    rename->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(rename->FileName, target.data(), name_bytes);
    Notify(hook, Point::BeforeCommit);
    if (cancelled) return false;
    // The open CREATE_NEW handle stays exclusive through the atomic rename.
    // Cancellation after this linearization point is a subsequent request.
    if (!SetFileInformationByHandle(temporary.file.value, FileRenameInfo, rename, static_cast<DWORD>(rename_size))) {
        error = IoError("cache_atomic_replace"); return false;
    }
    temporary.committed = true;
    return true;
}
bool Download(std::wstring_view url, size_t limit, const std::atomic<bool>& cancelled,
    const app::UpdateResponseReader& read, std::string& bytes, std::string& diagnostic, bool& reader_cancelled) {
    bool rejected = false;
    app::UpdateError category = app::UpdateError::Network;
    DWORD error = ERROR_SUCCESS;
    if (cancelled) return false;
    const bool ok = read(url, limit, cancelled, [&](const void* data, DWORD size) {
        if (cancelled || rejected) return false;
        if (size > limit - bytes.size() || (size && !data)) { rejected = true; return false; }
        if (size) bytes.append(static_cast<const char*>(data), size);
        return true;
    }, category, error);
    reader_cancelled = error == ERROR_CANCELLED;
    if (cancelled || reader_cancelled) return false;
    if (!ok || rejected) {
        diagnostic = rejected ? "catalog_response_size_or_chunk" :
            "catalog_transport:" + std::to_string(static_cast<int>(category)) + ":" + std::to_string(error);
        return false;
    }
    return true;
}
CatalogFetchStatus Fetch(const std::filesystem::path& path, const std::atomic<bool>& cancelled,
    CatalogSnapshot& out, std::string& error, const app::UpdateResponseReader& read,
    const Validator& validate, const Hook& hook, const CatalogCacheReady& cache_ready) {
    const auto requested = app::CurrentUpdateSource();
    error.clear();
    if (cancelled) return CatalogFetchStatus::Cancelled;
    if (!read || !validate || (!path.empty() && !CachePath(path))) {
        error = "catalog_invalid_arguments"; return CatalogFetchStatus::Failed;
    }
    CatalogSnapshot cached;
    std::string cache_error;
    bool cache_valid = !path.empty() && ReadCache(path, cancelled, cached, cache_error, validate, hook);
    if (cancelled) return CatalogFetchStatus::Cancelled;
    Watermark watermark;
    if (!path.empty()) {
        Watermark anchor;
        ReadAnchor(AnchorPath(path), anchor);
        if (cache_valid && anchor.valid &&
            (cached.catalog.generated < anchor.generated || cached.catalog.sequence < anchor.sequence)) {
            cache_valid = false;
            cached = {};
            cache_error = "cache_rollback_rejected";
            watermark = anchor;
        } else if (cache_valid) {
            watermark.generated = anchor.valid ? (std::max)(anchor.generated, cached.catalog.generated) : cached.catalog.generated;
            watermark.sequence = anchor.valid ? (std::max)(anchor.sequence, cached.catalog.sequence) : cached.catalog.sequence;
            watermark.valid = true;
            if (!anchor.valid || anchor.generated < watermark.generated || anchor.sequence < watermark.sequence)
                WriteAnchor(AnchorPath(path), watermark);
        } else if (anchor.valid) {
            watermark = anchor;
        }
    }
    if(cache_valid && cache_ready) {
        cached.offline=true; cached.cache_persisted=true; cached.requested_source=requested;
        cached.diagnostic=OfflineDiagnostic(cached, {});
        cache_ready(cached);
        if(cancelled) return CatalogFetchStatus::Cancelled;
    }
    const auto index_urls = app::UpdateSourceUrls(kCatalogIndexUrl, requested);
    const auto signature_urls = app::UpdateSourceUrls(kCatalogSignatureUrl, requested);
    std::string failures;
    for (size_t i = 0; i < index_urls.size() && i < signature_urls.size(); ++i) {
        if (cancelled) return CatalogFetchStatus::Cancelled;
        CatalogSnapshot snapshot;
        std::string diagnostic;
        bool reader_cancelled = false;
        const bool downloaded = Download(index_urls[i], kMaxCatalogBytes, cancelled, read, snapshot.index_bytes, diagnostic, reader_cancelled) &&
            Download(signature_urls[i], kMaxCatalogSignatureBytes, cancelled, read, snapshot.signature_bytes, diagnostic, reader_cancelled);
        if (cancelled || reader_cancelled) return CatalogFetchStatus::Cancelled;
        bool verified = false;
        if (downloaded) {
            verified = validate(snapshot.index_bytes, snapshot.signature_bytes, snapshot.catalog, diagnostic);
            if (verified && watermark.valid &&
                (snapshot.catalog.generated < watermark.generated || snapshot.catalog.sequence < watermark.sequence)) {
                verified = false; diagnostic = "catalog_rollback_rejected";
            }
        }
        if (cancelled) return CatalogFetchStatus::Cancelled;
        if (!verified) {
            if (!failures.empty()) failures += ";";
            failures += "source[" + std::to_string(i) + "]:" + diagnostic;
            continue;
        }
        snapshot.source = index_urls[i]; snapshot.requested_source = requested;
        snapshot.expired = IsCatalogExpired(snapshot.catalog);
        if (!path.empty()) {
            snapshot.cache_persisted = WriteCache(path, cancelled, snapshot, diagnostic, hook);
            if (!snapshot.cache_persisted && cancelled) return CatalogFetchStatus::Cancelled;
            if (!snapshot.cache_persisted) snapshot.diagnostic = "cache_persistence_failed:" + diagnostic;
            else {
                Watermark next_wm;
                next_wm.generated = watermark.valid ? (std::max)(watermark.generated, snapshot.catalog.generated) : snapshot.catalog.generated;
                next_wm.sequence = watermark.valid ? (std::max)(watermark.sequence, snapshot.catalog.sequence) : snapshot.catalog.sequence;
                next_wm.valid = true;
                WriteAnchor(AnchorPath(path), next_wm);
            }
            // Successful atomic cache commit is the operation's commit point.
        } else if (cancelled) return CatalogFetchStatus::Cancelled;
        if (snapshot.expired) AppendDiagnostic(snapshot.diagnostic, "catalog_expired:" + snapshot.catalog.expires);
        if (!failures.empty()) AppendDiagnostic(snapshot.diagnostic, failures);
        out = std::move(snapshot);
        return CatalogFetchStatus::Online;
    }
    if (cancelled) return CatalogFetchStatus::Cancelled;
    if (cache_valid) {
        cached.offline = true; cached.cache_persisted = true; cached.requested_source = requested;
        cached.diagnostic = OfflineDiagnostic(cached, failures);
        out = std::move(cached);
        return CatalogFetchStatus::Offline;
    }
    error = failures;
    if (!cache_error.empty()) error += ";" + cache_error;
    return CatalogFetchStatus::Failed;
}
} // namespace
CatalogFetchStatus FetchCatalog(const std::filesystem::path& cache_file, const std::atomic<bool>& cancelled,
    CatalogSnapshot& out, std::string& error, const app::UpdateResponseReader& read, const CatalogCacheReady& cache_ready) {
    return Fetch(cache_file, cancelled, out, error, read, LoadVerifiedCatalog, {}, cache_ready);
}
#ifdef PULSE_EXTENSION_CATALOG_CLIENT_TESTING
namespace testing {
CatalogFetchStatus FetchCatalogWithValidator(const std::filesystem::path& cache_file,
    const std::atomic<bool>& cancelled, CatalogSnapshot& out, std::string& error,
    const app::UpdateResponseReader& read, const CatalogValidator& validate, const CatalogCacheHook& hook, const CatalogCacheReady& cache_ready) {
    return Fetch(cache_file, cancelled, out, error, read, validate,
        hook ? Hook([&](Point point) { hook(static_cast<CatalogCachePoint>(point)); }) : Hook{}, cache_ready);
}
}
#endif
} // namespace pulse::extensions::store
