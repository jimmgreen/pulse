#include "explorer_source_scope.h"

namespace pulse::app {
std::wstring CanonicalExplorerScopePath(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!count || count >= result.size()) return {};
    result.resize(count); return result;
}
bool ExplorerScopeWindow(const ExplorerSourceScope& scope, HWND window) {
    if (!window || reinterpret_cast<uint64_t>(window) != scope.token.hwnd || GetAncestor(window, GA_ROOT) != window) return false;
    DWORD pid = 0;
    const DWORD tid = GetWindowThreadProcessId(window, &pid);
    if (pid != scope.token.pid || tid != scope.token.tid) return false;
    wchar_t desktop[256]{}; DWORD bytes = 0;
    if (!GetUserObjectInformationW(GetThreadDesktop(tid), UOI_NAME, desktop, sizeof(desktop), &bytes) ||
        wcscmp(desktop, L"Default") != 0) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    FILETIME created{}, exited{}, kernel{}, user{};
    wchar_t image[32768]{}, windows[32768]{}; DWORD count = ARRAYSIZE(image);
    const bool read = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
        QueryFullProcessImageNameW(process, 0, image, &count);
    CloseHandle(process);
    ExplorerScopeToken actual_token;
    actual_token.hwnd = reinterpret_cast<uint64_t>(window); actual_token.pid = pid; actual_token.tid = tid;
    actual_token.created = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    actual_token.owner = reinterpret_cast<uint64_t>(GetPropW(window, kExplorerScopeOwner));
    if (!read || !ExplorerScopeIdentity(scope.token, actual_token) ||
        !GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) return false;
    const auto expected = CanonicalExplorerScopePath(std::wstring(windows) + L"\\explorer.exe");
    const auto actual = CanonicalExplorerScopePath(image);
    return !expected.empty() && !actual.empty() && _wcsicmp(expected.c_str(), actual.c_str()) == 0;
}
}
