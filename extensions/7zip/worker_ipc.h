// worker_ipc.h - authenticated current-user overlapped Named Pipe transport
// shared by pulse_7zip.dll and pulse_7zip_worker.exe.
#pragma once
#include "worker_protocol.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace worker_ipc {

enum class IoStatus {
    Ok,
    PeerExited,
    Disconnected,
    TimedOut,
    Aborted,
    OversizedFrame,
    ProtocolError,
};

bool RandomBytes(void* out, size_t size) noexcept;
std::wstring MakeUniquePipeName();

// Creates a duplex, first-instance, overlapped, local-only Named Pipe restricted
// via DACL exclusively to the current process user SID (bInheritHandle = FALSE).
HANDLE CreateAuthenticatedServerPipe(const std::wstring& pipe_name);

// Waits for a client connection on an overlapped server pipe, aborting immediately
// if peer_process exits or timeout_ms elapses.
IoStatus WaitServerPipeConnect(HANDLE pipe, HANDLE peer_process, DWORD timeout_ms);

// Connects to the host's Named Pipe with FILE_FLAG_OVERLAPPED and
// SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION.
HANDLE ConnectWorkerClientPipe(const std::wstring& pipe_name);

class PipeChannel {
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    std::mutex write_lock_;
public:
    PipeChannel() = default;
    explicit PipeChannel(HANDLE pipe) noexcept : pipe_(pipe) {}
    ~PipeChannel() { Close(); }
    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;

    void Reset(HANDLE pipe = INVALID_HANDLE_VALUE) noexcept {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = pipe;
    }
    void Close() noexcept { Reset(INVALID_HANDLE_VALUE); }
    HANDLE NativeHandle() const noexcept { return pipe_; }
    bool Valid() const noexcept { return pipe_ != INVALID_HANDLE_VALUE; }

    IoStatus WriteRawHeaderOnly(const FrameHeader& header, HANDLE peer_process, DWORD timeout_ms);

    IoStatus WriteFrame(FrameType type, uint64_t request_id, uint64_t generation,
                        const void* payload, uint32_t payload_size,
                        HANDLE peer_process, DWORD timeout_ms);

    IoStatus WriteFrame(FrameType type, uint64_t request_id, uint64_t generation,
                        const std::vector<uint8_t>& payload,
                        HANDLE peer_process, DWORD timeout_ms) {
        if (payload.size() > kMaxFrameBytes) return IoStatus::OversizedFrame;
        return WriteFrame(type, request_id, generation, payload.data(),
                          static_cast<uint32_t>(payload.size()), peer_process, timeout_ms);
    }

    IoStatus ReadFrame(FrameHeader& header, std::vector<uint8_t>& payload,
                       HANDLE peer_process, const std::function<bool()>& should_abort,
                       DWORD timeout_ms);
};

} // namespace worker_ipc
