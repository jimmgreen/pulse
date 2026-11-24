#pragma once
#include "runtime_config.h"

namespace pulse::shell_bridge::runtime {
// An explicit, process-local bootstrap. Start copies all input before returning.
// Call outside loader lock; S_OK means queued, not ready. Query determines readiness.
// Experimental static-IAT coverage only. No business thread is suspended and no
// code instructions are patched. Late DLLs, delay imports, GetProcAddress and
// modules in Windows/the runtime bundle directory are not intercepted.
// This is not a general-purpose attachment guarantee for every activation path.
struct PulseRuntimeAutoStartV1 {
    uint32_t size = sizeof(PulseRuntimeAutoStartV1), version = 1;
    wchar_t nonce[40]{};
    wchar_t host[kPathUnits]{};
    wchar_t control[kPathUnits]{};
};
struct PulseRuntimeAutoCoverageV1 {
    uint32_t size = sizeof(PulseRuntimeAutoCoverageV1), version = 1;
    uint32_t api_mask = 0, patched_slots = 0, skipped_modules = 0, foreign_slots = 0;
    uint32_t protection_restored = 1;
};
// Published only for an explicitly completed, fully restored auto lease.
inline constexpr int32_t kAutoReadyForRebind = HRESULT_FROM_WIN32(ERROR_RETRY);

// Why a runtime never published ready: what the elevated helper reads after its
// ready wait expires. initialization stays E_PENDING while startup is still running.
struct PulseRuntimeAutoDiagnosticV1 {
    uint32_t size = sizeof(PulseRuntimeAutoDiagnosticV1), version = 1;
    int32_t initialization = 0;
    uint32_t api_mask = 0, patched_slots = 0, skipped_modules = 0, foreign_slots = 0;
    uint32_t protection_restored = 1;
};
}
#if defined(PULSE_RUNTIME_AUTO_ENTRY)
#define PULSE_AUTO_ENTRY_EXPORT __declspec(dllexport)
#else
#define PULSE_AUTO_ENTRY_EXPORT
#endif
extern "C" PULSE_AUTO_ENTRY_EXPORT HRESULT WINAPI PulseFileDialogRuntimeAutoStart(
    const pulse::shell_bridge::runtime::PulseRuntimeAutoStartV1* request, uint32_t size) noexcept;
// Single-argument thread ABI. The pointed-to fixed record owns no external
// pointers. S_OK means copied and queued; attachment requires a live binding.
extern "C" PULSE_AUTO_ENTRY_EXPORT HRESULT WINAPI PulseFileDialogRuntimeAutoAttach(void* request) noexcept;
// Non-elevated user-mode adaptive attach (rejects elevated targets; hooks legacy
// Win32 & WinRT dialog entrypoints and runs UI out-of-process via verified Pulse).
extern "C" PULSE_AUTO_ENTRY_EXPORT HRESULT WINAPI PulseFileDialogRuntimeUserAutoAttach(void* request) noexcept;
extern "C" PULSE_AUTO_ENTRY_EXPORT HRESULT WINAPI PulseFileDialogRuntimeAutoQuery(
    pulse::shell_bridge::runtime::PulseRuntimeAutoCoverageV1* output, uint32_t size) noexcept;
// Single-argument thread ABI, like AutoAttach: fills a PulseRuntimeAutoDiagnosticV1
// whose size and version are preset by the caller; returns a Win32 error code.
extern "C" PULSE_AUTO_ENTRY_EXPORT DWORD WINAPI PulseFileDialogRuntimeAutoDiagnose(void* output) noexcept;
#undef PULSE_AUTO_ENTRY_EXPORT

