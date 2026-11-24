#pragma once

#include "dialog_auto_registration.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <windows.h>

namespace pulse::dialog_auto {

inline std::wstring DefaultCommonDialogBridgeRoot() {
    wchar_t buf[MAX_PATH]{};
    DWORD len = GetEnvironmentVariableW(L"CommonProgramW6432", buf, ARRAYSIZE(buf));
    if (!len || len >= ARRAYSIZE(buf)) {
        len = GetEnvironmentVariableW(L"CommonProgramFiles", buf, ARRAYSIZE(buf));
    }
    if (!len || len >= ARRAYSIZE(buf)) return {};
    std::wstring root(buf, len);
    while (root.size() > 3 && (root.back() == L'\\' || root.back() == L'/')) root.pop_back();
    return root + L"\\Pulse\\DialogBridge";
}

inline bool IsOwnedDialogAutoDll(std::wstring base, bool installed_default, Architecture arch,
    const std::wstring& dll_path, std::wstring common_bridge_root = {}) {
    while (base.size() > 3 && base.back() == L'\\') base.pop_back();
    const wchar_t* name_ptr = DllNameFor(arch);
    const wchar_t* stub_ptr = StubDllNameFor(arch);
    if (base.empty() || dll_path.empty() || !name_ptr) return false;
    if (stub_ptr) {
        if (!common_bridge_root.empty()) {
            while (common_bridge_root.size() > 3 && common_bridge_root.back() == L'\\') common_bridge_root.pop_back();
            if (_wcsicmp(dll_path.c_str(), (common_bridge_root + L"\\" + stub_ptr).c_str()) == 0) return true;
        } else if (installed_default) {
            const auto default_common = DefaultCommonDialogBridgeRoot();
            if (!default_common.empty() &&
                _wcsicmp(dll_path.c_str(), (default_common + L"\\" + stub_ptr).c_str()) == 0) {
                return true;
            }
        }
    }
    const std::wstring name(name_ptr);
    if (_wcsicmp(dll_path.c_str(), (base + L"\\" + name).c_str()) == 0) return true;
    if (!installed_default) return false;
    const auto bridge = base + L"\\DialogBridge\\";
    if (_wcsicmp(dll_path.c_str(), (bridge + name).c_str()) == 0) return true;
    const std::wstring arch_dir = arch == Architecture::X86 ? L"x86\\" : L"x64\\";
    if (_wcsicmp(dll_path.c_str(), (bridge + arch_dir + name).c_str()) == 0) return true;
    if (dll_path.size() == bridge.size() + 65 + name.size() &&
        _wcsnicmp(dll_path.c_str(), bridge.c_str(), bridge.size()) == 0 &&
        _wcsicmp(dll_path.substr(bridge.size() + 64).c_str(), (L"\\" + name).c_str()) == 0) {
        const auto digest = dll_path.substr(bridge.size(), 64);
        return std::all_of(digest.begin(), digest.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        });
    }
    return false;
}

inline bool IsOrphanedMissingPulseDialogDll(Architecture arch, const Status& status) {
    // Only reclaim an active, consistent Pulse-owned registration when its old
    // on-disk DLL directory is genuinely missing; never disturb live foreign copies
    // or fail-open on access/reparse errors.
    if (!status.enabled || status.conflict || status.error || status.dll_path.empty()) return false;
    const auto& p = status.dll_path;
    if (p.size() < 4 || p.size() >= 32768 || p[1] != L':' || p[2] != L'\\' ||
        !((p[0] >= L'A' && p[0] <= L'Z') || (p[0] >= L'a' && p[0] <= L'z')) ||
        p.find_first_of(L"/<>\"|?*") != std::wstring::npos || p.find(L'\0') != std::wstring::npos ||
        p.find(L':', 2) != std::wstring::npos) return false;
    for (const wchar_t ch : p) if (ch < 32) return false;
    const size_t last_slash = p.find_last_of(L'\\');
    if (last_slash == std::wstring::npos || !IsAllowedDialogDllName(arch, p.c_str() + last_slash + 1)) return false;
    const wchar_t drive[] = {p[0], L':', L'\\', 0};
    if (GetDriveTypeW(drive) != DRIVE_FIXED) return false;
    size_t begin = 3;
    for (size_t i = 3; i <= p.size(); ++i) {
        if (i != p.size() && p[i] != L'\\') continue;
        const std::wstring_view component(p.data() + begin, i - begin);
        if (component.empty() || component == L"." || component == L".." ||
            component.back() == L'.' || component.back() == L' ') return false;
        const DWORD attrs = GetFileAttributesW(p.substr(0, i).c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD err = GetLastError();
            return err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND;
        }
        if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) return false;
        if (i != p.size() && !(attrs & FILE_ATTRIBUTE_DIRECTORY)) return false;
        begin = i + 1;
    }
    return false;
}

}  // namespace pulse::dialog_auto
