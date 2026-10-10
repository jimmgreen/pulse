// bitlocker_unlock.cpp — see bitlocker_unlock.h.
#include "bitlocker_unlock.h"

#include "../fs/bitlocker_volume.h"

#include <objbase.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <mutex>
#include <set>
#include <thread>

namespace pulse::app {
namespace {

constexpr ULONGLONG kMaxWaitMs = 30ull * 60 * 1000;  // a prompt left open does not pin a thread forever
constexpr DWORD kPollMs = 400;
constexpr ULONGLONG kClosedGraceMs = 1500;           // bdeunlock can hand over to another instance

std::mutex g_mutex;
std::set<std::wstring> g_pending;

bool UnlockPromptOpen() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool open = false;
    for (BOOL more = Process32FirstW(snapshot, &entry); more && !open; more = Process32NextW(snapshot, &entry))
        open = _wcsicmp(entry.szExeFile, L"bdeunlock.exe") == 0;
    CloseHandle(snapshot);
    return open;
}

// The registered verb first (what File Explorer runs), then its command.
HANDLE LaunchPrompt(HWND owner, const std::wstring& root, bool& launched) {
    SHELLEXECUTEINFOW execute{sizeof(execute)};
    execute.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    execute.hwnd = owner;
    execute.lpVerb = L"unlock-bde";
    execute.lpFile = root.c_str();
    execute.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&execute)) {
        launched = true;
        return execute.hProcess;
    }
    wchar_t system[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (!length || length >= MAX_PATH) return nullptr;
    std::wstring command = L"\"" + std::wstring(system) + L"\\bdeunlock.exe\" " + root;
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process))
        return nullptr;
    CloseHandle(process.hThread);
    launched = true;
    return process.hProcess;
}

bool VolumeAccessible(const std::wstring& root) {
    DWORD previous = 0;
    const bool mode_set = SetThreadErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX, &previous) != FALSE;
    const bool accessible = GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (mode_set) SetThreadErrorMode(previous, nullptr);
    return accessible;
}

void WaitForUnlock(HWND hwnd, UINT msg, std::wstring root) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool launched = UnlockPromptOpen();
    HANDLE process = launched ? nullptr : LaunchPrompt(hwnd, root, launched);
    bool unlocked = false;
    const ULONGLONG deadline = GetTickCount64() + kMaxWaitMs;
    ULONGLONG closed_since = 0;
    while (launched && GetTickCount64() < deadline) {
        if (VolumeAccessible(root)) { unlocked = true; break; }
        if (!fs::IsBitLockerLocked(root)) break;  // ejected or detached meanwhile
        const bool open = (process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT) || UnlockPromptOpen();
        const ULONGLONG now = GetTickCount64();
        if (open) closed_since = 0;
        else if (!closed_since) closed_since = now;
        else if (now - closed_since >= kClosedGraceMs) break;
        Sleep(kPollMs);
    }
    if (process) CloseHandle(process);
    if (SUCCEEDED(com)) CoUninitialize();
    {
        std::lock_guard lock(g_mutex);
        g_pending.erase(root);
    }
    auto* result = new BitLockerUnlockResult{std::move(root), unlocked};
    if (!IsWindow(hwnd) || !PostMessageW(hwnd, msg, 0, reinterpret_cast<LPARAM>(result))) delete result;
}

} // namespace

bool StartBitLockerUnlock(HWND hwnd, UINT msg, const std::wstring& path) {
    const std::wstring root = fs::DriveRootOf(path);
    if (root.empty()) return false;
    {
        std::lock_guard lock(g_mutex);
        if (!g_pending.insert(root).second) return false;
    }
    try {
        std::thread(WaitForUnlock, hwnd, msg, root).detach();
    } catch (...) {
        std::lock_guard lock(g_mutex);
        g_pending.erase(root);
        return false;
    }
    return true;
}

} // namespace pulse::app
