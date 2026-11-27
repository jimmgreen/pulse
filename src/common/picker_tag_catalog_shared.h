#pragma once
#include <windows.h>
#include <sddl.h>
#include <cstdint>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace pulse::picker_tags {
struct Snapshot {
    struct Tag {
        std::wstring name;
        uint32_t rgb = 0;
        bool operator==(const Tag&) const = default;
    };
    std::vector<Tag> tags;
    std::vector<std::pair<std::wstring, int>> paths;
    uint64_t revision = 0;
    bool available = false;
    bool operator==(const Snapshot&) const = default;
};
namespace detail {
inline constexpr DWORD kMagic = 0x31435450;
inline constexpr size_t kMaxPayloadBytes = 4 * 1024 * 1024;
struct Header {
    DWORD magic = kMagic, version = 1, pid = 0, available = 0;
    ULONGLONG timestamp = 0, revision = 0;
    DWORD bytes = 0, tags = 0, paths = 0, reserved = 0;
};
struct Blob {
    volatile LONG sequence = 0;
    DWORD reserved = 0;
    Header header;
    unsigned char payload[kMaxPayloadBytes];
};
static_assert(sizeof(Header) == 48 && offsetof(Blob, payload) == 56);
inline std::wstring Name() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<unsigned char> buffer(bytes);
    const BOOL ok = bytes && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes);
    CloseHandle(token);
    if (!ok) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid, &sid)) return {};
    DWORD session = 0;
    const BOOL session_ok = ProcessIdToSessionId(GetCurrentProcessId(), &session);
    const std::wstring name = session_ok ? L"Local\\Pulse.PickerPlaces.1." +
        std::to_wstring(session) + L"." + sid + L".TagCatalog.1" : L"";
    LocalFree(sid);
    return name;
}
struct MutexGuard {
    HANDLE handle = nullptr;
    explicit MutexGuard(HANDLE mutex) {
        const DWORD wait = mutex ? WaitForSingleObject(mutex, 0) : WAIT_FAILED;
        if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) handle = mutex;
    }
    ~MutexGuard() { if (handle) ReleaseMutex(handle); }
    MutexGuard(const MutexGuard&) = delete;
    MutexGuard& operator=(const MutexGuard&) = delete;
};
inline bool AddStringSize(const std::wstring& value, size_t& bytes) {
    if (value.empty() || value.size() > kMaxPayloadBytes / sizeof(wchar_t) ||
        value.find(L'\0') != std::wstring::npos) return false;
    const size_t row_bytes = 2 * sizeof(DWORD) + value.size() * sizeof(wchar_t);
    if (row_bytes > kMaxPayloadBytes - bytes) return false;
    bytes += row_bytes;
    return true;
}
inline bool Size(const Snapshot& snapshot, size_t& bytes) {
    bytes = 0;
    if (!snapshot.available) return true;
    if (snapshot.tags.size() > kMaxPayloadBytes / 10 || snapshot.paths.size() > kMaxPayloadBytes / 10) return false;
    for (const auto& tag : snapshot.tags) {
        if (tag.rgb > 0xffffff || !AddStringSize(tag.name, bytes)) return false;
    }
    for (const auto& [path, rank] : snapshot.paths) {
        if (rank < 0 || static_cast<size_t>(rank) >= snapshot.tags.size() || !AddStringSize(path, bytes)) return false;
    }
    return true;
}
inline void AppendRow(std::vector<unsigned char>& bytes, const std::wstring& text, DWORD value) {
    const DWORD length = static_cast<DWORD>(text.size());
    const size_t start = bytes.size();
    bytes.resize(start + 8 + text.size() * sizeof(wchar_t));
    std::memcpy(bytes.data() + start, &length, 4);
    std::memcpy(bytes.data() + start + 4, &value, 4);
    std::memcpy(bytes.data() + start + 8, text.data(), text.size() * sizeof(wchar_t));
}
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    Snapshot cached;
    std::vector<unsigned char> encoded;
    bool cached_valid = false;
    uint64_t encode_count = 0;
    explicit Writer(const std::wstring& name = Name()) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
            static_cast<DWORD>(sizeof(Blob)), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Live(DWORD pid) {
    HANDLE process = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}
inline bool Valid(const Header& header) {
    const auto now = GetTickCount64();
    if (header.magic != kMagic || header.version != 1 || !header.pid || header.available > 1 ||
        header.reserved || header.bytes > kMaxPayloadBytes || header.timestamp > now ||
        now - header.timestamp > 10000) return false;
    if (!header.available) return !header.bytes && !header.tags && !header.paths;
    return header.tags <= header.bytes / 10 && header.paths <= header.bytes / 10 &&
        static_cast<uint64_t>(header.tags) + header.paths <= header.bytes / 10;
}
inline bool ReadRow(const std::vector<unsigned char>& bytes, size_t& offset, std::wstring& text, DWORD& value) {
    if (offset > bytes.size() || bytes.size() - offset < 8) return false;
    DWORD length = 0;
    std::memcpy(&length, bytes.data() + offset, 4);
    std::memcpy(&value, bytes.data() + offset + 4, 4);
    offset += 8;
    if (!length || length > (bytes.size() - offset) / sizeof(wchar_t)) return false;
    text.resize(length);
    std::memcpy(text.data(), bytes.data() + offset, length * sizeof(wchar_t));
    offset += length * sizeof(wchar_t);
    return text.find(L'\0') == std::wstring::npos;
}
inline Snapshot Decode(const Header& header, const std::vector<unsigned char>& bytes) {
    Snapshot result;
    if (!Valid(header) || !header.available || bytes.size() != header.bytes || !Live(header.pid)) return result;
    size_t offset = 0;
    result.tags.reserve(header.tags);
    result.paths.reserve(header.paths);
    for (DWORD i = 0; i < header.tags; ++i) {
        Snapshot::Tag tag;
        DWORD rgb = 0;
        if (!ReadRow(bytes, offset, tag.name, rgb) || rgb > 0xffffff) return {};
        tag.rgb = rgb;
        result.tags.push_back(std::move(tag));
    }
    for (DWORD i = 0; i < header.paths; ++i) {
        std::wstring path;
        DWORD rank = 0;
        if (!ReadRow(bytes, offset, path, rank) || rank >= header.tags) return {};
        result.paths.emplace_back(std::move(path), static_cast<int>(rank));
    }
    if (offset != bytes.size()) return {};
    result.revision = header.revision;
    result.available = true;
    return result;
}
}
inline void Publish(const Snapshot& snapshot, detail::Writer& writer) {
    if (!writer.blob || !writer.mutex) return;
    detail::MutexGuard lock(writer.mutex);
    if (!lock.handle) return;
    const bool changed = !writer.cached_valid || snapshot != writer.cached;
    detail::Header header;
    header.pid = GetCurrentProcessId();
    header.timestamp = GetTickCount64();
    header.revision = snapshot.revision;
    size_t bytes = 0;
    const bool valid = !changed || detail::Size(snapshot, bytes);
    if (changed) {
        writer.encoded.clear();
        writer.cached_valid = valid;
        if (valid) {
            // Unavailable snapshots never retain caller-supplied catalog data.
            writer.cached = snapshot.available ? snapshot : Snapshot{};
            writer.cached.revision = snapshot.revision;
            writer.encoded.reserve(bytes);
            if (snapshot.available) {
                for (const auto& tag : snapshot.tags) detail::AppendRow(writer.encoded, tag.name, tag.rgb);
                for (const auto& [path, rank] : snapshot.paths) detail::AppendRow(writer.encoded, path, static_cast<DWORD>(rank));
            }
            ++writer.encode_count;
        }
    }
    if (valid && snapshot.available) {
        header.available = 1;
        header.bytes = static_cast<DWORD>(writer.encoded.size());
        header.tags = static_cast<DWORD>(snapshot.tags.size());
        header.paths = static_cast<DWORD>(snapshot.paths.size());
    }
    // A second publisher may have replaced the mapping since our last publish.
    const bool same_payload = !changed && !writer.blob->reserved &&
        writer.blob->header.magic == header.magic && writer.blob->header.version == header.version &&
        writer.blob->header.reserved == header.reserved && writer.blob->header.pid == header.pid &&
        writer.blob->header.revision == header.revision && writer.blob->header.available == header.available &&
        writer.blob->header.bytes == header.bytes && writer.blob->header.tags == header.tags &&
        writer.blob->header.paths == header.paths &&
        (!header.bytes || !std::memcmp(writer.blob->payload, writer.encoded.data(), header.bytes));
    if (InterlockedCompareExchange(&writer.blob->sequence, 0, 0) & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    if (same_payload) {
        writer.blob->header.timestamp = header.timestamp;
    } else {
        writer.blob->reserved = 0;
        writer.blob->header = header;
        if (header.bytes) std::memcpy(writer.blob->payload, writer.encoded.data(), header.bytes);
    }
    InterlockedIncrement(&writer.blob->sequence);
}
inline void Publish(const Snapshot& snapshot) { static detail::Writer writer; Publish(snapshot, writer); }
inline Snapshot Read(const std::wstring& name) {
    if (name.empty()) return {};
    HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L".Lock").c_str());
    if (!mutex) return {};
    Snapshot result;
    {
        detail::MutexGuard lock(mutex);
        if (lock.handle) {
            HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
            if (mapping) {
                const auto* blob = static_cast<const detail::Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(detail::Blob)));
                if (blob) {
                    const LONG before = blob->sequence;
                    MemoryBarrier();
                    const auto header = blob->header;
                    if (!(before & 1) && !blob->reserved && detail::Valid(header)) {
                        std::vector<unsigned char> bytes(header.bytes);
                        if (header.bytes) std::memcpy(bytes.data(), blob->payload, header.bytes);
                        MemoryBarrier();
                        if (before == blob->sequence) result = detail::Decode(header, bytes);
                    }
                    UnmapViewOfFile(blob);
                }
                CloseHandle(mapping);
            }
        }
    }
    CloseHandle(mutex);
    return result;
}
inline Snapshot Read() { return Read(detail::Name()); }
}
