#include "setup_process.h"

#include "setup_common.h"
#include "setup_log.h"
#include "setup_service.h"

#include <windows.h>
#include <tlhelp32.h>

namespace pulse::setup {
namespace {

struct ProcessEntry {
    DWORD pid;
    std::wstring path;  // empty when the image could not be queried
};

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[1024];
    DWORD size = 1024;
    std::wstring path;
    if (QueryFullProcessImageNameW(process, 0, buffer, &size)) path.assign(buffer, size);
    CloseHandle(process);
    return path;
}
bool VersionedDialogProcess(const std::wstring& path, const std::vector<std::wstring>& dirs) {
    for (const auto& dir : dirs) {
        const auto prefix = NormalizeDir(dir) + L"\\DialogBridge\\";
        if (path.size() <= prefix.size() + 65 ||
            CompareStringOrdinal(path.data(), static_cast<int>(prefix.size()), prefix.c_str(), -1, TRUE) != CSTR_EQUAL) continue;
        bool digest = true;
        for (size_t i = prefix.size(); i < prefix.size()+64; ++i)
            digest = digest && ((path[i] >= L'0' && path[i] <= L'9') || (path[i] >= L'a' && path[i] <= L'f'));
        if (digest && path[prefix.size()+64] == L'\\') return true;
    }
    return false;
}

// Processes named `image` running from `dirs`. Returns false when the
// snapshot failed: a failed query is not evidence that nothing runs.
// `unknown_inside`: whether a process whose image path cannot be read counts
// as ours. True only where it blocks setup (pulse.exe), never where it would
// end a process: an unelevated test cannot read the SYSTEM index service.
bool FindProcesses(const wchar_t* image, const std::vector<std::wstring>& dirs, std::vector<ProcessEntry>& out,
                   bool unknown_inside) {
    out.clear();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof(e)};
    for (BOOL ok = Process32FirstW(snap, &e); ok; ok = Process32NextW(snap, &e)) {
        if (CompareStringOrdinal(e.szExeFile, -1, image, -1, TRUE) != CSTR_EQUAL) continue;
        auto path = ProcessPath(e.th32ProcessID);
        if (VersionedDialogProcess(path, dirs)) continue;
        if (path.empty() ? unknown_inside : PathInDirectories(path, dirs)) out.push_back({e.th32ProcessID, path});
    }
    CloseHandle(snap);
    return true;
}

bool EndProcess(DWORD pid) {
    HANDLE process = OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid);
    if (!process) return GetLastError() == ERROR_INVALID_PARAMETER;  // already gone
    if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0) TerminateProcess(process, 0);
    const bool gone = WaitForSingleObject(process, 5000) == WAIT_OBJECT_0;
    CloseHandle(process);
    return gone;
}

// Pulse <= 1.0.48 predates the handshake: send what sign-out sends, where
// those versions save their session, then end the process.
bool EndLegacyPulse(HWND window, DWORD pid) {
    DWORD_PTR reply = 0;
    SendMessageTimeoutW(window, WM_QUERYENDSESSION, 0, ENDSESSION_CLOSEAPP, SMTO_ABORTIFHUNG, 10000, &reply);
    SendMessageTimeoutW(window, WM_ENDSESSION, 1, ENDSESSION_CLOSEAPP, SMTO_ABORTIFHUNG, 10000, &reply);
    const bool ok = EndProcess(pid);
    Log(L"Closed Pulse without update handshake (pid " + std::to_wstring(pid) + L"): " + std::to_wstring(ok));
    return ok;
}

CloseState ClosePulseWindows(const std::vector<std::wstring>& dirs, bool unattended) {
    const UINT msg = RegisterWindowMessageW(L"Pulse.PrepareUpdateShutdown.v1");
    if (!msg) return CloseState::Failed;
    HWND window = nullptr;
    for (int i = 0; i < 64; ++i) {
        window = FindWindowExW(nullptr, window, L"PulseMainWindow", nullptr);
        if (!window) return CloseState::Done;
        DWORD pid = 0;
        GetWindowThreadProcessId(window, &pid);
        const auto process_path = ProcessPath(pid);
        if (!PathInDirectories(process_path, dirs)) continue;
        DWORD_PTR reply = 0;
        if (!SendMessageTimeoutW(window, msg, 0, 0, SMTO_ABORTIFHUNG, 30000, &reply)) return CloseState::Failed;
        if (reply == 2) return CloseState::Busy;
        if (reply == 3) return CloseState::Failed;  // session persistence failed; keep files
        if (reply != 1) {
            if (unattended) return CloseState::Failed;
            if (!EndLegacyPulse(window, pid)) return CloseState::Failed;
        }
        window = nullptr;  // the window list changed; scan again
    }
    return CloseState::Failed;
}

// Waits for windowless pulse.exe instances; interactive setups end leftovers
// after ~10 s. A failed process query keeps setup blocked.
bool WaitForPulseProcesses(const std::vector<std::wstring>& dirs, bool unattended) {
    std::vector<ProcessEntry> list;
    for (int i = 1; i <= 42; ++i) {
        if (!FindProcesses(L"pulse.exe", dirs, list, true)) {
            Log(L"Could not query remaining Pulse processes");
            return false;
        }
        if (list.empty()) return true;
        if (i == 41 && !unattended)
            for (const auto& p : list) {
                if (p.path.empty()) continue;
                Log(L"Ending leftover Pulse process " + std::to_wstring(p.pid));
                EndProcess(p.pid);
            }
        if (i < 41) Sleep(250);
    }
    return false;
}

CloseState ClosePulseInstances(const std::vector<std::wstring>& dirs, int busy_seconds, bool unattended) {
    CloseState state = CloseState::Failed;
    for (int i = 0; i <= busy_seconds; ++i) {
        state = ClosePulseWindows(dirs, unattended);
        if (state != CloseState::Busy) break;
        if (i < busy_seconds) Sleep(1000);
    }
    if (state == CloseState::Done && !WaitForPulseProcesses(dirs, unattended)) state = CloseState::Failed;
    return state;
}

}  // namespace

CloseState ClosePulseForUpdate(const std::vector<std::wstring>& dirs, bool unattended_update) {
    std::wstring joined;
    for (const auto& d : dirs) joined += (joined.empty() ? L"" : L"|") + d;
    Log(L"Closing Pulse running from: " + joined);
    for (;;) {
        const CloseState state = ClosePulseInstances(dirs, 10, unattended_update);
        // Operations may take hours: an unattended update keeps asking for an
        // orderly exit instead of killing active work (same as the Inno script).
        if (unattended_update && state == CloseState::Busy) { Sleep(1000); continue; }
        // Interactive Retry is offered by the wizard (P2); here we report it.
        return state;
    }
}

bool WaitUntilImageGone(const wchar_t* image, const std::vector<std::wstring>& dirs, unsigned timeout_ms) {
    std::vector<ProcessEntry> list;
    for (unsigned waited = 0;; waited += 250) {
        if (FindProcesses(image, dirs, list, false) && list.empty()) return true;
        if (waited >= timeout_ms) return false;
        Sleep(250);
    }
}

void StopPulseHosts(const std::vector<std::wstring>& dirs, bool touch_service) {
    if (touch_service) {
        const auto r = StopService(kServiceName, 15000);
        Log(L"Stop PulseIndex service: " + std::to_wstring(r));
    }
    // Scoped to the install directories (the Inno script used taskkill /IM,
    // which also ended hosts of portable copies that do not lock our files).
    std::vector<ProcessEntry> list;
    for (const wchar_t* image : {L"Pulse.Index.exe", L"Pulse.Preview.exe", L"pulse_shell.exe", L"Pulse.AI.exe",
                                  L"Pulse.Content.exe", L"Pulse.Document.exe"}) {
        if (!FindProcesses(image, dirs, list, false)) continue;
        for (const auto& p : list) {
            const bool ok = EndProcess(p.pid);
            Log(std::wstring(L"Ended ") + image + L" pid " + std::to_wstring(p.pid) + L": " + std::to_wstring(ok));
        }
    }
}

}  // namespace pulse::setup
