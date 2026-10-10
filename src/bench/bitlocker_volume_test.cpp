// bitlocker_volume_test.cpp — recognizing locked BitLocker volumes (#13.4).
// Pure checks always run. Set PULSE_TEST_LOCKED_DRIVE=W to also probe a real
// locked volume, for example a mounted BitLocker-encrypted VHD.
#include "../fs/bitlocker_volume.h"
#include "../fs/fs_enum.h"

#include <cstdio>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}
const pulse::fs::DirEntry* DriveRow(const std::vector<pulse::fs::DirEntry>& rows, wchar_t letter) {
    for (const auto& row : rows)
        if (pulse::fs::DriveRootOf(row.full_path) == std::wstring{letter, L':', L'\\'}) return &row;
    return nullptr;
}
}

int main() {
    using namespace pulse::fs;
    Check(DriveRootOf(L"w:\\folder\\file.txt") == L"W:\\" && DriveRootOf(L"W:") == L"W:\\" &&
          DriveRootOf(L"\\\\?\\W:\\folder") == L"W:\\" && DriveRootOf(L"C:/x") == L"C:\\",
          "drive root of local, bare and long paths");
    Check(DriveRootOf(L"\\\\server\\share").empty() && DriveRootOf(L"\\\\?\\UNC\\server\\share").empty() &&
          DriveRootOf(L"pulse:recycle").empty() && DriveRootOf(L"").empty() && DriveRootOf(L"W:x").empty() &&
          DriveRootOf(L"1:\\").empty(), "UNC, virtual, relative and malformed paths have no drive root");
    Check(IsBitLockerLockedError(0x80310000u) && !IsBitLockerLockedError(ERROR_ACCESS_DENIED) &&
          !IsBitLockerLockedError(ERROR_NOT_READY) && !IsBitLockerLockedError(ERROR_SUCCESS),
          "only FVE_E_LOCKED_VOLUME means locked");
    Check(!IsBitLockerLocked(L"pulse:recycle") && !IsBitLockerLocked(L"\\\\server\\share"),
          "paths without a drive are never probed as locked");
    wchar_t windows[MAX_PATH]{};
    GetWindowsDirectoryW(windows, MAX_PATH);
    Check(!IsBitLockerLocked(windows), "the system drive is not reported as locked");

    std::vector<DirEntry> pc;
    EnumerateDirectory(L"", pc);
    const auto* system_row = DriveRow(pc, static_cast<wchar_t>(towupper(windows[0])));
    Check(system_row && !system_row->drive_locked && system_row->drive_total > 0,
          "This PC lists the system drive as unlocked with its capacity");

    wchar_t letter[4]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_LOCKED_DRIVE", letter, 4) && iswalpha(letter[0])) {
        const wchar_t drive = static_cast<wchar_t>(towupper(letter[0]));
        const std::wstring root{drive, L':', L'\\'};
        Check(IsBitLockerLocked(root) && IsBitLockerLocked(root + L"folder\\file.txt") &&
              IsBitLockerLocked(L"\\\\?\\" + root), "a locked volume is recognized from any path on it");
        const auto* row = DriveRow(pc, drive);
        Check(row && row->drive_locked && row->drive_total == 0, "This PC marks the locked drive");
        std::vector<DirEntry> inside;
        bool failed = false;
        try { EnumerateDirectory(NormalizePath(root), inside); } catch (...) { failed = true; }
        Check((failed || inside.empty()) && IsBitLockerLocked(root),
              "opening the locked drive fails and is attributed to BitLocker");
    } else {
        std::printf("[SKIP] PULSE_TEST_LOCKED_DRIVE not set; no real locked volume probed\n");
    }
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
