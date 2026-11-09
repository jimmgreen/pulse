#pragma once
#include "../common/runtime_log.h"
#include <windows.h>
#include <condition_variable>
#include <chrono>
#include <utility>
#include <functional>
#include <mutex>
#include <thread>
#include <system_error>

namespace pulse::ops {
// Numeric snapshots only: never copy filenames, and never wait on the operation lock.
struct TransferDiagnosticSnapshot {
    uint64_t bytes = 0, total = 0, items = 0, phase = 0;
    bool available = false, paused = false, cancelled = false;
};
class TransferDiagnosticMonitor {
public:
    TransferDiagnosticMonitor(uint64_t task, std::function<TransferDiagnosticSnapshot()> snapshot)
        : task_(task), started_(GetTickCount64()), snapshot_(std::move(snapshot)) {
        if (!diagnostics::runtime::Enabled()) return;
        diagnostics::runtime::Event("copy_monitor_begin", {{"task", task_}});
        try {
            worker_ = std::thread([this] {
                uint64_t previous_time = started_, previous_bytes = 0, last_change = started_;
                std::unique_lock lock(mutex_);
                while (!cv_.wait_for(lock, std::chrono::seconds(5), [this] { return stopped_; })) {
                    lock.unlock();
                    const auto value = snapshot_();
                    const auto now = GetTickCount64();
                    if (value.available && value.bytes != previous_bytes) last_change = now;
                    diagnostics::runtime::Event("copy_heartbeat", {{"task", task_},
                        {"elapsed_ms", now - started_}, {"interval_ms", now - previous_time},
                        {"snapshot_available", value.available}, {"bytes", value.bytes}, {"total_bytes", value.total},
                        {"interval_bytes", value.available && value.bytes >= previous_bytes ? value.bytes - previous_bytes : 0},
                        {"no_observed_progress_ms", now - last_change}, {"items", value.items},
                        {"phase", value.phase}, {"paused", value.paused}, {"cancel_requested", value.cancelled}});
                    if (value.available) { previous_bytes = value.bytes; previous_time = now; }
                    lock.lock();
                }
            });
        } catch (const std::system_error&) {
            diagnostics::runtime::Event("copy_monitor_unavailable", {{"task", task_}});
        }
    }
    ~TransferDiagnosticMonitor() {
        if (!worker_.joinable()) return;
        { std::lock_guard lock(mutex_); stopped_ = true; }
        cv_.notify_one();
        worker_.join();
        const auto value = snapshot_();
        diagnostics::runtime::Event("copy_monitor_end", {{"task", task_}, {"elapsed_ms", GetTickCount64() - started_},
            {"snapshot_available", value.available}, {"bytes", value.bytes}, {"total_bytes", value.total},
            {"phase", value.phase}, {"cancel_requested", value.cancelled}});
    }
private:
    uint64_t task_, started_;
    std::function<TransferDiagnosticSnapshot()> snapshot_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stopped_ = false;
    std::thread worker_;
};
}
