// takeover_record.h - what 接管压缩包 wrote to the registry, and how to undo it.
// Header-only Win32 so Pulse itself can undo a takeover whose extension is gone
// (an Explorer double-click then finds no 7-Zip extension to run).
//
// Everything lives under HKEY_CURRENT_USER:
//   Software\Classes\Pulse.Archive.<ext>             the file type: name, DefaultIcon, shell\open\command
//   Software\Classes\.<ext>\OpenWithProgids           value Pulse.Archive.<ext> (offered in 打开方式)
//   Software\Classes\.<ext> (Default)                 Pulse.Archive.<ext>, only where nothing opened .<ext>
//   Software\Pulse\ArchiveTakeover                    one REG_DWORD per extension: 1 = the default above was set
//   Software\Pulse\ArchiveCapabilities                Default Apps capabilities (registeredAppUser=Pulse)
//   Software\RegisteredApplications  value "Pulse"    -> Software\Pulse\ArchiveCapabilities
#pragma once
#include <windows.h>
#include <shlobj.h>
#include <string>
#include <vector>

namespace takeover {
inline constexpr wchar_t kRecordKey[] = L"Software\\Pulse\\ArchiveTakeover";
inline constexpr wchar_t kCapabilitiesKey[] = L"Software\\Pulse\\ArchiveCapabilities";
inline constexpr wchar_t kRegisteredApps[] = L"Software\\RegisteredApplications";
inline constexpr wchar_t kAppName[] = L"Pulse";
inline constexpr wchar_t kProgIdPrefix[] = L"Pulse.Archive.";
// The open command's argument: Pulse extracts, or shows the archive and previews it.
inline constexpr wchar_t kExtractLaunch[] = L"pulse:extract:";
inline constexpr wchar_t kPreviewLaunch[] = L"pulse:archive-preview:";

// ".zip" -> "Pulse.Archive.zip"
inline std::wstring ProgId(const std::wstring& extension) {
    return kProgIdPrefix + (extension.size() > 1 && extension[0] == L'.' ? extension.substr(1) : extension);
}
inline std::wstring ClassesKey(const std::wstring& name) { return L"Software\\Classes\\" + name; }

inline std::wstring ReadString(HKEY root, const std::wstring& key, const wchar_t* value) {
    wchar_t buffer[1024]{};
    DWORD bytes = sizeof(buffer) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buffer, &bytes) != ERROR_SUCCESS) return {};
    return buffer;
}
// Deletes a key that holds nothing (no values, no subkeys); a key with anything else in it stays.
inline void DeleteIfEmpty(HKEY root, const std::wstring& key) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return;
    DWORD subkeys = 0, values = 0;
    const bool empty = RegQueryInfoKeyW(handle, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                        nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS && !subkeys && !values;
    RegCloseKey(handle);
    if (empty) RegDeleteKeyW(root, key.c_str());
}
// Undoes one extension: the file type, the 打开方式 entry, and the default where it was ours.
inline void RemoveExtension(const std::wstring& extension) {
    const auto progid = ProgId(extension), ext_key = ClassesKey(extension), open_with = ext_key + L"\\OpenWithProgids";
    RegDeleteKeyValueW(HKEY_CURRENT_USER, open_with.c_str(), progid.c_str());
    if (_wcsicmp(ReadString(HKEY_CURRENT_USER, ext_key, nullptr).c_str(), progid.c_str()) == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, ext_key.c_str(), nullptr);
    RegDeleteTreeW(HKEY_CURRENT_USER, ClassesKey(progid).c_str());
    DeleteIfEmpty(HKEY_CURRENT_USER, open_with);
    DeleteIfEmpty(HKEY_CURRENT_USER, ext_key);
    std::wstring capability = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";
    RegDeleteKeyValueW(HKEY_CURRENT_USER, capability.c_str(), extension.c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRecordKey, extension.c_str());
}
// The extensions a takeover record lists.
inline std::vector<std::wstring> RecordedExtensions() {
    std::vector<std::wstring> result;
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRecordKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return result;
    for (DWORD index = 0;; ++index) {
        wchar_t name[64]{};
        DWORD length = ARRAYSIZE(name);
        const auto status = RegEnumValueW(key, index, name, &length, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) break;
        if (status == ERROR_SUCCESS && name[0] == L'.') result.emplace_back(name, length);
    }
    RegCloseKey(key);
    return result;
}
inline bool Recorded() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRecordKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}
// Undoes the whole takeover and lets Explorer redraw the icons. Returns whether anything was recorded.
inline bool RemoveAll() {
    if (!Recorded()) return false;
    for (const auto& extension : RecordedExtensions()) RemoveExtension(extension);
    if (_wcsicmp(ReadString(HKEY_CURRENT_USER, kRegisteredApps, kAppName).c_str(), kCapabilitiesKey) == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredApps, kAppName);
    RegDeleteTreeW(HKEY_CURRENT_USER, kCapabilitiesKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kRecordKey);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
    return true;
}
} // namespace takeover
