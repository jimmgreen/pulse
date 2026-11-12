// worker_client.h - host-side out-of-process RPC client & Job Object sandbox
// controller for pulse_7zip_worker.exe.
#pragma once
#include "engine.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace worker_client {

struct WorkerLimits {
    size_t preview_memory_bytes = 512ull * 1024u * 1024u;
    size_t task_memory_bytes = 2048ull * 1024u * 1024u;
    uint32_t preview_hard_timeout_slack_ms = 2500u;
    uint32_t task_hard_timeout_ms = 0u;  // 0 = governed by Budget::max_duration_ms + cooperative cancel
    uint32_t cancel_grace_ms = 500u;
    bool allow_fault_injection = false;
};

void ConfigureForTests(const WorkerLimits& limits) noexcept;
void ResetForTests() noexcept;
uint32_t WarmWorkerPidForTests() noexcept;

enum class FaultKind {
    CrashAccessViolation,
    HangInfinite,
    ExceedMemory,
    OversizeFrame,
    WrongGeneration,
};

struct FaultOutcome {
    engine::Status status = engine::Status::IoError;
    std::string error;
    uint32_t worker_pid = 0;
};

FaultOutcome RunFaultForTests(FaultKind kind,
                              uint64_t param_bytes = 0,
                              const std::wstring& lock_file = {},
                              const engine::Cancelled& cancelled = {},
                              bool use_warm_preview_slot = true);

} // namespace worker_client
