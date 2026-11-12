#define _CRT_RAND_S
#include <cstdlib>
#include <windows.h>
#include <cstdio>
#include <string>
#include <cstdarg>

constexpr UINT kProbe = WM_APP + 73;
struct State { volatile LONG callbacks, seen, early; DWORD pid, tid, callback_pid, callback_tid, elevated, restricted, integrity; };
static std::wstring Name(DWORD pid) {
    wchar_t nonce[128]{}; GetEnvironmentVariableW(L"PULSE_MESSAGE_PROBE_NONCE", nonce, 128);
    return L"Local\\PulseMessageEntryProbe-" + std::wstring(nonce) + L"-" + std::to_wstring(pid);
}
#ifdef PROBE_DLL
extern "C" __declspec(dllexport) LRESULT CALLBACK ProbeHook(int code, WPARAM wp, LPARAM lp) {
    if (code >= 0 && wp == PM_REMOVE && reinterpret_cast<MSG*>(lp)->message == kProbe) {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, Name(GetCurrentProcessId()).c_str());
        if (mapping) {
            auto* state = static_cast<State*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
            if (state) {
                state->callback_pid = GetCurrentProcessId(); state->callback_tid = GetCurrentThreadId();
                InterlockedIncrement(&state->callbacks); UnmapViewOfFile(state);
            }
            CloseHandle(mapping);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}
#else
static HANDLE report = INVALID_HANDLE_VALUE;
static void Print(const char* format, ...) {
    char line[1024]{}; va_list args; va_start(args, format); vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    std::fputs(line, stdout); std::fflush(stdout);
    if (report != INVALID_HANDLE_VALUE) { DWORD written = 0; WriteFile(report, line, static_cast<DWORD>(strlen(line)), &written, nullptr); FlushFileBuffers(report); }
}
static DWORD Elevated() {
    HANDLE token = nullptr; TOKEN_ELEVATION elevation{}; DWORD bytes = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 2;
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &bytes); CloseHandle(token);
    return ok ? elevation.TokenIsElevated : 2;
}
static void TokenStats(DWORD& restricted, DWORD& integrity) {
    restricted = integrity = MAXDWORD; HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return;
    restricted = IsTokenRestricted(token) != FALSE;
    alignas(TOKEN_MANDATORY_LABEL) BYTE buffer[512]{}; DWORD bytes = 0;
    if (GetTokenInformation(token, TokenIntegrityLevel, buffer, sizeof(buffer), &bytes)) {
        const auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer);
        integrity = *GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(*GetSidSubAuthorityCount(label->Label.Sid) - 1));
    }
    CloseHandle(token);
}
static DWORD WINAPI Early(void* value) { InterlockedIncrement(&static_cast<State*>(value)->early); return 0; }
static bool WaitValue(volatile LONG* value, LONG wanted) {
    const ULONGLONG until = GetTickCount64() + 5000;
    while (GetTickCount64() < until) { if (InterlockedCompareExchange(value, 0, 0) >= wanted) return true; Sleep(10); }
    return false;
}
static int Child() {
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(State), Name(GetCurrentProcessId()).c_str());
    if (!map) return 20;
    if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(map); return 24; }
    auto* state = static_cast<State*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
    if (!state) { CloseHandle(map); return 21; }
    HANDLE worker = CreateThread(nullptr, 0, Early, state, 0, nullptr);
    if (!worker) return 22;
    if (WaitForSingleObject(worker, 5000) != WAIT_OBJECT_0) return 23;
    CloseHandle(worker);
    MSG msg{}; PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);
    TokenStats(state->restricted, state->integrity);
    state->elevated = Elevated(); state->pid = GetCurrentProcessId(); state->tid = GetCurrentThreadId();
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { if (msg.message == kProbe) InterlockedIncrement(&state->seen); }
    UnmapViewOfFile(state); CloseHandle(map); return 0;
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--child") return Child();
    if (argc != 2 && argc != 4) return 2;
    wchar_t self[32768]{}; if (!GetModuleFileNameW(nullptr, self, 32768)) return 3;
    if (argc == 4) {
        const std::wstring root = std::wstring(self).substr(0, std::wstring(self).find_last_of(L"\\") + 1);
        const std::wstring path = argv[3];
        if (std::wstring(argv[2]) != L"--report" || path.size() <= root.size() || path.substr(0, root.size()) != root ||
            path.substr(root.size()).find_first_of(L"\\/:.") != std::wstring::npos) return 5;
        for (size_t end = 3; end < root.size(); ++end) if (root[end] == L'\\') {
            const DWORD attr = GetFileAttributesW(root.substr(0, end).c_str());
            if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_REPARSE_POINT)) return 6;
        }
        report = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (report == INVALID_HANDLE_VALUE) return 7;
    }
    Print("START parent_pid=%lu parent_elevated=%lu\n", GetCurrentProcessId(), Elevated());
    DWORD restricted = 0, integrity = 0; TokenStats(restricted, integrity);
    Print("TOKEN parent_restricted=%lu parent_integrity=%lu\n", restricted, integrity);
    unsigned int random[4]{};
    for (auto& part : random) if (rand_s(&part)) return 8;
    wchar_t nonce[128]{}; swprintf_s(nonce, L"%08x%08x%08x%08x", random[0], random[1], random[2], random[3]);
    if (!SetEnvironmentVariableW(L"PULSE_MESSAGE_PROBE_NONCE", nonce)) return 9;
    std::wstring command = L"\"" + std::wstring(self) + L"\" --child";
    STARTUPINFOW startup{}; startup.cb = sizeof(startup); PROCESS_INFORMATION process{};
    if (!CreateProcessW(self, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) return 4;
    Print("CREATED child_pid=%lu child_tid=%lu\n", process.dwProcessId, process.dwThreadId);
    FILETIME created{}, exit_time{}, kernel{}, user{};
    const bool identity = GetProcessTimes(process.hProcess, &created, &exit_time, &kernel, &user) &&
        GetProcessId(process.hProcess) == process.dwProcessId && GetProcessIdOfThread(process.hThread) == process.dwProcessId;
    HANDLE map = nullptr; State* state = nullptr; HMODULE dll = nullptr; HHOOK hook = nullptr; int result = 1;
    for (int i = 0; i != 500 && !map; ++i) { map = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, Name(process.dwProcessId).c_str()); if (!map) Sleep(10); }
    if (map) state = static_cast<State*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(State)));
    if (identity && state) {
        for (int i = 0; i != 500 && !state->tid; ++i) Sleep(10);
        FILETIME check{};
        const bool bound = state->pid == process.dwProcessId && state->tid == process.dwThreadId &&
            GetProcessTimes(process.hProcess, &check, &exit_time, &kernel, &user) && CompareFileTime(&created, &check) == 0;
        if (bound && PostThreadMessageW(state->tid, kProbe, 0, 0) && WaitValue(&state->seen, 1) && state->callbacks == 0 && state->early == 1) {
            Print("BASELINE early_worker=1 seen=1 callbacks=0 child_elevated=%lu PASS\n", state->elevated);
            Print("TOKEN child_restricted=%lu child_integrity=%lu\n", state->restricted, state->integrity);
            dll = LoadLibraryW(argv[1]);
            auto proc = dll ? reinterpret_cast<HOOKPROC>(GetProcAddress(dll, "ProbeHook")) : nullptr;
            hook = proc ? SetWindowsHookExW(WH_GETMESSAGE, proc, dll, state->tid) : nullptr;
            Print("HOOK dll_loaded=%d proc_found=%d installed=%d error=%lu\n", dll != nullptr, proc != nullptr, hook != nullptr, hook ? 0UL : GetLastError());
            const bool delivered = hook && PostThreadMessageW(state->tid, kProbe, 0, 0) && WaitValue(&state->seen, 2);
            Print("OBSERVED delivered=%d seen=%ld callbacks=%ld callback_pid=%lu callback_tid=%lu\n", delivered, state->seen, state->callbacks, state->callback_pid, state->callback_tid);
            if (delivered && state->callbacks == 1 &&
                state->callback_pid == process.dwProcessId && state->callback_tid == process.dwThreadId) {
                Print("MODIFIED callbacks=1 child_pid=%lu message_tid=%lu callback_pid=%lu callback_tid=%lu PASS\n", state->pid, state->tid, state->callback_pid, state->callback_tid);
                if (UnhookWindowsHookEx(hook)) {
                    hook = nullptr;
                    if (PostThreadMessageW(state->tid, kProbe, 0, 0) && WaitValue(&state->seen, 3) && state->callbacks == 1) {
                        Print("ROLLBACK seen=3 callbacks=1 no_new_callback PASS\n"); result = 0;
                    }
                }
            }
        }
    }
    if (hook) UnhookWindowsHookEx(hook);
    if (identity) {
        PostThreadMessageW(process.dwThreadId, WM_QUIT, 0, 0);
        if (WaitForSingleObject(process.hProcess, 5000) != WAIT_OBJECT_0) { TerminateProcess(process.hProcess, 30); WaitForSingleObject(process.hProcess, 5000); result = 1; }
    }
    DWORD child_exit = STILL_ACTIVE; GetExitCodeProcess(process.hProcess, &child_exit);
    if (child_exit != 0) result = 1;
    if (state) UnmapViewOfFile(state);
    if (map) CloseHandle(map);
    if (dll) FreeLibrary(dll);
    CloseHandle(process.hThread); CloseHandle(process.hProcess);
    HANDLE residual = OpenFileMappingW(FILE_MAP_READ, FALSE, Name(process.dwProcessId).c_str());
    if (residual) { CloseHandle(residual); result = 1; }
    Print("CLEANUP child_exit=%lu mapping_remaining=%d result=%d\n", child_exit, residual != nullptr, result);
    if (report != INVALID_HANDLE_VALUE) CloseHandle(report);
    return result;
}
#endif
