#include "../common/dialog_auto_arch_status.h"
#include <cstdio>
#include <filesystem>
#include <fstream>

namespace {
using namespace pulse::dialog_auto;
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* text) { ++checks; if (!okay) ++failures; std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", text); }
constexpr const wchar_t* classes[]{
    L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
    L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"};
struct Fixture {
    HKEY key = nullptr;
    std::wstring path = L"Software\\Pulse\\Tests\\DialogAutoArchStatus\\" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
    explicit Fixture(const wchar_t* suffix) {
        path += suffix;
        const auto error = RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key, nullptr);
        if (error) std::printf("DIAGNOSTIC fixture_create_error=%ld\n", error);
        Check(error == ERROR_SUCCESS, "create isolated registry fixture without HKCU remap");
    }
    ~Fixture() { if (key) RegCloseKey(key); RegDeleteTreeW(HKEY_CURRENT_USER, path.c_str()); }
};
void Text(HKEY root, const std::wstring& path, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(root, path.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    const auto written = opened ? opened : RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    if (written) std::printf("DIAGNOSTIC fixture_string_error=%ld\n", written);
    Check(written == ERROR_SUCCESS, "write only isolated string fixture");
    if (key) RegCloseKey(key);
}
void Enabled(HKEY root, bool x86, DWORD enabled) {
    HKEY key = nullptr;
    const auto opened = RegCreateKeyExW(root, x86 ? L"Software\\Pulse\\DialogAuto32" : L"Software\\Pulse\\DialogAuto", 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
    const auto written = opened ? opened : RegSetValueExW(key, L"Enabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&enabled), sizeof(enabled));
    if (written) std::printf("DIAGNOSTIC fixture_enabled_error=%ld\n", written);
    Check(written == ERROR_SUCCESS, "write only isolated enabled fixture");
    if (key) RegCloseKey(key);
}
void Registered(HKEY root, bool x86, const std::wstring& dll) {
    Text(root, x86 ? L"Software\\Pulse\\DialogAuto32" : L"Software\\Pulse\\DialogAuto", L"DllPath", dll);
    Enabled(root, x86, 1);
    for (const auto path : classes) {
        Text(root, path, L"PulseDialogAutoOwner", L"Pulse.DialogAuto.v1");
        Text(root, std::wstring(path) + L"\\InprocServer32", nullptr, dll);
        Text(root, std::wstring(path) + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    }
}
}

int main() {
    const auto directory = std::filesystem::absolute(L"bench_data") / (L"dialog-arch-status-" + std::to_wstring(GetCurrentProcessId()));
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return 2;
    const auto dll64 = directory / L"PulseDialogAuto64.dll", dll32 = directory / L"PulseDialogAuto32.dll";
    {
        Fixture fixture(L"-64");
        auto status = InspectArchStatus(fixture.key, false, directory.wstring());
        Check(status.component == ArchComponentState::Missing && status.registration == ArchRegistrationState::Unregistered, "missing component and absent registration remain distinct");
        std::ofstream(dll64) << "presence fixture, not an activatable DLL";
        status = InspectArchStatus(fixture.key, false, directory.wstring());
        Check(status.component == ArchComponentState::Present && status.registration == ArchRegistrationState::Unregistered, "component presence never claims registration or activation");
        Registered(fixture.key, false, dll64.wstring());
        status = InspectArchStatus(fixture.key, false);
        Check(status.component == ArchComponentState::Present && status.registration == ArchRegistrationState::Registered && !status.x86, "both owned x64 registrations match without performing activation");
        Enabled(fixture.key, false, 0);
        HKEY readonly = nullptr;
        Check(RegOpenKeyExW(HKEY_CURRENT_USER, fixture.path.c_str(), 0, KEY_READ, &readonly) == ERROR_SUCCESS, "open read-only fixture handle");
        status = InspectArchStatus(readonly, false);
        Check(status.registration == ArchRegistrationState::DisabledMatching, "read-only status distinguishes disabled matching keys from absent registration");
        if (readonly) RegCloseKey(readonly);
        Enabled(fixture.key, false, 1);
        Text(fixture.key, std::wstring(classes[0]) + L"\\InprocServer32", nullptr, L"C:\\unrelated\\other.dll");
        Check(InspectArchStatus(fixture.key, false).registration == ArchRegistrationState::Conflict, "mismatched registered DLL reports conflict");
        Registered(fixture.key, false, dll64.wstring());
        RegDeleteTreeW(fixture.key, classes[1]);
        Check(InspectArchStatus(fixture.key, false).registration == ArchRegistrationState::Conflict, "partial Open/Save registration is not reported ready");
        Registered(fixture.key, false, dll64.wstring());
        Text(fixture.key, classes[0], L"PulseDialogAutoOwner", L"external-owner");
        Check(InspectArchStatus(fixture.key, false).registration == ArchRegistrationState::Conflict, "external ownership remains conflict without overwriting");
        Registered(fixture.key, false, dll64.wstring());
        Text(fixture.key, L"Software\\Pulse\\DialogAuto", L"Enabled", L"malformed-dword");
        status = InspectArchStatus(fixture.key, false);
        Check(status.registration == ArchRegistrationState::ReadError && status.registry_error != ERROR_SUCCESS, "malformed enabled field remains read error");
        Enabled(fixture.key, false, 1);
        Check(DeleteFileW(dll64.c_str()) != FALSE, "remove only owned presence fixture");
        status = InspectArchStatus(fixture.key, false);
        Check(status.registration == ArchRegistrationState::Registered && status.component == ArchComponentState::Missing, "matching stale registry does not conceal missing component");
    }
    {
        Fixture fixture(L"-32");
        std::ofstream(dll32) << "x86 presence fixture, no activation";
        Registered(fixture.key, true, dll32.wstring());
        const auto status = InspectArchStatus(fixture.key, true);
        Check(status.x86 && status.component == ArchComponentState::Present && status.registration == ArchRegistrationState::Registered,
            "x86 reads explicit DialogAuto32 configuration independently");
    }
    const auto format = L"{arch}: {component} / {registration}";
    Check(FormatArchStatus(format, L"x86", L"missing", L"unregistered") == L"x86: missing / unregistered", "format substitutes exact named localized slots");
    Check(FormatArchStatus(L"{registration}\n{arch} {component}", L"x64", L"present", L"not activated") == L"not activated\nx64 present", "translation can reorder slots without losing architecture");
    Check(DeleteFileW(dll32.c_str()) != FALSE && RemoveDirectoryW(directory.c_str()) != FALSE, "remove only isolated owned architecture fixtures");
    std::printf("ARCH_STATUS checks=%u failures=%u activation_attempts=0\n", checks, failures);
    return failures ? 1 : 0;
}
