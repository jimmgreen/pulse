#include "../common/dialog_auto_registration.h"
#include <cstdio>
#include <string>
// Pass a built native architecture PulseDialogAuto DLL. HKCU is redirected before any production call.
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::puts("Usage: pulse_dialog_auto_registration_test <absolute fixture DLL>"); return 2; }
    std::wstring sandbox = L"Software\\PulseDialogAutoTest_" + std::to_wstring(GetCurrentProcessId());
    HKEY isolated = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &isolated, nullptr)) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, isolated)) { RegCloseKey(isolated); return 2; }
    int failures = 0;
    auto check = [&](bool condition, const char* name) { std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name); if (!condition) ++failures; };
    using namespace pulse::dialog_auto;
    // Explicit redirected location keeps this sentinel meaningful under RegOverridePredefKey,
    // whose private test root is outside the system Classes redirector.
    const wchar_t* view32 = L"Software\\Classes\\Wow6432Node\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}\\InprocServer32";
#ifdef _WIN64
    const wchar_t* other_config = L"Software\\Pulse\\DialogAuto32";
#else
    const wchar_t* other_config = L"Software\\Pulse\\DialogAuto";
#endif
    HKEY other = nullptr;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, other_config, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &other, nullptr) == ERROR_SUCCESS, "create other architecture configuration");
    const DWORD other_enabled = 1;
    if (other) RegSetValueExW(other, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&other_enabled), sizeof(other_enabled));
    HKEY sentinel = nullptr;
    check(RegCreateKeyExW(HKEY_CURRENT_USER, view32, 0, nullptr, 0, KEY_ALL_ACCESS | KEY_WOW64_32KEY, nullptr, &sentinel, nullptr) == ERROR_SUCCESS, "create explicit alternate subtree sentinel");
    const wchar_t sentinel_value[] = L"32-bit-foreign.dll";
    if (sentinel) { RegSetValueExW(sentinel, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(sentinel_value), sizeof(sentinel_value)); RegCloseKey(sentinel); }
    check(Enable(std::wstring(L"C:\\test\\..\\") + DllName) == ERROR_INVALID_NAME, "reject dot-dot component");
    check(Enable(std::wstring(L"C:\\.\\") + DllName) == ERROR_INVALID_NAME, "reject dot component");
    check(Enable(argv[1]) == ERROR_SUCCESS, "enable both dialogs");
    check(IsEnabledFor(argv[1]), "enabled matching path");
    check(Enable(argv[1]) == ERROR_SUCCESS, "repeat enable");
    check(DisableFor(std::wstring(L"C:\\OtherInstallation\\")+DllName)==ERROR_SUCCESS && IsEnabledFor(argv[1]), "uninstall of another installation preserves registration");
    check(DisableFor(argv[1])==ERROR_SUCCESS && !Inspect().enabled, "owned uninstall restores both dialogs");
    check(Enable(argv[1])==ERROR_SUCCESS, "enable again after owned uninstall");
    check(Disable() == ERROR_SUCCESS && !Inspect().enabled, "disable");
    check(Disable() == ERROR_SUCCESS, "repeat disable");
    const wchar_t* root = L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}";
    HKEY foreign = nullptr;
    RegCreateKeyExW(HKEY_CURRENT_USER, root, 0, nullptr, 0, KEY_ALL_ACCESS | RegistryView, nullptr, &foreign, nullptr);
    RegCloseKey(foreign);
    check(Enable(argv[1]) == ERROR_ALREADY_EXISTS, "preserve existing user CLSID");
    RegDeleteKeyExW(HKEY_CURRENT_USER, root, RegistryView, 0);
    check(Enable(argv[1]) == ERROR_SUCCESS, "enable for partial recovery");
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}");
    check(Enable(argv[1]) == ERROR_SUCCESS && Inspect().enabled, "recover missing second key");
    std::wstring child = std::wstring(root) + L"\\InprocServer32";
    RegOpenKeyExW(HKEY_CURRENT_USER, child.c_str(), 0, KEY_ALL_ACCESS | RegistryView, &foreign);
    const DWORD extra = 17;
    RegSetValueExW(foreign, L"ExternalValue", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&extra), sizeof(extra));
    check(Disable() == ERROR_ALREADY_EXISTS, "extra value blocks cleanup");
    wchar_t preserved[32768]{}; DWORD preserved_size = sizeof(preserved), preserved_type = 0;
    check(RegQueryValueExW(foreign, nullptr, nullptr, &preserved_type, reinterpret_cast<BYTE*>(preserved), &preserved_size) == ERROR_SUCCESS && preserved_type == REG_SZ && std::wstring(preserved) == argv[1], "extra value preserves original server");
    RegDeleteValueW(foreign, L"ExternalValue");
    RegSetValueExW(foreign, L"ThreadingModel", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&extra), sizeof(extra));
    check(Disable() == ERROR_ALREADY_EXISTS, "wrong type blocks cleanup");
    preserved_size = sizeof(preserved);
    check(RegQueryValueExW(foreign, nullptr, nullptr, &preserved_type, reinterpret_cast<BYTE*>(preserved), &preserved_size) == ERROR_SUCCESS && preserved_type == REG_SZ && std::wstring(preserved) == argv[1], "wrong type preserves original server");
    DWORD preserved_number = 0; preserved_size = sizeof(preserved_number);
    check(RegQueryValueExW(foreign, L"ThreadingModel", nullptr, &preserved_type, reinterpret_cast<BYTE*>(&preserved_number), &preserved_size) == ERROR_SUCCESS && preserved_type == REG_DWORD && preserved_number == extra, "wrong type value preserved");
    const wchar_t apartment[] = L"Apartment";
    RegSetValueExW(foreign, L"ThreadingModel", 0, REG_SZ, reinterpret_cast<const BYTE*>(apartment), sizeof(apartment));
    const wchar_t changed[] = L"foreign.dll";
    RegSetValueExW(foreign, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE*>(changed), sizeof(changed));
    RegCloseKey(foreign);
    check(Disable() == ERROR_ALREADY_EXISTS && !Inspect().enabled, "external mutation preserved and disabled");
    sentinel = nullptr;
    DWORD sentinel_error = RegOpenKeyExW(HKEY_CURRENT_USER, view32, 0, KEY_READ | KEY_WOW64_32KEY, &sentinel);
    wchar_t observed[64]{}; DWORD bytes = sizeof(observed), type = 0;
    if (!sentinel_error) sentinel_error = RegQueryValueExW(sentinel, nullptr, nullptr, &type, reinterpret_cast<BYTE*>(observed), &bytes);
    check(!sentinel_error && type == REG_SZ && std::wstring(observed) == sentinel_value, "alternate subtree sentinel unchanged");
    if (sentinel) RegCloseKey(sentinel);
    RegDeleteTreeW(HKEY_CURRENT_USER, ConfigKey);
    check(Inspect().conflict, "detect existing CLSID without configuration");
    RegOpenKeyExW(HKEY_CURRENT_USER,child.c_str(),0,KEY_ALL_ACCESS|RegistryView,&foreign);
    const DWORD fixture_bytes=static_cast<DWORD>((wcslen(argv[1])+1)*sizeof(wchar_t));
    if(foreign) {RegSetValueExW(foreign,nullptr,0,REG_SZ,reinterpret_cast<const BYTE*>(argv[1]),fixture_bytes);RegCloseKey(foreign);}
    check(DisableFor(argv[1])==ERROR_ALREADY_EXISTS,"missing recovery journal blocks uninstall of a referenced DLL");
    check(DisableFor(std::wstring(L"C:\\OtherInstallation\\")+DllName)==ERROR_SUCCESS,"missing journal does not block an unrelated installation");
    DWORD other_observed = 0, other_bytes = sizeof(other_observed);
    check(other && RegQueryValueExW(other, L"Enabled", nullptr, nullptr, reinterpret_cast<BYTE*>(&other_observed), &other_bytes) == ERROR_SUCCESS && other_observed == 1, "other architecture configuration unchanged");
    if (other) RegCloseKey(other);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(isolated);
    RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
    return failures ? 1 : 0;
}


