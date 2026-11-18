#pragma once
// Minimal Service Control Manager helpers for the PulseIndex service.

#include <windows.h>

#include <string>

namespace pulse::setup {

bool ServiceExists(const wchar_t* name);
DWORD ReadServiceStatus(const wchar_t* name, SERVICE_STATUS_PROCESS& status);
// Returns ERROR_SUCCESS when stopped (or absent), otherwise a Win32 error.
DWORD StopService(const wchar_t* name, unsigned timeout_ms);
// Reversible removal preparation: retains the account, dependencies and identity.
DWORD DisableService(const wchar_t* name);
// Existing services retain account, dependencies, security and recovery policy.
// Only changes the runtime while stopped; the host applies its configured state.
// `exists` is only false for ERROR_SERVICE_DOES_NOT_EXIST, never for access errors.
DWORD SwitchServiceRuntime(const wchar_t* name, const std::wstring& executable, bool& exists);
bool WaitUntilServiceGone(const wchar_t* name, unsigned timeout_ms);
std::wstring ServiceBinaryPath(const wchar_t* name);

}  // namespace pulse::setup
