#include "elevated_transfer_transport.h"
#include <sddl.h>

namespace pulse::elevated {
namespace {
bool Await(HANDLE pipe, OVERLAPPED& overlap, HANDLE peer, const Tick& tick, DWORD timeout_ms, DWORD& transferred) {
    const ULONGLONG start = GetTickCount64();
    HANDLE handles[2]{overlap.hEvent, peer};
    for (;;) {
        const DWORD waited = WaitForMultipleObjects(peer ? 2 : 1, handles, FALSE, tick || timeout_ms != INFINITE ? 100 : INFINITE);
        if (waited == WAIT_OBJECT_0) return GetOverlappedResult(pipe, &overlap, &transferred, FALSE) != FALSE;
        if (waited == WAIT_OBJECT_0 + 1 || waited == WAIT_FAILED ||
            (tick && !tick()) || (timeout_ms != INFINITE && GetTickCount64() - start >= timeout_ms)) {
            CancelIoEx(pipe, &overlap);
            GetOverlappedResult(pipe, &overlap, &transferred, TRUE);
            SetLastError(waited == WAIT_OBJECT_0 + 1 ? ERROR_BROKEN_PIPE : ERROR_TIMEOUT);
            return false;
        }
    }
}
bool Exact(HANDLE pipe, HANDLE peer, void* buffer, DWORD count, bool write, const Tick& tick, DWORD timeout_ms) {
    if (!count) return true;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    auto* bytes = static_cast<unsigned char*>(buffer);
    DWORD offset = 0;
    while (offset < count) {
        OVERLAPPED overlap{}; overlap.hEvent = event.Get(); ResetEvent(event.Get());
        DWORD transferred = 0;
        const BOOL complete = write ? WriteFile(pipe, bytes + offset, count - offset, &transferred, &overlap)
                                    : ReadFile(pipe, bytes + offset, count - offset, &transferred, &overlap);
        if (!complete && (GetLastError() != ERROR_IO_PENDING || !Await(pipe, overlap, peer, tick, timeout_ms, transferred))) return false;
        if (!transferred) { SetLastError(ERROR_BROKEN_PIPE); return false; }
        offset += transferred;
    }
    return true;
}
}
bool ReadFrame(HANDLE pipe, HANDLE peer, const Nonce& nonce, Header& header,
               std::vector<unsigned char>& payload, const Tick& tick, DWORD timeout_ms) {
    if (!Exact(pipe, peer, &header, sizeof(header), false, tick, timeout_ms)) return false;
    if (!ValidHeader(header, nonce)) { SetLastError(ERROR_INVALID_DATA); return false; }
    payload.resize(header.bytes);
    return Exact(pipe, peer, payload.data(), header.bytes, false, tick, timeout_ms);
}
bool WriteFrame(HANDLE pipe, HANDLE peer, const Nonce& nonce, Kind kind, uint64_t request,
                const Writer& payload, DWORD timeout_ms) {
    if (!payload.good || payload.bytes.size() > kMaxPayload) { SetLastError(ERROR_INVALID_DATA); return false; }
    Header header; header.kind = kind; header.request = request; header.nonce = nonce;
    header.bytes = static_cast<uint32_t>(payload.bytes.size());
    return Exact(pipe, peer, &header, sizeof(header), true, {}, timeout_ms) &&
        Exact(pipe, peer, const_cast<unsigned char*>(payload.bytes.data()), header.bytes, true, {}, timeout_ms);
}
bool WaitPipeConnection(HANDLE pipe, HANDLE peer, DWORD timeout_ms) {
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED overlap{}; overlap.hEvent = event.Get();
    if (ConnectNamedPipe(pipe, &overlap)) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_PIPE_CONNECTED) return true;
    if (error != ERROR_IO_PENDING) return false;
    DWORD transferred = 0;
    return Await(pipe, overlap, peer, {}, timeout_ms, transferred);
}
std::wstring ProcessImage(HANDLE process) {
    std::wstring path(32768, L'\0'); DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    path.resize(size); return path;
}
std::vector<unsigned char> UserSid(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return {};
    Handle token(raw);
    DWORD bytes = 0; GetTokenInformation(token.Get(), TokenUser, nullptr, 0, &bytes);
    if (!bytes || bytes > 65536) return {};
    std::vector<unsigned char> buffer(bytes);
    if (!GetTokenInformation(token.Get(), TokenUser, buffer.data(), bytes, &bytes)) return {};
    const auto sid = reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid;
    std::vector<unsigned char> result(GetLengthSid(sid));
    if (!CopySid(static_cast<DWORD>(result.size()), result.data(), sid)) return {};
    return result;
}
bool SameUser(HANDLE first, HANDLE second) {
    auto a = UserSid(first), b = UserSid(second);
    return !a.empty() && !b.empty() && EqualSid(a.data(), b.data());
}
bool IsElevated(HANDLE process) {
    HANDLE raw = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &raw)) return false;
    Handle token(raw); TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    return GetTokenInformation(token.Get(), TokenElevation, &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
}
std::wstring PipeName(DWORD parent, const Nonce& nonce) {
    return L"\\\\.\\pipe\\Pulse.Elevated." + std::to_wstring(parent) + L"." + NonceText(nonce);
}
Handle CreateSessionPipe(const std::wstring& name) {
    const auto sid = UserSid(GetCurrentProcess());
    if (sid.empty()) return {};
    LPWSTR sid_text = nullptr;
    if (!ConvertSidToStringSidW(const_cast<unsigned char*>(sid.data()), &sid_text)) return {};
    const std::wstring sddl = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;" + std::wstring(sid_text) + L")";
    LocalFree(sid_text);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &descriptor, nullptr)) return {};
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    Handle pipe(CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes));
    LocalFree(descriptor);
    return pipe;
}
}
