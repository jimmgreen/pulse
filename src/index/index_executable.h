// index_executable.h — Which Pulse.Index.exe the UI launches for index commands.
//
// The sibling of the running pulse.exe is preferred (installed copies and dev
// builds keep both side by side). A pulse.exe started from somewhere else, such
// as a test build in Downloads, has no sibling; it then manages the installed
// PulseIndex service through that service's own executable instead of asking
// Windows to run a file that does not exist.
#pragma once
#include <cwctype>
#include <functional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>
#include <winsvc.h>

namespace pulse::index {

inline constexpr std::wstring_view kIndexExecutableName = L"Pulse.Index.exe";

// Executable path from a service BINARY_PATH_NAME, e.g.
// "\"C:\\Program Files\\Pulse\\Pulse.Index.exe\" --service". Unquoted paths with
// spaces are accepted. Empty unless the image is named Pulse.Index.exe.
inline std::wstring IndexExecutableFromServiceCommand(std::wstring_view command) {
    while (!command.empty() && iswspace(command.front())) command.remove_prefix(1);
    const bool quoted = !command.empty() && command.front() == L'"';
    if (quoted) command.remove_prefix(1);
    const size_t n = kIndexExecutableName.size();
    for (size_t at = 0; at + n <= command.size(); ++at) {
        if (quoted && command[at] == L'"') return {};
        bool match = true;
        for (size_t i = 0; i < n && match; ++i)
            match = towlower(command[at + i]) == towlower(kIndexExecutableName[i]);
        if (!match) continue;
        // The name must be a whole path component that ends the image path.
        if (at > 0 && command[at - 1] != L'\\' && command[at - 1] != L'/') continue;
        const size_t end = at + n;
        if (end < command.size() && command[end] != L'"' && !iswspace(command[end])) continue;
        if (quoted && (end >= command.size() || command[end] != L'"')) return {};
        if (at == 0) return {};  // a bare name is not a usable location
        return std::wstring(command.substr(0, end));
    }
    return {};
}

// BINARY_PATH_NAME of an installed service; empty when it is not installed.
// SERVICE_QUERY_CONFIG is granted to interactive users, so no elevation.
inline std::wstring InstalledServiceCommand(const wchar_t* service_name) {
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return {};
    std::wstring command;
    if (SC_HANDLE svc = OpenServiceW(scm, service_name, SERVICE_QUERY_CONFIG)) {
        DWORD needed = 0;
        if (!QueryServiceConfigW(svc, nullptr, 0, &needed) && GetLastError() == ERROR_INSUFFICIENT_BUFFER && needed) {
            std::vector<BYTE> buffer(needed);
            auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
            if (QueryServiceConfigW(svc, config, needed, &needed) && config->lpBinaryPathName)
                command = config->lpBinaryPathName;
        }
        CloseServiceHandle(svc);
    }
    CloseServiceHandle(scm);
    return command;
}

// sibling: Pulse.Index.exe next to the running pulse.exe. service_command: the
// installed service's BINARY_PATH_NAME (empty when not installed).
inline std::wstring ResolveIndexExecutable(const std::wstring& sibling, std::wstring_view service_command,
                                           const std::function<bool(const std::wstring&)>& exists) {
    if (exists(sibling)) return sibling;
    const std::wstring installed = IndexExecutableFromServiceCommand(service_command);
    if (!installed.empty() && exists(installed)) return installed;
    return sibling;
}

} // namespace pulse::index
