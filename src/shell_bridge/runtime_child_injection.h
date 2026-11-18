#pragma once
#include <windows.h>
namespace pulse::shell_bridge::runtime {
struct ChildInjectionResult {
    DWORD error = ERROR_SUCCESS;
    bool injected = false;
    bool restored = false;
    bool rollback_failed = false;
};
// Only the caller's newly created suspended same-architecture native process.
// The test seam forces failure AFTER Detours writes, exercising real rollback.
ChildInjectionResult InjectChildRuntime(HANDLE process, const char* dll,
    bool fail_after_update_for_test = false) noexcept;
}
