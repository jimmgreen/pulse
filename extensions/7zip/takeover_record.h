// takeover_record.h - what 接管压缩包 wrote to the registry, and how to undo it.
// Header-only Win32 so Pulse itself can undo a takeover whose extension is gone
// (an Explorer double-click then finds no 7-Zip extension to run).
//
// Everything lives under HKEY_CURRENT_USER:
//   Software\Classes\Pulse.Archive.<ext>             the file type: name, DefaultIcon, shell\open\command
//   Software\Classes\.<ext>\OpenWithProgids           value Pulse.Archive.<ext> (offered in 打开方式)
//   Software\Classes\.<ext> (Default)                 Pulse.Archive.<ext>, where no usable archiver opened .<ext>
//   Software\Pulse\ArchiveTakeover                    one REG_DWORD per extension: 1 = the default above was set
//   Software\Pulse\ArchiveTakeover\Previous\.<ext> exact previous extension default
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
// Save the exact previous default once, before claiming it. Missing and empty values differ.
inline bool ClaimDefault(const std::wstring& extension) {
    const auto key = ClassesKey(extension), progid = ProgId(extension);
    if (_wcsicmp(ReadString(HKEY_CURRENT_USER, key, nullptr).c_str(), progid.c_str()) == 0) return true;
    const auto backup = std::wstring(kRecordKey) + L"\\Previous\\" + extension;
    DWORD saved = 0, size = sizeof(saved);
    const auto saved_status = RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Saved", RRF_RT_REG_DWORD, nullptr, &saved, &size);
    if (saved_status != ERROR_SUCCESS && saved_status != ERROR_FILE_NOT_FOUND) return false;
    if (saved_status == ERROR_SUCCESS && saved != 1) return false;
    if (saved_status == ERROR_FILE_NOT_FOUND) {
        HKEY handle = nullptr;
        auto status = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle);
        DWORD type = REG_NONE, bytes = 0;
        if (status == ERROR_SUCCESS) status = RegQueryValueExW(handle, nullptr, nullptr, &type, nullptr, &bytes);
        const bool present = status == ERROR_SUCCESS;
        if (!present && status != ERROR_FILE_NOT_FOUND) { if (handle) RegCloseKey(handle); return false; }
        std::vector<BYTE> data(bytes);
        if (present && bytes) status = RegQueryValueExW(handle, nullptr, nullptr, &type, data.data(), &bytes);
        if (handle) RegCloseKey(handle);
        if (present && status != ERROR_SUCCESS) return false;
        const DWORD exists = present ? 1 : 0;
        if (RegSetKeyValueW(HKEY_CURRENT_USER, backup.c_str(), L"Present", REG_DWORD, &exists, sizeof(exists)) != ERROR_SUCCESS ||
            RegSetKeyValueW(HKEY_CURRENT_USER, backup.c_str(), L"Type", REG_DWORD, &type, sizeof(type)) != ERROR_SUCCESS ||
            RegSetKeyValueW(HKEY_CURRENT_USER, backup.c_str(), L"Data", REG_BINARY, data.data(), bytes) != ERROR_SUCCESS) return false;
        saved = 1;
        if (RegSetKeyValueW(HKEY_CURRENT_USER, backup.c_str(), L"Saved", REG_DWORD, &saved, sizeof(saved)) != ERROR_SUCCESS) return false;
    }
    return RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), nullptr, REG_SZ, progid.c_str(),
                          static_cast<DWORD>((progid.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}
inline bool RestoreDefault(const std::wstring& extension) {
    const auto backup = std::wstring(kRecordKey) + L"\\Previous\\" + extension;
    DWORD saved = 0, present = 0, type = 0, size = sizeof(DWORD);
    const auto saved_status = RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Saved", RRF_RT_REG_DWORD, nullptr, &saved, &size);
    if (saved_status == ERROR_FILE_NOT_FOUND)
        return RegDeleteKeyValueW(HKEY_CURRENT_USER, ClassesKey(extension).c_str(), nullptr) == ERROR_SUCCESS;
    if (saved_status != ERROR_SUCCESS || saved != 1) return false;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Present", RRF_RT_REG_DWORD, nullptr, &present, &size) != ERROR_SUCCESS) return false;
    if (!present) return RegDeleteKeyValueW(HKEY_CURRENT_USER, ClassesKey(extension).c_str(), nullptr) == ERROR_SUCCESS;
    size = sizeof(DWORD);
    if (RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Type", RRF_RT_REG_DWORD, nullptr, &type, &size) != ERROR_SUCCESS) return false;
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Data", RRF_RT_REG_BINARY, nullptr, nullptr, &bytes) != ERROR_SUCCESS) return false;
    std::vector<BYTE> data(bytes);
    if (RegGetValueW(HKEY_CURRENT_USER, backup.c_str(), L"Data", RRF_RT_REG_BINARY, nullptr, data.data(), &bytes) != ERROR_SUCCESS) return false;
    return RegSetKeyValueW(HKEY_CURRENT_USER, ClassesKey(extension).c_str(), nullptr, type, data.data(), bytes) == ERROR_SUCCESS;
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
inline bool RemoveExtension(const std::wstring& extension) {
    const auto progid = ProgId(extension), ext_key = ClassesKey(extension), open_with = ext_key + L"\\OpenWithProgids";
    RegDeleteKeyValueW(HKEY_CURRENT_USER, open_with.c_str(), progid.c_str());
    if (_wcsicmp(ReadString(HKEY_CURRENT_USER, ext_key, nullptr).c_str(), progid.c_str()) == 0)
        if (!RestoreDefault(extension)) return false;
    RegDeleteTreeW(HKEY_CURRENT_USER, (std::wstring(kRecordKey) + L"\\Previous\\" + extension).c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, ClassesKey(progid).c_str());
    DeleteIfEmpty(HKEY_CURRENT_USER, open_with);
    DeleteIfEmpty(HKEY_CURRENT_USER, ext_key);
    std::wstring capability = std::wstring(kCapabilitiesKey) + L"\\FileAssociations";
    RegDeleteKeyValueW(HKEY_CURRENT_USER, capability.c_str(), extension.c_str());
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kRecordKey, extension.c_str());
    return true;
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
    bool restored = true;
    for (const auto& extension : RecordedExtensions()) restored = RemoveExtension(extension) && restored;
    if (!restored) return false;
    if (_wcsicmp(ReadString(HKEY_CURRENT_USER, kRegisteredApps, kAppName).c_str(), kCapabilitiesKey) == 0)
        RegDeleteKeyValueW(HKEY_CURRENT_USER, kRegisteredApps, kAppName);
    RegDeleteTreeW(HKEY_CURRENT_USER, kCapabilitiesKey);
    RegDeleteTreeW(HKEY_CURRENT_USER, kRecordKey);
#ifndef PULSE_ASSOCIATION_TEST
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
#endif
    return true;
}
} // namespace takeover
