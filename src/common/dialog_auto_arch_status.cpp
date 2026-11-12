#include "dialog_auto_arch_status.h"
#include <vector>

namespace pulse::dialog_auto {
namespace {
struct Key { HKEY value = nullptr; ~Key() { if (value) RegCloseKey(value); } };
DWORD String(HKEY key, const wchar_t* name, std::wstring& result) {
    DWORD type = 0, bytes = 0;
    DWORD error = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (error) return error;
    if (type != REG_SZ || bytes < sizeof(wchar_t) || bytes > 65536 || bytes % sizeof(wchar_t)) return ERROR_INVALID_DATA;
    std::vector<wchar_t> value(bytes / sizeof(wchar_t));
    const auto expected = bytes;
    error = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    if (error) return error;
    if (type != REG_SZ || bytes != expected || value.back() != L'\0') return ERROR_INVALID_DATA;
    for (size_t n = 0; n + 1 < value.size(); ++n) if (!value[n]) return ERROR_INVALID_DATA;
    result.assign(value.data(), value.size() - 1);
    return ERROR_SUCCESS;
}
}
ArchStatus InspectArchStatus(HKEY root, bool x86, const std::wstring& component_directory) noexcept {
    ArchStatus result; result.x86 = x86;
    try {
        const REGSAM view = x86 ? KEY_WOW64_32KEY : KEY_WOW64_64KEY;
        Key config;
        DWORD error = RegOpenKeyExW(root, x86 ? L"Software\\Pulse\\DialogAuto32" : L"Software\\Pulse\\DialogAuto", 0, KEY_READ | view, &config.value);
        DWORD enabled = 0;
        if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
        if (!error) {
            error = String(config.value, L"DllPath", result.dll_path);
            if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
            DWORD type = 0, bytes = sizeof(enabled);
            error = RegQueryValueExW(config.value, L"Enabled", nullptr, &type, reinterpret_cast<BYTE*>(&enabled), &bytes);
            if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
            if (!error && (type != REG_DWORD || bytes != sizeof(enabled) || enabled > 1)) result.registry_error = ERROR_INVALID_DATA;
        }
        unsigned present = 0;
        bool conflict = false;
        constexpr const wchar_t* classes[]{
            L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
            L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
        for (const auto path : classes) {
            Key clsid, server;
            error = RegOpenKeyExW(root, path, 0, KEY_READ | view, &clsid.value);
            if (error == ERROR_FILE_NOT_FOUND) continue;
            if (error) { result.registry_error = error; continue; }
            ++present;
            std::wstring owner, dll, model;
            error = String(clsid.value, L"PulseDialogAutoOwner", owner);
            if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
            if (error || owner != L"Pulse.DialogAuto.v1") conflict = true;
            error = RegOpenKeyExW(clsid.value, L"InprocServer32", 0, KEY_READ | view, &server.value);
            if (error) {
                if (error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
                conflict = true; continue;
            }
            error = String(server.value, nullptr, dll);
            if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
            if (error || dll.empty() || dll != result.dll_path) conflict = true;
            error = String(server.value, L"ThreadingModel", model);
            if (error && error != ERROR_FILE_NOT_FOUND) result.registry_error = error;
            if (error || model != L"Apartment") conflict = true;
        }
        if (result.registry_error) result.registration = ArchRegistrationState::ReadError;
        else if (conflict || present == 1) result.registration = ArchRegistrationState::Conflict;
        else if (present == 2 && enabled == 1) result.registration = ArchRegistrationState::Registered;
        else if (present == 2) result.registration = ArchRegistrationState::DisabledMatching;
        auto component = result.dll_path;
        if (component.empty() && !component_directory.empty())
            component = component_directory + L"\\" + (x86 ? L"PulseDialogAuto32.dll" : L"PulseDialogAuto64.dll");
        if (!component.empty()) {
            const auto attributes = GetFileAttributesW(component.c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES) result.component = (attributes & FILE_ATTRIBUTE_DIRECTORY) ? ArchComponentState::Missing : ArchComponentState::Present;
            else {
                result.component_error = GetLastError();
                if (result.component_error == ERROR_FILE_NOT_FOUND || result.component_error == ERROR_PATH_NOT_FOUND)
                    result.component = ArchComponentState::Missing;
            }
        }
    } catch (...) { result.registration = ArchRegistrationState::ReadError; result.registry_error = ERROR_NOT_ENOUGH_MEMORY; }
    return result;
}
std::wstring FormatArchStatus(std::wstring_view format, std::wstring_view architecture,
    std::wstring_view component, std::wstring_view registration) {
    std::wstring result(format);
    const auto replace = [&](std::wstring_view token, std::wstring_view value) {
        size_t offset = 0;
        while ((offset = result.find(token, offset)) != std::wstring::npos) {
            result.replace(offset, token.size(), value); offset += value.size();
        }
    };
    replace(L"{arch}", architecture); replace(L"{component}", component); replace(L"{registration}", registration);
    return result;
}
}
