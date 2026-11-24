// association_test.cpp - 接管压缩包 writes and undoes its registry entries. HKEY_CURRENT_USER is
// redirected to a throwaway key for the whole run, so the user's own associations are never touched.
#include "association.h"
#include "takeover_record.h"
#include <cstdio>
#include <string>

HRESULT query_error = S_OK;
HRESULT TestCurrentDefault(const wchar_t* extension, std::wstring& result) {
    if (FAILED(query_error)) return query_error;
    result = takeover::ReadString(HKEY_CURRENT_USER,
        std::wstring(L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\") + extension + L"\\UserChoice", L"ProgId");
    if (result.empty()) result = takeover::ReadString(HKEY_CURRENT_USER, takeover::ClassesKey(extension), nullptr);
    return result.empty() ? HRESULT_FROM_WIN32(ERROR_NO_ASSOCIATION) : S_OK;
}
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
    setvbuf(stdout, nullptr, _IONBF, 0);
    wchar_t sandbox[128]{};
    swprintf_s(sandbox, L"Software\\Pulse\\ArchiveRegression-%lu-%llu", GetCurrentProcessId(), GetTickCount64());
    HKEY root = nullptr;
    const LSTATUS created = RegCreateKeyExW(HKEY_CURRENT_USER, sandbox, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &root, nullptr);
    const LSTATUS redirected = created == ERROR_SUCCESS ? RegOverridePredefKey(HKEY_CURRENT_USER, root) : created;
    if (redirected != ERROR_SUCCESS) {
        std::printf("[FAIL] could not redirect HKEY_CURRENT_USER (%ld, %ld)\n", static_cast<long>(created), static_cast<long>(redirected));
        if (root) RegCloseKey(root);
        RegDeleteTreeW(HKEY_CURRENT_USER, sandbox);
        return 1;
    }
    HKEY classes = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Classes", 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &classes, nullptr) != ERROR_SUCCESS ||
        RegOverridePredefKey(HKEY_CLASSES_ROOT, classes) != ERROR_SUCCESS) return 2;
    wchar_t exe[32768]{};
    GetModuleFileNameW(nullptr, exe, ARRAYSIZE(exe));
    Write(L"Software\\Classes\\Other.7z\\shell\\open\\command", nullptr, std::wstring(L"\"") + exe + L"\" \"%1\"");
    Write(L"Software\\Classes\\.zip", nullptr, L"CompressedFolder");
    // What was there before: a program's own .7z default, and a .zip key holding another value.
    Write(L"Software\\Classes\\.7z", nullptr, L"Other.7z");
    Write(L"Software\\Classes\\.zip", L"Content Type", L"application/zip");

    Check(association::Apply(association::kAllGroups, true), "Apply writes every format");
    Check(Read(L"Software\\Classes\\.zip") == L"Pulse.Archive.zip", "nonempty Windows default is claimed");
    Check(association::Apply(association::kAllGroups, true), "repeat Apply succeeds without replacing backup");
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
    Check(Read(L"Software\\Classes\\.zip") == L"CompressedFolder", "repeated Apply restores original nonempty default");
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

    query_error = E_ACCESSDENIED;
    Check(static_cast<int>(association::Query(association::kAllGroups)[0].owner) == 4, "query errors are Unknown, not Free");
    query_error = S_OK;
    Write(L"Software\\Classes\\.zip", nullptr, L"CompressedFolder");
    association::Apply(1, true);
    Write(L"Software\\Classes\\.zip", nullptr, L"External.Change");
    association::Remove();
    Check(Read(L"Software\\Classes\\.zip") == L"External.Change", "Remove preserves an external default change");
    const std::wstring choice = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\FileExts\\.zip\\UserChoice";
    Write(choice, L"ProgId", L"Other.7z");
    Write(choice, L"Hash", L"untouched-fixture");
    Write(L"Software\\Classes\\.zip", nullptr, L"CompressedFolder");
    association::Apply(1, true);
    Check(Read(L"Software\\Classes\\.zip") == L"CompressedFolder", "valid UserChoice prevents claiming");
    association::Remove();
    Write(choice, L"ProgId", L"Uninstalled.Archiver");
    association::Apply(1, true);
    Check(Read(L"Software\\Classes\\.zip") == L"Pulse.Archive.zip", "stale UserChoice permits fallback registration");
    Check(association::Query(1)[0].owner != association::Owner::Pulse, "ineffective fallback is not reported as Pulse");
    Check(Read(choice, L"ProgId") == L"Uninstalled.Archiver" && Read(choice, L"Hash") == L"untouched-fixture", "UserChoice and hash are untouched");
    association::Remove();
    Check(Read(L"Software\\Classes\\.zip") == L"CompressedFolder", "stale choice fallback restores original default");
    RegDeleteTreeW(HKEY_CURRENT_USER, choice.c_str());
    association::Apply(1, true);
    const auto backup = std::wstring(takeover::kRecordKey) + L"\\Previous\\.zip";
    Write(backup, L"Saved", L"corrupt-marker");
    Check(!association::Apply(0, true), "disable reports a corrupt restore marker as failure");
    Check(Read(L"Software\\Classes\\.zip") == L"Pulse.Archive.zip" && KeyExists(backup) && KeyExists(zip),
          "failed restore preserves the default, backup and handler");
    Check(!association::Apply(1u << 2, true), "excluding a format reports restore failure");
    DWORD saved = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, backup.c_str(), L"Saved", REG_DWORD, &saved, sizeof(saved));
    Check(association::Apply(0, true) && Read(L"Software\\Classes\\.zip") == L"CompressedFolder",
          "repairing the marker allows the original default to be restored");
    RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
    RegCloseKey(classes);
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    RegCloseKey(root);
    RegDeleteTreeW(HKEY_CURRENT_USER, sandbox);
    std::printf("%s\n", failures ? "FAILED" : "ALL PASSED");
    return failures ? 1 : 0;
}
