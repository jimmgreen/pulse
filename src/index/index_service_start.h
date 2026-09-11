#pragma once
#include <windows.h>

namespace pulse::index {

// The caller supplies SCM operations; the same transitions can be tested without
// stopping or reconfiguring the installed service.
template<class Query, class Start, class Wait>
DWORD EnsureServiceRunning(Query query, Start start, Wait wait) {
    bool requested = false;
    for (DWORD elapsed = 0; elapsed <= 30000; elapsed += 100) {
        SERVICE_STATUS_PROCESS status{};
        if (const DWORD error = query(status)) return error;
        switch (status.dwCurrentState) {
        case SERVICE_RUNNING:
            if (!requested) return ERROR_SUCCESS;
            // A freshly started process must survive the early crash window.
            wait(3500);
            if (const DWORD error = query(status)) return error;
            if (status.dwCurrentState == SERVICE_RUNNING) return ERROR_SUCCESS;
            return status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
        case SERVICE_STOPPED:
            if (requested)
                return status.dwWin32ExitCode ? status.dwWin32ExitCode : ERROR_SERVICE_NOT_ACTIVE;
            if (const DWORD error = start(); error && error != ERROR_SERVICE_ALREADY_RUNNING)
                return error;
            requested = true;
            break;
        case SERVICE_START_PENDING:
        case SERVICE_STOP_PENDING:
        case SERVICE_CONTINUE_PENDING:
        case SERVICE_PAUSE_PENDING:
            break;
        default:
            return ERROR_SERVICE_CANNOT_ACCEPT_CTRL;
        }
        if (elapsed == 30000) break;
        wait(100);
    }
    return ERROR_SERVICE_REQUEST_TIMEOUT;
}

} // namespace pulse::index
