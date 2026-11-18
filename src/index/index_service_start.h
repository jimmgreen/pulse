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

// Installing the service is independent from enabling search. A disabled
// configuration requires a stopped service, never a failed start treated as OK.
template<class Query, class Start, class Stop, class Wait>
DWORD EnsureServiceState(bool search_enabled, Query query, Start start, Stop stop, Wait wait) {
    if (search_enabled) return EnsureServiceRunning(query, start, wait);
    bool requested = false;
    for (DWORD elapsed = 0; elapsed <= 30000; elapsed += 100) {
        SERVICE_STATUS_PROCESS status{};
        if (const DWORD error = query(status)) return error;
        if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        switch (status.dwCurrentState) {
        case SERVICE_RUNNING:
        case SERVICE_PAUSED:
            if (!requested) {
                const DWORD error = stop();
                if (error && error != ERROR_SERVICE_NOT_ACTIVE) return error;
                requested = true;
            }
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

inline DWORD EnsureServiceState(SC_HANDLE service, bool search_enabled) {
    return EnsureServiceState(search_enabled,
        [&](SERVICE_STATUS_PROCESS& status) -> DWORD {
            DWORD needed = 0;
            return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
                reinterpret_cast<LPBYTE>(&status), sizeof(status), &needed) ? ERROR_SUCCESS : GetLastError();
        },
        [&]() -> DWORD { return StartServiceW(service, 0, nullptr) ? ERROR_SUCCESS : GetLastError(); },
        [&]() -> DWORD {
            SERVICE_STATUS status{};
            return ControlService(service, SERVICE_CONTROL_STOP, &status) ? ERROR_SUCCESS : GetLastError();
        },
        [](DWORD delay) { Sleep(delay); });
}

inline bool RestartServiceAfterMigrationFailure(bool search_enabled, DWORD previous_state) {
    return search_enabled && previous_state != SERVICE_STOPPED;
}

} // namespace pulse::index
