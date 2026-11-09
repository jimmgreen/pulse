#pragma once
#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>
#include <cstring>
#include <cstdint>
#include <memory>
#include <cwchar>

namespace pulse::picker_places {
struct Entry { std::wstring path, label; bool open = false, pinned = false; };
struct Workspace { std::wstring root, name; uint32_t color_rgb = 0; };
struct StarredItem { std::wstring path; uint32_t kind = 0; std::wstring group; };
struct StarredGroup { std::wstring id, name; };
struct Snapshot {
    std::wstring current;
    std::vector<Entry> folders;
    std::vector<Workspace> workspaces;
    std::vector<StarredItem> starred_items;
    std::vector<StarredGroup> starred_groups;
    HWND endpoint = nullptr;
    DWORD endpoint_pid = 0;
    bool shared_available = false;
};
void PublishShared(const Snapshot& snapshot);
void ReadShared(Snapshot& snapshot);
namespace detail {
constexpr DWORD kMagic = 0x31535050;
struct Row { DWORD path_length, label_length, flags; wchar_t path[1024], label[128]; };
struct Payload { DWORD magic, version, pid, count, current_length; ULONGLONG timestamp; wchar_t current[1024]; Row rows[96]; };
struct Blob { volatile LONG sequence; Payload payload; };
static_assert(sizeof(Blob) <= 256 * 1024);
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
    std::wstring name = session_ok ? L"Local\\Pulse.PickerPlaces.1." + std::to_wstring(session) + L"." + sid : L"";
    LocalFree(sid);
    return name;
}
struct Writer {
    HANDLE mapping = nullptr, mutex = nullptr;
    Blob* blob = nullptr;
    explicit Writer(const std::wstring& name = Name()) {
        if (name.empty()) return;
        mutex = CreateMutexW(nullptr, FALSE, (name + L".Lock").c_str());
        mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Blob), name.c_str());
        if (mapping) blob = static_cast<Blob*>(MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, sizeof(Blob)));
    }
    ~Writer() { if (blob) UnmapViewOfFile(blob); if (mapping) CloseHandle(mapping); if (mutex) CloseHandle(mutex); }
};
inline bool Valid(const Payload& p) {
    if (p.magic != kMagic || p.version != 1 || !p.pid || p.count > 96 || p.current_length > 1024) return false;
    const auto now = GetTickCount64();
    if (p.timestamp > now || now - p.timestamp > 10000) return false;
    if (wmemchr(p.current, L'\0', p.current_length)) return false;
    for (DWORD i = 0; i < p.count; ++i) {
        const auto& r = p.rows[i];
        if (!r.path_length || r.path_length > 1024 || r.label_length > 128 || (r.flags & ~3u)) return false;
        if (wmemchr(r.path, L'\0', r.path_length) || wmemchr(r.label, L'\0', r.label_length)) return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, p.pid);
    if (!process) return false;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
}
}
inline void Publish(const Snapshot& snapshot) {
    PublishShared(snapshot);
    static detail::Writer writer;
    if (!writer.blob || !writer.mutex) return;
    // Allocate the bounded copy off the stack before taking the writer lock.
    auto p = std::make_unique<detail::Payload>();
    p->magic = detail::kMagic; p->version = 1; p->pid = GetCurrentProcessId(); p->timestamp = GetTickCount64();
    if (snapshot.current.size() <= 1024) {
        p->current_length = static_cast<DWORD>(snapshot.current.size());
        std::memcpy(p->current, snapshot.current.data(), snapshot.current.size() * sizeof(wchar_t));
    }
    for (const auto& entry : snapshot.folders) {
        if (p->count == 96) break;
        if (entry.path.empty() || entry.path.size() > 1024 || entry.label.size() > 128) continue;
        auto& r = p->rows[p->count++];
        r.path_length = static_cast<DWORD>(entry.path.size()); r.label_length = static_cast<DWORD>(entry.label.size());
        r.flags = (entry.open ? 1u : 0u) | (entry.pinned ? 2u : 0u);
        std::memcpy(r.path, entry.path.data(), entry.path.size() * sizeof(wchar_t));
        std::memcpy(r.label, entry.label.data(), entry.label.size() * sizeof(wchar_t));
    }
    const DWORD wait = WaitForSingleObject(writer.mutex, 0);
    if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return;
    LONG sequence = InterlockedCompareExchange(&writer.blob->sequence, 0, 0);
    if (sequence & 1) InterlockedIncrement(&writer.blob->sequence);
    InterlockedIncrement(&writer.blob->sequence);
    std::memcpy(&writer.blob->payload, p.get(), sizeof(*p));
    InterlockedIncrement(&writer.blob->sequence);
    ReleaseMutex(writer.mutex);
}
namespace detail {
inline Snapshot ReadMapping(const std::wstring& name) {
    Snapshot result;
    if (name.empty()) return result;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping) return result;
    const auto* blob = static_cast<const detail::Blob*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(detail::Blob)));
    auto p = std::make_unique<detail::Payload>();
    bool copied = false;
    if (blob) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            const LONG before = blob->sequence;
            MemoryBarrier();
            if (before & 1) continue;
            std::memcpy(p.get(), &blob->payload, sizeof(*p));
            MemoryBarrier();
            if (before == blob->sequence) { copied = true; break; }
        }
        UnmapViewOfFile(blob);
    }
    CloseHandle(mapping);
    if (!copied || !detail::Valid(*p)) return result;
    result.current.assign(p->current, p->current_length);
    for (DWORD i = 0; i < p->count; ++i) {
        const auto& r = p->rows[i];
        result.folders.push_back({std::wstring(r.path, r.path_length), std::wstring(r.label, r.label_length), (r.flags & 1) != 0, (r.flags & 2) != 0});
    }
    return result;
}
}
inline Snapshot Read() { auto result = detail::ReadMapping(detail::Name()); ReadShared(result); return result; }
}
#include "picker_places_shared.h"

