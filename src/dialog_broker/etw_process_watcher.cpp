#include "etw_process_watcher.h"

#include <evntrace.h>
#include <evntcons.h>
#include <cstring>
#include <vector>

namespace pulse::dialog_broker {
namespace {

// Microsoft-Windows-Kernel-Process {22FB2CD6-0E7B-422B-A0C7-2FAD1FD0E716}
constexpr GUID kKernelProcessProviderGuid = {
    0x22FB2CD6, 0x0E7B, 0x422B, {0xA0, 0xC7, 0x2F, 0xAD, 0x1F, 0xD0, 0xE7, 0x16}
};
constexpr ULONGLONG kKeywordProcess = 0x10;
constexpr USHORT kEventIdProcessStart = 1;

} // namespace

bool ParseProcessStartPayload(uint8_t version, const void* data, uint32_t length,
                              ProcessStartEvent& out) noexcept {
    out = {};
    if (!data) return false;
    const auto* bytes = static_cast<const uint8_t*>(data);
    if (version >= 3) {
        if (length < 36) return false;
        std::memcpy(&out.pid, bytes + 0, sizeof(uint32_t));
        std::memcpy(&out.create_time, bytes + 12, sizeof(uint64_t));
        std::memcpy(&out.parent_pid, bytes + 20, sizeof(uint32_t));
        std::memcpy(&out.session_id, bytes + 32, sizeof(uint32_t));
        return true;
    }
    if (length < 20) return false;
    std::memcpy(&out.pid, bytes + 0, sizeof(uint32_t));
    std::memcpy(&out.create_time, bytes + 4, sizeof(uint64_t));
    std::memcpy(&out.parent_pid, bytes + 12, sizeof(uint32_t));
    std::memcpy(&out.session_id, bytes + 16, sizeof(uint32_t));
    return true;
}

EtwProcessWatcher::~EtwProcessWatcher() { Stop(); }

void WINAPI EtwProcessWatcher::OnEventRecord(PEVENT_RECORD record) {
    if (!record || !record->UserContext) return;
    if (record->EventHeader.EventDescriptor.Id != kEventIdProcessStart) return;
    if (!IsEqualGUID(record->EventHeader.ProviderId, kKernelProcessProviderGuid)) return;

    auto* self = static_cast<EtwProcessWatcher*>(record->UserContext);
    if (!self->running_.load(std::memory_order_acquire) || !self->callback_) return;

    ProcessStartEvent evt{};
    if (!ParseProcessStartPayload(record->EventHeader.EventDescriptor.Version,
                                  record->UserData, record->UserDataLength, evt)) {
        evt.pid = record->EventHeader.ProcessId;
        evt.session_id = 1; // unknown session; let ProcessCandidate verify via ProcessIdToSessionId
    }

    if (evt.pid <= 4) return;
    self->events_received_.fetch_add(1, std::memory_order_relaxed);
    try {
        self->callback_(evt);
    } catch (...) {}
}

ULONG WINAPI EtwProcessWatcher::OnBufferCallback(PEVENT_TRACE_LOGFILEW logfile) noexcept {
    if (!logfile || !logfile->Context) return TRUE;
    auto* self = static_cast<EtwProcessWatcher*>(logfile->Context);
    return self->running_.load(std::memory_order_acquire) ? TRUE : FALSE;
}

DWORD WINAPI EtwProcessWatcher::ConsumerThreadProc(void* param) noexcept {
    auto* self = static_cast<EtwProcessWatcher*>(param);
    TRACEHANDLE handle = static_cast<TRACEHANDLE>(self->trace_handle_);
    const ULONG rc = ProcessTrace(&handle, 1, nullptr, nullptr);
    self->last_process_trace_error_.store(rc, std::memory_order_release);
    self->consumer_alive_.store(false, std::memory_order_release);
    self->running_.store(false, std::memory_order_release);
    return rc;
}

DWORD EtwProcessWatcher::Start(Callback on_process_start, const std::wstring& session_name) noexcept {
    try {
        Stop();
        callback_ = std::move(on_process_start);
        session_name_ = session_name.empty() ? std::wstring(kDefaultEtwSessionName) : session_name;
        last_start_stage_ = "prepare";
        last_process_trace_error_.store(ERROR_SUCCESS, std::memory_order_release);
        events_received_.store(0, std::memory_order_release);

        const size_t name_bytes = (session_name_.size() + 1) * sizeof(wchar_t);
        const size_t prop_bytes = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes;
        std::vector<uint8_t> prop_buf(prop_bytes, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(prop_buf.data());
        props->Wnode.BufferSize = static_cast<ULONG>(prop_bytes);
        props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
        props->Wnode.ClientContext = 1; // QPC clock
        props->LogFileMode = EVENT_TRACE_REAL_TIME_MODE;
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        std::memcpy(prop_buf.data() + props->LoggerNameOffset, session_name_.c_str(), name_bytes);

        // Stop any orphaned session of the same name from a prior crash before starting fresh.
        ControlTraceW(0, session_name_.c_str(), props, EVENT_TRACE_CONTROL_STOP);

        // Process starts are rare. With per-CPU buffers an event can wait in an idle
        // CPU's partially filled buffer; broker logs showed 10-50 s from process start
        // to injection on a 20-CPU machine despite FlushTimer=1, so new elevated apps
        // were injected after their first file dialog. One shared buffer set lets the
        // flush timer bound delivery; retry without it if the system rejects the mode.
        const auto start_session = [&](ULONG mode, TRACEHANDLE& session) {
            std::memset(prop_buf.data(), 0, prop_bytes);
            props->Wnode.BufferSize = static_cast<ULONG>(prop_bytes);
            props->Wnode.Flags = WNODE_FLAG_TRACED_GUID;
            props->Wnode.ClientContext = 1;
            props->LogFileMode = mode;
            props->BufferSize = 64;
            // Let ETW size MinimumBuffers/MaximumBuffers for the host logical CPU count.
            props->MinimumBuffers = 0;
            props->MaximumBuffers = 0;
            props->FlushTimer = 1;
            props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
            std::memcpy(prop_buf.data() + props->LoggerNameOffset, session_name_.c_str(), name_bytes);
            return StartTraceW(&session, session_name_.c_str(), props);
        };

        last_start_stage_ = "start_trace";
        TRACEHANDLE session = 0;
        shared_buffering_ = true;
        ULONG err = start_session(EVENT_TRACE_REAL_TIME_MODE | EVENT_TRACE_NO_PER_PROCESSOR_BUFFERING, session);
        if (err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED) {
            shared_buffering_ = false;
            session = 0;
            err = start_session(EVENT_TRACE_REAL_TIME_MODE, session);
        }
        if (err != ERROR_SUCCESS) { shared_buffering_ = false; return err; }
        session_handle_ = static_cast<uint64_t>(session);

        last_start_stage_ = "enable_trace_ex2";
        err = EnableTraceEx2(session, &kKernelProcessProviderGuid, EVENT_CONTROL_CODE_ENABLE_PROVIDER,
                             TRACE_LEVEL_INFORMATION, kKeywordProcess, 0, 0, nullptr);
        if (err != ERROR_SUCCESS) {
            Stop();
            return err;
        }

        last_start_stage_ = "open_trace";
        EVENT_TRACE_LOGFILEW logfile{};
        logfile.LoggerName = const_cast<LPWSTR>(session_name_.c_str());
        logfile.ProcessTraceMode = PROCESS_TRACE_MODE_REAL_TIME | PROCESS_TRACE_MODE_EVENT_RECORD;
        logfile.EventRecordCallback = &EtwProcessWatcher::OnEventRecord;
        logfile.BufferCallback = &EtwProcessWatcher::OnBufferCallback;
        logfile.Context = this;

        TRACEHANDLE trace = OpenTraceW(&logfile);
        if (trace == INVALID_PROCESSTRACE_HANDLE) {
            const DWORD open_err = GetLastError();
            Stop();
            return open_err ? open_err : ERROR_GEN_FAILURE;
        }
        trace_handle_ = static_cast<uint64_t>(trace);
        running_.store(true, std::memory_order_release);
        consumer_alive_.store(true, std::memory_order_release);

        last_start_stage_ = "create_thread";
        consumer_thread_ = CreateThread(nullptr, 0, &EtwProcessWatcher::ConsumerThreadProc, this, 0, nullptr);
        if (!consumer_thread_) {
            const DWORD thread_err = GetLastError();
            consumer_alive_.store(false, std::memory_order_release);
            Stop();
            return thread_err ? thread_err : ERROR_GEN_FAILURE;
        }

        last_start_stage_ = "ok";
        return ERROR_SUCCESS;
    } catch (...) {
        last_start_stage_ = "exception";
        Stop();
        return ERROR_NOT_ENOUGH_MEMORY;
    }
}

void EtwProcessWatcher::Stop() noexcept {
    running_.store(false, std::memory_order_release);
    // Stop the ETW session first so real-time buffers flush and ProcessTrace exits promptly.
    if (session_handle_ != 0) {
        const std::wstring& active_name = session_name_.empty()
            ? std::wstring(kDefaultEtwSessionName) : session_name_;
        const size_t name_bytes = (active_name.size() + 1) * sizeof(wchar_t);
        const size_t prop_bytes = sizeof(EVENT_TRACE_PROPERTIES) + name_bytes;
        std::vector<uint8_t> prop_buf(prop_bytes, 0);
        auto* props = reinterpret_cast<EVENT_TRACE_PROPERTIES*>(prop_buf.data());
        props->Wnode.BufferSize = static_cast<ULONG>(prop_bytes);
        props->LoggerNameOffset = sizeof(EVENT_TRACE_PROPERTIES);
        std::memcpy(prop_buf.data() + props->LoggerNameOffset, active_name.c_str(), name_bytes);
        ControlTraceW(static_cast<TRACEHANDLE>(session_handle_), nullptr, props, EVENT_TRACE_CONTROL_STOP);
        session_handle_ = 0;
    }
    if (trace_handle_ != static_cast<uint64_t>(INVALID_PROCESSTRACE_HANDLE)) {
        CloseTrace(static_cast<TRACEHANDLE>(trace_handle_));
        trace_handle_ = static_cast<uint64_t>(INVALID_PROCESSTRACE_HANDLE);
    }
    if (consumer_thread_) {
        WaitForSingleObject(consumer_thread_, 3000);
        CloseHandle(consumer_thread_);
        consumer_thread_ = nullptr;
    }
    consumer_alive_.store(false, std::memory_order_release);
    callback_ = nullptr;
}

} // namespace pulse::dialog_broker
