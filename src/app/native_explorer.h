#pragma once
#include <windows.h>
#include <string>
#include <string_view>
#include <vector>

namespace pulse::app::native_explorer {
enum class Intent { OpenFolder, SelectFile };
struct Request {
    std::wstring path;
    Intent intent = Intent::OpenFolder;
};
inline constexpr UINT kCompletedMessage = WM_APP + 151;

// Lexical only: no filesystem, Shell, COM or network access on the UI thread.
inline bool IsFilesystemPath(std::wstring_view path) noexcept {
    if (path.empty() || path.size() >= 32768 || path.find_first_of(L"\"\r\n\t") != path.npos ||
        path.find(L'\0') != path.npos) return false;
    if (path.starts_with(L"\\\\?\\UNC\\")) {
        path.remove_prefix(8);
        const auto slash = path.find(L'\\');
        return path.find(L':') == path.npos && slash != path.npos && slash > 0 &&
            slash + 1 < path.size() && path[slash + 1] != L'\\';
    }
    else if (path.starts_with(L"\\\\?\\")) path.remove_prefix(4);
    if (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
        (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
        (path[2] == L'\\' || path[2] == L'/')) return path.find(L':', 2) == path.npos;
    if (!path.starts_with(L"\\\\") || path.starts_with(L"\\\\.\\") || path.find(L':') != path.npos) return false;
    const auto slash = path.find(L'\\', 2);
    return slash != path.npos && slash > 2 && slash + 1 < path.size() && path[slash + 1] != L'\\';
}

// Zero means queued, not that Explorer was displayed. Owns a value-copy on a
// bounded worker; never captures AppState. Completion has no heap payload:
// wParam=DWORD error (zero on success), lParam=owner lifetime cookie.
DWORD OpenAsync(Request request, HWND completion_owner = nullptr) noexcept;
bool IsCompletionFor(HWND owner, LPARAM cookie) noexcept;

// Shared by every takeover observer. Exact native HWND/process lifetime only;
// A bounded pending intent precedes exact HWND retention; no settings change.
bool IsExplicitNativeWindow(HWND window) noexcept;
}

namespace pulse::app::native_explorer {
// Takeover-worker only. Delays cosmetic veiling, not ordinary handoff decisions.
bool ShouldDelayVeil(HWND window) noexcept;
bool ShouldWaitForRequest(HWND window, const std::wstring& folder) noexcept;
bool RetainRequestedWindow(HWND window, const std::wstring& folder,
    const std::vector<std::wstring>& selected, bool selection_read) noexcept;
}
