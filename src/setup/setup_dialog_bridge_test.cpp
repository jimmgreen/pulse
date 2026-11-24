#include "setup_dialog_bridge.h"
#include "../common/dialog_bridge_paths.h"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
namespace paths = pulse::dialog_bridge_paths;
namespace setup = pulse::setup;

namespace {
struct TempDir {
    fs::path dir;
    ~TempDir() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};

void WriteBytes(const fs::path& p, const std::string& data) {
    fs::create_directories(p.parent_path());
    std::ofstream f{p, std::ios::binary | std::ios::trunc};
    f.write(data.data(), static_cast<std::streamsize>(data.size()));
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    TempDir fixture;
    fixture.dir = fs::path(temp) / (L"pulse-setup-bridge-test-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));

    const fs::path fake_app = fixture.dir / L"D_Pulse";
    const fs::path stage_v1 = fixture.dir / L"stage_v1";
    const fs::path stage_v2 = fixture.dir / L"stage_v2";
    const fs::path stage_v3 = fixture.dir / L"stage_v3";
    const fs::path bridge_root = fixture.dir / L"CommonDialogBridge";

    // Optional real Stub64 DLL passed on argv[1] to test genuine LoadLibrary-locked upgrade!
    const bool has_real_stub = argc >= 2 && GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES;

    if (has_real_stub) {
        fs::create_directories(stage_v1);
        fs::copy_file(argv[1], stage_v1 / paths::kStubDllName64, fs::copy_options::overwrite_existing);
    } else {
        WriteBytes(stage_v1 / paths::kStubDllName64, "STUB64_V1");
    }
    WriteBytes(stage_v1 / paths::kStubDllName32, "STUB32_V1");
    WriteBytes(stage_v1 / paths::kCoreDllName64, "CORE64_PAYLOAD_V1");
    WriteBytes(stage_v1 / paths::kCoreDllName32, "CORE32_PAYLOAD_V1");
    WriteBytes(stage_v1 / paths::kBrokerExeName, "BROKER_V1");
    WriteBytes(stage_v1 / paths::kInject32ExeName, "INJECT32_V1");
    WriteBytes(stage_v1 / paths::kOverlayExeName, "OVERLAY_V1");

    // 1. Initial provision (simulating non-C: D:\Pulse install into isolated Common Files bridge root).
    setup::BridgeStageRequest req1;
    req1.app_dir = fake_app.wstring();
    req1.stage_source_dir = stage_v1.wstring();
    req1.host_exe = (fake_app / L"pulse.exe").wstring();
    req1.bridge_root_override = bridge_root.wstring();
    req1.harden_dacl = false;       // non-elevated test
    req1.configure_service = false; // never touch live SCM in tests
    req1.register_hklm_com = false;

    const auto r1 = setup::ProvisionDialogBridge(req1);
    check(r1.error == ERROR_SUCCESS && r1.generation == 1 &&
              r1.stub64_staged && r1.stub32_staged &&
              r1.core64_staged && r1.core32_staged &&
              r1.broker_staged && r1.inject32_staged && r1.overlay_staged,
          "initial ProvisionDialogBridge stages all 7 x64/x86 components at generation 1");

    paths::ActiveBridgePointer ptr1{};
    check(paths::ReadActiveBridgePointer(bridge_root.wstring(), ptr1) == ERROR_SUCCESS &&
              ptr1.generation == 1 &&
              ptr1.core_dir_x64 == r1.core_dir_x64 &&
              ptr1.core_dir_x86 == r1.core_dir_x86 &&
              ptr1.host_exe == req1.host_exe &&
              ptr1.core_digest_x64.size() == 64 &&
              ptr1.core_digest_x86.size() == 64,
          "active-bridge.json persists generation 1, SHA-256 digests, and non-C: host_exe");

    // 2. Upgrade to v2 while holding an exclusive/mapped lock on Stub64!
    WriteBytes(stage_v2 / paths::kStubDllName64, "STUB64_V2_REPLACED");
    WriteBytes(stage_v2 / paths::kCoreDllName64, "CORE64_PAYLOAD_V2");
    WriteBytes(stage_v2 / paths::kCoreDllName32, "CORE32_PAYLOAD_V2");

    // Lock Stub64 (mapped via LoadLibrary if real PE, or FILE_SHARE_READ | FILE_SHARE_DELETE handle simulating mapped image).
    HMODULE loaded_stub = has_real_stub ? LoadLibraryExW((bridge_root / paths::kStubDllName64).c_str(), nullptr,
                                                         DONT_RESOLVE_DLL_REFERENCES) : nullptr;
    HANDLE lock_handle = INVALID_HANDLE_VALUE;
    if (!loaded_stub) {
        lock_handle = CreateFileW((bridge_root / paths::kStubDllName64).c_str(), GENERIC_READ,
                                  FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    }

    setup::BridgeStageRequest req2 = req1;
    req2.stage_source_dir = stage_v2.wstring();
    const auto r2 = setup::ProvisionDialogBridge(req2);
    check(r2.error == ERROR_SUCCESS && r2.generation == 2 &&
              r2.stub64_staged && r2.core64_staged &&
              r2.core_dir_x64 != r1.core_dir_x64,
          "upgrade to v2 succeeds even while Stub64 is locked and advances generation to 2");

    if (loaded_stub) FreeLibrary(loaded_stub);
    if (lock_handle != INVALID_HANDLE_VALUE) CloseHandle(lock_handle);

    check(fs::exists(r1.core_dir_x64) && fs::exists(r2.core_dir_x64),
          "both current (v2) and previous (v1) Core directories retained after v2 upgrade");

    // 3. Create one locked old dir (alphabetically first) + one unlocked old dir + stale .old. file.
    const fs::path locked_old_dir = bridge_root / L"v" / L"0000000000000001";
    const fs::path unlocked_old_dir = bridge_root / L"v" / L"fffffffffffffffe";
    WriteBytes(locked_old_dir / paths::kCoreDllName64, "LOCKED_OLD_CORE");
    WriteBytes(unlocked_old_dir / paths::kCoreDllName64, "UNLOCKED_OLD_CORE");
    const fs::path stale_old_file = bridge_root / L"PulseDialogStub64.dll.old.123456";
    WriteBytes(stale_old_file, "STALE_OLD_STUB");
    HANDLE locked_old_handle = CreateFileW((locked_old_dir / paths::kCoreDllName64).c_str(), GENERIC_READ,
                                           FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);

    // Upgrade to v3 -> v1 obsolete Core directory and unlocked_old_dir + stale .old. must be pruned!
    WriteBytes(stage_v3 / paths::kCoreDllName64, "CORE64_PAYLOAD_V3");
    setup::BridgeStageRequest req3 = req1;
    req3.stage_source_dir = stage_v3.wstring();
    const auto r3 = setup::ProvisionDialogBridge(req3);
    if (locked_old_handle != INVALID_HANDLE_VALUE) CloseHandle(locked_old_handle);
    check(r3.error == ERROR_SUCCESS && r3.generation == 3 && r3.core64_staged,
          "upgrade to v3 advances generation to 3");
    check(!fs::exists(r1.core_dir_x64) && fs::exists(r2.core_dir_x64) && fs::exists(r3.core_dir_x64),
          "obsolete v1 Core directory pruned while v2 (previous) and v3 (current) retained");
    check(fs::exists(locked_old_dir) && !fs::exists(unlocked_old_dir),
          "locked old Core directory does not abort pruning subsequent unlocked Core directories");
    check(!fs::exists(stale_old_file),
          "unlocked stale .old. artifact is pruned from bridge root");

    paths::ActiveBridgePointer ptr3{};
    check(paths::ReadActiveBridgePointer(bridge_root.wstring(), ptr3) == ERROR_SUCCESS &&
              ptr3.generation == 3 &&
              ptr3.core_dir_x64 == r3.core_dir_x64 &&
              ptr3.core_dir_x86 == r2.core_dir_x86,
          "partial x64-only update preserves previous x86 Core pointer cleanly");

    return failures ? 1 : 0;
}
