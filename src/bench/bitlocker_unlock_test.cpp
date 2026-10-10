// bitlocker_unlock_test.cpp — the system unlock prompt for a locked drive (#13.4).
// Real prompts only: set PULSE_TEST_BITLOCKER_DRIVE=W (a locked BitLocker
// volume) and PULSE_TEST_BITLOCKER_PASSWORD. The unlock step runs an elevated
// Unlock-BitLocker in place of typing the password; the volume stays unlocked.
#include "../app/bitlocker_unlock.h"
#include "../fs/bitlocker_volume.h"

#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>

namespace {
constexpr UINT kResultMessage = WM_APP + 75;
int failures = 0;
void Check(bool ok, const char* label) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
    if (!ok) ++failures;
}

int PromptCount(bool kill) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    int count = 0;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"bdeunlock.exe") != 0) continue;
        ++count;
        if (!kill) continue;
        if (HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, entry.th32ProcessID)) {
            TerminateProcess(process, 1);
            WaitForSingleObject(process, 3000);
            CloseHandle(process);
        }
    }
    CloseHandle(snapshot);
    return count;
}

// Keeps pumping like Pulse's UI thread: the prompt is launched with this
// window as its owner, which needs the owner's thread to answer.
void Pump(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    for (;;) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE)) {
            if (msg.message == kResultMessage) return;  // left for WaitForResult
            PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
            DispatchMessageW(&msg);
        }
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline) return;
        MsgWaitForMultipleObjects(0, nullptr, FALSE, static_cast<DWORD>((std::min)(deadline - now, 50ull)), QS_ALLINPUT);
    }
}

bool WaitForPrompt(DWORD ms) {
    for (DWORD waited = 0; waited < ms; waited += 100) {
        if (PromptCount(false) > 0) return true;
        Pump(100);
    }
    return false;
}

// Pumps until the result arrives; nullptr on timeout.
std::unique_ptr<pulse::app::BitLockerUnlockResult> WaitForResult(DWORD ms) {
    const ULONGLONG deadline = GetTickCount64() + ms;
    while (GetTickCount64() < deadline) {
        MSG msg{};
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == kResultMessage)
                return std::unique_ptr<pulse::app::BitLockerUnlockResult>(
                    reinterpret_cast<pulse::app::BitLockerUnlockResult*>(msg.lParam));
            DispatchMessageW(&msg);
        }
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT);
    }
    return nullptr;
}

bool ElevatedUnlock(const std::wstring& drive, const std::wstring& password) {
    std::wstring script = L"-NoProfile -Command \"Unlock-BitLocker -MountPoint '" + drive +
        L":' -Password (ConvertTo-SecureString '" + password + L"' -AsPlainText -Force) | Out-Null\"";
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    execute.lpVerb = L"runas";
    execute.lpFile = L"powershell.exe";
    execute.lpParameters = script.c_str();
    execute.nShow = SW_HIDE;
    if (!ShellExecuteExW(&execute) || !execute.hProcess) return false;
    // Unlocking with a password takes a few seconds; the poller must not need us.
    CloseHandle(execute.hProcess);
    return true;
}
}

int main() {
    wchar_t drive_text[4]{}, password[256]{};
    if (!GetEnvironmentVariableW(L"PULSE_TEST_BITLOCKER_DRIVE", drive_text, 4) ||
        !GetEnvironmentVariableW(L"PULSE_TEST_BITLOCKER_PASSWORD", password, 256)) {
        std::printf("[SKIP] PULSE_TEST_BITLOCKER_DRIVE / PULSE_TEST_BITLOCKER_PASSWORD not set\n");
        return 0;
    }
    const std::wstring drive(1, static_cast<wchar_t>(towupper(drive_text[0])));
    const std::wstring root = drive + L":\\";
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HWND hwnd = CreateWindowExW(0, L"STATIC", L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    Check(hwnd && pulse::fs::IsBitLockerLocked(root), "fixture: drive starts locked");
    PromptCount(true);

    Check(!pulse::app::StartBitLockerUnlock(hwnd, kResultMessage, L"\\\\server\\share"),
          "no prompt for paths without a drive");
    // 1. The user closes the prompt without unlocking.
    Check(pulse::app::StartBitLockerUnlock(hwnd, kResultMessage, root + L"folder\\file.txt"), "unlock wait starts");
    Check(!pulse::app::StartBitLockerUnlock(hwnd, kResultMessage, root), "second request for the same drive is merged");
    Check(WaitForPrompt(8000), "system unlock prompt (bdeunlock) is shown");
    Pump(500);
    PromptCount(true);
    auto cancelled = WaitForResult(10000);
    Check(cancelled && cancelled->root == root && !cancelled->unlocked && pulse::fs::IsBitLockerLocked(root),
          "closing the prompt reports the drive as still locked");

    // 2. The user enters the password (elevated Unlock-BitLocker stands in).
    Check(pulse::app::StartBitLockerUnlock(hwnd, kResultMessage, root), "unlock wait restarts after a cancel");
    Check(WaitForPrompt(8000), "prompt is shown again");
    const ULONGLONG started = GetTickCount64();
    Check(ElevatedUnlock(drive, password), "elevated unlock launched");
    auto unlocked = WaitForResult(90000);
    const double seconds = (GetTickCount64() - started) / 1000.0;
    Check(unlocked && unlocked->root == root && unlocked->unlocked, "unlocking reports success with the drive root");
    Check(!pulse::fs::IsBitLockerLocked(root) && GetFileAttributesW((root + L"hello.txt").c_str()) != INVALID_FILE_ATTRIBUTES,
          "the drive's files are readable after the result");
    std::printf("unlock reported %.1f s after the password was submitted\n", seconds);
    PromptCount(true);
    DestroyWindow(hwnd);
    CoUninitialize();
    std::printf("failures=%d\n", failures);
    return failures ? 1 : 0;
}
