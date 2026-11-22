#include "setup_url_protocol.h"
#include <iostream>
#include <stdexcept>

using namespace pulse::setup;
namespace {
unsigned failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
struct RegistryFixture {
    HKEY key = nullptr;
    std::wstring path = L"Software\\PulseUrlProtocolTest-" + std::to_wstring(GetCurrentProcessId()) +
                        L"-" + std::to_wstring(GetTickCount64());
    RegistryFixture() {
        if (RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &key, nullptr) != ERROR_SUCCESS)
            throw std::runtime_error("create isolated registry root");
        if (RegOverridePredefKey(HKEY_CURRENT_USER, key) != ERROR_SUCCESS) {
            RegCloseKey(key);
            RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
            throw std::runtime_error("override HKCU");
        }
    }
    ~RegistryFixture() {
        RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        RegCloseKey(key);
        RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str());
        RegDeleteKeyW(HKEY_CURRENT_USER, path.c_str());
    }
};
}
int main() {
    RegistryFixture fixture;
    std::wstring error;
    const std::wstring app = L"C:\\Program Files\\Pulse";
    Check(ConfigureUrlProtocol(app, true, error), "register protocol in isolated HKCU");
    Check(ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"") == L"URL:Pulse Protocol" &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"URL Protocol") == L"" &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"\"C:\\Program Files\\Pulse\\pulse.exe\" --shell-open \"%1\"",
          "description marker and quoted shell-open command");
    Check(ConfigureUrlProtocol(app, false, error) &&
          !ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"URL Protocol"), "uninstall removes initially absent values");
    Check(ConfigureUrlProtocol(app, true, error) &&
          WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"", L"replacement.exe %1") &&
          ConfigureUrlProtocol(app, false, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"replacement.exe %1",
          "replacement survives uninstall after initially unregistered protocol");
    HKEY backup_key = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, kProtocolBackup, 0, KEY_QUERY_VALUE, &backup_key) == ERROR_FILE_NOT_FOUND,
          "replacement uninstall discards obsolete backup");
    if (backup_key) RegCloseKey(backup_key);
    Check(ConfigureUrlProtocol(app, true, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"\"C:\\Program Files\\Pulse\\pulse.exe\" --shell-open \"%1\"" &&
          ConfigureUrlProtocol(app, false, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"replacement.exe %1",
          "reinstall and eventual uninstall restore replacement owner");
    Check(WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"", L"Original protocol") &&
          WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"", L"original.exe %1") &&
          WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"Unrelated", L"keep"), "seed previous owner");
    HKEY key = nullptr;
    Check(RegOpenKeyExW(HKEY_CURRENT_USER, kProtocolKeys[0], 0, KEY_SET_VALUE, &key) == ERROR_SUCCESS, "open marker fixture");
    const DWORD marker = 42;
    Check(key && RegSetValueExW(key, L"URL Protocol", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&marker), sizeof(marker)) == ERROR_SUCCESS,
          "seed non-string original value");
    if (key) RegCloseKey(key);
    Check(ConfigureUrlProtocol(app, true, error) && ConfigureUrlProtocol(app, true, error) &&
          ConfigureUrlProtocol(L"D:\\Moved Pulse", true, error), "repeat and directory-move upgrades retain first backup");
    Check(ConfigureUrlProtocol(app, false, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"\"D:\\Moved Pulse\\pulse.exe\" --shell-open \"%1\"",
          "old installation uninstall preserves new installation command");
    Check(ConfigureUrlProtocol(L"D:\\Moved Pulse", false, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"") == L"Original protocol" &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"original.exe %1" &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[0], L"Unrelated") == L"keep", "uninstall restores original owner and preserves unrelated value");
    DWORD type = 0, restored = 0;
    std::vector<BYTE> bytes;
    const auto result = ReadProtocolValue(kProtocolKeys[0], L"URL Protocol", type, bytes);
    if (bytes.size() == sizeof(restored)) std::memcpy(&restored, bytes.data(), sizeof(restored));
    Check(result == ERROR_SUCCESS && type == REG_DWORD && restored == marker, "restore exact registry type and bytes");
    Check(ConfigureUrlProtocol(app, true, error) &&
          WriteString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"", L"foreign.exe %1") &&
          ConfigureUrlProtocol(app, false, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"foreign.exe %1", "preserve replacement owner on uninstall");
    Check(WriteString(HKEY_CURRENT_USER, 0, kProtocolBackup, kProtocolBackupNames[1], L"invalid"), "inject corrupt backup");
    Check(!ConfigureUrlProtocol(app, true, error) &&
          ReadString(HKEY_CURRENT_USER, 0, kProtocolKeys[2], L"") == L"foreign.exe %1",
          "invalid backup fails before publishing a partial protocol registration");
    std::cout << failures << " failures\n";
    return failures ? 1 : 0;
}
