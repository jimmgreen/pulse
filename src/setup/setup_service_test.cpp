#include <windows.h>
#include <string>
#include <cstdio>

namespace fake {
DWORD state = SERVICE_STOPPED, open_error = 0, config_error = 0, query_error = 0;
unsigned stops = 0, starts = 0, changes = 0;
bool stopped_at_change = false, policy_preserved = false;
std::wstring binary;
SC_HANDLE WINAPI Manager(LPCWSTR, LPCWSTR, DWORD) { return reinterpret_cast<SC_HANDLE>(1); }
SC_HANDLE WINAPI Open(SC_HANDLE, LPCWSTR, DWORD) {
    if (open_error) { SetLastError(open_error); return nullptr; }
    return reinterpret_cast<SC_HANDLE>(2);
}
BOOL WINAPI Close(SC_HANDLE) { return TRUE; }
BOOL WINAPI Stop(SC_HANDLE, DWORD command, LPSERVICE_STATUS status) {
    ++stops;
    if (command != SERVICE_CONTROL_STOP) return FALSE;
    if (state == SERVICE_STOPPED) { SetLastError(ERROR_SERVICE_NOT_ACTIVE); return FALSE; }
    status->dwCurrentState = state = SERVICE_STOPPED;
    return TRUE;
}
BOOL WINAPI Query(SC_HANDLE, LPSERVICE_STATUS status) { status->dwCurrentState = state; return TRUE; }
BOOL WINAPI QueryEx(SC_HANDLE, SC_STATUS_TYPE, LPBYTE bytes, DWORD, LPDWORD needed) {
    if (query_error) { SetLastError(query_error); return FALSE; }
    auto& status = *reinterpret_cast<SERVICE_STATUS_PROCESS*>(bytes);
    status = {}; status.dwCurrentState = state; *needed = sizeof(status); return TRUE;
}
BOOL WINAPI Configure(SC_HANDLE, DWORD type, DWORD start, DWORD error, LPCWSTR path, LPCWSTR group,
    LPDWORD tag, LPCWSTR dependencies, LPCWSTR account, LPCWSTR password, LPCWSTR display) {
    ++changes; stopped_at_change = state == SERVICE_STOPPED;
    policy_preserved = type == SERVICE_NO_CHANGE && start == SERVICE_AUTO_START && error == SERVICE_NO_CHANGE &&
        !group && !tag && !dependencies && !account && !password && !display;
    binary = path ? path : L"";
    if (config_error) { SetLastError(config_error); return FALSE; }
    return TRUE;
}
BOOL WINAPI Start(SC_HANDLE, DWORD, LPCWSTR*) { ++starts; return TRUE; }
void Reset() { state = SERVICE_STOPPED; open_error = config_error = query_error = 0;
    stops = starts = changes = 0; stopped_at_change = policy_preserved = false; binary.clear(); }
}

#define OpenSCManagerW fake::Manager
#define OpenServiceW fake::Open
#define CloseServiceHandle fake::Close
#define ControlService fake::Stop
#define QueryServiceStatus fake::Query
#define QueryServiceStatusEx fake::QueryEx
#define ChangeServiceConfigW fake::Configure
#define StartServiceW fake::Start
#include "setup_service.cpp"

int main() {
    unsigned failures = 0;
    const auto check = [&](bool value, const char* name) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", name); if (!value) ++failures;
    };
    bool exists = true;
    fake::Reset();
    check(pulse::setup::SwitchServiceRuntime(L"fixture", L"C:\\fixture\\Pulse.Index.exe", exists) == 0 &&
        exists && fake::changes == 1 && fake::starts == 0 && fake::stopped_at_change && fake::policy_preserved &&
        fake::binary == L"\"C:\\fixture\\Pulse.Index.exe\" --service",
        "stopped service switches runtime without starting or replacing account/security policy");
    fake::Reset(); fake::state = SERVICE_RUNNING;
    check(pulse::setup::SwitchServiceRuntime(L"fixture", L"C:\\new.exe", exists) == 0 &&
        fake::stops == 1 && fake::stopped_at_change && fake::starts == 0,
        "running old runtime stops before replacement and host decides subsequent state");
    fake::Reset(); fake::open_error = ERROR_SERVICE_DOES_NOT_EXIST;
    check(pulse::setup::SwitchServiceRuntime(L"fixture", L"C:\\new.exe", exists) == 0 && !exists &&
        fake::changes == 0 && fake::starts == 0, "missing service is explicitly delegated to creation");
    fake::Reset(); fake::open_error = ERROR_ACCESS_DENIED;
    check(pulse::setup::SwitchServiceRuntime(L"fixture", L"C:\\new.exe", exists) == ERROR_ACCESS_DENIED && exists,
        "access error never masquerades as missing service");
    fake::Reset(); fake::config_error = ERROR_ACCESS_DENIED;
    check(pulse::setup::SwitchServiceRuntime(L"fixture", L"C:\\new.exe", exists) == ERROR_ACCESS_DENIED && !fake::starts,
        "runtime configuration failure is retained without a start attempt");
    fake::Reset(); SERVICE_STATUS_PROCESS status{};
    check(pulse::setup::ReadServiceStatus(L"fixture", status) == 0 && status.dwCurrentState == SERVICE_STOPPED,
        "finish summary sees actual stopped state");
    fake::state = SERVICE_RUNNING;
    check(pulse::setup::ReadServiceStatus(L"fixture", status) == 0 && status.dwCurrentState == SERVICE_RUNNING,
        "finish summary sees actual running state");
    fake::query_error = ERROR_ACCESS_DENIED;
    check(pulse::setup::ReadServiceStatus(L"fixture", status) == ERROR_ACCESS_DENIED && status.dwCurrentState == 0,
        "status failure cannot reuse stale running state");
    return failures ? 1 : 0;
}
