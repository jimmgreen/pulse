#pragma once
#include "../common/current_user_security.h"
#include "../ipc/deadline_pipe.h"
#include <string>
#include <cstdint>

namespace pulse::start_pin {
inline constexpr wchar_t kClass[] = L"{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}";
inline constexpr wchar_t kPipePrefix[] = L"\\\\.\\pipe\\Pulse.StartPin.";
inline constexpr DWORD kTimeout = 15000;
struct Request { uint32_t version = 1; uint32_t pin = 0; uint32_t characters = 0; };
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE v = INVALID_HANDLE_VALUE) : value(v) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
inline bool ValidNonce(const std::wstring& value) {
    if (value.size() != 38 || value.front() != L'{' || value.back() != L'}') return false;
    for (size_t i = 1; i < 37; ++i) {
        if (i == 9 || i == 14 || i == 19 || i == 24) { if (value[i] != L'-') return false; }
        else if (!((value[i] >= L'0' && value[i] <= L'9') ||
                   (value[i] >= L'A' && value[i] <= L'F') || (value[i] >= L'a' && value[i] <= L'f'))) return false;
    }
    return true;
}
inline bool Io(HANDLE pipe, void* data, DWORD bytes, bool write, ULONGLONG deadline) {
    return ipc::DeadlinePipeIo(pipe, static_cast<uint8_t*>(data), bytes, write, deadline, [] { return false; });
}
inline std::wstring ImagePath(HANDLE process) {
    std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size); return path;
}
inline bool PeerIs(HANDLE pipe, bool server, const std::wstring& expected) {
    ULONG pid = 0;
    if (!(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid))) return false;
    Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    DWORD own_session = 0, peer_session = 0;
    if (!process || !ProcessIdToSessionId(pid, &peer_session) ||
        !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) || peer_session != own_session) return false;
    const auto sid = CurrentUserSidString();
    return !sid.empty() && sid == ProcessUserSidString(process.value) &&
        !expected.empty() && _wcsicmp(ImagePath(process.value).c_str(), expected.c_str()) == 0;
}
}
