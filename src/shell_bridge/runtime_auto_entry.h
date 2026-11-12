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
extern "C" PULSE_AUTO_ENTRY_EXPORT HRESULT WINAPI PulseFileDialogRuntimeAutoQuery(
    pulse::shell_bridge::runtime::PulseRuntimeAutoCoverageV1* output, uint32_t size) noexcept;
#undef PULSE_AUTO_ENTRY_EXPORT

