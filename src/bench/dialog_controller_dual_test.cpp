#include "../app/dialog_management_controller.h"
#include "../common/dialog_auto_registration.h"
#include "../common/json_value.h"
#include <filesystem>
#include <algorithm>
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
            pulse::bench::registry_views::enabled = false;
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
    pulse::bench::registry_views::enabled = true;

    // 1. Verify full unpacked 1.0.61 dual-arch bundle is recognized (x64 + x86 components present).
    {
        const auto dual_state = state_dir / L"state_dual_refresh";
        const auto res = RunController(L"refresh", dual_install, dual_state.wstring());
        const bool ok = res.has_value() && !res->error && res->bundle_verified &&
            res->auto_arch[0].component == ArchComponentState::Present &&
            res->auto_arch[1].component == ArchComponentState::Present;
        check(ok, "unpacked 1.0.61 bundle resolves verified x64 and x86 dialog bridges", res ? res->bundle_error : ERROR_TIMEOUT);
    }

    // These legacy upgrade scenarios intentionally exercise an x64-only bundle.
    // Native x86 inspection still runs against its independent real-key subtree.
    fs::copy(fs::path(dual_install), x64_install, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    fs::remove(x64_install / L"dialog-x86-current.json", ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        std::cout << "[FAIL] cannot prepare x64-only view: " << ec.message() << "\n";
        return 1;
    }

    auto reset_hive = [] {
        RegDeleteTreeW(HKEY_CURRENT_USER, L"PulseTestViews");
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
    auto clsids_absent = [](REGSAM view) {
        bool absent = true;
        for (const auto* path : {
            L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
            L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            HKEY key = nullptr;
            const auto result = RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ | view, &key);
            if (key) RegCloseKey(key);
            absent = absent && result == ERROR_FILE_NOT_FOUND;
        }
        return absent;
    };
    {
        const auto* clsid = L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}";
        check(set_sz(clsid, KEY_WOW64_64KEY, L"ViewProbe", L"x64") &&
            !clsids_absent(KEY_WOW64_64KEY) && clsids_absent(KEY_WOW64_32KEY),
            "real isolated x64 CLSID key is invisible to x86 view");
        reset_hive();
        check(set_sz(clsid, KEY_WOW64_32KEY, L"ViewProbe", L"x86") &&
            !clsids_absent(KEY_WOW64_32KEY) && clsids_absent(KEY_WOW64_64KEY),
            "real isolated x86 CLSID key is invisible to x64 view");
        reset_hive();
    }

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
            disabled->auto_arch[0].registration == ArchRegistrationState::Unregistered &&
            clsids_absent(KEY_WOW64_64KEY) && clsids_absent(KEY_WOW64_32KEY);
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

    // Cross-install retired journals are not the owned-digest upgrade above.
    const auto moved_old = state_dir / L"旧 安装 moved" / L"DialogBridge" /
        std::wstring(64, L'b') / L"PulseDialogAuto64.dll";
    const auto seed_retired = [&] {
        reset_hive();
        return set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", moved_old.wstring()) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 0) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 0) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Host", 7);
    };
    {
        check(seed_retired(), "seed disabled deleted cross-install digest with Unicode and spaces");
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"retired_success").wstring());
        check(res && !res->error && !res->auto_rollback_error && res->auto_enabled &&
            _wcsicmp(res->auto_dll.c_str(), moved_old.c_str()) != 0,
            "EnableAuto migrates disabled deleted cross-install journal", res ? res->error : ERROR_TIMEOUT);
        const auto again = RunController(L"enable", x64_install.wstring(), (state_dir / L"retired_repeat").wstring());
        check(again && !again->error && again->auto_enabled,
            "repeated EnableAuto after migration is idempotent", again ? again->error : ERROR_TIMEOUT);
    }
    for (unsigned variant = 0; variant < 4; ++variant) {
        check(seed_retired(), "seed cross-install receipt for rejected normalization case");
        HKEY key = nullptr;
        const DWORD opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto", 0,
            KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &key);
        check(opened == ERROR_SUCCESS, "open isolated receipt for flag boundary", opened);
        if (opened) continue;
        const wchar_t* flag = variant == 3 ? L"Enabled" : L"Stage";
        if (variant == 0) RegDeleteValueW(key, flag);
        else if (variant == 1) set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, flag, L"0");
        else set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, flag, 1);
        DWORD before_type = 0, before_bytes = 16; BYTE before[16]{};
        const DWORD before_result = RegQueryValueExW(key, flag, nullptr, &before_type, before, &before_bytes);
        const auto res = RunController(L"enable", x64_install.wstring(),
            (state_dir / (L"retired_reject_" + std::to_wstring(variant))).wstring());
        const DWORD expected = variant == 0 ? ERROR_FILE_NOT_FOUND : variant == 1 ? ERROR_INVALID_DATA : ERROR_ALREADY_EXISTS;
        check(res && res->error == expected && !res->auto_enabled && !res->auto_rollback_error &&
            res->auto_dll == moved_old.wstring(), "retired malformed/nonzero flags are not normalized", res ? res->error : ERROR_TIMEOUT);
        DWORD after_type = 0, after_bytes = 16; BYTE after[16]{};
        const DWORD after_result = RegQueryValueExW(key, flag, nullptr, &after_type, after, &after_bytes);
        check(before_result == after_result && (before_result != ERROR_SUCCESS ||
            (before_type == after_type && before_bytes == after_bytes &&
                std::equal(before, before + before_bytes, after))), "rejected retired receipt remains byte-identical");
        RegCloseKey(key);
    }
    {
        check(seed_retired(), "seed existing cross-install receipt");
        fs::create_directories(moved_old.parent_path());
        { std::ofstream file(moved_old, std::ios::binary); file << "still installed"; }
        const auto res = RunController(L"enable", x64_install.wstring(), (state_dir / L"retired_live").wstring());
        check(res && res->error == ERROR_ALREADY_EXISTS && !res->auto_enabled && res->auto_dll == moved_old.wstring(),
            "disabled other installation with existing component remains untouched", res ? res->error : ERROR_TIMEOUT);
    }
    // Second support report: x86 journal lost DllPath, while both owned CLSIDs
    // still point to a deleted installation. Exercise the full dual bundle.
    const auto missing_journal_old = state_dir / L"旧 安装 journal" / L"DialogBridge" /
        std::wstring(64, L'c') / L"PulseDialogAuto32.dll";
    const auto seed_missing_journal = [&] {
        reset_hive();
        bool ok = set_dw(L"Software\\Pulse\\DialogAuto32", KEY_WOW64_32KEY, L"Enabled", 1) &&
            set_dw(L"Software\\Pulse\\DialogAuto32", KEY_WOW64_32KEY, L"Stage", 1) &&
            set_dw(L"Software\\Pulse\\DialogAuto32", KEY_WOW64_32KEY, L"Host", 7);
        for (const auto* clsid : {
            L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
            L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
            const auto server = std::wstring(clsid) + L"\\InprocServer32";
            ok = ok && set_sz(clsid, KEY_WOW64_32KEY, L"PulseDialogAutoOwner", L"Pulse.DialogAuto.v1") &&
                set_sz(server.c_str(), KEY_WOW64_32KEY, nullptr, missing_journal_old.wstring()) &&
                set_sz(server.c_str(), KEY_WOW64_32KEY, L"ThreadingModel", L"Apartment");
        }
        return ok;
    };
    for (unsigned variant = 0; variant < 3; ++variant) {
        check(seed_missing_journal(), "seed second user missing x86 journal with retained COM registration");
        HKEY key = nullptr;
        const DWORD opened = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto32", 0,
            KEY_READ | KEY_WRITE | KEY_WOW64_32KEY, &key);
        check(opened == ERROR_SUCCESS, "open second user isolated journal", opened);
        if (opened) continue;
        if (variant == 1) set_dw(L"Software\\Pulse\\DialogAuto32", KEY_WOW64_32KEY, L"Stage", 0);
        if (variant == 2) RegDeleteValueW(key, L"Stage");
        const auto old64 = state_dir / L"retired64" / L"DialogBridge" / std::wstring(64, L'd') / L"PulseDialogAuto64.dll";
        check(set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", old64.wstring()) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 0) &&
            set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 0),
            "combine first user x64 retired state with second user x86 missing journal");
        const auto result = RunController(L"enable", dual_install,
            (state_dir / (L"missing_journal_" + std::to_wstring(variant))).wstring());
        check(result && !result->error && !result->auto_rollback_error &&
            result->auto_arch[0].registration == ArchRegistrationState::Registered &&
            result->auto_arch[1].registration == ArchRegistrationState::Registered,
            "EnableAuto repairs both user states and enables both architectures", result ? result->error : ERROR_TIMEOUT);
        DWORD host = 0, bytes = sizeof(host), type = 0;
        check(RegQueryValueExW(key, L"Host", nullptr, &type, reinterpret_cast<BYTE*>(&host), &bytes) == ERROR_SUCCESS &&
            type == REG_DWORD && host == 7, "journal repair and migration preserve user preferences");
        RegCloseKey(key);
        const auto repeated = RunController(L"enable", dual_install, (state_dir / L"missing_repeat").wstring());
        check(repeated && !repeated->error && repeated->auto_enabled, "repeated dual enable after journal repair is idempotent");
        const auto disabled = RunController(L"disable", dual_install, (state_dir / L"missing_disable").wstring());
        check(disabled && !disabled->error && !disabled->auto_enabled &&
            clsids_absent(KEY_WOW64_64KEY) && clsids_absent(KEY_WOW64_32KEY),
            "both repaired architectures disable and remove owned COM roots");
    }
    check(seed_missing_journal(), "seed missing journal alongside another live installation");
    const auto other_live = state_dir / L"other_live" / L"PulseDialogAuto64.dll";
    fs::create_directories(other_live.parent_path());
    { std::ofstream file(other_live, std::ios::binary); file << "still installed"; }
    bool other_seeded = set_sz(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"DllPath", other_live.wstring()) &&
        set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Enabled", 1) &&
        set_dw(L"Software\\Pulse\\DialogAuto", KEY_WOW64_64KEY, L"Stage", 1);
    for (const auto* clsid : {
        L"Software\\Classes\\CLSID\\{DC1C5A9C-E88A-4DDE-A5A1-60F82A20AEF7}",
        L"Software\\Classes\\CLSID\\{C0B4E2F3-BA21-4773-8DBA-335EC946EB8B}"}) {
        const auto server = std::wstring(clsid) + L"\\InprocServer32";
        other_seeded = other_seeded && set_sz(clsid, KEY_WOW64_64KEY, L"PulseDialogAutoOwner", L"Pulse.DialogAuto.v1") &&
            set_sz(server.c_str(), KEY_WOW64_64KEY, nullptr, other_live.wstring()) &&
            set_sz(server.c_str(), KEY_WOW64_64KEY, L"ThreadingModel", L"Apartment");
    }
    check(other_seeded, "seed consistent x64 registration belonging to live other installation");
    const auto foreign_result = RunController(L"enable", dual_install, (state_dir / L"missing_foreign").wstring());
    HKEY untouched_key = nullptr;
    const DWORD untouched_open = RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Pulse\\DialogAuto32", 0,
        KEY_READ | KEY_WOW64_32KEY, &untouched_key);
    DWORD value_bytes = 0;
    const DWORD untouched_value = untouched_open ? untouched_open :
        RegQueryValueExW(untouched_key, L"DllPath", nullptr, nullptr, nullptr, &value_bytes);
    if (untouched_key) RegCloseKey(untouched_key);
    check(foreign_result && foreign_result->error == ERROR_ALREADY_EXISTS && untouched_value == ERROR_FILE_NOT_FOUND &&
        pulse::dialog_auto::Inspect(pulse::dialog_auto::Architecture::X64).dll_path == other_live.wstring(),
        "live other architecture is rejected before writing the missing journal");
    check(seed_missing_journal(), "seed missing journal for live old component rejection");
    fs::create_directories(missing_journal_old.parent_path());
    { std::ofstream file(missing_journal_old, std::ios::binary); file << "still installed"; }
    const auto live_result = RunController(L"enable", dual_install, (state_dir / L"missing_live").wstring());
    check(live_result && live_result->error == ERROR_ALREADY_EXISTS && !live_result->auto_enabled,
        "missing journal does not authorize taking over a live old installation", live_result ? live_result->error : ERROR_TIMEOUT);
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
        return RunUpgradeRebindTest(std::filesystem::path(argv[2]).make_preferred().wstring(),
            std::filesystem::path(argv[3]).make_preferred().wstring());
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
