#pragma once
#include <windows.h>
#include <propkeydef.h>
#include <string>
#include <vector>

namespace pulse::shell_bridge {
// Pre-Show only. Shell discovery runs on one bounded background COM worker;
// the caller waits at most timeout_ms while dispatching COM/window messages.
// Returns only value keys, never apartment-bound interfaces. No files are created.
// Failure/timeout means retain the native dialog, not an empty default schema.
HRESULT ResolveCollectedDefaults(const std::wstring& extension,
    std::vector<PROPERTYKEY>& keys, DWORD timeout_ms = 200) noexcept;
#ifdef PULSE_DEFAULTS_RESOLVER_TESTING
void SetCollectedDefaultsTestDelay(DWORD milliseconds) noexcept;
#endif
}
