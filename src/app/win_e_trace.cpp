#include "win_e_trace.h"
#include "win_e_context.h"
#include "../common/runtime_log.h"
#include <utility>

namespace pulse::app {
namespace {
namespace log = diagnostics::runtime;
template<size_t N, size_t... I>
void TraceEvent(const char* event, const log::Field (&fields)[N], log::Level level, std::index_sequence<I...>) {
    static_assert(N <= 12, "Win+E trace exceeds runtime logger field capacity");
    log::Event(event, {fields[I]...}, level);
}
template<size_t N>
void TraceEvent(const char* event, const log::Field (&fields)[N], log::Level level = log::Level::Info) {
    TraceEvent(event, fields, level, std::make_index_sequence<N>{});
}
}
WinETrace::WinETrace() : WinETrace(nullptr) {}
WinETrace::WinETrace(HANDLE test_start_gate) {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!wake_) {
        TraceEvent("win_e_trace_failed", {{"error", GetLastError()}, {"stage", 1}}, diagnostics::runtime::Level::Error);
        return;
    }
    try {
        worker_ = std::thread([this, test_start_gate] {
            if (test_start_gate) WaitForSingleObject(test_start_gate, INFINITE);
            Run();
        });
    } catch (...) {
        CloseHandle(wake_); wake_ = nullptr;
        TraceEvent("win_e_trace_failed", {{"error", ERROR_GEN_FAILURE}, {"stage", 2}}, diagnostics::runtime::Level::Error);
    }
}
WinETrace::~WinETrace() {
    stopping_ = true;
    if (wake_) SetEvent(wake_);
    if (worker_.joinable()) worker_.join();
    if (wake_) CloseHandle(wake_);
}
void WinETrace::Push(const WinETraceRecord& record) noexcept {
    const auto written = written_.load(std::memory_order_relaxed);
    if (!wake_ || written - read_.load(std::memory_order_acquire) >= records_.size()) {
        ++dropped_;
        return;
    }
    records_[written % records_.size()] = record;
    written_.store(written + 1, std::memory_order_release);
    SetEvent(wake_);
}
void WinETrace::Run() noexcept {
    WinEContextObserver observer;
    uint64_t heartbeat = 0, reported_drops = 0, reported_context = 0;
    try {
        for (;;) {
            auto read = read_.load(std::memory_order_relaxed);
            while (read != written_.load(std::memory_order_acquire)) {
                const auto record = records_[read % records_.size()];
                read_.store(++read, std::memory_order_release);
                const auto now = GetTickCount64();
                const auto context = observer.Poll(record.foreground, now);
                if (record.kind == WinETraceRecord::Input) {
                    TraceEvent("win_e_input", {{"cycle", record.cycle}, {"context_id", context},
                        {"captured_tick", record.tick}, {"input_time_u32", record.input_time},
                        {"phase", record.phase}, {"action", record.action}, {"observed", record.observed},
                        {"asynchronous", record.asynchronous}, {"flags", record.flags},
                        {"accepted", record.result}, {"generation", record.generation}, {"delay_ms", now - record.tick}});
                } else if (record.kind == WinETraceRecord::Submission) {
                    TraceEvent("win_e_chord", {{"cycle", record.cycle}, {"context_id", context},
                        {"captured_tick", record.tick}, {"accepted", record.result}, {"menu_mask", record.phase},
                        {"id_high", record.id_high}, {"id_low", record.id_low}, {"prepared", record.action},
                        {"wake_error", record.error}, {"generation", record.generation}});
                } else {
                    TraceEvent("win_e_hook_refresh", {{"context_id", context},
                        {"captured_tick", record.tick}, {"generation", record.generation},
                        {"success", record.result}, {"error", record.error}, {"reason", record.phase}});
                }
            }
            const auto now = GetTickCount64();
            const auto context = observer.Poll(GetForegroundWindow(), now);
            const auto drops = dropped_.load();
            if (!heartbeat || now - heartbeat >= 10000 || drops != reported_drops || context != reported_context || stopping_) {
                TraceEvent("win_e_trace_health", {{"context_id", observer.CurrentContextId()},
                    {"records", read}, {"dropped", drops}, {"stopping", stopping_.load()},
                    {"e_down", e_down_.load()}, {"e_up", e_up_.load()}});
                heartbeat = now; reported_context = context;
                reported_drops = drops;
            }
            if (stopping_ && read == written_.load(std::memory_order_acquire)) break;
            WaitForSingleObject(wake_, 250);
        }
    } catch (...) {
        TraceEvent("win_e_trace_failed", {{"error", ERROR_NOT_ENOUGH_MEMORY},
            {"remaining", written_.load() - read_.load()}, {"dropped", dropped_.load()}, {"stage", 3}},
            diagnostics::runtime::Level::Error);
    }
}
}
