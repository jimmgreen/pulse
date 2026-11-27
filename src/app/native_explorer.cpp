#include "native_explorer.h"
#include "../common/shell_open_entry.h"
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <process.h>


namespace pulse::app::native_explorer {
namespace {
constexpr wchar_t kOwner[] = L"Pulse.NativeExplorer.Completion";
constexpr wchar_t kNative[] = L"Pulse.NativeExplorer.Window.v1";
constexpr wchar_t kPid[] = L"Pulse.NativeExplorer.Pid";
constexpr wchar_t kTid[] = L"Pulse.NativeExplorer.Tid";
constexpr wchar_t kCreatedLow[] = L"Pulse.NativeExplorer.CreatedLow";
constexpr wchar_t kCreatedHigh[] = L"Pulse.NativeExplorer.CreatedHigh";
constexpr ULONGLONG kRequestMs = 10000;
std::atomic<ULONG_PTR> next_cookie{1};
std::atomic<unsigned> workers{0};
struct Pending {
    ULONG_PTR cookie = 0;
    std::wstring folder, canonical, selected, desktop;
    std::unordered_set<HWND> before;
    ULONGLONG expires = 0, started = 0;
    bool claimed = false;
};
struct PendingStore {
    std::mutex mutex;
    std::vector<Pending> requests;
};
PendingStore& Store() {
    // Detached requests must not race CRT static destruction at app shutdown.
    // Bounded to eight intents; the process owns this store until OS teardown.
    static auto* store = new PendingStore;
    return *store;
}
std::wstring DesktopName(DWORD thread) {
    wchar_t name[256]{}; DWORD size = 0;
    const auto desktop = GetThreadDesktop(thread);
    return desktop && GetUserObjectInformationW(desktop, UOI_NAME, name, sizeof(name), &size) ? name : L"";
}
std::wstring Normalize(std::wstring path) {
    for (auto& ch : path) if (ch == L'/') ch = L'\\';
    if (path.starts_with(L"\\\\?\\UNC\\")) path = L"\\\\" + path.substr(8);
    else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    while (path.size() > 3 && path.back() == L'\\') path.pop_back();
    return path;
}
bool Same(const std::wstring& a, const std::wstring& b) {
    return !a.empty() && !b.empty() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}
std::wstring Canonical(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (file == INVALID_HANDLE_VALUE) return {};
    std::wstring result(32768, 0);
    const DWORD count = GetFinalPathNameByHandleW(file, result.data(), static_cast<DWORD>(result.size()), FILE_NAME_NORMALIZED);
    CloseHandle(file);
    if (!count || count >= result.size()) return {};
    result.resize(count); return Normalize(std::move(result));
}
bool Identity(HWND window, DWORD& pid, DWORD& tid, FILETIME& created) {
    tid = GetWindowThreadProcessId(window, &pid);
    if (!tid || !pid) return false;
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    FILETIME exited{}, kernel{}, user{};
    const bool ok = process && GetProcessTimes(process, &created, &exited, &kernel, &user);
    if (process) CloseHandle(process);
    return ok;
}
void ExpireLocked() {
    const auto now = GetTickCount64();
    std::erase_if(Store().requests, [now](const auto& item) { return item.expires <= now; });
}
bool NewWindow(const Pending& item, HWND window, const std::wstring& desktop) {
    return !item.before.contains(window) && Same(item.desktop, desktop);
}
bool Mark(HWND window, ULONG_PTR cookie) {
    DWORD pid = 0, tid = 0; FILETIME created{};
    if (!Identity(window, pid, tid, created)) return false;
    // Properties disappear with the HWND, and process birth time rejects PID
    // reuse. Publish the final marker only after every identity field is set.
    if (!SetPropW(window, kPid, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(pid))) ||
        !SetPropW(window, kTid, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(tid))) ||
        !SetPropW(window, kCreatedLow, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(created.dwLowDateTime))) ||
        !SetPropW(window, kCreatedHigh, reinterpret_cast<HANDLE>(static_cast<ULONG_PTR>(created.dwHighDateTime))) ||
        !SetPropW(window, kNative, reinterpret_cast<HANDLE>(cookie))) {
        RemovePropW(window, kNative); RemovePropW(window, kPid); RemovePropW(window, kTid);
        RemovePropW(window, kCreatedLow); RemovePropW(window, kCreatedHigh);
        return false;
    }
    return true;
}
struct Work {
    Request request;
    HWND owner = nullptr;
    ULONG_PTR owner_cookie = 0;
    std::wstring desktop;
};
DWORD Launch(const Work& work, HDESK desktop, decltype(&CreateProcessW) create) {
    const DWORD attributes = GetFileAttributesW(work.request.path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) return GetLastError();
    if ((work.request.intent == Intent::OpenFolder) != ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)) return ERROR_DIRECTORY;
    Pending item;
    item.cookie = next_cookie.fetch_add(1);
    item.desktop = work.desktop;
    auto target = Normalize(work.request.path);
    if (work.request.intent == Intent::SelectFile) {
        const auto slash = target.find_last_of(L'\\');
        if (slash == target.npos) return ERROR_INVALID_NAME;
        item.selected = target.substr(slash + 1);
        item.folder = target.substr(0, slash == 2 ? 3 : slash);
    } else item.folder = target;
    item.canonical = Canonical(item.folder);
    // Exact lexical matching remains available if canonical access is denied.
    bool snapshot_ok = true;
    if (!EnumDesktopWindows(desktop, [](HWND window, LPARAM arg) -> BOOL {
        auto& value = *reinterpret_cast<Pending*>(arg);
        // Include all existing handles, not just ready Explorer windows.
        if (value.before.size() >= 16384) return FALSE;
        try { value.before.insert(window); return TRUE; } catch (...) { return FALSE; }
    }, reinterpret_cast<LPARAM>(&item))) snapshot_ok = false;
    if (!snapshot_ok) return ERROR_NOT_ENOUGH_MEMORY;
    item.started = GetTickCount64();
    item.expires = item.started + kRequestMs;
    const auto cookie = item.cookie;
    {
        std::lock_guard lock(Store().mutex); ExpireLocked();
        if (Store().requests.size() >= 8) return ERROR_BUSY;
        Store().requests.push_back(std::move(item));
    }
    DWORD error = ERROR_PATH_NOT_FOUND;
    try {
        wchar_t windows[32768]{};
        const UINT count = GetWindowsDirectoryW(windows, ARRAYSIZE(windows));
        if (count && count < ARRAYSIZE(windows)) {
            const std::wstring explorer = std::wstring(windows) + L"\\explorer.exe";
            auto command = shell_open_entry::Quote(explorer) +
                (work.request.intent == Intent::SelectFile ? L" /n,/select," : L" /n,/e,") +
                shell_open_entry::Quote(target);
            STARTUPINFOW startup{}; startup.cb = sizeof(startup);
            // Explicit desktop also excludes this user-requested launch from the
            // process bridge. No association, registry or takeover switch changes.
            startup.lpDesktop = const_cast<wchar_t*>(work.desktop.c_str());
            PROCESS_INFORMATION process{};
            if (create(explorer.c_str(), command.data(), nullptr, nullptr, FALSE,
                CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &startup, &process)) {
                CloseHandle(process.hThread); CloseHandle(process.hProcess); error = ERROR_SUCCESS;
            } else error = GetLastError();
        }
    } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    if (error) {
        std::lock_guard lock(Store().mutex);
        std::erase_if(Store().requests, [cookie](const auto& value) { return value.cookie == cookie; });
    }
    return error;
}
unsigned __stdcall Worker(void* argument) {
    std::unique_ptr<Work> work(static_cast<Work*>(argument));
    DWORD error = ERROR_GEN_FAILURE;
    HDESK desktop = nullptr;
    try {
        desktop = OpenDesktopW(work->desktop.c_str(), 0, FALSE,
            DESKTOP_READOBJECTS | DESKTOP_WRITEOBJECTS | DESKTOP_ENUMERATE | DESKTOP_CREATEWINDOW);
        if (!desktop) error = GetLastError();
        else error = Launch(*work, desktop, &CreateProcessW);
    } catch (...) { error = ERROR_NOT_ENOUGH_MEMORY; }
    if (work->owner && IsCompletionFor(work->owner, static_cast<LPARAM>(work->owner_cookie)))
        PostMessageW(work->owner, kCompletedMessage, error, static_cast<LPARAM>(work->owner_cookie));
    // Enumeration uses this explicit handle; the worker never switches its
    // thread desktop, creates a window, or pumps messages.
    if (desktop) CloseDesktop(desktop);
    workers.fetch_sub(1);
    return error;
}
}

bool IsCompletionFor(HWND owner, LPARAM cookie) noexcept {
    return owner && cookie && reinterpret_cast<ULONG_PTR>(GetPropW(owner, kOwner)) == static_cast<ULONG_PTR>(cookie);
}
DWORD OpenAsync(Request request, HWND owner) noexcept {
    if (!IsFilesystemPath(request.path)) return ERROR_INVALID_PARAMETER;
    try {
        auto work = std::make_unique<Work>(); work->request = std::move(request); work->owner = owner;
        work->desktop = DesktopName(GetCurrentThreadId());
        if (work->desktop.empty()) return ERROR_INVALID_HANDLE;
        if (owner) {
            DWORD pid = 0; const DWORD tid = GetWindowThreadProcessId(owner, &pid);
            if (pid != GetCurrentProcessId() || tid != GetCurrentThreadId()) return ERROR_INVALID_WINDOW_HANDLE;
            auto cookie = reinterpret_cast<ULONG_PTR>(GetPropW(owner, kOwner));
            if (!cookie) {
                cookie = next_cookie.fetch_add(1);
                if (!SetPropW(owner, kOwner, reinterpret_cast<HANDLE>(cookie))) return GetLastError();
            }
            work->owner_cookie = cookie;
        }
        if (workers.fetch_add(1) >= 4) { workers.fetch_sub(1); return ERROR_BUSY; }
        HANDLE thread = reinterpret_cast<HANDLE>(_beginthreadex(nullptr, 0, Worker, work.get(), 0, nullptr));
        if (!thread) {
            workers.fetch_sub(1);
            unsigned long error = 0; _get_doserrno(&error);
            return error ? error : ERROR_NOT_ENOUGH_MEMORY;
        }
        work.release(); CloseHandle(thread); return ERROR_SUCCESS;
    } catch (...) { return ERROR_NOT_ENOUGH_MEMORY; }
}
bool IsExplicitNativeWindow(HWND window) noexcept {
    if (!window || !GetPropW(window, kNative)) return false;
    DWORD pid = 0, tid = 0; FILETIME created{};
    if (!Identity(window, pid, tid, created)) return false;
    return reinterpret_cast<ULONG_PTR>(GetPropW(window, kPid)) == pid &&
        reinterpret_cast<ULONG_PTR>(GetPropW(window, kTid)) == tid &&
        reinterpret_cast<ULONG_PTR>(GetPropW(window, kCreatedLow)) == created.dwLowDateTime &&
        reinterpret_cast<ULONG_PTR>(GetPropW(window, kCreatedHigh)) == created.dwHighDateTime;
}
bool ShouldDelayVeil(HWND window) noexcept {
    try {
        if (IsExplicitNativeWindow(window)) return true;
        const auto desktop = DesktopName(GetWindowThreadProcessId(window, nullptr));
        std::lock_guard lock(Store().mutex); ExpireLocked();
        return std::any_of(Store().requests.begin(), Store().requests.end(), [&](const auto& item) { return NewWindow(item, window, desktop); });
    } catch (...) { return false; }
}
bool ShouldWaitForRequest(HWND window, const std::wstring& folder) noexcept {
    try {
        const auto desktop = DesktopName(GetWindowThreadProcessId(window, nullptr));
        const auto normalized = Normalize(folder);
        std::lock_guard lock(Store().mutex); ExpireLocked();
        const auto now = GetTickCount64();
        for (const auto& item : Store().requests)
            if (NewWindow(item, window, desktop) && !item.claimed &&
                (now - item.started < 1500 || Same(item.folder, normalized) || Same(item.canonical, normalized))) return true;
    } catch (...) {}
    return false;
}
bool RetainRequestedWindow(HWND window, const std::wstring& folder,
                           const std::vector<std::wstring>& selected, bool selection_read) noexcept {
    try {
        if (IsExplicitNativeWindow(window)) return true;
        if (!selection_read || folder.empty()) return false;
        const auto desktop = DesktopName(GetWindowThreadProcessId(window, nullptr));
        // No filesystem resolution while holding the small shared request lock.
        const auto normalized = Normalize(folder);
        std::wstring canonical;
        {
            std::lock_guard lock(Store().mutex); ExpireLocked();
            if (Store().requests.empty()) return false;
        }
        canonical = Canonical(folder);
        std::lock_guard lock(Store().mutex); ExpireLocked();
        const auto now = GetTickCount64();
        for (auto& item : Store().requests) {
            if (!NewWindow(item, window, desktop) ||
                (!Same(item.folder, normalized) && !Same(item.canonical, canonical))) continue;
            if (!item.selected.empty() && (selected.size() != 1 || !Same(item.selected, selected[0]))) continue;
            if (!Mark(window, item.cookie)) return false;
            // A same-path concurrent request is not uniquely attributable after
            // Explorer brokers the process. Briefly retain both matching new
            // windows rather than closing the explicitly requested native one.
            if (!item.claimed) { item.claimed = true; item.expires = (std::min)(item.expires, now + 400); }
            return true;
        }
    } catch (...) {}
    return false;
}

#ifdef PULSE_NATIVE_EXPLORER_TEST
DWORD LaunchForTest(const Request& request, decltype(&CreateProcessW) create) {
    Work work; work.request = request; work.desktop = DesktopName(GetCurrentThreadId()); return Launch(work, GetThreadDesktop(GetCurrentThreadId()), create);
}
void ClearPendingForTest() { std::lock_guard lock(Store().mutex); Store().requests.clear(); }
void ExpirePendingForTest() { std::lock_guard lock(Store().mutex); for (auto& item : Store().requests) item.expires = 0; }
#endif
}
