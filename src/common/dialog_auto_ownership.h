#pragma once

#include "dialog_auto_registration.h"
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>
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

inline bool IsRetiredDisabledPulseDialogDll(Architecture arch, const Status& status) {
    if (status.enabled || status.conflict || status.error || status.dll_path.empty()) return false;
    const auto& path = status.dll_path;
    const wchar_t* dll = DllNameFor(arch);
    const wchar_t* stub = StubDllNameFor(arch);
    if (!dll || !stub || path.size() < 4 || path.size() >= 32768 || path[1] != L':' || path[2] != L'\\' ||
        !((path[0] >= L'A' && path[0] <= L'Z') || (path[0] >= L'a' && path[0] <= L'z')) ||
        path.find_first_of(L"/<>\"|?*") != std::wstring::npos || path.find(L'\0') != std::wstring::npos ||
        path.find(L':', 2) != std::wstring::npos) return false;
    for (const wchar_t c : path) if (c < 32) return false;
    const auto equal = [](std::wstring_view a, std::wstring_view b) {
        return a.size() == b.size() && CompareStringOrdinal(a.data(), static_cast<int>(a.size()),
            b.data(), static_cast<int>(b.size()), TRUE) == CSTR_EQUAL;
    };
    std::vector<std::wstring_view> parts;
    size_t begin = 3;
    // Validate the entire suffix before accepting any missing ancestor.
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\') continue;
        const std::wstring_view part(path.data() + begin, i - begin);
        if (part.empty() || part == L"." || part == L".." || part.back() == L'.' || part.back() == L' ') return false;
        const auto device = part.substr(0, part.find(L'.'));
        if (equal(device, L"CON") || equal(device, L"PRN") || equal(device, L"AUX") || equal(device, L"NUL") ||
            (device.size() == 4 && (equal(device.substr(0, 3), L"COM") || equal(device.substr(0, 3), L"LPT")) &&
                ((device[3] >= L'1' && device[3] <= L'9') || device[3] == L'\u00b9' || device[3] == L'\u00b2' || device[3] == L'\u00b3')))
            return false;
        parts.push_back(part);
        begin = i + 1;
    }
    if (parts.size() < 3) return false;
    const auto parent = parts[parts.size() - 2];
    const auto grandparent = parts[parts.size() - 3];
    bool layout = false;
    if (equal(parts.back(), dll)) {
        const bool digest = parent.size() == 64 && std::all_of(parent.begin(), parent.end(), [](wchar_t c) {
            return (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f') || (c >= L'A' && c <= L'F');
        });
        layout = equal(parent, L"DialogBridge") ||
            (parts.size() >= 4 && equal(grandparent, L"DialogBridge") &&
                (digest || equal(parent, arch == Architecture::X64 ? L"x64" : L"x86")));
    } else if (equal(parts.back(), stub)) {
        layout = equal(parent, L"DialogBridge") && equal(grandparent, L"Pulse");
    }
    if (!layout) return false;
    std::wstring full(32768, L'\0');
    const DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (!length || length >= full.size()) return false;
    full.resize(length);
    if (!equal(path, full)) return false;
    const wchar_t drive[]{path[0], L':', L'\\', 0};
    if (GetDriveTypeW(drive) != DRIVE_FIXED) return false;
    const DWORD root_attributes = GetFileAttributesW(drive);
    if (root_attributes == INVALID_FILE_ATTRIBUTES || !(root_attributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    for (size_t i = 3; i <= path.size(); ++i) {
        if (i != path.size() && path[i] != L'\\') continue;
        const DWORD attributes = GetFileAttributesW(path.substr(0, i).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            const DWORD error = GetLastError();
            return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND;
        }
        if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            (i != path.size() && !(attributes & FILE_ATTRIBUTE_DIRECTORY))) return false;
    }
    return false;
}

}  // namespace pulse::dialog_auto
