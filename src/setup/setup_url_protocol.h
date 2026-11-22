#pragma once

#include "setup_registry.h"
#include <cstring>
#include <vector>

namespace pulse::setup {
inline constexpr const wchar_t* kProtocolKeys[] = {
    L"Software\\Classes\\pulse", L"Software\\Classes\\pulse",
    L"Software\\Classes\\pulse\\shell\\open\\command"};
inline constexpr const wchar_t* kProtocolNames[] = {L"", L"URL Protocol", L""};
inline constexpr wchar_t kProtocolBackup[] = L"Software\\Pulse\\SetupUrlProtocol";
inline constexpr const wchar_t* kProtocolBackupNames[] = {L"Description", L"Marker", L"Command"};

inline LSTATUS ReadProtocolValue(const wchar_t* path, const wchar_t* name, DWORD& type, std::vector<BYTE>& data) {
    HKEY key = nullptr;
    auto result = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_QUERY_VALUE, &key);
    if (result != ERROR_SUCCESS) return result;
    DWORD size = 0;
    result = RegQueryValueExW(key, name, nullptr, &type, nullptr, &size);
    if (result == ERROR_SUCCESS) {
        if (size > 65536) result = ERROR_INVALID_DATA;
        else {
            data.resize(size);
            result = RegQueryValueExW(key, name, nullptr, &type, data.data(), &size);
            if (result == ERROR_SUCCESS) data.resize(size);
        }
    }
    RegCloseKey(key);
    return result;
}

inline bool ConfigureUrlProtocol(const std::wstring& app, bool install, std::wstring& error) {
    auto fail = [&](LSTATUS code) { error = L"Pulse URL protocol registration failed: " + std::to_wstring(code); return false; };
    const auto command = L"\"" + app + L"\\pulse.exe\" --shell-open \"%1\"";
    if (!install && ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") != command) return true;
    // Prepare every backup before changing any public value. Never replace the
    // original backup during an in-place or directory-move upgrade.
    std::vector<BYTE> saved[3];
    for (size_t i = 0; i < 3; ++i) {
        DWORD type = 0;
        auto result = ReadProtocolValue(kProtocolBackup, kProtocolBackupNames[i], type, saved[i]);
        if (result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND) {
            if (!install) return fail(ERROR_INVALID_DATA);
            std::vector<BYTE> bytes;
            result = ReadProtocolValue(kProtocolKeys[i], kProtocolNames[i], type, bytes);
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND && result != ERROR_PATH_NOT_FOUND) return fail(result);
            if (bytes.size() > 65536 - 2 * sizeof(DWORD)) return fail(ERROR_INVALID_DATA);
            const DWORD header[] = {result == ERROR_SUCCESS ? 1u : 0u, type};
            saved[i].resize(sizeof(header) + bytes.size());
            std::memcpy(saved[i].data(), header, sizeof(header));
            if (!bytes.empty()) std::memcpy(saved[i].data() + sizeof(header), bytes.data(), bytes.size());
            HKEY key = nullptr;
            result = RegCreateKeyExW(HKEY_CURRENT_USER, kProtocolBackup, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (result != ERROR_SUCCESS) return fail(result);
            result = RegSetValueExW(key, kProtocolBackupNames[i], 0, REG_BINARY, saved[i].data(), static_cast<DWORD>(saved[i].size()));
            RegCloseKey(key);
            if (result != ERROR_SUCCESS) return fail(result);
        } else if (result != ERROR_SUCCESS || type != REG_BINARY) return fail(result == ERROR_SUCCESS ? ERROR_INVALID_DATA : result);
        DWORD exists = 0;
        if (saved[i].size() < 2 * sizeof(DWORD)) return fail(ERROR_INVALID_DATA);
        std::memcpy(&exists, saved[i].data(), sizeof(exists));
        if (exists > 1) return fail(ERROR_INVALID_DATA);
    }
    if (install) {
        const std::wstring values[] = {L"URL:Pulse Protocol", L"", command};
        for (size_t i = 0; i < 3; ++i)
            if (!WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[i], kProtocolNames[i], values[i])) return fail(ERROR_WRITE_FAULT);
    } else {
        for (size_t i = 0; i < 3; ++i) {
            DWORD header[2];
            std::memcpy(header, saved[i].data(), sizeof(header));
            HKEY key = nullptr;
            auto result = RegCreateKeyExW(HKEY_CURRENT_USER, kProtocolKeys[i], 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
            if (result != ERROR_SUCCESS) return fail(result);
            result = header[0] ? RegSetValueExW(key, kProtocolNames[i], 0, header[1], saved[i].data() + sizeof(header), static_cast<DWORD>(saved[i].size() - sizeof(header)))
                               : RegDeleteValueW(key, kProtocolNames[i]);
            RegCloseKey(key);
            if (result != ERROR_SUCCESS && result != ERROR_FILE_NOT_FOUND) return fail(result);
        }
        for (auto name : kProtocolBackupNames)
            if (!DeleteValue(HKEY_CURRENT_USER, 0, kProtocolBackup, name)) return fail(ERROR_WRITE_FAULT);
        RegDeleteKeyW(HKEY_CURRENT_USER, kProtocolBackup);
    }
    return true;
}
} // namespace pulse::setup
