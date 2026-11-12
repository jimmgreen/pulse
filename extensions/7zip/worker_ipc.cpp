#include "worker_ipc.h"
#include <bcrypt.h>
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace worker_ipc {
namespace {

struct ScopedEvent {
    HANDLE value = nullptr;
    ScopedEvent() noexcept : value(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    ~ScopedEvent() { if (value) CloseHandle(value); }
    ScopedEvent(const ScopedEvent&) = delete;
    ScopedEvent& operator=(const ScopedEvent&) = delete;
};

std::wstring CurrentProcessUserSid() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    std::vector<BYTE> buffer(bytes);
    const bool ok = bytes != 0 && GetTokenInformation(token, TokenUser, buffer.data(), bytes, &bytes) != FALSE;
    CloseHandle(token);
    if (!ok) return {};
    const auto* user = reinterpret_cast<const TOKEN_USER*>(buffer.data());
    wchar_t* raw_sid = nullptr;
    if (!ConvertSidToStringSidW(user->User.Sid, &raw_sid)) return {};
    std::wstring result(raw_sid);
    LocalFree(raw_sid);
    return result;
}

void DrainCancelledOverlapped(HANDLE pipe, OVERLAPPED& ov) noexcept {
    CancelIoEx(pipe, &ov);
    DWORD ignored = 0;
    GetOverlappedResult(pipe, &ov, &ignored, TRUE);
}

IoStatus ClassifyPipeFailure(DWORD err, HANDLE peer_process) noexcept {
    if (peer_process && WaitForSingleObject(peer_process, 0) == WAIT_OBJECT_0) {
        return IoStatus::PeerExited;
    }
    if (err == ERROR_OPERATION_ABORTED) return IoStatus::Aborted;
    if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED || err == ERROR_NO_DATA || err == ERROR_HANDLE_EOF) {
        return IoStatus::Disconnected;
    }
    return IoStatus::ProtocolError;
}

IoStatus WaitOverlappedTransfer(HANDLE pipe, OVERLAPPED& ov, DWORD& transferred,
                                HANDLE peer_process, const std::function<bool()>* should_abort,
                                ULONGLONG deadline_tick, bool has_deadline) {
    HANDLE handles[2]{ov.hEvent, peer_process};
    const DWORD handle_count = peer_process ? 2u : 1u;
    for (;;) {
        if (should_abort && *should_abort && (*should_abort)()) {
            DrainCancelledOverlapped(pipe, ov);
            return IoStatus::Aborted;
        }
        DWORD slice_ms = should_abort && *should_abort ? 25u : INFINITE;
        if (has_deadline) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline_tick) {
                DrainCancelledOverlapped(pipe, ov);
                return IoStatus::TimedOut;
            }
            const ULONGLONG rem = deadline_tick - now;
            const DWORD rem_ms = rem > 0x7FFFFFFFull ? 0x7FFFFFFFu : static_cast<DWORD>(rem);
            slice_ms = (slice_ms == INFINITE) ? rem_ms : std::min(slice_ms, rem_ms);
        }
        const DWORD waited = WaitForMultipleObjects(handle_count, handles, FALSE, slice_ms);
        if (waited == WAIT_OBJECT_0) {
            transferred = 0;
            if (GetOverlappedResult(pipe, &ov, &transferred, FALSE)) {
                return IoStatus::Ok;
            }
            return ClassifyPipeFailure(GetLastError(), peer_process);
        }
        if (peer_process && waited == WAIT_OBJECT_0 + 1) {
            // Peer process exited; check if all bytes already landed in the kernel pipe buffer first.
            transferred = 0;
            if (WaitForSingleObject(ov.hEvent, 0) == WAIT_OBJECT_0 &&
                GetOverlappedResult(pipe, &ov, &transferred, FALSE)) {
                return IoStatus::Ok;
            }
            DrainCancelledOverlapped(pipe, ov);
            return IoStatus::PeerExited;
        }
        if (waited != WAIT_TIMEOUT) {
            DrainCancelledOverlapped(pipe, ov);
            return IoStatus::ProtocolError;
        }
    }
}

IoStatus WriteAll(HANDLE pipe, const uint8_t* data, size_t total,
                  HANDLE peer_process, ULONGLONG deadline_tick, bool has_deadline) {
    size_t offset = 0;
    while (offset < total) {
        ScopedEvent ev;
        if (!ev.value) return IoStatus::ProtocolError;
        OVERLAPPED ov{};
        ov.hEvent = ev.value;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(total - offset, 1u << 20));
        DWORD written = 0;
        if (!WriteFile(pipe, data + offset, chunk, &written, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return ClassifyPipeFailure(err, peer_process);
            }
            const auto status = WaitOverlappedTransfer(pipe, ov, written, peer_process, nullptr, deadline_tick, has_deadline);
            if (status != IoStatus::Ok) return status;
        }
        if (written == 0) return ClassifyPipeFailure(ERROR_BROKEN_PIPE, peer_process);
        offset += written;
    }
    return IoStatus::Ok;
}

IoStatus ReadAll(HANDLE pipe, uint8_t* data, size_t total,
                 HANDLE peer_process, const std::function<bool()>* should_abort,
                 ULONGLONG deadline_tick, bool has_deadline) {
    size_t offset = 0;
    while (offset < total) {
        ScopedEvent ev;
        if (!ev.value) return IoStatus::ProtocolError;
        OVERLAPPED ov{};
        ov.hEvent = ev.value;
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(total - offset, 1u << 20));
        DWORD got = 0;
        if (!ReadFile(pipe, data + offset, chunk, &got, &ov)) {
            const DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                return ClassifyPipeFailure(err, peer_process);
            }
            const auto status = WaitOverlappedTransfer(pipe, ov, got, peer_process, should_abort, deadline_tick, has_deadline);
            if (status != IoStatus::Ok) return status;
        }
        if (got == 0) return ClassifyPipeFailure(ERROR_BROKEN_PIPE, peer_process);
        offset += got;
    }
    return IoStatus::Ok;
}

} // namespace

bool RandomBytes(void* out, size_t size) noexcept {
    if (!out || !size) return false;
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), static_cast<ULONG>(size),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
}

std::wstring MakeUniquePipeName() {
    static std::atomic<uint64_t> counter{0};
    uint8_t rand_bytes[16]{};
    RandomBytes(rand_bytes, sizeof(rand_bytes));
    wchar_t hex[33]{};
    for (size_t i = 0; i < 16; ++i) {
        swprintf_s(hex + i * 2, 3, L"%02x", rand_bytes[i]);
    }
    const uint64_t seq = ++counter;
    return L"\\\\.\\pipe\\Pulse.7ZipWorker." + std::to_wstring(GetCurrentProcessId()) + L"." +
           std::to_wstring(seq) + L"." + hex;
}

HANDLE CreateAuthenticatedServerPipe(const std::wstring& pipe_name) {
    const std::wstring sid = CurrentProcessUserSid();
    if (sid.empty()) return INVALID_HANDLE_VALUE;
    const std::wstring sddl = L"D:P(A;;GA;;;" + sid + L")";
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        return INVALID_HANDLE_VALUE;
    }
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.lpSecurityDescriptor = sd;
    sa.bInheritHandle = FALSE;

    HANDLE pipe = CreateNamedPipeW(
        pipe_name.c_str(),
        PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1,
        65536,
        65536,
        0,
        &sa);
    LocalFree(sd);
    return pipe;
}

IoStatus WaitServerPipeConnect(HANDLE pipe, HANDLE peer_process, DWORD timeout_ms) {
    if (pipe == INVALID_HANDLE_VALUE) return IoStatus::ProtocolError;
    ScopedEvent ev;
    if (!ev.value) return IoStatus::ProtocolError;
    OVERLAPPED ov{};
    ov.hEvent = ev.value;
    if (ConnectNamedPipe(pipe, &ov)) {
        return IoStatus::Ok;
    }
    const DWORD err = GetLastError();
    if (err == ERROR_PIPE_CONNECTED) return IoStatus::Ok;
    if (err != ERROR_IO_PENDING) return ClassifyPipeFailure(err, peer_process);

    DWORD dummy = 0;
    const bool has_deadline = timeout_ms != INFINITE;
    const ULONGLONG deadline = has_deadline ? GetTickCount64() + timeout_ms : 0;
    return WaitOverlappedTransfer(pipe, ov, dummy, peer_process, nullptr, deadline, has_deadline);
}

HANDLE ConnectWorkerClientPipe(const std::wstring& pipe_name) {
    return CreateFileW(
        pipe_name.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION,
        nullptr);
}

IoStatus PipeChannel::WriteRawHeaderOnly(const FrameHeader& header, HANDLE peer_process, DWORD timeout_ms) {
    if (pipe_ == INVALID_HANDLE_VALUE) return IoStatus::Disconnected;
    std::lock_guard guard(write_lock_);
    const bool has_deadline = timeout_ms != INFINITE;
    const ULONGLONG deadline = has_deadline ? GetTickCount64() + timeout_ms : 0;
    return WriteAll(pipe_, reinterpret_cast<const uint8_t*>(&header), sizeof(header), peer_process, deadline, has_deadline);
}

IoStatus PipeChannel::WriteFrame(FrameType type, uint64_t request_id, uint64_t generation,
                                 const void* payload, uint32_t payload_size,
                                 HANDLE peer_process, DWORD timeout_ms) {
    if (pipe_ == INVALID_HANDLE_VALUE) return IoStatus::Disconnected;
    if (payload_size > kMaxFrameBytes) return IoStatus::OversizedFrame;
    if (payload_size > 0 && !payload) return IoStatus::ProtocolError;
    FrameHeader header{};
    header.magic = kProtocolMagic;
    header.version = kProtocolVersion;
    header.type = static_cast<uint16_t>(type);
    header.request_id = request_id;
    header.generation = generation;
    header.payload_size = payload_size;
    header.reserved = 0;

    std::lock_guard guard(write_lock_);
    const bool has_deadline = timeout_ms != INFINITE;
    const ULONGLONG deadline = has_deadline ? GetTickCount64() + timeout_ms : 0;
    auto status = WriteAll(pipe_, reinterpret_cast<const uint8_t*>(&header), sizeof(header),
                           peer_process, deadline, has_deadline);
    if (status != IoStatus::Ok || payload_size == 0) return status;
    return WriteAll(pipe_, static_cast<const uint8_t*>(payload), payload_size,
                    peer_process, deadline, has_deadline);
}

IoStatus PipeChannel::ReadFrame(FrameHeader& header, std::vector<uint8_t>& payload,
                                HANDLE peer_process, const std::function<bool()>& should_abort,
                                DWORD timeout_ms) {
    payload.clear();
    if (pipe_ == INVALID_HANDLE_VALUE) return IoStatus::Disconnected;
    const bool has_deadline = timeout_ms != INFINITE;
    const ULONGLONG deadline = has_deadline ? GetTickCount64() + timeout_ms : 0;

    auto status = ReadAll(pipe_, reinterpret_cast<uint8_t*>(&header), sizeof(header),
                          peer_process, should_abort ? &should_abort : nullptr, deadline, has_deadline);
    if (status != IoStatus::Ok) return status;

    if (header.magic != kProtocolMagic || header.version != kProtocolVersion || header.reserved != 0) {
        return IoStatus::ProtocolError;
    }
    if (header.payload_size > kMaxFrameBytes) {
        return IoStatus::OversizedFrame;
    }
    if (header.payload_size == 0) return IoStatus::Ok;

    try {
        payload.resize(header.payload_size);
    } catch (...) {
        return IoStatus::OversizedFrame;
    }
    status = ReadAll(pipe_, payload.data(), payload.size(),
                     peer_process, should_abort ? &should_abort : nullptr, deadline, has_deadline);
    if (status != IoStatus::Ok) {
        WipeSensitive(payload);
    }
    return status;
}

} // namespace worker_ipc
