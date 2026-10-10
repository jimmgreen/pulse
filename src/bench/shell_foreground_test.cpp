// Exercises the real ShellClient with an isolated host, including an editor's
// second process activating an existing window (the Notepad++ launch pattern).
#include "../ipc/shell_client.h"

#include <filesystem>
#include <cstdio>
#include <mutex>
#include <map>

namespace {

using namespace pulse::ipc;
std::mutex result_mutex;
std::map<uint32_t, uint32_t> results;

HWND CreateTestWindow(const wchar_t* title) {
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseShellForegroundTest";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(0, wc.lpszClassName, title, WS_OVERLAPPEDWINDOW,
                                 40, 40, 320, 160, nullptr, nullptr, wc.hInstance, nullptr);
    if (window) ShowWindow(window, SW_SHOWNOACTIVATE);
    return window;
}

void Pump() {
    MSG msg{};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

bool WaitFor(HANDLE process, DWORD timeout) {
    const auto deadline = GetTickCount64() + timeout;
    while (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
        if (GetTickCount64() >= deadline) return false;
        Pump();
        Sleep(10);
    }
    return true;
}

int RunChild(const std::wstring& command) {
    auto mutable_command = command;
    STARTUPINFOW si{ sizeof(si) };
    si.dwFlags = STARTF_FORCEOFFFEEDBACK | STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, mutable_command.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        std::printf("[FAIL] child launch failed: %lu\n", GetLastError());
        return 2;
    }
    CloseHandle(pi.hThread);
    const bool completed = WaitFor(pi.hProcess, 10000);
    if (!completed) {
        TerminateProcess(pi.hProcess, 2);
        WaitForSingleObject(pi.hProcess, 2000);
    }
    DWORD exit_code = 2;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exit_code);
}

std::wstring ExePath() {
    wchar_t path[32768]{};
    GetModuleFileNameW(nullptr, path, ARRAYSIZE(path));
    return path;
}

int Host(DWORD parent_pid) {
    HWND window = CreateTestWindow(L"Pulse foreground test: background editor");
    if (!window) return 2;
    const auto name = PipeNameFor(parent_pid);
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
                                  PIPE_TYPE_BYTE | PIPE_WAIT, 1, 4096, 4096, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 2;
    std::thread worker([&] {
        OVERLAPPED connection{};
        connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        DWORD ignored = 0;
        bool connected = ConnectNamedPipe(pipe, &connection) != FALSE;
        if (!connected) {
            const auto error = GetLastError();
            connected = error == ERROR_PIPE_CONNECTED ||
                (error == ERROR_IO_PENDING && GetOverlappedResult(pipe, &connection, &ignored, TRUE));
        }
        CloseHandle(connection.hEvent);
        while (connected) {
            MsgHeader request{};
            if (!PipeRead(pipe, reinterpret_cast<uint8_t*>(&request), sizeof(request)) ||
                request.magic != kMagic || request.payload_size > kMaxPayload) break;
            std::vector<uint8_t> payload(request.payload_size);
            if (!payload.empty() && !PipeRead(pipe, payload.data(), request.payload_size)) break;
            uint32_t session = 0, item = 0;
            PayloadReader reader(payload.data(), payload.size());
            const bool nested = request.type == REQ_CTX_INVOKE &&
                reader.GetU32(session) && reader.GetU32(item) && item == 2;
            const bool activated = nested
                ? RunChild(L"\"" + ExePath() + L"\" --editor " +
                           std::to_wstring(reinterpret_cast<UINT_PTR>(window))) == 0
                : SetForegroundWindow(window) != FALSE;
            // Let the test client reclaim focus after the attempted activation.
            AllowSetForegroundWindow(parent_pid);
            PayloadWriter response;
            response.PutU32(activated ? S_OK : E_ACCESSDENIED);
            response.PutU32(0);
            response.PutString(L"");
            MsgHeader header;
            header.type = RSP_DONE;
            header.request_id = request.request_id;
            header.payload_size = static_cast<uint32_t>(response.data().size());
            if (!PipeWrite(pipe, reinterpret_cast<const uint8_t*>(&header), sizeof(header)) ||
                !PipeWrite(pipe, response.data().data(), header.payload_size)) break;
        }
        PostMessageW(window, WM_APP, 0, 0);
    });
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0 && msg.message != WM_APP) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    worker.join();
    CloseHandle(pipe);
    DestroyWindow(window);
    return 0;
}

uint32_t Result(uint32_t request) {
    const auto deadline = GetTickCount64() + 5000;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard lock(result_mutex);
            auto it = results.find(request);
            if (it != results.end()) return it->second;
        }
        Pump();
        Sleep(10);
    }
    return static_cast<uint32_t>(E_PENDING);
}

bool FocusClient(HWND window) {
    // Click only this fixture's client area to establish real input ownership and
    // expire the host's startup/granted permission, regardless of the test runner.
    POINT cursor{};
    GetCursorPos(&cursor);
    RECT rect{};
    GetWindowRect(window, &rect);
    SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetForegroundWindow(window);
    Pump();
    INPUT input[3]{};
    for (auto& event : input) event.type = INPUT_MOUSE;
    input[0].mi.dx = (rect.left + 80) * 65535 / (GetSystemMetrics(SM_CXSCREEN) - 1);
    input[0].mi.dy = (rect.top + 90) * 65535 / (GetSystemMetrics(SM_CYSCREEN) - 1);
    input[0].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input[1].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    const UINT sent = SendInput(3, input, sizeof(INPUT));
    const auto deadline = GetTickCount64() + 1000;
    do {
        Pump();
        Sleep(10);
    } while (GetForegroundWindow() != window && GetTickCount64() < deadline);
    SetWindowPos(window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    SetCursorPos(cursor.x, cursor.y);
    const bool focused = sent == 3 && GetForegroundWindow() == window;
    if (!focused) {
        DWORD foreground_pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &foreground_pid);
        std::printf("  input events=%u, client pid=%lu, foreground pid=%lu, rect=%ld,%ld,%ld,%ld\n",
                    sent, GetCurrentProcessId(), foreground_pid,
                    rect.left, rect.top, rect.right, rect.bottom);
    }
    return focused;
}

int Client() {
    HWND previous = GetForegroundWindow();
    HWND window = CreateTestWindow(L"Pulse foreground test: menu owner");
    if (!window || !FocusClient(window)) {
        std::printf("[FAIL] fixture could not receive desktop input\n");
        if (window) DestroyWindow(window);
        return 2;
    }
    auto& client = ShellClient::Instance();
    ShellClient::Callbacks callbacks;
    callbacks.done = [](uint32_t id, uint32_t hr, bool, std::wstring) {
        std::lock_guard lock(result_mutex);
        results[id] = hr;
    };
    client.Start(std::move(callbacks));
    // Consume the freshly spawned host's startup activation permission first.
    client.Ping();
    Result(1);
    bool ok = true;
    auto check = [&](bool passed, const char* text) {
        std::printf("[%s] %s\n", passed ? "PASS" : "FAIL", text);
        ok &= passed;
    };
    check(FocusClient(window), "menu owner receives input before the background query");
    check(Result(client.QueryContextMenu({ L"fixture.txt" }, 0, false, false)) == E_ACCESSDENIED,
          "querying a menu does not grant foreground activation");
    check(FocusClient(window), "menu owner has focus before invocation");
    check(Result(client.InvokeContextMenu(1, 1)) == S_OK,
          "invoking a menu grants foreground activation to the host");
    check(FocusClient(window), "menu owner receives new input before the editor handoff");
    check(Result(client.InvokeContextMenu(1, 2)) == S_OK,
          "an editor launcher can activate its existing window through the host");
    check(FocusClient(window), "menu owner receives input after invocation");
    check(Result(client.QueryContextMenu({ L"fixture.txt" }, 0, false, false)) == E_ACCESSDENIED,
          "subsequent background queries do not retain activation permission");
    client.Stop();
    if (previous && IsWindow(previous)) SetForegroundWindow(previous);
    DestroyWindow(window);
    return ok ? 0 : 1;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    SetProcessDPIAware();
    if (argc == 2 && wcscmp(argv[1], L"--client") == 0) return Client();
    if (argc == 3 && wcscmp(argv[1], L"--editor") == 0)
        return SetForegroundWindow(reinterpret_cast<HWND>(_wcstoui64(argv[2], nullptr, 10))) ? 0 : 1;
    if (argc == 2) return Host(wcstoul(argv[1], nullptr, 10));
    const auto dir = std::filesystem::temp_directory_path() /
        (L"pulse-shell-foreground-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(dir);
    const auto client = dir / L"test.exe";
    const auto host = dir / L"pulse_shell.exe";
    std::filesystem::copy_file(ExePath(), client);
    std::filesystem::copy_file(ExePath(), host);
    const int result = RunChild(L"\"" + client.wstring() + L"\" --client");
    std::filesystem::remove(client);
    std::filesystem::remove(host);
    std::filesystem::remove(dir);
    return result;
}
