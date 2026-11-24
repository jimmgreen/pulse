#pragma once
#include <windows.h>
namespace pulse {
struct DirectRevealInstallDiagnostic {
    DWORD stage = 0, error = 0, thread_id = 0;
};
struct DirectRevealBootstrapDiagnostic {
    DWORD size = sizeof(DirectRevealBootstrapDiagnostic), version = 1;
    DWORD stage = 0, token_reason = 0, token_error = 0, log_error = 0;
    HRESULT folder_status = E_PENDING;
    DirectRevealInstallDiagnostic install;
};
}
