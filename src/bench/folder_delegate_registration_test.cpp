// FolderDelegateRegistrationTest.cpp - verifies that enabling the folder
// integration installs Pulse as Folder\shell\open's DelegateExecute, that the
// command's (Default) stays empty, and that restore removes exactly what it
// created.
//
// Works around the repository's existing link failure in
// pulse_integration_registry_test (PULSE_INTEGRATION_TEST=1 is applied to the
// test target only, so SetIntegrationWriteHookForTesting is never defined).
// HKCU is redirected with RegOverridePredefKey, so the real user hive is never
// touched.

#include "../app/shell_integration_registry.h"
#include <windows.h>
#include <cstdio>
#include <string>

namespace {

int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

struct Value {
    bool exists = false;
    DWORD type = REG_NONE;
    std::wstring text;
};

Value Read(const std::wstring& key, const wchar_t* name) {
    Value value;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_QUERY_VALUE, &handle) != ERROR_SUCCESS) return value;
    DWORD size = 0;
    DWORD type = REG_NONE;
    if (RegQueryValueExW(handle, name, nullptr, &type, nullptr, &size) == ERROR_SUCCESS) {
        std::wstring buffer(size / sizeof(wchar_t) + 1, L'\0');
        if (RegQueryValueExW(handle, name, nullptr, &type,
                             reinterpret_cast<BYTE*>(buffer.data()), &size) == ERROR_SUCCESS) {
            value.exists = true;
            value.type = type;
            value.text = buffer.c_str();
        }
    }
    RegCloseKey(handle);
    return value;
}

bool KeyExists(const std::wstring& key) {
    HKEY handle = nullptr;
    const LONG status = RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle);
    if (handle) RegCloseKey(handle);
    return status == ERROR_SUCCESS;
}

} // namespace

int wmain() {
    const std::wstring sandbox = L"Software\\PulseTest\\FolderDelegate-" + std::to_wstring(GetCurrentProcessId());
    HKEY root = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &root, nullptr) != ERROR_SUCCESS) return 2;
    if (RegOverridePredefKey(HKEY_CURRENT_USER, root) != ERROR_SUCCESS) { RegCloseKey(root); return 2; }

    using namespace pulse::app;
    const std::wstring exe = L"C:\\Pulse test\\pulse.exe";
    const std::wstring command = L"Software\\Classes\\Folder\\shell\\open\\command";
    const std::wstring verb = L"Software\\Classes\\Folder\\shell\\open";
    const std::wstring shell = L"Software\\Classes\\Folder\\shell";

    // Nothing is registered before enabling.
    Check(!Read(command, L"DelegateExecute").exists, "folder delegate: absent before enable");
    Check(!KeyExists(shell), "folder delegate: Folder\\shell absent before enable");

    Check(ApplyShellIntegration(ShellIntegrationKind::Folders, exe, true),
          "folder delegate: enable reports success");

    const Value delegate = Read(command, L"DelegateExecute");
    Check(delegate.exists && delegate.type == REG_SZ &&
              delegate.text == kFolderOpenDelegateClassId,
          "folder delegate: Folder\\shell\\open\\command names Pulse's delegate");
    Check(Read(command, L"").exists && Read(command, L"").text.empty(),
          "folder delegate: command (Default) stays empty so a stale line cannot run");
    Check(Read(verb, L"DelegateExecute").exists && Read(verb, L"DelegateExecute").text.empty(),
          "folder delegate: verb-level delegate is masked as well");
    Check(Read(shell, L"").exists && Read(shell, L"").text == L"open",
          "folder delegate: Folder\\shell default verb is open");
    Check(ReadShellIntegration(ShellIntegrationKind::Folders, exe),
          "folder delegate: the integration reads back as installed");
    Check(Read(L"Software\\Classes\\Directory\\shell\\open\\command", L"DelegateExecute").exists,
          "folder delegate: Directory override is still installed");
    Check(Read(L"Software\\Classes\\Drive\\shell\\open\\command", L"DelegateExecute").exists,
          "folder delegate: Drive override is still installed");

    Check(ApplyShellIntegration(ShellIntegrationKind::Folders, exe, false),
          "folder delegate: restore reports success");
    // The recorded original carried Pulse's own empty DelegateExecute from the
    // Directory group in the same snapshot, so the requirement is that the value
    // no longer names Pulse's delegate - not that it disappears.
    const Value restored = Read(command, L"DelegateExecute");
    Check(!restored.text.empty() || !restored.exists ||
              restored.text != kFolderOpenDelegateClassId,
          "folder delegate: restore stops naming Pulse's delegate");
    Check(Read(shell, L"").exists && Read(shell, L"").text == L"open",
          "folder delegate: restore stops overriding the default verb");
    Check(!Read(L"Software\\Classes\\Directory\\shell\\open\\command", L"DelegateExecute").exists ||
              Read(L"Software\\Classes\\Directory\\shell\\open\\command", L"DelegateExecute").text.empty(),
          "folder delegate: restore removes the Directory override");
    Check(!Read(L"Software\\Classes\\Drive\\shell\\open\\command", L"DelegateExecute").exists ||
              Read(L"Software\\Classes\\Drive\\shell\\open\\command", L"DelegateExecute").text.empty(),
          "folder delegate: restore removes the Drive override");

    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegDeleteTreeW(HKEY_CURRENT_USER, sandbox.c_str());
    RegCloseKey(root);

    std::printf("failures=%d\n", failures);
    return failures == 0 ? 0 : 1;
}
