#include "setup_service.h"

#include <vector>

namespace pulse::setup {
namespace {

struct ScHandle {
    SC_HANDLE h = nullptr;
    ~ScHandle() { if (h) CloseServiceHandle(h); }
};

}  // namespace

bool ServiceExists(const wchar_t* name) {
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return false;
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_QUERY_STATUS)};
    return svc.h != nullptr;
}
DWORD ReadServiceStatus(const wchar_t* name, SERVICE_STATUS_PROCESS& status) {
    status = {};
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return GetLastError();
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_QUERY_STATUS)};
    if (!svc.h) return GetLastError();
    DWORD needed = 0;
    return QueryServiceStatusEx(svc.h, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) ? ERROR_SUCCESS : GetLastError();
}
DWORD DisableService(const wchar_t* name) {
    const DWORD stopped = StopService(name, 15000);
    if (stopped) return stopped;
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return GetLastError();
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_CHANGE_CONFIG)};
    if (!svc.h) { const DWORD error = GetLastError(); return error == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : error; }
    return ChangeServiceConfigW(svc.h, SERVICE_NO_CHANGE, SERVICE_DISABLED, SERVICE_NO_CHANGE,
        nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr) ? ERROR_SUCCESS : GetLastError();
}
DWORD SwitchServiceRuntime(const wchar_t* name, const std::wstring& executable, bool& exists) {
    exists = true;
    if (executable.empty()) return ERROR_INVALID_PARAMETER;
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return GetLastError();
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_CHANGE_CONFIG)};
    if (!svc.h) {
        const DWORD error = GetLastError();
        if (error == ERROR_SERVICE_DOES_NOT_EXIST) { exists = false; return ERROR_SUCCESS; }
        return error;
    }
    const std::wstring command = L"\"" + executable + L"\" --service";
    const DWORD stopped = StopService(name, 15000);
    if (stopped) return stopped;
    if (!ChangeServiceConfigW(svc.h, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
        command.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, nullptr)) return GetLastError();
    return ERROR_SUCCESS;
}

DWORD StopService(const wchar_t* name, unsigned timeout_ms) {
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return GetLastError();
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_STOP | SERVICE_QUERY_STATUS)};
    if (!svc.h) {
        const DWORD e = GetLastError();
        return e == ERROR_SERVICE_DOES_NOT_EXIST ? ERROR_SUCCESS : e;
    }
    SERVICE_STATUS status{};
    if (!ControlService(svc.h, SERVICE_CONTROL_STOP, &status)) {
        const DWORD e = GetLastError();
        if (e == ERROR_SERVICE_NOT_ACTIVE) return ERROR_SUCCESS;
        if (e != ERROR_SERVICE_CANNOT_ACCEPT_CTRL) return e;
    }
    for (unsigned waited = 0; waited <= timeout_ms; waited += 250) {
        if (!QueryServiceStatus(svc.h, &status)) return GetLastError();
        if (status.dwCurrentState == SERVICE_STOPPED) return ERROR_SUCCESS;
        Sleep(250);
    }
    return ERROR_SERVICE_REQUEST_TIMEOUT;
}

bool WaitUntilServiceGone(const wchar_t* name, unsigned timeout_ms) {
    for (unsigned waited = 0;; waited += 250) {
        if (!ServiceExists(name)) return true;
        if (waited >= timeout_ms) return false;
        Sleep(250);
    }
}

std::wstring ServiceBinaryPath(const wchar_t* name) {
    ScHandle scm{OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT)};
    if (!scm.h) return {};
    ScHandle svc{OpenServiceW(scm.h, name, SERVICE_QUERY_CONFIG)};
    if (!svc.h) return {};
    DWORD needed = 0;
    QueryServiceConfigW(svc.h, nullptr, 0, &needed);
    if (!needed) return {};
    std::vector<BYTE> buffer(needed);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(svc.h, config, needed, &needed) || !config->lpBinaryPathName) return {};
    return config->lpBinaryPathName;
}

}  // namespace pulse::setup
