#pragma once
#include "../shell_bridge/runtime_auto_entry.h"
namespace pulse::dialog_launcher::admin_auto {
inline constexpr unsigned kMaxAutoRebinds = 8;
inline constexpr ULONGLONG kAutoRebindCooldownMs = 5000;
inline bool CanReuseAutoRuntime(bool exact_trusted_path, bool already_found, uintptr_t attach_offset,
    uintptr_t diagnose_offset, uint32_t module_size) noexcept {
    return exact_trusted_path && !already_found && attach_offset != 0 && diagnose_offset != 0 &&
        attach_offset < module_size && diagnose_offset < module_size;
}
inline bool CanAutoRebind(const pulse::shell_bridge::runtime::PulseRuntimeAutoDiagnosticV1& value) noexcept {
    return value.size == sizeof(value) && value.version == 1 &&
        value.initialization == pulse::shell_bridge::runtime::kAutoReadyForRebind &&
        value.api_mask == 0 && value.patched_slots == 0 && value.protection_restored == 1;
}
inline bool ShouldContinueProbing(const pulse::shell_bridge::runtime::PulseRuntimeAutoDiagnosticV1& value) noexcept {
    // S_OK (not kAutoReadyForRebind) means the runtime is already active in this
    // target; stop probing so we never inject periodic remote threads forever.
    return value.size == sizeof(value) && value.version == 1 && value.initialization != S_OK;
}
}
