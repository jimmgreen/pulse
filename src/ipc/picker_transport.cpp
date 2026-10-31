#include "picker_transport.h"
#include "../common/current_user_security.h"

#include <bcrypt.h>
#include <algorithm>

namespace pulse::ipc::picker {
namespace {
constexpr wchar_t kPrefix[] = L"\\\\.\\pipe\\Pulse.Picker.";
struct Deadline {
    WaitOptions options;
    ULONGLONG start = GetTickCount64();
    DWORD Remaining() const {
        if (options.timeout_ms == INFINITE) return INFINITE;
        const auto elapsed = GetTickCount64() - start;
        return elapsed >= options.timeout_ms ? 0 : options.timeout_ms - static_cast<DWORD>(elapsed);
    }
    DWORD Check(HANDLE peer) const {
        if (options.cancel) {
            const DWORD state = WaitForSingleObject(options.cancel, 0);
            if (state == WAIT_OBJECT_0) return ERROR_CANCELLED;
            if (state == WAIT_FAILED) return ERROR_INVALID_HANDLE;
        }
        if (peer && WaitForSingleObject(peer, 0) != WAIT_TIMEOUT) return ERROR_BROKEN_PIPE;
        return Remaining() == 0 ? ERROR_TIMEOUT : ERROR_SUCCESS;
    }
};
PipeHandle Duplicate(HANDLE source) {
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), source, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) return {};
    return PipeHandle(copy);
}
bool PeerIdentity(HANDLE pipe, HANDLE expected, bool server) {
    ULONG pid = 0;
    if (!(server ? GetNamedPipeClientProcessId(pipe, &pid) : GetNamedPipeServerProcessId(pipe, &pid)) ||
        !pid || pid != GetProcessId(expected) || WaitForSingleObject(expected, 0) != WAIT_TIMEOUT) return false;
    DWORD peer_session = 0, own_session = 0;
    if (!ProcessIdToSessionId(pid, &peer_session) || !ProcessIdToSessionId(GetCurrentProcessId(), &own_session) ||
        peer_session != own_session) return false;
    const auto own_sid = CurrentUserSidString();
    return !own_sid.empty() && own_sid == ProcessUserSidString(expected);
}
DWORD Await(HANDLE pipe, OVERLAPPED& overlap, HANDLE peer, const Deadline& deadline, DWORD& bytes) {
    HANDLE handles[3]{overlap.hEvent, peer, deadline.options.cancel};
    DWORD count = 2;
    if (deadline.options.cancel) ++count;
    const DWORD waited = WaitForMultipleObjects(count, handles, FALSE, deadline.Remaining());
    if (waited == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, &overlap, &bytes, FALSE) ? ERROR_SUCCESS : GetLastError();
    if (waited == WAIT_OBJECT_0 + 1 && GetOverlappedResult(pipe, &overlap, &bytes, FALSE)) return ERROR_SUCCESS;
    const DWORD error = waited == WAIT_OBJECT_0 + 1 ? ERROR_BROKEN_PIPE :
        waited == WAIT_OBJECT_0 + 2 ? ERROR_CANCELLED : waited == WAIT_TIMEOUT ? ERROR_TIMEOUT : GetLastError();
    CancelIoEx(pipe, &overlap);
    GetOverlappedResult(pipe, &overlap, &bytes, TRUE); // Keep OVERLAPPED alive until cancellation completes.
    return error;
}
DWORD Exact(HANDLE pipe, HANDLE peer, uint8_t* data, DWORD size, bool write, const Deadline& deadline) {
    PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return GetLastError();
    DWORD offset = 0;
    while (offset < size) {
        // A departed peer can leave a complete terminal frame in the pipe.
        // Attempt reads first; pending reads still watch peer death in Await.
        const auto stopped = deadline.Check(write ? peer : nullptr);
        if (stopped) return stopped;
        ResetEvent(event.Get());
        OVERLAPPED overlap{}; overlap.hEvent = event.Get();
        DWORD bytes = 0;
        const BOOL complete = write ? WriteFile(pipe, data + offset, size - offset, &bytes, &overlap)
                                    : ReadFile(pipe, data + offset, size - offset, &bytes, &overlap);
        if (!complete) {
            DWORD error = GetLastError();
            if (error == ERROR_IO_PENDING) error = Await(pipe, overlap, peer, deadline, bytes);
            if (error) return error;
        }
        if (!bytes) return ERROR_BROKEN_PIPE;
        offset += bytes;
    }
    return ERROR_SUCCESS;
}
}

bool IsValidPipeName(const std::wstring& name) {
    const std::wstring prefix(kPrefix);
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    const auto separator = name.find(L'.', prefix.size());
    if (separator == std::wstring::npos || separator == prefix.size() || separator - prefix.size() > 10 ||
        name.size() - separator - 1 != 32 || name[prefix.size()] == L'0') return false;
    uint64_t pid = 0;
    for (size_t i = prefix.size(); i < separator; ++i) {
        if (name[i] < L'0' || name[i] > L'9') return false;
        pid = pid * 10 + (name[i] - L'0');
    }
    if (!pid || pid > MAXDWORD) return false;
    for (size_t i = separator + 1; i < name.size(); ++i)
        if (!((name[i] >= L'0' && name[i] <= L'9') || (name[i] >= L'a' && name[i] <= L'f'))) return false;
    return true;
}

TransportResult Server::Create(Server& output) {
    std::array<uint8_t, 16> nonce{};
    if (BCryptGenRandom(nullptr, nonce.data(), static_cast<ULONG>(nonce.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
        return {ERROR_GEN_FAILURE};
    Server server;
    server.name_ = kPrefix + std::to_wstring(GetCurrentProcessId()) + L".";
    for (const auto b : nonce) {
        server.name_.push_back(L"0123456789abcdef"[b >> 4]);
        server.name_.push_back(L"0123456789abcdef"[b & 15]);
    }
    CurrentUserSecurityAttributes security;
    if (!security) return {ERROR_ACCESS_DENIED};
    server.pipe_.Reset(CreateNamedPipeW(server.name_.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, security.get()));
    if (!server.pipe_) return {GetLastError()};
    output = std::move(server);
    return {};
}

TransportResult Server::Accept(HANDLE child, Channel& output, WaitOptions wait) {
    PipeHandle pipe(std::move(pipe_)); // A single attempt, including failures.
    if (!pipe || !child || !GetProcessId(child)) return {ERROR_INVALID_HANDLE};
    auto peer = Duplicate(child); // Convert pseudo handles before multi-object waits.
    if (!peer) return {GetLastError()};
    child = peer.Get();
    const Deadline deadline{wait};
    if (const auto stopped = deadline.Check(child)) return {stopped};
    PipeHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return {GetLastError()};
    OVERLAPPED overlap{}; overlap.hEvent = event.Get();
    if (!ConnectNamedPipe(pipe.Get(), &overlap)) {
        DWORD error = GetLastError();
        DWORD bytes = 0;
        if (error == ERROR_IO_PENDING) error = Await(pipe.Get(), overlap, child, deadline, bytes);
        if (error != ERROR_SUCCESS && error != ERROR_PIPE_CONNECTED) return {error};
    }
    if (!PeerIdentity(pipe.Get(), child, true)) return {ERROR_ACCESS_DENIED};
    output = Channel(std::move(pipe), std::move(peer));
    return {};
}

TransportResult Connect(const std::wstring& name, HANDLE parent, Channel& output, WaitOptions wait) {
    if (!IsValidPipeName(name)) return {ERROR_INVALID_NAME};
    if (!parent || !GetProcessId(parent)) return {ERROR_INVALID_HANDLE};
    auto peer = Duplicate(parent);
    if (!peer) return {GetLastError()};
    parent = peer.Get();
    const auto expected_prefix = std::wstring(kPrefix) + std::to_wstring(GetProcessId(parent)) + L".";
    if (name.compare(0, expected_prefix.size(), expected_prefix) != 0) return {ERROR_ACCESS_DENIED};
    const Deadline deadline{wait};
    PipeHandle pipe;
    for (;;) {
        if (const auto stopped = deadline.Check(parent)) return {stopped};
        pipe.Reset(CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr));
        if (pipe) break;
        const DWORD error = GetLastError();
        if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND) return {error};
        HANDLE handles[2]{parent, wait.cancel};
        const DWORD result = WaitForMultipleObjects(wait.cancel ? 2 : 1, handles, FALSE, (std::min)(deadline.Remaining(), DWORD{20}));
        if (result == WAIT_OBJECT_0) return {ERROR_BROKEN_PIPE};
        if (result == WAIT_OBJECT_0 + 1) return {ERROR_CANCELLED};
        if (result == WAIT_FAILED) return {GetLastError()};
    }
    if (!PeerIdentity(pipe.Get(), parent, false)) return {ERROR_ACCESS_DENIED};
    output = Channel(std::move(pipe), std::move(peer));
    return {};
}

TransportResult Channel::Send(const Message& message, WaitOptions wait) {
    std::vector<uint8_t> wire;
    const auto codec = Encode(message, wire);
    if (codec != CodecError::None) return {ERROR_INVALID_DATA, codec};
    return {Exact(pipe_.Get(), peer_.Get(), wire.data(), static_cast<DWORD>(wire.size()), true, Deadline{wait})};
}

TransportResult Channel::Receive(Message& message, WaitOptions wait) {
    // The codec header already contains the payload size; no second framing format.
    std::vector<uint8_t> wire(28);
    const Deadline deadline{wait};
    auto error = Exact(pipe_.Get(), peer_.Get(), wire.data(), 28, false, deadline);
    if (error) return {error};
    if (wire[0] != 0x50 || wire[1] != 0x50 || wire[2] != 0x43 || wire[3] != 0x4b ||
        wire[7] != 0 || wire[6] < 1 || wire[6] > 3)
        return {ERROR_INVALID_DATA, CodecError::Malformed};
    if (wire[4] != static_cast<uint8_t>(kVersion) || wire[5] != static_cast<uint8_t>(kVersion >> 8))
        return {ERROR_INVALID_DATA, CodecError::UnsupportedVersion};
    uint32_t payload = 0;
    for (size_t i = 0; i < 4; ++i) payload |= static_cast<uint32_t>(wire[8 + i]) << (i * 8);
    if (payload > kMaxMessageBytes - 28) return {ERROR_INVALID_DATA, CodecError::Malformed};
    wire.resize(28 + payload);
    error = Exact(pipe_.Get(), peer_.Get(), wire.data() + 28, payload, false, deadline);
    if (error) return {error};
    const auto codec = Decode(wire, message);
    return codec == CodecError::None ? TransportResult{} : TransportResult{ERROR_INVALID_DATA, codec};
}
} // namespace pulse::ipc::picker
