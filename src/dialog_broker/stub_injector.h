#pragma once

#include <windows.h>
#include <cstdint>
#include <string>

namespace pulse::dialog_broker {

struct InjectionResult {
    DWORD error = ERROR_SUCCESS;
    DWORD activate_result = ERROR_SUCCESS;
    bool already_loaded = false;
};

// Injects a same-architecture Stub DLL (e.g. PulseDialogStub64.dll from x64 broker,
// or PulseDialogStub32.dll from x86 PulseDialogInject32.exe) into `pid` and calls
// `PulseStubActivate(activate_flags)`.
InjectionResult InjectSameArchStub(DWORD pid, const std::wstring& stub_dll_path,
                                   uint32_t activate_flags, DWORD timeout_ms = 5000) noexcept;

// Delegates WOW64 x86 injection from a 64-bit broker to PulseDialogInject32.exe.
InjectionResult InjectViaHelper32(const std::wstring& helper32_exe, DWORD pid,
                                  const std::wstring& stub32_dll_path,
                                  uint32_t activate_flags, DWORD timeout_ms = 8000) noexcept;

// Computes the RVA of a named PE export directly from the on-disk DLL image.
DWORD FindExportRva(const std::wstring& dll_path, const char* export_name, DWORD& rva_out) noexcept;

// Computes the RVA of a named PE export directly from an already-mapped module inside
// a remote process via ReadProcessMemory (safe even if the on-disk DLL was replaced/renamed).
DWORD FindRemoteExportRva(HANDLE process, HMODULE remote_mod, const char* export_name, DWORD& rva_out) noexcept;

} // namespace pulse::dialog_broker
