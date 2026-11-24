#include "../common/dialog_auto_repair.h"
#include "../common/dialog_auto_ownership.h"
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
using namespace pulse::dialog_auto;
unsigned checks = 0, failures = 0;
void Check(bool okay, const char* name) {
    ++checks; if (!okay) ++failures;
    std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", name); std::fflush(stdout);
}
size_t Index(Architecture a) { return a == Architecture::X64 ? 0 : 1; }
struct Fake {
    std::array<Status, 2> states{};
    std::vector<std::wstring> calls;
    DWORD fail_enable = ERROR_SUCCESS;
    RegistrationApi Api() {
        RegistrationApi api;
        api.inspect = [this](Architecture a) { return states[Index(a)]; };
        api.enable = [this](Architecture a, const std::wstring& p) {
            calls.push_back(L"enable " + p); auto& s = states[Index(a)]; s.enabled = true; s.dll_path = p; return DWORD{0};
        };
        api.enable_if_unclaimed = [this](Architecture a, const std::wstring& p) {
            calls.push_back(L"unclaimed " + p); auto& s = states[Index(a)];
            if (s.enabled || (!s.dll_path.empty() && _wcsicmp(s.dll_path.c_str(), p.c_str()) != 0)) return DWORD{ERROR_ALREADY_EXISTS};
            s.enabled = true; s.dll_path = p; return DWORD{0};
        };
        api.disable_for = [this](Architecture a, const std::wstring& p) {
            calls.push_back(L"disable " + p); auto& s = states[Index(a)];
            if (_wcsicmp(s.dll_path.c_str(), p.c_str()) != 0) return DWORD{ERROR_ALREADY_EXISTS};
            s.enabled = false; return DWORD{0};  // the journal keeps the old path
        };
        api.enable_if_disabled_for = [this](Architecture a, const std::wstring& old_path, const std::wstring& p) {
            calls.push_back(L"rebind " + p); auto& s = states[Index(a)];
            if (fail_enable) return fail_enable;
            if (s.enabled || _wcsicmp(s.dll_path.c_str(), old_path.c_str()) != 0) return DWORD{ERROR_ALREADY_EXISTS};
            s.enabled = true; s.dll_path = p; return DWORD{0};
        };
        return api;
    }
};
std::wstring Dll(const fs::path& root, const std::wstring& digest, Architecture a) {
    return (root / L"DialogBridge" / digest / DllNameFor(a)).wstring();
}
}

int main() try {
    const fs::path base = fs::absolute(L"bench_data");
    const fs::path root = base / (L"dialog-rebind-" + std::to_wstring(GetCurrentProcessId()));
    const fs::path foreign = base / (L"dialog-rebind-foreign-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(root, ec); fs::remove_all(foreign, ec);
    const std::wstring old_digest(64, L'e'), new64(64, L'3'), new86(64, L'5');
    for (const auto& d : {new64, new86}) fs::create_directories(root / L"DialogBridge" / d);
    const std::vector<InstalledDialogDll> current{{Architecture::X64, Dll(root, new64, Architecture::X64)},
                                                  {Architecture::X86, Dll(root, new86, Architecture::X86)}};
    {
        Fake fake;  // the field case: both registrations on a version setup already removed
        fake.states[0] = {true, false, 0, Dll(root, old_digest, Architecture::X64)};
        fake.states[1] = {true, false, 0, Dll(root, old_digest, Architecture::X86)};
        const auto result = RebindStaleRegistration(root.wstring(), current, fake.Api());
        Check(!result.error && result.rebound == 2 && fake.states[0].enabled && fake.states[1].enabled &&
            fake.states[0].dll_path == current[0].dll_path && fake.states[1].dll_path == current[1].dll_path,
            "registrations on a removed version move to the current bundles and stay enabled");
        Check(fake.calls.size() >= 4 && fake.calls[0].starts_with(L"disable ") && fake.calls.back().starts_with(L"rebind "),
            "old path is disabled before the guarded rebind");
        const auto again = RebindStaleRegistration(root.wstring(), current, fake.Api());
        Check(!again.error && again.rebound == 0 && again.skipped == 2, "already current registrations are left alone");
    }
    {
        Fake fake;  // owned but still present older version (upgrade before cleanup)
        fs::create_directories(root / L"DialogBridge" / std::wstring(64, L'f'));
        fake.states[0] = {true, false, 0, Dll(root, std::wstring(64, L'f'), Architecture::X64)};
        const auto result = RebindStaleRegistration(root.wstring(), {current[0]}, fake.Api());
        Check(!result.error && result.rebound == 1 && fake.states[0].dll_path == current[0].dll_path,
            "registration on an older present version of this install is moved");
    }
    {
        Fake fake;
        fake.states[0] = {false, false, 0, Dll(root, old_digest, Architecture::X64)};
        const auto result = RebindStaleRegistration(root.wstring(), {current[0]}, fake.Api());
        Check(result.rebound == 0 && fake.calls.empty() && !fake.states[0].enabled, "disabled registration is never turned on");
    }
    {
        Fake fake;  // another installation whose DLL exists
        fs::create_directories(foreign);
        const auto other = (foreign / DllNameFor(Architecture::X64)).wstring();
        HANDLE h = CreateFileW(other.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        fake.states[0] = {true, false, 0, other};
        const auto result = RebindStaleRegistration(root.wstring(), {current[0]}, fake.Api());
        Check(result.rebound == 0 && fake.calls.empty() && fake.states[0].dll_path == other, "live foreign registration is untouched");
    }
    {
        Fake fake;
        fake.states[0] = {true, true, 0, Dll(root, old_digest, Architecture::X64)};
        const auto result = RebindStaleRegistration(root.wstring(), {current[0]}, fake.Api());
        Check(result.rebound == 0 && fake.calls.empty(), "conflicting registration is untouched");
    }
    {
        Fake fake;
        fake.fail_enable = ERROR_ACCESS_DENIED;
        fake.states[0] = {true, false, 0, Dll(root, old_digest, Architecture::X64)};
        const auto result = RebindStaleRegistration(root.wstring(), {current[0]}, fake.Api());
        Check(result.error != ERROR_SUCCESS && result.rebound == 0, "failed rebind is reported, never counted");
    }
    {
        Fake fake;
        fake.states[0] = {true, false, 0, Dll(root, std::wstring(64, L'A'), Architecture::X64)};
        fake.states[1] = {false, false, 0, Dll(root, new86, Architecture::X86)};
        const auto digests = RegisteredBundleDigests(root.wstring(), fake.Api());
        Check(digests.size() == 1 && digests[0] == std::wstring(64, L'a'), "enabled registration digest is reported in lowercase; disabled one is not");
        fake.states[0].dll_path = (foreign / DllNameFor(Architecture::X64)).wstring();
        Check(RegisteredBundleDigests(root.wstring(), fake.Api()).empty(), "registration outside this install names no digest");
    }
    {
        Fake fake;
        const std::wstring common_stub64 = DefaultCommonDialogBridgeRoot() + L"\\" + StubDllNameFor(Architecture::X64);
        const std::wstring common_stub32 = DefaultCommonDialogBridgeRoot() + L"\\" + StubDllNameFor(Architecture::X86);
        fake.states[0] = {true, false, 0, Dll(root, old_digest, Architecture::X64)};
        fake.states[1] = {true, false, 0, Dll(root, old_digest, Architecture::X86)};
        const std::vector<InstalledDialogDll> stubs{{Architecture::X64, common_stub64}, {Architecture::X86, common_stub32}};
        const auto result = RebindStaleRegistration(root.wstring(), stubs, fake.Api());
        Check(!result.error && result.rebound == 2 &&
            fake.states[0].enabled && fake.states[0].dll_path == common_stub64 &&
            fake.states[1].enabled && fake.states[1].dll_path == common_stub32,
            "legacy DialogBridge registrations rebind to fixed-path Common Files Stub DLLs");
        Check(RegisteredBundleDigests(root.wstring(), fake.Api()).empty(),
            "after rebinding to Stub DLLs no legacy DialogBridge digest stays protected");
        const auto again = RebindStaleRegistration(root.wstring(), stubs, fake.Api());
        Check(!again.error && again.rebound == 0 && again.skipped == 2,
            "Stub registrations are recognized as current and skipped on repeat rebind");
    }
    fs::remove_all(root, ec); fs::remove_all(foreign, ec);
    std::printf("checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
} catch (const std::exception& e) {
    std::printf("[FAIL] unexpected exception: %s\n", e.what());
    return 2;
}
