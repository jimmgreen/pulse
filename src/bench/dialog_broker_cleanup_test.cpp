#include "../dialog_broker/deferred_cleanup.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
namespace broker = pulse::dialog_broker;

int g_checks = 0;
int g_failures = 0;

void Check(bool ok, const char* name) {
    ++g_checks;
    if (!ok) ++g_failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    std::fflush(stdout);
}

bool WriteText(const fs::path& path, const std::string& content) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
    return out.good();
}

} // namespace

// Usage:
//   pulse_dialog_broker_cleanup_test.exe [<setup_exe> <legacy_dual_pkg> <app_only_pkg>]
// Without args: runs Common Files bridge cleanup + SYSTEM trust-gate tests.
// With args: also runs full end-to-end uninstall.exe /CLEANUPVERSIONS retirement test.
int wmain(int argc, wchar_t** argv) {
    const fs::path root = fs::temp_directory_path() /
        (L"pulse_broker_cleanup_test_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()));
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // 1. Common Files bridge root cleanup: unlocked .old/.new & obsolete v\<16-hex> removed,
    //    active x64/x86 Core dirs, locked files/dirs, and unknown user dirs preserved.
    {
        const fs::path bridge = root / L"common_bridge";
        const fs::path v_root = bridge / L"v";
        const fs::path cur64 = v_root / L"5436db48fc8d0d85";
        const fs::path cur86 = v_root / L"25a963fa88401885";
        const fs::path old_free = v_root / L"06338f27faf718b3";
        const fs::path old_locked = v_root / L"0daee38aa87e626e";
        const fs::path unknown_dir = v_root / L"user_custom_dir";

        WriteText(cur64 / L"PulseDialogCore64.dll", "core64");
        WriteText(cur86 / L"PulseDialogCore32.dll", "core32");
        WriteText(old_free / L"PulseDialogCore64.dll", "old-free");
        WriteText(old_locked / L"PulseDialogCore64.dll", "old-locked");
        WriteText(unknown_dir / L"notes.txt", "keep-me");
        WriteText(bridge / L"PulseDialogStub64.dll", "stub64");
        WriteText(bridge / L"PulseDialogStub64.dll.old.111", "stale-free");
        WriteText(bridge / L"PulseDialogStub64.dll.old.222", "stale-locked");
        WriteText(bridge / L"PulseDialogStub64.dll.new.333", "stale-new");

        paths::ActiveBridgePointer ptr{};
        ptr.schema = 1;
        ptr.generation = 8;
        ptr.core_dir_x64 = cur64.wstring();
        ptr.core_dir_x86 = cur86.wstring();

        // Hold non-share-delete locks on stale-locked and old_locked\PulseDialogCore64.dll.
        HANDLE h_stale = CreateFileW((bridge / L"PulseDialogStub64.dll.old.222").c_str(),
                                     GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        HANDLE h_core = CreateFileW((old_locked / L"PulseDialogCore64.dll").c_str(),
                                    GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(h_stale != INVALID_HANDLE_VALUE && h_core != INVALID_HANDLE_VALUE,
              "acquire real non-share-delete locks on stale artifact and old Core DLL");

        Check(broker::HasPendingResidualCleanup(bridge.wstring(), ptr),
              "HasPendingResidualCleanup detects stale .old/.new and obsolete v\\ Core dirs");

        // Security gate: default verify_protected=true must refuse unprotected temp dir.
        broker::ResidualCleanupOptions protected_opts;
        protected_opts.verify_protected = true;
        protected_opts.min_core_age_ms = 0;
        const auto blocked_by_trust = broker::RunResidualCleanupPass(bridge.wstring(), ptr, protected_opts);
        Check(!blocked_by_trust.ran_bridge_prune && fs::exists(old_free) &&
              fs::exists(bridge / L"PulseDialogStub64.dll.old.111"),
              "verify_protected=true refuses to mutate unprotected directory");

        // First pass with locks held: removes unlocked stale files + unlocked old Core, retains locked & active & unknown.
        broker::ResidualCleanupOptions opts;
        opts.verify_protected = false;
        opts.min_core_age_ms = 0;
        const auto pass1 = broker::RunResidualCleanupPass(bridge.wstring(), ptr, opts);
        Check(pass1.ran_bridge_prune && pass1.stale_files_removed == 2 && pass1.core_dirs_removed == 1 &&
              pass1.pending_after &&
              !fs::exists(bridge / L"PulseDialogStub64.dll.old.111") &&
              !fs::exists(bridge / L"PulseDialogStub64.dll.new.333") &&
              !fs::exists(old_free) &&
              fs::exists(bridge / L"PulseDialogStub64.dll.old.222") &&
              fs::exists(old_locked / L"PulseDialogCore64.dll") &&
              fs::exists(cur64 / L"PulseDialogCore64.dll") &&
              fs::exists(cur86 / L"PulseDialogCore32.dll") &&
              fs::exists(unknown_dir / L"notes.txt"),
              "pass 1 prunes unlocked .old/.new and obsolete Core while preserving locked, active, and unknown dirs");

        // Release locks and run second pass: clears final locked items and reaches zero pending work!
        CloseHandle(h_stale);
        CloseHandle(h_core);
        const auto pass2 = broker::RunResidualCleanupPass(bridge.wstring(), ptr, opts);
        Check(pass2.stale_files_removed == 1 && pass2.core_dirs_removed == 1 &&
              !pass2.pending_after &&
              !fs::exists(bridge / L"PulseDialogStub64.dll.old.222") &&
              !fs::exists(old_locked) &&
              fs::exists(cur64 / L"PulseDialogCore64.dll") &&
              fs::exists(cur86 / L"PulseDialogCore32.dll") &&
              fs::exists(unknown_dir / L"notes.txt"),
              "pass 2 cleans remaining unlocked artifacts and reaches zero-polling clean state");
    }

    // 2. End-to-end install root cleanup via real uninstall.exe /CLEANUPVERSIONS /PULSETEST.
    if (argc == 4) {
        const fs::path setup_exe = argv[1];
        const fs::path legacy_pkg = argv[2];
        const fs::path app_only_pkg = argv[3];
        const fs::path app = root / L"fake_install";
        const fs::path bridge = root / L"common_bridge_e2e";
        fs::create_directories(app, ec);
        fs::create_directories(bridge / L"v" / L"5436db48fc8d0d85", ec);

        // Copy clean AppRuntime-only install layout + real pulse_setup.exe as uninstall.exe.
        fs::copy(app_only_pkg, app, fs::copy_options::recursive, ec);
        fs::copy_file(setup_exe, app / L"uninstall.exe", fs::copy_options::overwrite_existing, ec);

        // Seed legacy DialogBridge\<hash>\ bundles from legacy_pkg (WITHOUT dialog-current.json!).
        fs::copy(legacy_pkg / L"DialogBridge", app / L"DialogBridge", fs::copy_options::recursive, ec);
        fs::path locked_dll;
        size_t seeded_dirs = 0;
        for (const auto& entry : fs::directory_iterator(app / L"DialogBridge")) {
            if (!entry.is_directory()) continue;
            ++seeded_dirs;
            const auto candidate = entry.path() / L"PulseDialogAuto64.dll";
            if (locked_dll.empty() && fs::exists(candidate)) locked_dll = candidate;
        }
        Check(seeded_dirs >= 2 && !locked_dll.empty() && fs::exists(app / L"uninstall.exe"),
              "seeded migrated AppRuntime install with retired legacy DialogBridge bundles and real uninstall.exe");

        paths::ActiveBridgePointer ptr{};
        ptr.schema = 1;
        ptr.generation = 9;
        ptr.install_root = app.wstring();
        ptr.core_dir_x64 = (bridge / L"v" / L"5436db48fc8d0d85").wstring();

        // Security gate: verify_protected=true must refuse to launch unprotected uninstall.exe as SYSTEM.
        broker::ResidualCleanupOptions sec_opts;
        sec_opts.verify_protected = true;
        const auto sec_res = broker::RunResidualCleanupPass(bridge.wstring(), ptr, sec_opts);
        Check(!sec_res.ran_install_cleanup && !sec_res.install_cleanup_launched,
              "SYSTEM security gate refuses to execute uninstall.exe from unprotected directory");

        // Hold a real lock on one legacy bundle's PulseDialogAuto64.dll.
        HANDLE h_dll = CreateFileW(locked_dll.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(h_dll != INVALID_HANDLE_VALUE, "lock one retired legacy DialogBridge bundle");

        broker::ResidualCleanupOptions e2e_opts;
        e2e_opts.verify_protected = false;
        e2e_opts.pass_test_flag = true;
        e2e_opts.min_core_age_ms = 0;

        // Pass 1: uninstall.exe /CLEANUPVERSIONS deletes unlocked bundle(s) and preserves locked one safely.
        const auto r1 = broker::RunResidualCleanupPass(bridge.wstring(), ptr, e2e_opts);
        size_t remaining_after_r1 = 0;
        if (fs::exists(app / L"DialogBridge")) {
            for (const auto& entry : fs::directory_iterator(app / L"DialogBridge")) {
                if (entry.is_directory()) ++remaining_after_r1;
            }
        }
        Check(r1.ran_install_cleanup && r1.install_cleanup_launched &&
              r1.pending_after && remaining_after_r1 == 1 && fs::exists(locked_dll),
              "Broker pass 1 invokes real uninstall.exe /CLEANUPVERSIONS, removes unlocked legacy bundle and keeps locked one");

        // Pass 2 after releasing lock: removes final legacy bundle AND deletes empty DialogBridge\ directory!
        CloseHandle(h_dll);
        const auto r2 = broker::RunResidualCleanupPass(bridge.wstring(), ptr, e2e_opts);
        Check(r2.ran_install_cleanup && r2.install_cleanup_launched &&
              r2.install_cleanup_exit_code == 0 && !r2.pending_after &&
              !fs::exists(app / L"DialogBridge") && fs::exists(app / L"app-current.json"),
              "Broker pass 2 removes last legacy bundle, deletes empty DialogBridge\\, and stops polling");
    }

    fs::remove_all(root, ec);
    std::printf("BROKER_CLEANUP checks=%d failures=%d\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
