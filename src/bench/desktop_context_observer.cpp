#include <windows.h>
#include <sddl.h>
#include <string>
#include <vector>

namespace {
std::wstring Number(unsigned long long value) { return std::to_wstring(value); }
void Line(std::wstring& output, const std::wstring& key, const std::wstring& value) { output += key + L"=" + value + L"\r\n"; }
std::wstring NativeKeyName(HKEY key) {
    using Query = LONG (NTAPI*)(HANDLE, int, void*, ULONG, ULONG*);
    const auto module = GetModuleHandleW(L"ntdll.dll");
    const auto query = module ? reinterpret_cast<Query>(GetProcAddress(module, "NtQueryKey")) : nullptr;
    if (!query) return L"unavailable";
    ULONG bytes = 0;
    query(key, 3, nullptr, 0, &bytes);
    if (bytes < sizeof(ULONG) || bytes > 65536) return L"query_size_unavailable";
    std::vector<BYTE> buffer(bytes);
    const LONG status = query(key, 3, buffer.data(), bytes, &bytes);
    if (status < 0) return L"query_status_" + Number(static_cast<ULONG>(status));
    const ULONG length = *reinterpret_cast<const ULONG*>(buffer.data());
    if (length > buffer.size() - sizeof(ULONG) || length % sizeof(wchar_t)) return L"invalid_key_name";
    return {reinterpret_cast<const wchar_t*>(buffer.data() + sizeof(ULONG)), length / sizeof(wchar_t)};
}
std::wstring RegistryString(HKEY key, const wchar_t* name) {
    DWORD type = 0, bytes = 0;
    LSTATUS status = RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes);
    if (status != ERROR_SUCCESS) return L"error_" + Number(status);
    if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes > 65536 || bytes % sizeof(wchar_t)) return L"invalid_type_" + Number(type);
    std::vector<wchar_t> value(bytes / sizeof(wchar_t) + 1);
    status = RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes);
    if (status != ERROR_SUCCESS) return L"error_" + Number(status);
    return std::wstring(value.data(), wcsnlen_s(value.data(), value.size()));
}
void Key(std::wstring& output, const std::wstring& label, HKEY root, const wchar_t* path, REGSAM view, bool config) {
    HKEY key = nullptr;
    const auto status = RegOpenKeyExW(root, path, 0, KEY_READ | view, &key);
    Line(output, label + L".open_error", Number(status));
    if (status != ERROR_SUCCESS) return;
    Line(output, label + L".native_key", NativeKeyName(key));
    if (config) {
        DWORD enabled = 0, type = 0, bytes = sizeof(enabled);
        const auto read = RegQueryValueExW(key, L"Enabled", nullptr, &type, reinterpret_cast<BYTE*>(&enabled), &bytes);
        Line(output, label + L".Enabled.error", Number(read));
        Line(output, label + L".Enabled.type", Number(type));
        if (read == ERROR_SUCCESS && type == REG_DWORD && bytes == sizeof(enabled)) Line(output, label + L".Enabled", Number(enabled));
        Line(output, label + L".DllPath", RegistryString(key, L"DllPath"));
    } else {
        Line(output, label + L".Default", RegistryString(key, nullptr));
        Line(output, label + L".ThreadingModel", RegistryString(key, L"ThreadingModel"));
    }
    RegCloseKey(key);
}
void Token(std::wstring& output) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) { Line(output, L"token.error", Number(GetLastError())); return; }
    DWORD bytes = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
    if (bytes && bytes <= 65536) {
        std::vector<BYTE> user(bytes);
        if (GetTokenInformation(token, TokenUser, user.data(), bytes, &bytes)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
                Line(output, L"token.user_sid", sid); LocalFree(sid);
            }
        }
    }
    TOKEN_ELEVATION elevation{};
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes)) Line(output, L"token.elevated", Number(elevation.TokenIsElevated));
    DWORD container = 0;
    if (GetTokenInformation(token, TokenIsAppContainer, &container, sizeof(container), &bytes)) Line(output, L"token.appcontainer", Number(container));
    DWORD virtualized = 0;
    if (GetTokenInformation(token, TokenVirtualizationEnabled, &virtualized, sizeof(virtualized), &bytes)) Line(output, L"token.virtualization_enabled", Number(virtualized));
    CloseHandle(token);
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    try {
        wchar_t module[32768]{};
        const DWORD count = GetModuleFileNameW(nullptr, module, 32768);
        if (!count || count >= 32768) return 2;
        std::wstring directory(module, count);
        const auto split = directory.find_last_of(L"\\/");
        if (split == std::wstring::npos) return 2;
        directory.resize(split);
        std::wstring output;
        Line(output, L"pid", Number(GetCurrentProcessId()));
        Line(output, L"process_arch", Number(sizeof(void*) * 8));
        Token(output);
        HKEY current = nullptr;
        const auto opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_READ, &current);
        Line(output, L"HKCU.open_error", Number(opened));
        if (opened == ERROR_SUCCESS) { Line(output, L"HKCU.native_key", NativeKeyName(current)); RegCloseKey(current); }
        for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            const auto tag = view == KEY_WOW64_64KEY ? L"64" : L"32";
            Key(output, std::wstring(L"HKCU.DialogAuto.view") + tag, HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto", view, true);
            Key(output, std::wstring(L"HKCU.DialogAuto32.view") + tag, HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto32", view, true);
            constexpr const wchar_t* ids[]{L"{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}", L"{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
            for (size_t index = 0; index < 2; ++index) {
                const auto path = std::wstring(L"CLSID\\") + ids[index] + L"\\InprocServer32";
                const auto kind = index ? L"Save" : L"Open";
                Key(output, std::wstring(L"HKCU.") + kind + L".view" + tag, HKEY_CURRENT_USER, (L"Software\\Classes\\" + path).c_str(), view, false);
                Key(output, std::wstring(L"HKCR.") + kind + L".view" + tag, HKEY_CLASSES_ROOT, path.c_str(), view, false);
            }
        }
        const int size = WideCharToMultiByte(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), nullptr, 0, nullptr, nullptr);
        if (size <= 0) return 3;
        std::string utf8(size, '\0');
        WideCharToMultiByte(CP_UTF8, 0, output.data(), static_cast<int>(output.size()), utf8.data(), size, nullptr, nullptr);
        const auto file = CreateFileW((directory + L"\\desktop-context.txt").c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return 4;
        DWORD written = 0;
        const bool okay = WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr) && written == utf8.size();
        CloseHandle(file);
        return okay ? 0 : 5;
    } catch (...) { return 1; }
}
