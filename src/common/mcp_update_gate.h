#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace pulse::mcp_update {

inline constexpr DWORD kRetryAfterUpdate = ERROR_INSTALL_ALREADY_RUNNING;
inline constexpr char kRetryMessage[] = "Pulse: An update is in progress. Reconnect to MCP after it finishes.";

inline std::wstring GatePath(std::wstring_view directory) {
    return std::wstring(directory) + L"\\.pulse-mcp-update.lock";
}

inline bool IsGated(const std::wstring& gate) {
    if (GetFileAttributesW(gate.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    // Some Windows file systems report DELETE_PENDING as access denied while
    // the installer holds the delete-on-close file handle.
    const auto error = GetLastError();
    return error == ERROR_ACCESS_DENIED || error == ERROR_DELETE_PENDING || error == ERROR_SHARING_VIOLATION;
}

inline std::wstring_view Parent(std::wstring_view path) {
    const auto slash = path.find_last_of(L"\\/");
    return slash == path.npos ? std::wstring_view{} : path.substr(0, slash);
}

inline bool Same(std::wstring_view a, std::wstring_view b) {
    return !a.empty() && !b.empty() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(),
        static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
}

inline std::wstring_view InstallRootForHost(std::wstring_view image) {
    const auto directory = Parent(image);
    const auto family_path = Parent(directory);
    const auto family_root = Parent(family_path);
    if (family_root.empty()) return directory;
    const auto family = family_path.substr(family_root.size() + 1);
    const auto digest = directory.substr(family_path.size() + 1);
    if (digest.size() != 64 || digest.find_first_not_of(L"0123456789abcdef") != digest.npos ||
        (!Same(family, L"DialogBridge") && !Same(family, L"AppRuntime"))) return directory;
    return family_root;
}

} // namespace pulse::mcp_update
