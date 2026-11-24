#include "../app/dialog_management_controller.h"
#include "../common/dialog_auto_registration.h"
#include "../common/json_value.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
namespace {std::wstring install_root,state_root;}
namespace pulse::app {std::wstring GetPulseDataDir(){return state_root;}}
namespace pulse::app_runtime_paths {
std::wstring InstallRoot(){return install_root;}
std::wstring RuntimeDirectory(){return install_root;}
std::wstring StableEntryPath(){return install_root+L"\\pulse.exe";}
}
namespace {
namespace fs = std::filesystem;
using namespace pulse::app;
using namespace pulse::dialog_auto;

std::optional<DialogManagementResult> RunController(const std::wstring& operation_name, const std::wstring& root,
    const std::wstring& state) {
    install_root = root;
    state_root = state;
    DialogManagementController controller(nullptr, 0, state);
    const auto generation = operation_name == L"enable" ? controller.EnableAuto() :
        operation_name == L"disable" ? controller.DisableAuto() : controller.Refresh();
    const auto deadline = GetTickCount64() + 60000;
    while (GetTickCount64() < deadline) {
        for (auto& entry : controller.Drain()) {
            if (entry.generation == generation) return std::move(entry);
        }
        Sleep(10);
    }
    return std::nullopt;
}

int RunUpgradeRebindTest(const std::wstring& dual_install, const std::wstring& base_state) {
    unsigned passed = 0, failed = 0;
    auto check = [&](bool ok, const char* label, DWORD err = 0) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << label;
        if (!ok && err) std::cout << " (error=" << err << ")";
        std::cout << '\n';
        ok ? ++passed : ++failed;
    };

    const fs::path state_dir = fs::path(base_state) / L"upgrade-rebind-work";
    const fs::path x64_install = state_dir / L"x64_install";
    std::error_code ec;
    fs::remove_all(state_dir, ec);
    fs::create_directories(x64_install, ec);
    if (ec) {
        std::cout << "[FAIL] cannot create work dir\n";
        return 1;
    }

    const std::wstring sandbox_subkey = L"Software\\PulseDialogDualUpgradeTest_" +
        std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(GetTickCount64());
    HKEY sandbox_key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, sandbox_subkey.c_str(), 0, nullptr, 0, KEY_ALL_ACCESS,
            nullptr, &sandbox_key, nullptr) != ERROR_SUCCESS) {
        std::cout << "[FAIL] cannot create isolated HKCU key\n";
        return 1;
    }
    struct HiveGuard {
        std::wstring subkey;
        HKEY key = nullptr;
        bool overridden = false;
        ~HiveGuard() {
            if (overridden) RegOverridePredefKey(HKEY_CURRENT_USER, nullptr);
            if (key) RegCloseKey(key);
            RegDeleteTreeW(HKEY_CURRENT_USER, subkey.c_str());
        }
    } guard{sandbox_subkey, sandbox_key, false};

    if (RegOverridePredefKey(HKEY_CURRENT_USER, sandbox_key) != ERROR_SUCCESS) {
        std::cout << "[FAIL] RegOverridePredefKey failed\n";
        return 1;
    }
    guard.overridden = true;

    // 1. Verify full unpacked 1.0.61 dual-arch bundle is recognized (x64 + x86 components present).
    {
        const auto dual_state = state_dir / L"state_dual_refresh";
        const auto res = RunController(L"refresh", dual_install, dual_state.wstring());
        const bool ok = res.has_value() && !res->error && res->bundle_verified &&
            res->auto_arch[0].component == ArchComponentState::Present &&
            res->auto_arch[1].component == ArchComponentState::Present;
        check(ok, "unpacked 1.0.61 bundle resolves verified x64 and x86 dialog bridges", res ? res->bundle_error : ERROR_TIMEOUT);
    }

    // Prepare single-arch x64 view in %TEMP% (without dialog-x86-current.json) because RegOverridePredefKey
    // does not split HKCU\Software\Classes WOW64 redirection views on a synthetic root.
    fs::copy(fs::path(dual_install), x64_install, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::remove(x64_install / L"dialog-x86-current.json", ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        std::cout << "[FAIL] cannot prepare x64-only view: " << ec.message() << "\n";
        return 1;
    }

    auto reset_hive = [] {
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto32");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}");
        RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}");
    };
    auto set_sz = [](const wchar_t* subkey, REGSAM view, const wchar_t* name, const std::wstring& value) {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        const LSTATUS st = RegSetValueExW(k, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
            static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        RegCloseKey(k);
        return st == ERROR_SUCCESS;
    };
    auto set_dw = [](const wchar_t* subkey, REGSAM view, const wchar_t* name, DWORD value) {
        HKEY k = nullptr;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, subkey, 0, nullptr, 0, KEY_WRITE | view, nullptr, &k, nullptr) != ERROR_SUCCESS)
            return false;
        const LSTATUS st = RegSetValueExW(k, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
        RegCloseKey(k);
        return st == ERROR_SUCCESS;
    };

    const std::wstring retired_old_dll = x64_install.wstring() +
        L"\\DialogBridge\\e8465a107b894ad50b082afa255fc19573c89a1a2b5b223c2d09a42d2dd379e6\\PulseDialogAuto64.dll";

    // Case A: Exact user state from support-report.json (disabled journal pointing to deleted old digest).
    {
        reset_hive();
        const bool seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", retired_old_dll) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 0) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 0);
        check(seeded, "seed retired disabled journal matching support-report.json");
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_a").wstring());
        const bool ok = res.has_value() && !res->error && !res->auto_rollback_error &&
            res->auto_enabled && !res->auto_conflict && !res->auto_path_mismatch &&
            res->auto_arch[0].registration == ArchRegistrationState::Registered &&
            !res->auto_dll.empty() && _wcsicmp(res->auto_dll.c_str(), retired_old_dll.c_str()) != 0;
        check(ok, "EnableAuto rebinds disabled retired digest journal without error 183", res ? res->error : ERROR_TIMEOUT);
    }

    // Case B: Owned enabled stale digest mismatch (auto_path_mismatch).
    {
        reset_hive();
        const std::wstring stale_enabled = x64_install.wstring() + L"\\DialogBridge\\" +
            std::wstring(64, L'a') + L"\\PulseDialogAuto64.dll";
        bool seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", stale_enabled) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 1) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 1);
        for (const auto* clsid : {
            L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
            L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            const std::wstring sub = std::wstring(clsid) + L"\\InprocServer32";
            seeded = seeded && set_sz(clsid, KEY_WOW64_64KEY, L"PulseDialogAutoOwner", L"Pulse.DialogAuto.v1") &&
                set_sz(sub.c_str(), KEY_WOW64_64KEY, nullptr, stale_enabled) &&
                set_sz(sub.c_str(), KEY_WOW64_64KEY, L"ThreadingModel", L"Apartment");
        }
        check(seeded, "seed owned enabled stale digest mismatch");
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_b").wstring());
        const bool ok = res.has_value() && !res->error && !res->auto_rollback_error &&
            res->auto_enabled && !res->auto_conflict && !res->auto_path_mismatch &&
            res->auto_arch[0].registration == ArchRegistrationState::Registered;
        check(ok, "EnableAuto heals owned enabled mismatch and binds current bundle", res ? res->error : ERROR_TIMEOUT);
    }

    // Case C: Owned disabled journal missing Stage value.
    {
        reset_hive();
        const bool seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", retired_old_dll) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 0);
        check(seeded, "seed owned disabled journal missing Stage value");
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_c").wstring());
        const bool ok = res.has_value() && !res->error && !res->auto_rollback_error &&
            res->auto_enabled && !res->auto_conflict && !res->auto_path_mismatch;
        check(ok, "EnableAuto normalizes owned disabled journal missing Stage", res ? res->error : ERROR_TIMEOUT);
    }

    // Case D: Foreign disabled installation must still be refused with ERROR_ALREADY_EXISTS (183).
    {
        reset_hive();
        const std::wstring foreign = L"C:\\ForeignPulse\\PulseDialogAuto64.dll";
        const bool seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", foreign) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 0) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 0);
        check(seeded, "seed foreign disabled installation");
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_d").wstring());
        const bool ok = res.has_value() && res->error == ERROR_ALREADY_EXISTS && !res->auto_enabled &&
            _wcsicmp(res->auto_dll.c_str(), foreign.c_str()) == 0;
        check(ok, "EnableAuto still refuses foreign disabled installation with ERROR_ALREADY_EXISTS", res ? res->error : ERROR_TIMEOUT);
    }

    auto seed_active_registration = [&](const std::wstring& dll_path) {
        reset_hive();
        bool seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", dll_path) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 1) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 1);
        for (const auto* clsid : {
            L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
            L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            const std::wstring sub = std::wstring(clsid) + L"\\InprocServer32";
            seeded = seeded && set_sz(clsid, KEY_WOW64_64KEY, L"PulseDialogAutoOwner", L"Pulse.DialogAuto.v1") &&
                set_sz(sub.c_str(), KEY_WOW64_64KEY, nullptr, dll_path) &&
                set_sz(sub.c_str(), KEY_WOW64_64KEY, L"ThreadingModel", L"Apartment");
        }
        return seeded;
    };

    // Case E: Active Pulse registration pointing to a missing directory outside current install_root self-heals on enable and disable.
    {
        const std::wstring orphan_missing = (state_dir / L"deleted_old_pulse" / L"PulseDialogAuto64.dll").wstring();
        check(seed_active_registration(orphan_missing), "seed active orphaned missing-DLL Pulse registration (enable)");
        const auto enabled = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_e_enable").wstring());
        const bool enable_ok = enabled.has_value() && !enabled->error && !enabled->auto_rollback_error &&
            enabled->auto_enabled && !enabled->auto_conflict && !enabled->auto_path_mismatch &&
            enabled->auto_arch[0].registration == ArchRegistrationState::Registered &&
            !enabled->auto_dll.empty() && _wcsicmp(enabled->auto_dll.c_str(), orphan_missing.c_str()) != 0;
        check(enable_ok, "EnableAuto heals active orphaned missing-DLL registration without error 183",
            enabled ? enabled->error : ERROR_TIMEOUT);

        check(seed_active_registration(orphan_missing), "seed active orphaned missing-DLL Pulse registration (disable)");
        const auto disabled = RunController(L"disable", x64_install.wstring(), (state_dir / L"case_e_disable").wstring());
        const bool disable_ok = disabled.has_value() && !disabled->error && !disabled->auto_rollback_error &&
            !disabled->auto_enabled && !disabled->auto_conflict &&
            disabled->auto_arch[0].registration == ArchRegistrationState::DisabledMatching;
        check(disable_ok, "DisableAuto cleanly disables active orphaned missing-DLL registration without error 183",
            disabled ? disabled->error : ERROR_TIMEOUT);
    }

    // Case F: Active Pulse registration pointing to a LIVE file on disk outside current install_root must still fail with ERROR_ALREADY_EXISTS (183).
    {
        const fs::path live_dir = state_dir / L"foreign_live";
        const fs::path live_dll = live_dir / L"PulseDialogAuto64.dll";
        fs::create_directories(live_dir, ec);
        {
            std::ofstream out(live_dll, std::ios::binary);
            out << "live";
        }
        check(fs::exists(live_dll) && seed_active_registration(live_dll.wstring()), "seed active live foreign Pulse registration");
        const auto enabled = RunController(L"enable", x64_install.wstring(), (state_dir / L"case_f").wstring());
        const bool ok = enabled.has_value() && enabled->error == ERROR_ALREADY_EXISTS && !enabled->auto_rollback_error &&
            enabled->auto_enabled && !enabled->auto_conflict &&
            _wcsicmp(enabled->auto_dll.c_str(), live_dll.wstring().c_str()) == 0;
        check(ok, "EnableAuto still refuses live foreign Pulse installation with ERROR_ALREADY_EXISTS",
            enabled ? enabled->error : ERROR_TIMEOUT);
        fs::remove_all(live_dir, ec);
    }

    reset_hive();
    fs::remove_all(state_dir, ec);
    std::cout << "Summary: " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
}
int wmain(int argc,wchar_t** argv) {
    using namespace pulse::app;
    if(argc!=4)return 2;
    const std::wstring operation(argv[1]);
    if (operation == L"--test-upgrade-rebind") {
        if (!std::filesystem::path(argv[2]).is_absolute() || !std::filesystem::path(argv[3]).is_absolute()) return 2;
        return RunUpgradeRebindTest(argv[2], argv[3]);
    }
    if(operation!=L"--refresh" && operation!=L"--enable" && operation!=L"--disable")return 2;
    install_root=argv[2];state_root=argv[3];
    if(!std::filesystem::path(install_root).is_absolute() || !std::filesystem::path(state_root).is_absolute())return 2;
    DialogManagementController controller(nullptr,0,state_root);
    const auto generation=operation==L"--enable"?controller.EnableAuto():operation==L"--disable"?controller.DisableAuto():controller.Refresh();
    std::optional<DialogManagementResult> result;
    const auto deadline=GetTickCount64()+60000;
    while(GetTickCount64()<deadline&&!result) {
        for(auto& entry:controller.Drain())if(entry.generation==generation)result=std::move(entry);
        if(!result)Sleep(10);
    }
    if(!result){std::cout<<"CONTROLLER timeout\n";return 3;}
    auto json=pulse::json::Value::Object();
    json.set("operation",pulse::json::Utf8(operation));json.set("error",static_cast<int>(result->error));
    json.set("rollback_error",static_cast<int>(result->auto_rollback_error));
    json.set("bundle_verified",result->bundle_verified);json.set("bundle_error",static_cast<int>(result->bundle_error));
    json.set("enabled",result->auto_enabled);json.set("conflict",result->auto_conflict);json.set("path_mismatch",result->auto_path_mismatch);
    auto arches=pulse::json::Value::Array();
    bool both=true;
    for(const auto& arch:result->auto_arch) {
        auto item=pulse::json::Value::Object();item.set("architecture",arch.x86?"x86":"x64");
        item.set("component",static_cast<int>(arch.component));item.set("registration",static_cast<int>(arch.registration));
        item.set("registry_error",static_cast<int>(arch.registry_error));item.set("dll_path",pulse::json::Utf8(arch.dll_path));arches.push(std::move(item));
        if(operation==L"--enable")both=both && arch.registration==pulse::dialog_auto::ArchRegistrationState::Registered && arch.component==pulse::dialog_auto::ArchComponentState::Present;
        if(operation==L"--disable")both=both && (arch.registration==pulse::dialog_auto::ArchRegistrationState::Unregistered || arch.registration==pulse::dialog_auto::ArchRegistrationState::DisabledMatching);
    }
    json.set("architectures",std::move(arches));json.set("requested_state",both);
    std::cout<<pulse::json::Serialize(json)<<'\n';
    return result->error||result->auto_rollback_error||!both?1:0;
}
