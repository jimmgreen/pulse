#pragma once
#include "../app/open_request_pipe.h"
#include "current_user_security.h"
#include <array>
#include <vector>
#include <cstring>
#include <cstddef>
#include <string_view>
#include <objbase.h>
namespace pulse {
enum class DirectRevealResult { Rejected, Accepted, Indeterminate };
struct DirectRevealDiagnostics { app::OpenRequestPipe::Result transport; unsigned attempts = 0; };
namespace direct_reveal_detail {
using app::OpenRequestPipe;
struct Endpoint { HWND window; DWORD pid; FILETIME created; std::wstring nonce, pipe_name; };
struct EndpointRecord {
    volatile LONG published = 0;
    DWORD version = 1;
    DWORD pid = 0;
    FILETIME created{};
    uint64_t window = 0;
    wchar_t nonce[40]{};
};
constexpr DWORD kRawV2Magic = 0x32524550; // PER2
constexpr DWORD kRawV2Capability = 1;
struct RawV2EndpointRecord {
    volatile LONG published = 0;
    DWORD magic = kRawV2Magic, version = 2, size = 128;
    DWORD capabilities = kRawV2Capability, reserved = 0, pid = 0, reserved2 = 0;
    FILETIME created{};
    uint64_t window = 0;
    wchar_t nonce[40]{};
};
static_assert(sizeof(RawV2EndpointRecord) == 128 && offsetof(RawV2EndpointRecord, nonce) == 48);
inline std::wstring CapabilityName(const std::wstring& base, const std::wstring& nonce) {
    return base + L".Endpoint.RawV2." + nonce;
}
inline std::wstring EndpointName(std::wstring_view requested) {
    if (!requested.empty()) return std::wstring(requested);
    DWORD session = 0;
    const auto sid = CurrentUserSidString();
    if (sid.empty() || !ProcessIdToSessionId(GetCurrentProcessId(), &session)) return {};
    return std::wstring(L"Local\\Pulse.Singleton") + L"-" + sid + L"-" + std::to_wstring(session);
}
inline bool Created(HANDLE process, FILETIME& value) {
    FILETIME exit{}, kernel{}, user{};
    return GetProcessTimes(process, &value, &exit, &kernel, &user) != FALSE;
}

inline std::optional<Endpoint> ReadPrimary(std::wstring_view endpoint_name) {
    const auto base = EndpointName(endpoint_name);
    if (base.empty()) return std::nullopt;
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, (base + L".Endpoint").c_str());
    if (!mapping) return std::nullopt;
    auto* record = static_cast<const EndpointRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(EndpointRecord)));
    EndpointRecord copy{};
    bool valid = record && record->published == 1;
    if (valid) { MemoryBarrier(); std::memcpy(&copy, record, sizeof(copy)); MemoryBarrier();
        valid = record->published == 1 && copy.version == 1 && copy.nonce[39] == 0; }
    if (record) UnmapViewOfFile(record);
    CloseHandle(mapping);
    if (!valid) return std::nullopt;
    HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(copy.window));
    DWORD pid = 0, session = 0, own_session = 0;
    GetWindowThreadProcessId(window, &pid);
    if (!pid || pid != copy.pid || !ProcessIdToSessionId(pid, &session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || session != own_session ||
        GetPropW(window, copy.nonce) != reinterpret_cast<HANDLE>(1)) return std::nullopt;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid);
    if (!process) return std::nullopt;
    FILETIME created{};
    valid = WaitForSingleObject(process, 0) == WAIT_TIMEOUT && Created(process, created) &&
        CompareFileTime(&created, &copy.created) == 0 && ProcessUserSidString(process) == CurrentUserSidString();
    CloseHandle(process);
    if (!valid) return std::nullopt;
    return Endpoint{window, pid, copy.created, copy.nonce, {}};
}

inline std::optional<Endpoint> FindPrimaryEndpoint(std::wstring_view endpoint_name) {
    try {
        auto endpoint = ReadPrimary(endpoint_name);
        if (!endpoint) return std::nullopt;
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, CapabilityName(EndpointName(endpoint_name), endpoint->nonce).c_str());
        if (!mapping) {
            if (GetLastError() != ERROR_FILE_NOT_FOUND) return std::nullopt;
            const auto again = ReadPrimary(endpoint_name);
            if (!again || again->window != endpoint->window || again->pid != endpoint->pid || again->nonce != endpoint->nonce ||
                CompareFileTime(&again->created, &endpoint->created)) return std::nullopt;
            return endpoint; // Explicitly absent capability, verified legacy endpoint.
        }
        const auto* view = static_cast<const RawV2EndpointRecord*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(RawV2EndpointRecord)));
        RawV2EndpointRecord copy{};
        bool valid = view && view->published == 1;
        if (valid) { MemoryBarrier(); std::memcpy(&copy, view, sizeof(copy)); MemoryBarrier(); valid = view->published == 1; }
        if (view) UnmapViewOfFile(view);
        CloseHandle(mapping);
        if (!valid || copy.magic != kRawV2Magic || copy.version != 2 || copy.size != sizeof(copy) || copy.capabilities != kRawV2Capability ||
            copy.reserved || copy.reserved2 || copy.nonce[39] || copy.pid != endpoint->pid || copy.window != reinterpret_cast<uintptr_t>(endpoint->window) ||
            CompareFileTime(&copy.created, &endpoint->created) || endpoint->nonce != copy.nonce) return std::nullopt;
        endpoint->pipe_name = OpenRequestPipe::Name(endpoint->pid, endpoint->nonce);
        if (endpoint->pipe_name.empty()) return std::nullopt;
        return endpoint;
    } catch (...) { return std::nullopt; }
}

}
// Indeterminate: the request may have reached Pulse without a reply. The
// in-process hook then still opens natively, preferring a rare duplicate
// window to showing nothing.
namespace direct_reveal_detail {
inline DirectRevealResult SendDirectRequest(uint32_t wire_version, const std::wstring& path, DWORD timeout_ms, std::wstring_view endpoint_name, DirectRevealDiagnostics* diagnostics) noexcept {
    if (diagnostics) *diagnostics = {};
    try {
        if (path.size() < 3 || path.size() >= 32768 || path[1] != L':' || path[2] != L'\\' ||
            !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
            path.find(L'\0') != std::wstring::npos) return DirectRevealResult::Rejected;
        const auto endpoint = FindPrimaryEndpoint(endpoint_name);
        if (!endpoint || endpoint->pipe_name.empty()) return DirectRevealResult::Rejected;
        struct Header { uint32_t version, chars; uint64_t deadline; GUID id; } header{};
        static_assert(sizeof(Header) == 32);
        if (FAILED(CoCreateGuid(&header.id))) return DirectRevealResult::Rejected;
        header.version = wire_version;
        header.chars = static_cast<uint32_t>(path.size() + 1);
        header.deadline = GetTickCount64() + 5000;
        std::vector<unsigned char> bytes(sizeof(header) + header.chars * sizeof(wchar_t));
        memcpy(bytes.data(), &header, sizeof(header));
        memcpy(bytes.data() + sizeof(header), path.c_str(), header.chars * sizeof(wchar_t));
        AllowSetForegroundWindow(endpoint->pid);
        const uint64_t finish = GetTickCount64() + timeout_ms;
        bool ambiguous = false;
        // Retries carry exactly the same ID, intent and acceptance deadline.
        for (unsigned attempt = 0; GetTickCount64() < finish; ++attempt) {
            const uint64_t now = GetTickCount64();
            const uint64_t exchange_deadline = now + ((finish - now > 125) ? 125 : finish - now);
            const auto result = app::OpenRequestPipe::Exchange(endpoint->pipe_name,
                {endpoint->pid, endpoint->created}, bytes, exchange_deadline);
            if (diagnostics) { diagnostics->transport = result; diagnostics->attempts = attempt + 1; }
            if (result.reply == app::OpenRequestPipe::Reply::Accepted || result.reply == app::OpenRequestPipe::Reply::Duplicate)
                return DirectRevealResult::Accepted;
            if (!result.reply && result.request_may_have_been_sent) ambiguous = true;
            if (result.reply && result.reply != app::OpenRequestPipe::Reply::Retry)
                return ambiguous ? DirectRevealResult::Indeterminate : DirectRevealResult::Rejected;
            // The single pipe instance can still be disconnecting after its ACK.
            // Back off within the same bounded budget instead of exhausting two
            // immediate ERROR_PIPE_BUSY attempts before the worker reconnects.
            const auto after = GetTickCount64();
            if (after < finish) Sleep(static_cast<DWORD>((finish - after > 10) ? 10 : finish - after));
        }
        return ambiguous ? DirectRevealResult::Indeterminate : DirectRevealResult::Rejected;
    } catch (...) { return DirectRevealResult::Rejected; }
}
}
inline DirectRevealResult TryDirectReveal(const std::wstring& path, DWORD timeout_ms = 250, std::wstring_view endpoint_name = {}, DirectRevealDiagnostics* diagnostics = nullptr) noexcept {
    return direct_reveal_detail::SendDirectRequest(3, path, timeout_ms, endpoint_name, diagnostics);
}
inline DirectRevealResult TryDirectOpenFolder(const std::wstring& path, DWORD timeout_ms = 250, std::wstring_view endpoint_name = {}, DirectRevealDiagnostics* diagnostics = nullptr) noexcept {
    return direct_reveal_detail::SendDirectRequest(4, path, timeout_ms, endpoint_name, diagnostics);
}
}
