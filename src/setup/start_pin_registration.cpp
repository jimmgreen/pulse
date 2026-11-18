#include "start_pin_registration.h"
#include "setup_common.h"
#include "setup_registry.h"
#include "setup_dialog_versions.h"
#include <shlobj.h>
#include <vector>

namespace pulse::setup {
namespace {
constexpr wchar_t kClsid[] = L"{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}";
constexpr wchar_t kClass[] = L"Software\\Classes\\CLSID\\{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}";
constexpr wchar_t kServer[] = L"Software\\Classes\\CLSID\\{B3C9ED45-7A51-48A2-9DF1-D46BA187A312}\\InprocServer32";
constexpr wchar_t kExtension[] = L"Software\\Classes\\.pulse-start-pin";
constexpr wchar_t kProgId[] = L"Software\\Classes\\Pulse.StartPin.Request";
}

bool RegisterStartPin(const std::wstring& app, std::wstring& error) {
    const auto runtime_dll = InstalledRuntimeFile(app, L"pulse_start_pin.dll");
    if (runtime_dll.empty()) { error = L"Cannot resolve Start pin runtime."; return false; }
    const std::wstring verb = std::wstring(kProgId) + L"\\shell\\PulseStartPin";
    const std::wstring command = verb + L"\\command";
    struct PreviousValue {
        std::wstring key, name;
        DWORD type = 0;
        std::vector<BYTE> data;
        bool existed = false;
    };
    std::vector<PreviousValue> previous;
    std::vector<std::wstring> created;
    auto write = [&](const wchar_t* path, const wchar_t* name, const std::wstring& value) {
        const std::wstring key(path);
        LSTATUS status = ERROR_SUCCESS;
        HKEY handle = nullptr;
        // Record every new ancestor so rollback never removes a pre-existing key.
        for (size_t end = key.find(L'\\'); ; end = key.find(L'\\', end + 1)) {
            const std::wstring part = key.substr(0, end);
            DWORD disposition = 0;
            status = RegCreateKeyExW(HKEY_LOCAL_MACHINE, part.c_str(), 0, nullptr, 0,
                                    KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, nullptr, &handle, &disposition);
            if (status != ERROR_SUCCESS) break;
            if (disposition == REG_CREATED_NEW_KEY) created.push_back(part);
            if (end == std::wstring::npos) break;
            RegCloseKey(handle);
            handle = nullptr;
        }
        if (status == ERROR_SUCCESS) {
            PreviousValue old{key, name};
            DWORD size = 0;
            status = RegQueryValueExW(handle, name, nullptr, &old.type, nullptr, &size);
            old.existed = status == ERROR_SUCCESS;
            if (old.existed) {
                old.data.resize(size);
                status = RegQueryValueExW(handle, name, nullptr, &old.type, old.data.data(), &size);
            } else if (status == ERROR_FILE_NOT_FOUND) status = ERROR_SUCCESS;
            if (status == ERROR_SUCCESS) {
                previous.push_back(std::move(old));
                status = RegSetValueExW(handle, name, 0, REG_SZ,
                    reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
            }
        }
        if (handle) RegCloseKey(handle);
        if (status == ERROR_SUCCESS) return true;
        error = L"Start pin registration failed: " + key + L": " + Win32Error(status);
        return false;
    };
    const bool ok = write(kServer, L"", runtime_dll) &&
           write(kServer, L"ThreadingModel", L"Apartment") &&
           write(kExtension, L"", L"Pulse.StartPin.Request") &&
           write(verb.c_str(), L"", L"Pulse") &&
           write(command.c_str(), L"", L"") &&
           write(command.c_str(), L"DelegateExecute", kClsid);
    if (!ok) {
        auto failure = [&](LSTATUS status) {
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND)
                error += L"; rollback: " + Win32Error(status);
        };
        for (auto it = previous.rbegin(); it != previous.rend(); ++it) {
            HKEY key = nullptr;
            const LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, it->key.c_str(), 0,
                                                 KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
            failure(status);
            if (status != ERROR_SUCCESS) continue;
            failure(it->existed ? RegSetValueExW(key, it->name.c_str(), 0, it->type,
                it->data.data(), static_cast<DWORD>(it->data.size())) : RegDeleteValueW(key, it->name.c_str()));
            RegCloseKey(key);
        }
        for (auto it = created.rbegin(); it != created.rend(); ++it)
            failure(RegDeleteKeyExW(HKEY_LOCAL_MACHINE, it->c_str(), KEY_WOW64_64KEY, 0));
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return ok;
}

bool UnregisterStartPin(const std::wstring& app, std::wstring& error) {
    HKEY server = nullptr;
    LSTATUS status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kServer, 0, KEY_READ | KEY_WOW64_64KEY, &server);
    if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND) return true;
    if (status != ERROR_SUCCESS) {
        error = L"Cannot inspect Start pin registration: " + Win32Error(status);
        return false;
    }
    RegCloseKey(server);
    const auto dll = ReadString(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, kServer, L"");
    if (!dll) {
        error = L"Cannot read Start pin registration owner.";
        return false;
    }
    const auto root = NormalizeDir(app);
    const auto prefix = root + L"\\";
    const bool version_owned = dll->size() > prefix.size() &&
        CompareStringOrdinal(dll->data(), static_cast<int>(prefix.size()), prefix.c_str(), -1, TRUE) == CSTR_EQUAL &&
        IsDialogVersionFile(dll->substr(prefix.size())) &&
        EqualsNoCase(dll->substr(dll->find_last_of(L"\\")+1), L"pulse_start_pin.dll");
    if (!SameDirectory(*dll, root + L"\\pulse_start_pin.dll") && !version_owned) return true;
    const std::wstring command = std::wstring(kProgId) + L"\\shell\\PulseStartPin\\command";
    auto owns = [&](const wchar_t* root, const wchar_t* key, const wchar_t* name, const wchar_t* expected) {
        HKEY handle = nullptr;
        const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, root, 0, KEY_READ | KEY_WOW64_64KEY, &handle);
        if (opened == ERROR_FILE_NOT_FOUND || opened == ERROR_PATH_NOT_FOUND) return true;
        if (opened != ERROR_SUCCESS) return false;
        RegCloseKey(handle);
        const auto value = ReadString(HKEY_LOCAL_MACHINE, KEY_WOW64_64KEY, key, name);
        return value && EqualsNoCase(*value, expected);
    };
    // Absent roots permit retry after partial cleanup; unreadable or changed owners do not.
    if (!owns(kExtension, kExtension, L"", L"Pulse.StartPin.Request") ||
        !owns(kProgId, command.c_str(), L"DelegateExecute", kClsid)) {
        error = L"Start pin association ownership changed or cannot be read; registration preserved.";
        return false;
    }
    HKEY machine = nullptr;
    status = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"", 0, KEY_ALL_ACCESS | KEY_WOW64_64KEY, &machine);
    if (status == ERROR_SUCCESS) {
        // Delete ownership evidence last, so failed cleanup can be retried.
        for (const wchar_t* key : {kExtension, kProgId, kClass}) {
            status = RegDeleteTreeW(machine, key);
            if (status != ERROR_SUCCESS && status != ERROR_FILE_NOT_FOUND && status != ERROR_PATH_NOT_FOUND) break;
            status = ERROR_SUCCESS;
        }
        RegCloseKey(machine);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (status == ERROR_SUCCESS) return true;
    error = L"Start pin unregistration failed: " + Win32Error(status);
    return false;
}
}
