// App-private message probe. No desktop clicks, key injection, or user-file edits.
#include <windows.h>
#include <cstdio>
#include <cwchar>
#include <vector>
#include "../app/app_messages.h"
#include "../app/global_search_hotkey.h"

struct Windows { DWORD pid = 0; HWND main = nullptr, search = nullptr; };
BOOL CALLBACK Find(HWND hwnd, LPARAM context) {
    auto& windows = *reinterpret_cast<Windows*>(context);
    DWORD pid = 0; GetWindowThreadProcessId(hwnd, &pid);
    if (pid != windows.pid) return TRUE;
    wchar_t name[128]{}; GetClassNameW(hwnd, name, 128);
    if (wcscmp(name, L"Pulse.GlobalSearch") == 0) windows.search = hwnd;
    if (wcscmp(name, L"PulseMainWindow") == 0) windows.main = hwnd;
    return TRUE;
}
Windows Query(DWORD pid) { Windows result{pid}; EnumWindows(Find, reinterpret_cast<LPARAM>(&result)); return result; }
void Integrity(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid), token = nullptr;
    if (process && OpenProcessToken(process, TOKEN_QUERY, &token)) {
        DWORD bytes = 0;
        GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &bytes);
        std::vector<BYTE> data(bytes);
        if (GetTokenInformation(token, TokenIntegrityLevel, data.data(), bytes, &bytes)) {
            const auto sid = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(data.data())->Label.Sid;
            std::printf("integrity pid=%lu rid=%lu\n", pid,
                *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1));
        } else std::printf("integrity pid=%lu error=%lu\n", pid, GetLastError());
        CloseHandle(token);
    } else std::printf("integrity pid=%lu error=%lu\n", pid, GetLastError());
    if (process) CloseHandle(process);
}
void Snapshot(const Windows& windows) {
    GUITHREADINFO info{sizeof(info)};
    const auto thread = GetWindowThreadProcessId(windows.search ? windows.search : windows.main, nullptr);
    const bool gui = thread && GetGUIThreadInfo(thread, &info);
    std::printf("pid=%lu main=%p search=%p visible=%d foreground=%p active=%p focus=%p topmost=%d\n",
        windows.pid, static_cast<void*>(windows.main), static_cast<void*>(windows.search),
        windows.search && IsWindowVisible(windows.search), static_cast<void*>(GetForegroundWindow()),
        gui ? static_cast<void*>(info.hwndActive) : nullptr, gui ? static_cast<void*>(info.hwndFocus) : nullptr,
        windows.search && (GetWindowLongPtrW(windows.search, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 3 && argc != 4) return 2;
    FILE* output = nullptr;
    if (argc == 4 && _wfreopen_s(&output, argv[3], L"w", stdout)) return 5;
    const DWORD pid = wcstoul(argv[2], nullptr, 10);
    if (wcscmp(argv[1], L"integrity") == 0) { Integrity(pid ? pid : GetCurrentProcessId()); return 0; }
    auto windows = Query(pid);
    if (!windows.main) { Snapshot(windows); std::puts("main window not found"); return 3; }
    if (wcscmp(argv[1], L"inspect") == 0) { Snapshot(windows); return 0; }
    if (wcscmp(argv[1], L"exit") == 0) {
        const HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
        if (!process) return 4;
        const BOOL posted = PostMessageW(windows.main, pulse::WM_EXIT_PULSE, 0, 0);
        const DWORD post_error = posted ? ERROR_SUCCESS : GetLastError();
        const DWORD wait = posted ? WaitForSingleObject(process, 15000) : WAIT_FAILED;
        CloseHandle(process);
        std::printf("WM_EXIT_PULSE posted=%d error=%lu wait=%lu\n", posted, post_error, wait);
        return wait == WAIT_OBJECT_0 ? 0 : 1;
    }
    if (wcscmp(argv[1], L"summon") == 0) {
        const BOOL posted = PostMessageW(windows.main, WM_HOTKEY, pulse::GlobalSearchHotkey::kId,
                                        MAKELPARAM(MOD_CONTROL | MOD_ALT, VK_F10));
        const auto deadline = GetTickCount64() + 8000;
        do { Sleep(20); windows = Query(pid); }
        while ((!windows.search || !IsWindowVisible(windows.search)) && GetTickCount64() < deadline);
        Snapshot(windows);
        std::printf("app-private WM_HOTKEY posted=%d; this is NOT a physical hotkey test\n", posted);
        return posted && windows.search && IsWindowVisible(windows.search) ? 0 : 1;
    }
    if (wcscmp(argv[1], L"dismiss") == 0 && windows.search) {
        const BOOL posted = PostMessageW(windows.search, WM_CLOSE, 0, 0);
        const auto deadline = GetTickCount64() + 3000;
        while (IsWindowVisible(windows.search) && GetTickCount64() < deadline) Sleep(10);
        Snapshot(Query(pid));
        return posted && !IsWindowVisible(windows.search) ? 0 : 1;
    }
    return 2;
}
