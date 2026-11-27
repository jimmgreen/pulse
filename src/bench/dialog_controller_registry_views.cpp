#include "dialog_controller_registry_views.h"
#undef RegCreateKeyExW
#undef RegOpenKeyExW
#undef RegDeleteKeyExW
#include <string>

namespace pulse::bench::registry_views {
namespace {
std::wstring Path(HKEY key, LPCWSTR path, REGSAM flags, bool isolated) {
    if (!isolated || key != HKEY_CURRENT_USER) return path ? path : L"";
    std::wstring result = flags & KEY_WOW64_32KEY ? L"PulseTestViews\\x86" : L"PulseTestViews\\x64";
    if (path && *path) { result += L'\\'; result += path; }
    return result;
}
REGSAM Access(REGSAM access, bool isolated) {
    return isolated ? access & ~(KEY_WOW64_64KEY | KEY_WOW64_32KEY) : access;
}
}
LSTATUS WINAPI Create(HKEY key, LPCWSTR path, DWORD reserved, LPWSTR cls, DWORD options, REGSAM access,
    const LPSECURITY_ATTRIBUTES security, PHKEY result, LPDWORD disposition) {
    const bool isolated = enabled.load();
    const auto mapped = Path(key, path, access, isolated);
    return ::RegCreateKeyExW(key, mapped.c_str(), reserved, cls, options, Access(access, isolated), security, result, disposition);
}
LSTATUS WINAPI Open(HKEY key, LPCWSTR path, DWORD options, REGSAM access, PHKEY result) {
    const bool isolated = enabled.load();
    const auto mapped = Path(key, path, access, isolated);
    return ::RegOpenKeyExW(key, mapped.c_str(), options, Access(access, isolated), result);
}
LSTATUS WINAPI DeleteKey(HKEY key, LPCWSTR path, REGSAM access, DWORD reserved) {
    const bool isolated = enabled.load();
    const auto mapped = Path(key, path, access, isolated);
    return ::RegDeleteKeyExW(key, mapped.c_str(), Access(access, isolated), reserved);
}
}

// Compile the unmodified production registration implementation against the
// same real-registry view mapping as the controller's arch-status reader.
#define RegCreateKeyExW pulse::bench::registry_views::Create
#define RegOpenKeyExW pulse::bench::registry_views::Open
#define RegDeleteKeyExW pulse::bench::registry_views::DeleteKey
#include "../common/dialog_auto_registration.cpp"
