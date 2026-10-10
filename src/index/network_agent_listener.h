#pragma once
#include "network_agent_security.h"

namespace pulse::index::agent {
// Keep a listening instance alive while the previous request is dispatched so
// another process cannot claim the endpoint between short-lived connections.
class PipeListener {
public:
    PipeListener(const std::wstring& name, EndpointSecurity& security, bool overlapped = false)
        : name_(name), security_(security), overlapped_(overlapped), pipe_(Create(true)) {}
    ~PipeListener() { Close(); }
    PipeListener(const PipeListener&) = delete;
    PipeListener& operator=(const PipeListener&) = delete;
    HANDLE get() const { return pipe_; }
    void Close() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    HANDLE TakeConnected() {
        const HANDLE next = Create(false);
        if (next == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;
        const HANDLE connected = pipe_;
        pipe_ = next;
        return connected;
    }
    // Hands over the connected instance without a replacement, so no second
    // client queues behind it with a running deadline. Call Rearm() before
    // closing the returned handle to keep the endpoint reserved.
    HANDLE Release() {
        const HANDLE connected = pipe_;
        pipe_ = INVALID_HANDLE_VALUE;
        return connected;
    }
    bool Rearm() {
        if (pipe_ == INVALID_HANDLE_VALUE) pipe_ = Create(false);
        return pipe_ != INVALID_HANDLE_VALUE;
    }
private:
    HANDLE Create(bool first) {
        if (name_.empty() || !security_) {
            SetLastError(ERROR_ACCESS_DENIED);
            return INVALID_HANDLE_VALUE;
        }
        return CreateNamedPipeW(name_.c_str(), PIPE_ACCESS_DUPLEX |
            (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0) | (overlapped_ ? FILE_FLAG_OVERLAPPED : 0),
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, security_.get());
    }
    std::wstring name_;
    EndpointSecurity& security_;
    bool overlapped_ = false;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
};
} // namespace pulse::index::agent
