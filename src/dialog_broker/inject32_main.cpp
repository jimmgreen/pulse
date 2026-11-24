#include "stub_injector.h"
#include "../common/dialog_admin_trust.h"

#include <windows.h>
#include <cstdlib>
#include <cwchar>
#include <string>

// Minimal 32-bit helper invoked by 64-bit PulseDialogBroker.exe to inject
// PulseDialogStub32.dll into WOW64 x86 target processes cleanly.
int wmain(int argc, wchar_t** argv) {
    DWORD pid = 0;
    std::wstring dll_path;
    uint32_t flags = 0;

    for (int i = 1; i + 1 < argc; ++i) {
        if (_wcsicmp(argv[i], L"--pid") == 0) {
            pid = static_cast<DWORD>(wcstoul(argv[++i], nullptr, 10));
        } else if (_wcsicmp(argv[i], L"--dll") == 0) {
            dll_path = argv[++i];
        } else if (_wcsicmp(argv[i], L"--flags") == 0) {
            flags = static_cast<uint32_t>(wcstoul(argv[++i], nullptr, 10));
        }
    }

    if (pid <= 4 || dll_path.empty()) return ERROR_INVALID_PARAMETER;
    if (const DWORD trust_err = pulse::dialog_admin::VerifyProtectedPath(dll_path, false);
        trust_err != ERROR_SUCCESS) {
        return static_cast<int>(trust_err);
    }

    const auto res = pulse::dialog_broker::InjectSameArchStub(pid, dll_path, flags);
    if (res.error != ERROR_SUCCESS) return static_cast<int>(res.error);
    return static_cast<int>(res.activate_result);
}
