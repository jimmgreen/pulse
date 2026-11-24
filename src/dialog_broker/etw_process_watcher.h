#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

struct _EVENT_RECORD;
struct _EVENT_TRACE_LOGFILEW;

namespace pulse::dialog_broker {

inline constexpr wchar_t kDefaultEtwSessionName[] = L"PulseDialogBrokerProcessSession";

struct ProcessStartEvent {
    DWORD pid = 0;
    DWORD parent_pid = 0;
    DWORD session_id = 0;
};

// Real-time ETW subscriber for Microsoft-Windows-Kernel-Process
// ({22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}, WINEVENT_KEYWORD_PROCESS = 0x10).
// Requires SYSTEM / Administrator (SeSystemProfilePrivilege / Performance Log Users).
class EtwProcessWatcher {
public:
    using Callback = std::function<void(const ProcessStartEvent&)>;

    EtwProcessWatcher() = default;
    ~EtwProcessWatcher();
    EtwProcessWatcher(const EtwProcessWatcher&) = delete;
    EtwProcessWatcher& operator=(const EtwProcessWatcher&) = delete;

    DWORD Start(Callback on_process_start,
                const std::wstring& session_name = kDefaultEtwSessionName) noexcept;
    void Stop() noexcept;
    bool Running() const noexcept { return running_.load(std::memory_order_acquire); }
    bool Healthy() const noexcept {
        return running_.load(std::memory_order_acquire) &&
               consumer_alive_.load(std::memory_order_acquire);
    }
    const char* LastStartStage() const noexcept { return last_start_stage_; }
    DWORD LastProcessTraceError() const noexcept {
        return last_process_trace_error_.load(std::memory_order_acquire);
    }
    uint64_t EventsReceived() const noexcept {
        return events_received_.load(std::memory_order_acquire);
    }
    const std::wstring& SessionName() const noexcept { return session_name_; }

private:
    static void WINAPI OnEventRecord(::_EVENT_RECORD* record);
    static ULONG WINAPI OnBufferCallback(::_EVENT_TRACE_LOGFILEW* logfile) noexcept;
    static DWORD WINAPI ConsumerThreadProc(void* param) noexcept;

    Callback callback_;
    std::wstring session_name_;
    uint64_t session_handle_ = 0;
    uint64_t trace_handle_ = ~0ull;
    HANDLE consumer_thread_ = nullptr;
    const char* last_start_stage_ = "none";
    std::atomic<bool> running_{false};
    std::atomic<bool> consumer_alive_{false};
    std::atomic<DWORD> last_process_trace_error_{ERROR_SUCCESS};
    std::atomic<uint64_t> events_received_{0};
};

} // namespace pulse::dialog_broker
