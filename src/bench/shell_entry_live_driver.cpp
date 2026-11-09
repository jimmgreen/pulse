#include "../app/shell_integration_registry.h"
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <cstdio>
#include <string>
#include <filesystem>

namespace {
std::wstring Quote(const wchar_t* argument) {
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t* p = argument; *p; ++p) {
        if (*p == L'\\') { ++slashes; continue; }
        result.append(*p == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        result += *p;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
}

// Standalone driver: real HKCU registration implementation and real Windows
// entry points. The PowerShell owner snapshots/restores state around each run.
int wmain(int argc, wchar_t** argv) {
    wchar_t self[32768]{};
    if (!GetModuleFileNameW(nullptr, self, ARRAYSIZE(self))) return 2;
    const std::filesystem::path location(self);
    if (location.filename() == L"pulse_coverage_relay.exe") {
        const auto run = std::filesystem::canonical(location.parent_path());
        if (!run.filename().wstring().starts_with(L"shell-entry-live-") ||
            run.parent_path().filename() != L"bench_data") return 2;
        wchar_t target[32768]{};
        GetPrivateProfileStringW(L"relay", L"pulse", L"", target, ARRAYSIZE(target),
            (run / L"relay.ini").c_str());
        std::error_code error;
        const auto executable = std::filesystem::canonical(target, error);
        if (error || executable.filename() != L"pulse.exe" ||
            executable.parent_path().parent_path() != run.parent_path().parent_path()) return 2;
        SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", run.c_str());
        SetEnvironmentVariableW(L"PULSE_TEST_LIVE_SHELL_ENTRY", L"1");
        SetEnvironmentVariableW(L"PULSE_TEST_WIN_E_HOTKEY", L"1");
        auto command = Quote(executable.c_str());
        for (int i = 1; i < argc; ++i) command += L" " + Quote(argv[i]);
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process)) return 1;
        // Save only the child PID so the script can stop precisely its own process.
        const auto pid_file = CreateFileW((run / L"relay-pid.txt").c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (pid_file != INVALID_HANDLE_VALUE) {
            const auto pid = std::to_string(process.dwProcessId);
            DWORD written = 0;
            WriteFile(pid_file, pid.data(), static_cast<DWORD>(pid.size()), &written, nullptr);
            CloseHandle(pid_file);
        }
        CloseHandle(process.hThread); CloseHandle(process.hProcess);
        return 0;
    }
    if (argc == 3 && wcscmp(argv[1], L"select") == 0) {
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(initialized)) return 1;
        PIDLIST_ABSOLUTE item = nullptr;
        HRESULT result = SHParseDisplayName(argv[2], nullptr, &item, 0, nullptr);
        if (SUCCEEDED(result)) {
            // A full item PIDL with cidl=0 asks Shell to open its parent and
            // select the item, exactly as callers of the public API do.
            result = SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
            CoTaskMemFree(item);
        }
        std::wprintf(L"SHOPENFOLDERANDSELECTITEMS\tresult=0x%08lx\t%s\n",
            static_cast<unsigned long>(result), argv[2]);
        CoUninitialize();
        return SUCCEEDED(result) ? 0 : 1;
    }
    if (argc == 2 && wcscmp(argv[1], L"hotkey-capability") == 0) {
        const bool registered = RegisterHotKey(nullptr, 0x5055, MOD_WIN | MOD_NOREPEAT, 'E') != FALSE;
        const DWORD error = registered ? 0 : GetLastError();
        const bool released = !registered || UnregisterHotKey(nullptr, 0x5055) != FALSE;
        std::printf("REGISTER-HOTKEY\tWin+E\tregistered=%u\terror=%lu\treleased=%u\n", registered, error, released);
        return released ? 0 : 1;
    }
    if (argc == 3 && (wcscmp(argv[1], L"native") == 0 ||
        wcscmp(argv[1], L"native-path") == 0 || wcscmp(argv[1], L"native-select") == 0)) {
        wchar_t windows[MAX_PATH]{};
        if (!GetWindowsDirectoryW(windows, ARRAYSIZE(windows))) return 2;
        const std::wstring executable = std::wstring(windows) + L"\\explorer.exe";
        const auto arguments = wcscmp(argv[1], L"native-path") == 0 ? Quote(argv[2]) :
            wcscmp(argv[1], L"native-select") == 0 ? L"/select," + Quote(argv[2]) : std::wstring(argv[2]);
        auto command = L"\"" + executable + L"\" " + arguments;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        const bool started = CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
            0, nullptr, nullptr, &startup, &process) != FALSE;
        std::wprintf(L"NATIVE-CREATEPROCESS\t%u\terror=%lu\tpid=%lu\t%s\n", started,
            started ? 0 : GetLastError(), process.dwProcessId, command.c_str());
        if (started) { CloseHandle(process.hThread); CloseHandle(process.hProcess); }
        return started ? 0 : 1;
    }
    if (argc == 5 && wcscmp(argv[1], L"apply") == 0) {
        using pulse::app::ShellIntegrationKind;
        ShellIntegrationKind kind;
        const std::wstring name = argv[2];
        if (name == L"folders") kind = ShellIntegrationKind::Folders;
        else if (name == L"wine") kind = ShellIntegrationKind::WinE;
        else if (name == L"pc") kind = ShellIntegrationKind::ThisPc;
        else if (name == L"recycle") kind = ShellIntegrationKind::RecycleBin;
        else return 2;
        const bool success = pulse::app::ApplyShellIntegration(kind, argv[4], wcscmp(argv[3], L"on") == 0);
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSH, nullptr, nullptr);
        const auto failure = pulse::app::LastShellIntegrationFailure();
        std::wprintf(L"APPLY\t%s\t%s\t%d\terror=%ld\t%s\n", argv[2], argv[3], success,
            failure.status, failure.key.c_str());
        return success ? 0 : 1;
    }
    if (argc == 3 && wcscmp(argv[1], L"shell") == 0) {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        SHELLEXECUTEINFOW request{sizeof(request)};
        request.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        request.lpFile = argv[2];
        request.nShow = SW_SHOWNORMAL;
        const bool success = ShellExecuteExW(&request) != FALSE;
        std::wprintf(L"SHELLEXECUTE\t%d\terror=%lu\tpid=%lu\t%s\n", success,
            success ? 0 : GetLastError(), request.hProcess ? GetProcessId(request.hProcess) : 0, request.lpFile);
        if (request.hProcess) CloseHandle(request.hProcess);
        CoUninitialize();
        return success ? 0 : 1;
    }
    if (argc == 2 && wcscmp(argv[1], L"wine") == 0) {
        wchar_t thread_desktop[128]{}, input_desktop[128]{}, foreground_class[128]{};
        DWORD bytes = 0, foreground_pid = 0;
        GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, thread_desktop, sizeof(thread_desktop), &bytes);
        const auto input = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (input) { GetUserObjectInformationW(input, UOI_NAME, input_desktop, sizeof(input_desktop), &bytes); CloseDesktop(input); }
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        GetClassNameW(GetForegroundWindow(), foreground_class, ARRAYSIZE(foreground_class));
        std::wprintf(L"INPUT-CONTEXT\tthread=%s\tinput=%s\tforeground=%s\tpid=%lu\tctrl=%u\talt=%u\tshift=%u\n",
            thread_desktop, input_desktop, foreground_class, foreground_pid,
            (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0, (GetAsyncKeyState(VK_MENU) & 0x8000) != 0,
            (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0);
        INPUT keys[4]{};
        for (auto& key : keys) key.type = INPUT_KEYBOARD;
        keys[0].ki.wVk = VK_LWIN;
        keys[1].ki.wVk = 'E';
        keys[2].ki.wVk = 'E'; keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
        keys[3].ki.wVk = VK_LWIN; keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
        const auto sent = SendInput(4, keys, sizeof(INPUT));
        std::printf("SENDINPUT\tWin+E\t%u/4\n", sent);
        return sent == 4 ? 0 : 1;
    }
    return 2;
}
