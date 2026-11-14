#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "../fs/wsl_distros.h"

namespace {

int failures = 0;

void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

void SetString(HKEY key, const wchar_t* name, const std::wstring& value) {
    RegSetValueExW(key, name, 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(value.c_str()),
                   static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}

HKEY CreateKey(HKEY parent, const wchar_t* name) {
    HKEY key = nullptr;
    RegCreateKeyExW(parent, name, 0, nullptr, REG_OPTION_VOLATILE, KEY_ALL_ACCESS,
                    nullptr, &key, nullptr);
    return key;
}

} // namespace

int main() {
    const std::wstring fixture = L"Software\\PulseWslDistroTest\\" +
        std::to_wstring(GetCurrentProcessId());
    HKEY override_root = CreateKey(HKEY_CURRENT_USER, fixture.c_str());
    Check(override_root != nullptr, "fixture root created");
    if (!override_root) return 1;

    HKEY lxss = CreateKey(override_root, L"Software\\Microsoft\\Windows\\CurrentVersion\\Lxss");
    Check(lxss != nullptr, "fixture Lxss key created");
    if (!lxss) {
        RegCloseKey(override_root);
        RegDeleteTreeW(HKEY_CURRENT_USER, fixture.c_str());
        return 1;
    }
    const auto add = [&](const wchar_t* subkey, const std::wstring& name) {
        HKEY key = CreateKey(lxss, subkey);
        Check(key != nullptr, "fixture distribution key created");
        if (key) {
            SetString(key, L"DistributionName", name);
            RegCloseKey(key);
        }
    };
    add(L"{1}", L"Ubuntu");
    add(L"{2}", L"alpine");
    add(L"{3}", L"UBUNTU");
    add(L"{4}", L"bad/name");
    add(L"{5}", L"bad\\name");
    add(L"{6}", L"bad\x01name");
    add(L"{7}", L".");
    add(L"{8}", L"..");
    RegCloseKey(lxss);

    Check(RegOverridePredefKey(HKEY_CURRENT_USER, override_root) == ERROR_SUCCESS,
          "HKCU redirected to isolated fixture");
    const auto distributions = pulse::fs::EnumerateRegisteredWslDistributions();
    Check(distributions.size() == 2, "valid names are filtered and duplicates removed");
    if (distributions.size() == 2) {
        Check(distributions[0].name == L"alpine" && distributions[1].name == L"Ubuntu",
              "names are sorted case-insensitively with stable spelling");
        for (const auto& distro : distributions) {
            Check(distro.path == L"\\\\?\\UNC\\wsl.localhost\\" + distro.name,
                  "distribution path uses the canonical WSL UNC prefix");
        }
    }

    HKEY empty_root = CreateKey(HKEY_CURRENT_USER, (fixture + L"Empty").c_str());
    Check(empty_root != nullptr && RegOverridePredefKey(HKEY_CURRENT_USER, empty_root) == ERROR_SUCCESS,
          "HKCU redirected to fixture without Lxss");
    Check(pulse::fs::EnumerateRegisteredWslDistributions().empty(),
          "missing Lxss registry returns an empty list");
    RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
    if (empty_root) RegCloseKey(empty_root);
    RegCloseKey(override_root);
    RegDeleteTreeW(HKEY_CURRENT_USER, fixture.c_str());
    RegDeleteTreeW(HKEY_CURRENT_USER, (fixture + L"Empty").c_str());
    return failures ? 1 : 0;
}
