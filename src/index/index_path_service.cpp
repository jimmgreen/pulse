#include "index_path_service.h"
#include "index_config.h"
#include "index_migration.h"
#include "index_directory_security.h"
#include "index_service_start.h"
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

DWORD StopAndWait(SC_HANDLE service) {
    return EnsureServiceState(service, false);
}
}

int ConfigureServiceIndexPath(const std::wstring& path) {
    SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
    IndexConfig previous;
    if (!LoadMachineConfig(previous, nullptr)) return ERROR_INVALID_DATA;
    if (SameIndexLocation(previous.index_path, path)) {
        if (ProtectIndexDirectory(previous.index_path)) return ERROR_SUCCESS;
        // Setup re-applies the current location on every upgrade; adopt a folder
        // chosen before #66 here too, as the service does on start.
        const std::filesystem::path current(previous.index_path);
        PrivateIndexDirectoryLock current_lock;
        return current_lock.Acquire(current) && AdoptIndexDirectory(current) &&
                ProtectIndexDirectory(previous.index_path)
            ? ERROR_SUCCESS : static_cast<int>(kIndexDirectoryNotPrivate);
    }
    // Hold the destination identity across copy, config publication, service
    // restart and source cleanup, including rollback branches.
    PrivateIndexDirectoryLock migration_lock;
    try {
        const std::filesystem::path destination(ResolveIndexMigrationTarget(path));
        if (!destination.is_absolute() || destination == destination.root_path()) return ERROR_INVALID_PARAMETER;
        std::filesystem::create_directories(destination);
        if (!migration_lock.Acquire(destination)) return ERROR_ACCESS_DENIED;
    } catch (...) { return ERROR_ACCESS_DENIED; }
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
        if (!CopyIndexForMigration(previous.index_path, path, migration, message, true))
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
    const bool restart = RestartServiceAfterMigrationFailure(previous.search_enabled, state);
    error = StopAndWait(service.value);
    if (error) return static_cast<int>(error);
    IndexMigration migration;
    std::wstring message;
    if (!CopyIndexForMigration(previous.index_path, path, migration, message, true)) {
        if (restart) {
            if (const DWORD start_error = EnsureServiceState(service.value, true)) return static_cast<int>(start_error);
        }
        return static_cast<int>(migration.failure);
    }
    // Do not publish a new root while the old process still owns mapped files/deltas.
    if (!ConfigureIndexPath(migration.target, nullptr)) {
        DiscardIndexMigrationCopies(migration);
        if (restart) {
            if (const DWORD start_error = EnsureServiceState(service.value, true)) return static_cast<int>(start_error);
        }
        return ERROR_WRITE_FAULT;
    }
    error = EnsureServiceState(service.value, previous.search_enabled);
    if (error) {
        // Do not restore configuration until the unsuccessful new host has stopped.
        if (const DWORD stop_error = StopAndWait(service.value)) return static_cast<int>(stop_error);
        if (!SaveMachineConfig(previous, nullptr)) return ERROR_WRITE_FAULT;
        DiscardIndexMigrationCopies(migration);
        if (restart) {
            if (const DWORD start_error = EnsureServiceState(service.value, true)) return static_cast<int>(start_error);
        }
        return static_cast<int>(error);
    }
    return RemoveMigratedIndexSource(migration, message) ? 0 : ERROR_PARTIAL_COPY;
}
}
