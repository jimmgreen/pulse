#include "../common/dialog_admin_trust.h"
#include "../common/dialog_bridge_paths.h"
#include <sddl.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace paths = pulse::dialog_bridge_paths;
namespace admin = pulse::dialog_admin;
namespace fs = std::filesystem;

struct TempDir {
    fs::path dir;
    ~TempDir() {
        std::error_code ec;
        if (!dir.empty()) fs::remove_all(dir, ec);
    }
};
} // namespace

int wmain() {
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };

    // 1. Common Files bridge root path shape and Program Files classifier.
    const auto common = paths::CommonDialogBridgeRoot();
    check(!common.empty() && common.ends_with(L"\\Pulse\\DialogBridge"),
          "CommonDialogBridgeRoot resolves under Common Files\\Pulse\\DialogBridge");
    check(paths::IsUnderProgramFiles(common), "CommonDialogBridgeRoot recognized as under Program Files");
    check(!paths::IsUnderProgramFiles(L"D:\\Pulse"), "D:\\Pulse rejected by IsUnderProgramFiles");
    check(!paths::IsUnderProgramFiles(L"E:\\A_software\\Pulse"), "E:\\A_software\\Pulse rejected by IsUnderProgramFiles");
    check(paths::CommonDialogBridgeRoot(L"C:\\Override\\Bridge\\") == L"C:\\Override\\Bridge",
          "override root trims trailing backslash");

    // 2. Protected bridge SDDL includes ALL APPLICATION PACKAGES (AC / S-1-15-2-1).
    const std::wstring sddl_ac = paths::ProtectedBridgeSddl(true);
    const std::wstring sddl_no_ac = paths::ProtectedBridgeSddl(false);
    check(sddl_ac.find(L"(A;OICI;GRGX;;;AC)") != std::wstring::npos &&
              sddl_ac.find(L"(A;OICI;GA;;;SY)") != std::wstring::npos &&
              sddl_ac.find(L"(A;OICI;GA;;;BA)") != std::wstring::npos,
          "ProtectedBridgeSddl(true) grants SYSTEM/Admins GA and AppContainer GRGX");
    check(sddl_no_ac.find(L";;;AC)") == std::wstring::npos,
          "ProtectedBridgeSddl(false) omits AppContainer ACE");

    PSECURITY_DESCRIPTOR sd = nullptr;
    const bool parsed_sddl = ConvertStringSecurityDescriptorToSecurityDescriptorW(
        sddl_ac.c_str(), SDDL_REVISION_1, &sd, nullptr) != FALSE;
    check(parsed_sddl && sd != nullptr, "ProtectedBridgeSddl parses cleanly via Windows SDDL API");
    if (sd) LocalFree(sd);

    // 3. Active bridge pointer atomic round-trip in isolated temp directory.
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    TempDir fixture;
    fixture.dir = fs::path(temp) / (L"pulse-bridge-paths-test-" + std::to_wstring(GetCurrentProcessId()) +
                                    L"-" + std::to_wstring(GetTickCount64()));
    const fs::path common_override = fixture.dir / L"CommonBridge";
    const fs::path legacy_install = fixture.dir / L"D_Pulse";
    std::error_code ec;
    fs::create_directories(common_override, ec);
    fs::create_directories(legacy_install, ec);

    paths::ActiveBridgePointer written;
    written.schema = 1;
    written.core_dir_x64 = (common_override / L"v" / L"abc123").wstring();
    written.core_dir_x86 = (common_override / L"v" / L"def456").wstring();
    written.core_digest_x64 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    written.core_digest_x86 = "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    written.host_exe = L"D:\\Pulse\\pulse.exe";
    written.install_root = L"D:\\Pulse";
    written.generation = 42;

    const DWORD write_err = paths::WriteActiveBridgePointer(common_override.wstring(), written);
    check(write_err == ERROR_SUCCESS, "WriteActiveBridgePointer succeeds atomically");

    paths::ActiveBridgePointer loaded;
    const DWORD read_err = paths::ReadActiveBridgePointer(common_override.wstring(), loaded);
    check(read_err == ERROR_SUCCESS &&
              loaded.schema == 1 &&
              loaded.core_dir_x64 == written.core_dir_x64 &&
              loaded.core_dir_x86 == written.core_dir_x86 &&
              loaded.core_digest_x64 == written.core_digest_x64 &&
              loaded.core_digest_x86 == written.core_digest_x86 &&
              loaded.host_exe == written.host_exe &&
              loaded.install_root == written.install_root &&
              loaded.generation == 42,
          "ReadActiveBridgePointer round-trips all fields faithfully");
    check(loaded.explorer_bridge_sha256_x64.empty() && loaded.explorer_bridge_sha256_x86.empty(),
          "legacy pointer without optional Explorer helper digests remains accepted");
    written.explorer_bridge_sha256_x64 = std::string(64, 'c');
    written.explorer_bridge_sha256_x86 = std::string(64, 'd');
    written.disabled = true;
    check(paths::WriteActiveBridgePointer(common_override.wstring(), written) == ERROR_SUCCESS &&
          paths::ReadActiveBridgePointer(common_override.wstring(), loaded) == ERROR_SUCCESS &&
          loaded.explorer_bridge_sha256_x64 == written.explorer_bridge_sha256_x64 &&
          loaded.explorer_bridge_sha256_x86 == written.explorer_bridge_sha256_x86 && loaded.disabled,
          "optional architecture-specific Explorer helper hashes and disabled state round-trip");
    written.explorer_bridge_sha256_x64 = "invalid";
    check(paths::WriteActiveBridgePointer(common_override.wstring(), written) == ERROR_INVALID_DATA &&
          paths::ReadActiveBridgePointer(common_override.wstring(), loaded) == ERROR_SUCCESS &&
          loaded.explorer_bridge_sha256_x64 == std::string(64, 'c'),
          "invalid helper digest never replaces the valid pointer");
    written.explorer_bridge_sha256_x64 = std::string(64, 'c');

    // 4. ResolveActiveBridgeRoot chooses provisioned override even for non-C: install_root.
    const auto resolved = paths::ResolveActiveBridgeRoot(legacy_install.wstring(), common_override.wstring());
    check(resolved == common_override.wstring(),
          "ResolveActiveBridgeRoot prefers provisioned Common Files bridge over non-C: install");

    // Without active-bridge.json, falls back to install_root for unmigrated layouts.
    const fs::path empty_common = fixture.dir / L"EmptyCommon";
    fs::create_directories(empty_common, ec);
    const auto fallback = paths::ResolveActiveBridgeRoot(legacy_install.wstring(), empty_common.wstring());
    check(fallback == legacy_install.wstring(),
          "ResolveActiveBridgeRoot falls back to install_root when Common Files bridge is absent");

    // 5. VerifyProtectedBridgeLocation rejects unknown machine and user-owned directories.
    check(admin::VerifyProtectedBridgeLocation(common_override.wstring(), written.core_dir_x64, 0) ==
              ERROR_EXE_MACHINE_TYPE_MISMATCH,
          "VerifyProtectedBridgeLocation rejects invalid PE machine");
    check(admin::VerifyProtectedBridgeLocation(common_override.wstring(), written.core_dir_x64,
                                               IMAGE_FILE_MACHINE_AMD64) != ERROR_SUCCESS,
          "VerifyProtectedBridgeLocation rejects user-writable temp directory");

    // 6. Relative path rejected by ReadActiveBridgePointer / WriteActiveBridgePointer.
    check(paths::ReadActiveBridgePointer(L"relative\\bridge", loaded) == ERROR_INVALID_NAME,
          "ReadActiveBridgePointer rejects relative directory");
    check(paths::WriteActiveBridgePointer(L"relative\\bridge", written) == ERROR_INVALID_NAME,
          "WriteActiveBridgePointer rejects relative directory");
    {
        std::ofstream file(common_override / paths::kActivePointerFile, std::ios::trunc);
        file << "{\"schema\":1,\"explorer_bridge_sha256_x64\":42}";
    }
    check(paths::ReadActiveBridgePointer(common_override.wstring(), loaded) == ERROR_INVALID_DATA,
          "present non-string helper digest is rejected instead of treated as legacy absent");

    return failures ? 1 : 0;
}
