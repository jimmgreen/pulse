#include "../common/dialog_adaptive_attach.h"
#include "../common/dialog_admin_trust.h"
#include "../common/dialog_auto_binding.h"
#include <commdlg.h>
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace adaptive = pulse::dialog_adaptive;
namespace binding = pulse::dialog_auto_binding;

// Keep a real static import on GetOpenFileNameW so the remote PE preflight test
// exercises actual PE import table scanning on this executable.
static volatile auto kKeepGetOpenFileNameImport = &GetOpenFileNameW;

int main() {
    (void)kKeepGetOpenFileNameImport;
    unsigned checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* label) {
        ++checks;
        if (!ok) ++failures;
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    };

    // 1. Hard exclusion policy
    const std::wstring win = L"C:\\Windows";
    const std::wstring pulse_root = L"C:\\Program Files\\Pulse";
    check(adaptive::IsExcludedExecutable(L"C:\\Windows\\explorer.exe", win, pulse_root),
          "excludes Windows directory binaries");
    check(adaptive::IsExcludedExecutable(L"C:\\Windows\\System32\\notepad.exe", win, pulse_root),
          "excludes System32 binaries");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\Pulse\\AppRuntime\\abc\\pulse.exe", win, pulse_root),
          "excludes Pulse install root");
    check(adaptive::IsExcludedExecutable(L"C:\\Users\\User\\AppData\\Local\\Pulse\\extensions\\pkg\\pulse_desktop_host.exe", win, pulse_root),
          "excludes pulse_desktop_host.exe");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\Google\\Chrome\\Application\\chrome.exe", win, pulse_root),
          "excludes Chrome");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\msedge.exe", win, pulse_root),
          "excludes Edge");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\Mozilla Firefox\\firefox.exe", win, pulse_root),
          "excludes Firefox");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\WindowsApps\\Microsoft.SecHealthUI_1000.22621.1.0_x64__8wekyb3d8bbwe\\SecHealthUI.exe", win, pulse_root),
          "excludes WindowsApps security/system package");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\WindowsApps\\Microsoft.WidgetsPlatformRuntime_1.6.19.0_x64__8wekyb3d8bbwe\\WidgetService.exe", win, pulse_root),
          "excludes WindowsApps widgets runtime package");
    check(adaptive::IsExcludedExecutable(L"C:\\Program Files\\WindowsApps\\Microsoft.PowerShell_7.7.0.0_x64__8wekyb3d8bbwe\\pwsh.exe", win, pulse_root),
          "excludes WindowsApps PowerShell console");
    check(!adaptive::IsExcludedExecutable(L"C:\\Program Files\\WindowsApps\\Microsoft.Paint_11.2609.441.0_x64__8wekyb3d8bbwe\\PaintApp\\mspaint.exe", win, pulse_root),
          "allows Windows 11 Paint under WindowsApps");
    check(!adaptive::IsExcludedExecutable(L"C:\\Program Files\\WindowsApps\\Foo.Editor_1.0.0.0_x64__abc\\editor.exe", win, pulse_root),
          "allows full-trust Store desktop app under WindowsApps");
    check(adaptive::IsExcludedExecutable(L"C:\\Games\\Foo\\EasyAntiCheat_EOS.exe", win, pulse_root),
          "excludes EasyAntiCheat prefix");
    check(!adaptive::IsExcludedExecutable(L"C:\\Tools\\MyLegacyApp\\editor.exe", win, pulse_root),
          "allows ordinary third-party desktop app");

    // 2. Crash exit code classification
    check(!adaptive::IsCrashExitCode(0), "exit 0 is not a crash");
    check(!adaptive::IsCrashExitCode(1), "exit 1 is not a crash");
    check(!adaptive::IsCrashExitCode(STILL_ACTIVE), "STILL_ACTIVE is not a crash");
    check(adaptive::IsCrashExitCode(STATUS_ACCESS_VIOLATION), "access violation is a crash");
    check(adaptive::IsCrashExitCode(STATUS_STACK_BUFFER_OVERRUN), "failfast is a crash");
    check(adaptive::IsCrashExitCode(STATUS_STACK_OVERFLOW), "stack overflow is a crash");
    check(adaptive::IsCrashExitCode(STATUS_BREAKPOINT), "breakpoint is a crash");

    // 3. Read-only remote PE import preflight on self
#if defined(_WIN64)
    constexpr WORD kMachine = IMAGE_FILE_MACHINE_AMD64;
#else
    constexpr WORD kMachine = IMAGE_FILE_MACHINE_I386;
#endif
    uint32_t match_mask = 0;
    const DWORD scan_err = adaptive::ScanRemoteProcessForAdaptiveMatch(
        GetCurrentProcess(), GetCurrentProcessId(), kMachine, win, pulse_root, match_mask);
    check(scan_err == ERROR_SUCCESS && (match_mask & adaptive::kMatchLegacyImports) != 0,
          "read-only PE preflight detects GetOpenFileNameW import");

    // 4. Fingerprint + AdaptiveTargetStore persistence & circuit breaker
    const auto temp_dir = std::filesystem::temp_directory_path() /
        (L"pulse-adaptive-test-" + std::to_wstring(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::create_directories(temp_dir, ec);
    const auto dummy_exe = temp_dir / L"sample_app.exe";
    const auto store_file = temp_dir / L"dialog_adaptive_targets.json";
    {
        std::ofstream f(dummy_exe, std::ios::binary);
        f << "v1-payload";
    }
    const std::string fp1 = adaptive::ComputeBinaryFingerprint(dummy_exe.wstring());
    check(!fp1.empty(), "computes fingerprint for existing file");
    {
        std::ofstream f(dummy_exe, std::ios::binary | std::ios::app);
        f << "-extra-bytes";
    }
    const std::string fp2 = adaptive::ComputeBinaryFingerprint(dummy_exe.wstring());
    check(!fp2.empty() && fp2 != fp1, "fingerprint changes when binary changes");

    adaptive::AdaptiveTargetStore store;
    check(store.RecordLearned(fp1, L"sample_app.exe"), "records newly learned target");
    check(store.IsLearned(fp1) && !store.IsBlocked(fp1), "learned target active and unblocked");
    check(!store.RecordFailure(fp1, L"sample_app.exe", false), "single transient failure does not trip breaker");
    check(!store.IsBlocked(fp1), "still unblocked after 1 transient failure");
    check(store.RecordFailure(fp1, L"sample_app.exe", false), "second failure trips circuit breaker");
    check(store.IsBlocked(fp1) && !store.IsLearned(fp1), "blocked target evicted from learned");
    check(store.RecordFailure(fp2, L"crash_app.exe", true), "crash/timeout immediately trips circuit breaker");
    check(store.IsBlocked(fp2), "immediate block active");
    check(store.Save(store_file), "saves adaptive store atomically");

    adaptive::AdaptiveTargetStore reloaded;
    check(reloaded.Load(store_file), "reloads adaptive store from JSON");
    check(reloaded.IsBlocked(fp1) && reloaded.IsBlocked(fp2) && reloaded.BlockedCount() == 2,
          "reloaded store preserves blocked entries");

    // 4a. Paused list and per-program resume for the settings page; a running helper
    // picks up the edited file instead of saving its stale copy over it.
    {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        const uint64_t now = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
        const auto paused = reloaded.PausedTargets(now);
        check(paused.size() == 2, "paused list shows both active breakers");
        bool fields = paused.size() == 2;
        for (const auto& item : paused)
            fields = fields && !item.fingerprint.empty() && !item.basename.empty() && item.failures >= 1 &&
                item.retry_ft == item.paused_ft + adaptive::AdaptiveTargetStore::kBlockedRetryAfter;
        check(fields, "paused entries carry name, failures and the automatic retry time");
        check(reloaded.PausedTargets(now + adaptive::AdaptiveTargetStore::kBlockedRetryAfter + 1).empty(),
              "expired breakers are not listed as paused");

        adaptive::AdaptiveTargetStore helper; // the long-running helper's copy
        check(helper.Load(store_file) && helper.IsBlocked(fp2), "helper loads the paused program");
        check(!helper.ReloadIfChanged(store_file), "unchanged file is not reloaded");
        adaptive::AdaptiveTargetStore page;   // the settings page's short-lived copy
        check(page.Load(store_file), "settings page loads the store");
        check(page.ResumeBasename("CRASH_APP.EXE") == 1 && !page.IsBlocked(fp2) && page.IsBlocked(fp1),
              "resume clears only that program (case-insensitive)");
        check(page.ResumeBasename("crash_app.exe") == 0 && page.ResumeBasename("") == 0, "resume of an unpaused name is a no-op");
        Sleep(20); // distinct last-write time even on coarse timestamp file systems
        check(page.Save(store_file), "settings page saves the resumed store");
        check(helper.ReloadIfChanged(store_file) && !helper.IsBlocked(fp2) && helper.IsBlocked(fp1),
              "helper reloads the edited store before its next decision");
        check(helper.Save(store_file) && !helper.ReloadIfChanged(store_file), "helper's own save is not seen as an external change");
        adaptive::AdaptiveTargetStore after;
        check(after.Load(store_file) && !after.IsBlocked(fp2) && after.IsBlocked(fp1), "helper save keeps the resume");
        // Unicode names: stored as UTF-8 and compared ignoring case.
        adaptive::AdaptiveTargetStore unicode;
        (void)unicode.RecordFailure("u-fp", L"\u7F16\u8F91\u5668\u00C9.exe", true);
        check(unicode.ResumeBasename("\xE7\xBC\x96\xE8\xBE\x91\xE5\x99\xA8\xC3\xA9.EXE") == 1, "resume matches a Unicode name ignoring case");
    }

    reloaded.ClearBlocked();
    check(reloaded.BlockedCount() == 0 && !reloaded.IsBlocked(fp1), "ClearBlocked resets circuit breaker");

    // 4b. Version-stable fingerprint (v2), exact persisted timestamps, breaker expiry
    const auto now_ft = [] {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    check(adaptive::ComputeBinaryFingerprintV2(dummy_exe.wstring()) == adaptive::ComputeBinaryFingerprint(dummy_exe.wstring()),
          "unversioned binary keeps the v1 key");
    check(adaptive::ComputeBinaryFingerprintV2((temp_dir / L"missing.exe").wstring()).empty(), "missing binary has no v2 key");
    {
        wchar_t system_dll[MAX_PATH]{};
        const DWORD n = GetModuleFileNameW(GetModuleHandleW(L"kernel32.dll"), system_dll, MAX_PATH);
        const auto versioned = temp_dir / L"versioned_app.exe";
        const bool copied = n && n < MAX_PATH && CopyFileW(system_dll, versioned.c_str(), FALSE);
        check(copied, "copies a versioned fixture binary");
        const std::string v2a = adaptive::ComputeBinaryFingerprintV2(versioned.wstring());
        const std::string v1a = adaptive::ComputeBinaryFingerprint(versioned.wstring());
        check(v2a.rfind("v2-", 0) == 0, "versioned binary gets a v2 key");
        {
            std::ofstream f(versioned, std::ios::binary | std::ios::app);
            f << "overlay-bytes";
        }
        HANDLE h = CreateFileW(versioned.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME later{};
            const uint64_t t = now_ft() + 3600ull * 10000000ull;
            later.dwLowDateTime = static_cast<DWORD>(t); later.dwHighDateTime = static_cast<DWORD>(t >> 32);
            SetFileTime(h, nullptr, nullptr, &later);
            CloseHandle(h);
        }
        check(adaptive::ComputeBinaryFingerprint(versioned.wstring()) != v1a, "v1 key changes after size/mtime change");
        check(adaptive::ComputeBinaryFingerprintV2(versioned.wstring()) == v2a, "v2 key survives same-version size/mtime change");
        const auto moved = temp_dir / L"other_dir_app.exe";
        CopyFileW(versioned.c_str(), moved.c_str(), FALSE);
        check(adaptive::ComputeBinaryFingerprintV2(moved.wstring()) != v2a, "v2 key still separates install paths");

        adaptive::AdaptiveTargetStore migrate;
        check(migrate.RecordLearned(v1a, L"versioned_app.exe"), "legacy learned entry recorded");
        check(migrate.AdoptLegacyLearned(v2a, v1a) && migrate.IsLearned(v2a) && !migrate.IsLearned(v1a),
              "legacy learned entry migrates to the v2 key");
        check(!migrate.AdoptLegacyLearned(v2a, v1a), "migration is idempotent");
    }

    adaptive::AdaptiveTargetStore expiry;
    const std::string fp_expiry = "v2-expiry";
    const uint64_t before_block = now_ft();
    check(expiry.RecordFailure(fp_expiry, L"expiry.exe", true), "immediate block for expiry fixture");
    const uint64_t ttl = adaptive::AdaptiveTargetStore::kBlockedRetryAfter;
    check(expiry.IsBlockedAt(fp_expiry, before_block + ttl - 10000000ull), "block holds inside the retry window");
    check(!expiry.IsBlockedAt(fp_expiry, before_block + ttl + 600ull * 10000000ull), "block expires after the retry window");
    check(expiry.IsBlockedAt(fp_expiry, 0), "clock moved backwards keeps the block");
    const auto expiry_file = temp_dir / L"expiry.json";
    check(expiry.Save(expiry_file), "saves store with exact timestamps");
    adaptive::AdaptiveTargetStore expiry_loaded;
    check(expiry_loaded.Load(expiry_file) &&
          expiry_loaded.IsBlockedAt(fp_expiry, before_block + ttl - 10000000ull) &&
          !expiry_loaded.IsBlockedAt(fp_expiry, before_block + ttl + 600ull * 10000000ull),
          "reloaded block keeps its exact retry window");
    const auto legacy_file = temp_dir / L"legacy.json";
    {
        std::ofstream f(legacy_file, std::ios::binary);
        f << R"({"version":1,"learned":{},"failures":{"v1-legacy":{"basename":"old.exe","failures":2,"blocked":true,"updated_ft":8507199254740991}}})";
    }
    adaptive::AdaptiveTargetStore legacy_loaded;
    check(legacy_loaded.Load(legacy_file) && legacy_loaded.IsBlocked("v1-legacy"),
          "legacy masked timestamp restarts the window instead of expiring at once");

    std::filesystem::remove_all(temp_dir, ec);

    // 5. Non-elevated vs elevated binding separation
    GUID nonce{};
    check(SUCCEEDED(CoCreateGuid(&nonce)), "generate test nonce");
    binding::BindingV1 bound{};
    if (!pulse::dialog_admin::IsElevated()) {
        check(binding::CaptureCurrentWithHelperUser(GetCurrentProcess(), 1, nonce, bound) == ERROR_SUCCESS,
              "user binding accepts non-elevated same-user process");
        check(binding::ValidateFormat(bound) == ERROR_SUCCESS, "user binding passes format validation");
        binding::BindingV1 admin_bound{};
        check(binding::CaptureCurrentWithHelper(GetCurrentProcess(), 1, nonce, admin_bound) == ERROR_ELEVATION_REQUIRED,
              "admin binding still strictly rejects non-elevated process");
    } else {
        check(binding::CaptureCurrentWithHelperUser(GetCurrentProcess(), 1, nonce, bound) == ERROR_ACCESS_DENIED,
              "user binding strictly rejects elevated process");
    }

    std::printf("ADAPTIVE_ATTACH_TEST checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
