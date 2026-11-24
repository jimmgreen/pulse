#include "setup_process.h"

#include "setup_common.h"
#include "setup_log.h"
#include "setup_service.h"
#include "../common/mcp_update_gate.h"

#include <windows.h>
#include <tlhelp32.h>
#include <algorithm>

namespace pulse::setup {
namespace {

struct ProcessEntry {
    DWORD pid;
    std::wstring path;  // empty when the image could not be queried
};

std::wstring ProcessPath(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return {};
    wchar_t buffer[32768];
    DWORD size = static_cast<DWORD>(std::size(buffer));
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

struct ScopedProcess {
    HANDLE handle;
    explicit ScopedProcess(DWORD pid, DWORD additional_access = 0)
        : handle(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE | additional_access, FALSE, pid)) {}
    ~ScopedProcess() { if (handle) CloseHandle(handle); }
    ScopedProcess(const ScopedProcess&) = delete;
    ScopedProcess& operator=(const ScopedProcess&) = delete;

    std::wstring Image() const {
        if (!handle || WaitForSingleObject(handle, 0) != WAIT_TIMEOUT) return {};
        std::wstring path(32768, L'\0');
        DWORD size = static_cast<DWORD>(path.size());
        if (!QueryFullProcessImageNameW(handle, 0, path.data(), &size)) return {};
        path.resize(size);
        return path;
    }
    bool CreatedBefore(const ScopedProcess& child) const {
        FILETIME parent_started{}, child_started{}, exit{}, kernel{}, user{};
        return handle && child.handle && GetProcessTimes(handle, &parent_started, &exit, &kernel, &user) &&
            GetProcessTimes(child.handle, &child_started, &exit, &kernel, &user) &&
            CompareFileTime(&parent_started, &child_started) <= 0;
    }
};

// Older MCP hosts do not observe the update gate. After all UI processes have
// accepted the shutdown handshake, close only their known stdio transport:
// a job-owned Pulse.AI.exe sibling, parented by the matching Pulse binary.
// The inner and outer launchers return naturally on that child's exit.
void DisconnectLegacyMcp(const std::vector<std::wstring>& dirs) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return;
    struct Link { DWORD pid, parent; };
    std::vector<Link> hosts;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL ok = Process32FirstW(snapshot, &entry); ok; ok = Process32NextW(snapshot, &entry))
        if (mcp_update::Same(entry.szExeFile, L"Pulse.AI.exe")) hosts.push_back({entry.th32ProcessID, entry.th32ParentProcessID});
    CloseHandle(snapshot);
    for (const auto& [pid, parent_pid] : hosts) {
        const ScopedProcess child(pid, PROCESS_TERMINATE);
        const auto image = child.Image();
        if (image.empty()) continue;
        const auto parent_dir = mcp_update::Parent(image);
        const auto root = mcp_update::InstallRootForHost(image);
        if (std::none_of(dirs.begin(), dirs.end(), [&](const auto& dir) { return mcp_update::Same(root, NormalizeDir(dir)); })) continue;
        const ScopedProcess parent(parent_pid);
        if (!mcp_update::Same(parent.Image(), std::wstring(parent_dir) + L"\\pulse.exe") || !parent.CreatedBefore(child)) continue;
        BOOL in_job = FALSE;
        if (!IsProcessInJob(child.handle, nullptr, &in_job) || !in_job) continue;
        if (TerminateProcess(child.handle, mcp_update::kRetryAfterUpdate))
            Log(L"Disconnected legacy Pulse MCP stdio host (pid " + std::to_wstring(pid) + L")");
    }
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
        if (process_path.empty()) return CloseState::Failed;
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
bool WaitForPulseProcesses(const std::vector<std::wstring>& dirs, bool unattended, bool drain_mcp) {
    std::vector<ProcessEntry> list;
    for (int i = 1; i <= 42; ++i) {
        if (!FindProcesses(L"pulse.exe", dirs, list, true)) {
            Log(L"Could not query remaining Pulse processes");
            return false;
        }
        if (list.empty()) return true;
        if (drain_mcp && i >= 9) DisconnectLegacyMcp(dirs);
        if (i == 41 && !unattended)
            for (const auto& p : list) {
                if (p.path.empty()) continue;
                Log(L"Ending leftover Pulse process " + std::to_wstring(p.pid));
                EndProcess(p.pid);
            }
        if (i < 41) Sleep(250);
    }
    for (const auto& p : list) Log(L"Pulse still running (pid " + std::to_wstring(p.pid) + L"): " + p.path);
    return false;
}

CloseState ClosePulseInstances(const std::vector<std::wstring>& dirs, int busy_seconds, bool unattended,
                               McpUpdateGuard* mcp_update) {
    CloseState state = CloseState::Failed;
    for (int i = 0; i <= busy_seconds; ++i) {
        state = ClosePulseWindows(dirs, unattended);
        if (state != CloseState::Busy) break;
        if (i < busy_seconds) Sleep(1000);
    }
    if (state != CloseState::Done) return state;
    if (mcp_update && !mcp_update->Activate(dirs)) return CloseState::McpGateFailed;
    if (!WaitForPulseProcesses(dirs, unattended, mcp_update != nullptr)) return CloseState::ProcessesRemain;
    return state;
}

}  // namespace

McpUpdateGuard::~McpUpdateGuard() {
    for (const auto file : files_) CloseHandle(file);
}

bool McpUpdateGuard::Activate(const std::vector<std::wstring>& dirs) {
    if (active_) return true;
    for (const auto& dir : dirs) {
        const auto path = mcp_update::GatePath(NormalizeDir(dir));
        const HANDLE file = CreateFileW(path.c_str(), DELETE | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_HIDDEN | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            // A migration may already have removed the old directory.
            if (error == ERROR_PATH_NOT_FOUND) continue;
            Log(L"Could not pause MCP connections at " + path + L": " + Win32Error(error));
            return false;
        }
        files_.push_back(file);
    }
    active_ = true;
    return true;
}

CloseState ClosePulseForUpdate(const std::vector<std::wstring>& dirs, bool unattended_update, McpUpdateGuard* mcp_update) {
    std::wstring joined;
    for (const auto& d : dirs) joined += (joined.empty() ? L"" : L"|") + d;
    Log(L"Closing Pulse running from: " + joined);
    for (;;) {
        const CloseState state = ClosePulseInstances(dirs, 10, unattended_update, mcp_update);
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
