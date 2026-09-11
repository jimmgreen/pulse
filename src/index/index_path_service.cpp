#include "index_path_service.h"
#include "index_config.h"
#include "index_migration.h"
#include <windows.h>
#include <winsvc.h>

namespace pulse::index {
namespace {
struct ServiceHandle {
    SC_HANDLE value = nullptr;
    ~ServiceHandle() { if (value) CloseServiceHandle(value); }
};

DWORD ServiceState(SC_HANDLE service, DWORD& state) {
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    if (!QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status), sizeof(status), &needed)) return GetLastError();
    state = status.dwCurrentState;
    return ERROR_SUCCESS;
}

DWORD WaitForState(SC_HANDLE service, DWORD target) {
    const ULONGLONG deadline = GetTickCount64() + 60000;
    do {
        DWORD state = 0;
        const DWORD error = ServiceState(service, state);
        if (error) return error;
        if (state == target) return ERROR_SUCCESS;
        if (target == SERVICE_RUNNING && state == SERVICE_STOPPED) return ERROR_SERVICE_NOT_ACTIVE;
        Sleep(100);
    } while (GetTickCount64() < deadline);
    return ERROR_SERVICE_REQUEST_TIMEOUT;
}

DWORD StartAndWait(SC_HANDLE service) {
    if (!StartServiceW(service, 0, nullptr)) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_ALREADY_RUNNING) return error;
    }
    const DWORD error = WaitForState(service, SERVICE_RUNNING);
    if (error) return error;
    Sleep(3500);
    DWORD state = 0;
    const DWORD query = ServiceState(service, state);
    return query ? query : state == SERVICE_RUNNING ? ERROR_SUCCESS : ERROR_SERVICE_NOT_ACTIVE;
}

DWORD StopAndWait(SC_HANDLE service) {
    DWORD state = 0;
    DWORD error = ServiceState(service, state);
    if (error || state == SERVICE_STOPPED) return error;
    if (state == SERVICE_START_PENDING) {
        error = WaitForState(service, SERVICE_RUNNING);
        if (error) return error;
    }
    if (state != SERVICE_STOP_PENDING) {
        SERVICE_STATUS status{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &status)) return GetLastError();
    }
    return WaitForState(service, SERVICE_STOPPED);
}
}

int ConfigureServiceIndexPath(const std::wstring& path) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    IndexConfig previous;
    if (!LoadMachineConfig(previous, nullptr)) return ERROR_INVALID_DATA;
    if (SameIndexLocation(previous.index_path, path)) return ERROR_SUCCESS;
    ServiceHandle manager{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!manager.value) return static_cast<int>(GetLastError());
    ServiceHandle service{OpenServiceW(manager.value, L"PulseIndex",
        SERVICE_QUERY_STATUS | SERVICE_STOP | SERVICE_START)};
    if (!service.value) {
        const DWORD error = GetLastError();
        if (error != ERROR_SERVICE_DOES_NOT_EXIST) return static_cast<int>(error);
        // Installation configures the path before registering the service.
        IndexMigration migration;
        std::wstring message;
        if (!CopyIndexForMigration(previous.index_path, path, migration, message))
            return static_cast<int>(migration.failure);
        if (!ConfigureIndexPath(migration.target, nullptr)) {
            DiscardIndexMigrationCopies(migration);
            return ERROR_WRITE_FAULT;
        }
        return RemoveMigratedIndexSource(migration, message) ? 0 : ERROR_PARTIAL_COPY;
    }
    DWORD state = 0;
    DWORD error = ServiceState(service.value, state);
    if (error) return static_cast<int>(error);
    const bool restart = state != SERVICE_STOPPED;
    error = StopAndWait(service.value);
    if (error) return static_cast<int>(error);
    IndexMigration migration;
    std::wstring message;
    if (!CopyIndexForMigration(previous.index_path, path, migration, message)) {
        if (restart && StartAndWait(service.value)) return ERROR_SERVICE_NOT_ACTIVE;
        return static_cast<int>(migration.failure);
    }
    // Do not publish a new root while the old process still owns mapped files/deltas.
    if (!ConfigureIndexPath(migration.target, nullptr)) {
        DiscardIndexMigrationCopies(migration);
        if (restart && StartAndWait(service.value)) return ERROR_SERVICE_NOT_ACTIVE;
        return ERROR_WRITE_FAULT;
    }
    error = StartAndWait(service.value);
    if (error) {
        // Do not restore configuration until the unsuccessful new host has stopped.
        if (StopAndWait(service.value) == 0 && SaveMachineConfig(previous, nullptr)) {
            DiscardIndexMigrationCopies(migration);
            if (restart && StartAndWait(service.value)) return ERROR_SERVICE_NOT_ACTIVE;
        } else {
            return ERROR_SERVICE_NOT_ACTIVE;
        }
        return static_cast<int>(error);
    }
    return RemoveMigratedIndexSource(migration, message) ? 0 : ERROR_PARTIAL_COPY;
}
}
