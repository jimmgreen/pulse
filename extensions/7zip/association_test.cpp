// association_test.cpp - 接管压缩包 writes and undoes its registry entries. HKEY_CURRENT_USER is
// redirected to a throwaway key for the whole run, so the user's own associations are never touched.
#include "association.h"
#include "takeover_record.h"
#include <cstdio>
#include <string>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
bool KeyExists(const std::wstring& key) {
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return false;
    RegCloseKey(handle);
    return true;
}
bool ValueExists(const std::wstring& key, const wchar_t* name) {
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), name, RRF_RT_ANY, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
std::wstring Read(const std::wstring& key, const wchar_t* name = nullptr) { return takeover::ReadString(HKEY_CURRENT_USER, key, name); }
void Write(const std::wstring& key, const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, key.c_str(), name, REG_SZ, value.c_str(), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}
} // namespace

int main() {
    wchar_t sandbox[128]{};
    swprintf_s(sandbox, L"Software\\PulseTest\\ArchiveTakeover-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    HKEY root = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, sandbox, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &root, nullptr);
    const LSTATUS redirected = created == ERROR_SUCCESS ? RegOverridePredefKey(HKEY_CURRENT_USER, root) : created;
    if (redirected != ERROR_SUCCESS) {
        std::printf("[FAIL] could not redirect HKEY_CURRENT_USER (%ld, %ld)\n", static_cast<long>(created), static_cast<long>(redirected));
        if (root) RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, sandbox);
        RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\PulseTest");
        return 1;
    }
    // What was there before: a program's own .7z default, and a .zip key holding another value.
    Write(L"Software\\Classes\\.7z", nullptr, L"Other.7z");
    Write(L"Software\\Classes\\.zip", L"Content Type", L"application/zip");

    Check(association::Apply(association::kAllGroups, true), "Apply writes every format");
    const std::wstring zip = L"Software\\Classes\\Pulse.Archive.zip";
    Check(Read(zip).size() > 0 && !Read(zip + L"\\DefaultIcon").empty(), "the ZIP type has a name and an icon");
    Check(Read(zip + L"\\DefaultIcon").find(L",-101") != std::wstring::npos, "ZIP's icon is resource 101");
    Check(Read(L"Software\\Classes\\Pulse.Archive.lha\\DefaultIcon").find(L",-113") != std::wstring::npos, "LHA's icon is resource 113");
    const auto command = Read(zip + L"\\shell\\open\\command");
    Check(command.find(L"\"pulse:extract:%1\"") != std::wstring::npos, "a double-click runs pulse:extract:");
    Check(ValueExists(L"Software\\Classes\\.zip\\OpenWithProgids", L"Pulse.Archive.zip"), "ZIP is offered in Open with");
    Check(Read(L"Software\\Classes\\.7z") == L"Other.7z", "a program's own .7z default is kept");
    Check(ValueExists(takeover::kRecordKey, L".tgz") && ValueExists(takeover::kRecordKey, L".cpio"), "every extension is recorded");
    Check(Read(takeover::kRegisteredApps, takeover::kAppName) == takeover::kCapabilitiesKey, "Pulse is listed in Default apps");

    Check(association::Apply(association::kAllGroups & ~1u, false), "Apply without ZIP, double-click previews");
    Check(!KeyExists(zip) && !ValueExists(L"Software\\Classes\\.zip\\OpenWithProgids", L"Pulse.Archive.zip"), "ZIP is given back");
    Check(Read(L"Software\\Classes\\Pulse.Archive.7z\\shell\\open\\command").find(L"\"pulse:archive-preview:%1\"") != std::wstring::npos,
          "the preview command replaces the extract one");

    association::Remove();
    Check(!KeyExists(L"Software\\Classes\\Pulse.Archive.7z") && !KeyExists(L"Software\\Classes\\Pulse.Archive.cpio"), "Remove deletes the types");
    Check(!KeyExists(takeover::kRecordKey) && !KeyExists(takeover::kCapabilitiesKey), "Remove deletes the record");
    Check(!ValueExists(takeover::kRegisteredApps, takeover::kAppName), "Remove leaves Default apps");
    Check(Read(L"Software\\Classes\\.7z") == L"Other.7z", ".7z still opens with the other program");
    Check(Read(L"Software\\Classes\\.zip", L"Content Type") == L"application/zip", ".zip keeps its other value");
    Check(!KeyExists(L"Software\\Classes\\.cpio"), "keys Pulse created are gone");

    // Pulse's own cleanup (extension uninstalled) undoes the same record.
    association::Apply(association::kAllGroups, true);
    Check(takeover::RemoveAll() && !KeyExists(L"Software\\Classes\\Pulse.Archive.rar") && !KeyExists(takeover::kRecordKey),
          "takeover::RemoveAll undoes a takeover without the extension");
    Check(!takeover::RemoveAll(), "nothing recorded, nothing to undo");

    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(root);
    RegDeleteTreeW(HKEY_CURRENT_USER, sandbox);
    RegDeleteKeyW(HKEY_CURRENT_USER, L"Software\\PulseTest");
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
