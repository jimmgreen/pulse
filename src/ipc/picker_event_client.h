#pragma once

#include "picker_transport.h"
#include <functional>

namespace pulse::ipc::picker {

class PickerEventContext;
using PickerEventCallback = std::function<uint32_t(PickerEventContext&, const SessionEvent&)>;

// Experimental callback transport, not a production admission capability.
// The caller supplies an authenticated channel and its verified host PID after
// establishing a session. This consumes the channel, including on failure.
SessionTerminal RunPickerEventClient(Channel channel, RequestId request_id, DWORD host_pid,
                                    const PickerEventCallback& callback, HANDLE cancel = nullptr);

class PickerEventContext {
public:
    uint32_t Query(uint64_t expected_generation, EventSnapshot& output);
    uint32_t GetWindow(uint64_t& output);
    uint32_t Close(uint32_t hresult);
    const EventSnapshot& Snapshot() const { return snapshot_; }
private:
    friend SessionTerminal RunPickerEventClient(Channel, RequestId, DWORD, const PickerEventCallback&, HANDLE);
    PickerEventContext(Channel& channel, RequestId id, DWORD pid, HANDLE cancel)
        : channel_(channel), id_(id), host_pid_(pid), cancel_(cancel), thread_(GetCurrentThreadId()) {}
    ~PickerEventContext();
    TransportResult Io(const std::function<TransportResult(HANDLE)>& operation);
    uint32_t Command(CommandKind kind, uint64_t generation, uint32_t hresult, EventSnapshot* output);
    bool WindowMatches(const EventSnapshot& snapshot) const;
    Channel& channel_;
    RequestId id_;
    DWORD host_pid_;
    HANDLE cancel_;
    const DWORD thread_;
    EventSnapshot snapshot_;
    uint64_t command_id_ = 0;
    bool io_active_ = false, in_callback_ = false, close_requested_ = false, quit_ = false;
    uint32_t failure_ = 0, close_result_ = 0;
    int quit_code_ = 0;
};

} // namespace pulse::ipc::picker
