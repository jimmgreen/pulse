#include "../shell_bridge/file_dialog_client_state.h"
#include <cstdio>
using pulse::shell_bridge::FileDialogClientState;
int wmain() {
    const std::wstring root = L"Software\\Pulse\\Tests\\FileDialogClientState-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    struct Cleanup {
        std::wstring root;
        ~Cleanup() { RegDeleteTreeW(HKEY_CURRENT_USER, root.c_str()); }
    } cleanup{ root };
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    const GUID a{ 1 }, b{ 2 };
    FileDialogClientState first(L"a", root), second(L"a", root), other(L"b", root);
    check(!first.Read(a), "missing state");
    check(first.Write(a, L"C:\\fixture"), "write directory");
    check(second.Read(a) == std::optional<std::wstring>(L"C:\\fixture"), "cross instance read");
    check(!second.Read(b) && !other.Read(a), "GUID and application isolation");
    check(first.Write(b, L"\\\\server\\share"), "UNC directory");
    check(second.Clear(a) && !first.Read(a) && first.Read(b).has_value(), "clear only own GUID");
    check(second.Clear(a), "clear missing state");
    FileDialogClientState long_app(std::wstring(300, L'x'), root);
    check(long_app.Write(a, L"C:\\long-app") && long_app.Read(a).has_value(), "long application identity");
    HKEY key = nullptr;
    const auto corrupt = root + L"\\0061\\Clients\\00000001-0000-0000-0000-000000000000";
    check(RegCreateKeyExW(HKEY_CURRENT_USER, corrupt.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) == ERROR_SUCCESS, "create corruption fixture");
    if (key) {
        DWORD invalid = 42;
        RegSetValueExW(key, L"Directory", 0, REG_DWORD, reinterpret_cast<BYTE*>(&invalid), sizeof(invalid));
        check(!first.Read(a), "reject wrong value type");
        const wchar_t unterminated[] = { L'x', L'y' };
        RegSetValueExW(key, L"Directory", 0, REG_SZ, reinterpret_cast<const BYTE*>(unterminated), sizeof(unterminated));
        check(!first.Read(a), "reject unterminated value");
        const wchar_t embedded[] = { L'x', 0, L'y', 0 };
        RegSetValueExW(key, L"Directory", 0, REG_SZ, reinterpret_cast<const BYTE*>(embedded), sizeof(embedded));
        check(!first.Read(a), "reject embedded null");
        RegCloseKey(key);
    }
    check(!first.Write(a, L""), "reject empty directory");
    for (const auto* bad : { L"relative", L"C:relative", L"\\server", L"C:\\bad*name", L"C:\\..\\folder", L"\\\\server\\" }) {
        check(!first.Write(a, bad), "reject invalid directory on write");
        HKEY invalid_key = nullptr;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, corrupt.c_str(), 0, KEY_SET_VALUE, &invalid_key) == ERROR_SUCCESS) {
            RegSetValueExW(invalid_key, L"Directory", 0, REG_SZ, reinterpret_cast<const BYTE*>(bad),
                static_cast<DWORD>((wcslen(bad) + 1) * sizeof(wchar_t)));
            RegCloseKey(invalid_key);
            check(!first.Read(a), "reject invalid directory on read");
        }
    }
    check(first.Write(a, L"\\\\?\\C:\\long-folder"), "accept extended absolute directory");
    FileDialogClientState unavailable(L"", root);
    check(!unavailable.Read(a) && !unavailable.Write(a, L"C:\\fixture") && !unavailable.Clear(a), "unavailable identity fails safely");
    return failures ? 1 : 0;
}

