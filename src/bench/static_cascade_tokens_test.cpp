#include "../app/shell_verbs.h"
#include <windows.h>
#include <cstdio>
#include <stdexcept>

namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
void Require(LSTATUS status) {
    if (status != ERROR_SUCCESS) {
        std::printf("[ERROR] registry fixture status=%ld\n", status);
        throw std::runtime_error("registry fixture");
    }
}
struct Fixture {
    HKEY real_user = nullptr, classes = nullptr, machine = nullptr, user = nullptr;
    bool override_classes = false, override_machine = false, override_user = false;
    std::wstring base = L"Software\\PulseTest\\StaticCascade_" + std::to_wstring(GetCurrentProcessId());
    void Init() {
        Require(RegOpenKeyExW(HKEY_CURRENT_USER, L"", 0, KEY_ALL_ACCESS, &real_user));
        for (const auto& part : {std::pair{L"Classes", &classes}, {L"Machine", &machine}, {L"User", &user}})
            Require(RegCreateKeyExW(real_user, (base + L"\\" + part.first).c_str(), 0, nullptr,
                REG_OPTION_VOLATILE, KEY_ALL_ACCESS, nullptr, part.second, nullptr));
    }
    ~Fixture() {
        if (override_user) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
        if (override_machine) RegOverridePredefKey(HKEY_LOCAL_MACHINE, nullptr);
        if (override_classes) RegOverridePredefKey(HKEY_CLASSES_ROOT, nullptr);
        for (auto key : {classes, machine, user}) if (key) RegCloseKey(key);
        if (real_user) { RegDeleteTreeW(real_user, base.c_str()); RegCloseKey(real_user); }
    }
    void Value(HKEY root, const std::wstring& path, const wchar_t* name, const wchar_t* value) {
        HKEY key = nullptr;
        Require(RegCreateKeyExW(root, path.c_str(), 0, nullptr, REG_OPTION_VOLATILE,
            KEY_SET_VALUE, nullptr, &key, nullptr));
        const auto status = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value), static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
        RegCloseKey(key); Require(status);
    }
    void Override() {
        Require(RegOverridePredefKey(HKEY_CLASSES_ROOT, classes)); override_classes = true;
        Require(RegOverridePredefKey(HKEY_LOCAL_MACHINE, machine)); override_machine = true;
        Require(RegOverridePredefKey(HKEY_CURRENT_USER, user)); override_user = true;
    }
};
}
int main() {
    try {
        Fixture f; f.Init();
        const std::wstring store = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Explorer\\CommandStore\\shell\\";
        const std::wstring parent = L"Directory\\shell\\FixtureCascade";
        f.Value(f.classes, parent, L"MUIVerb", L"Fixture cascade");
        f.Value(f.classes, parent, L"SubCommands", L" ; \t\r\n two words \t\r\n ; tab\tinside ; plain;; user only; nested menu; ;");
        for (const auto* name : {L"two words", L"tab\tinside", L"plain", L"deep leaf", L"two", L"words"}) {
            f.Value(f.machine, store + name, L"MUIVerb", name);
            f.Value(f.machine, store + name + L"\\command", nullptr, L"fixture.exe \"%1\"");
        }
        f.Value(f.user, store + L"user only", L"MUIVerb", L"User leaf");
        f.Value(f.user, store + L"user only\\command", nullptr, L"user.exe \"%1\"");
        f.Value(f.user, store + L"plain", L"MUIVerb", L"Shadow plain");
        f.Value(f.user, store + L"plain\\command", nullptr, L"shadow.exe");
        f.Value(f.machine, store + L"nested menu", L"MUIVerb", L"Nested");
        f.Value(f.machine, store + L"nested menu", L"SubCommands", L"\r\n deep leaf \t;");
        f.Override();
        const auto rows = pulse::app::EnumerateStaticVerbs(L":folder");
        Check(rows.size() == 1 && rows[0].verb == L"FixtureCascade", "actual EnumerateStaticVerbs returns fixture cascade");
        const std::vector<pulse::app::StaticVerb> empty;
        const auto& c = rows.empty() ? empty : rows[0].children;
        Check(c.size() == 5, "semicolon list ignores empty tokens and preserves five entries");
        Check(c.size() >= 1 && c[0].verb == L"two words", "boundary whitespace trimmed; internal spaces preserved, not split decoys");
        Check(c.size() >= 2 && c[1].verb == L"tab\tinside", "internal tab remains part of CommandStore key");
        Check(c.size() >= 3 && c[2].verb == L"plain" && c[2].reg_root == 1 && c[2].display == L"plain", "ordinary token order and machine precedence unchanged");
        Check(c.size() >= 4 && c[3].verb == L"user only" && c[3].reg_root == 2 && c[3].command == L"user.exe \"%1\"" && c[3].reg_key == store + L"user only", "user fallback retains exact command and invocation registry route");
        Check(c.size() >= 5 && c[4].verb == L"nested menu" && c[4].children.size() == 1 && c[4].children[0].verb == L"deep leaf" && c[4].children[0].command == L"fixture.exe \"%1\"", "nested production cascade resolves spaced key and command unchanged");
        std::printf("failures=%d\n", failures);
        return failures ? 1 : 0;
    } catch (const std::exception&) { return 2; }
}
