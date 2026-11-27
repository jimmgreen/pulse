#pragma once
#include <windows.h>
#include <string>
#include "../common/browse_request.h"
#include "../common/direct_reveal_client.h"
#include "../common/native_browse_guard.h"
namespace pulse::shell_bridge::explorer_process {
using Native = decltype(&::CreateProcessW);
using Gate = bool (*)() noexcept;
// Worker only: validates the protected installed package and holds its files
// against replacement for the lifetime of published routing configuration.
DWORD Configure(HMODULE owner, Gate gate) noexcept;
void Disable() noexcept;
enum class Readiness { Disabled, Configuring, Ready, Unavailable };
struct Status { Readiness readiness; DWORD error; };
Status QueryStatus() noexcept;
bool IsReady() noexcept;
inline bool NativeFallbackActive() noexcept { return native_browse::Active(); }
using NativeFallbackScope = native_browse::Scope;
// Common Shell adapter endpoint. Accepted means IPC acceptance or a real broker
// child was launched; it never promises that the target window was painted.
DirectRevealResult SubmitBrowse(const browse::Request& request, DWORD timeout_ms = 250) noexcept;
// False delegates the original call unchanged, including unsupported requests,
// unavailable configuration and bridge creation failure. True means the real
// bridge was created; its own dispatcher owns any later native fallback.
bool TryRoute(Native original, LPCWSTR app, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information, BOOL& result) noexcept;
// Exact original contract; registers recognized native browsing before calling
// the real API. Failure to register never replaces the native result.
BOOL FallbackCreateProcess(Native original, LPCWSTR app, LPWSTR command,
    LPSECURITY_ATTRIBUTES process_attributes, LPSECURITY_ATTRIBUTES thread_attributes,
    BOOL inherit, DWORD flags, LPVOID environment, LPCWSTR directory,
    LPSTARTUPINFOW startup, LPPROCESS_INFORMATION information) noexcept;
#ifdef PULSE_EXPLORER_PROCESS_TEST
void RevokeForTest(DWORD error) noexcept;
void SetClassificationFailureForTest(bool enabled) noexcept;
DWORD VerifyBridgeForTest(const std::wstring& bridge, const std::string& digest) noexcept;
DWORD ConfigureForTest(const std::wstring& bridge, Gate gate) noexcept;
#endif
}
