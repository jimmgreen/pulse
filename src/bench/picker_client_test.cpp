#include "../ipc/picker_session.h"
#include "../ipc/picker_client.h"

#include <iostream>
#include <thread>
#include "../common/current_user_security.h"

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0, ticks = 0;
HANDLE cancellation = nullptr;
HANDLE host_ready = nullptr;
bool reenter = false, destroy_on_disable = false, destroy_on_timer = false;
Status nested_status = Status::Accepted;
DWORD nested_error = 0;
std::wstring client_executable;
Message nested_request;
HWND replacement = nullptr;
void Check(bool value, const char* name) {
    ++checks;
    if (!value) ++failures;
    std::cout << (value ? "[PASS] " : "[FAIL] ") << name << std::endl;
}
LRESULT CALLBACK OwnerProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == WM_ENABLE && !wp && destroy_on_disable) {
        destroy_on_disable = false;
        DestroyWindow(window);
        return 0;
    }
    if (message == WM_TIMER) {
        ++ticks;
        if (reenter) {
            reenter = false;
            const auto nested = RunPickerClient(client_executable, nested_request, window);
            nested_status = std::get<Response>(nested.body).status;
            nested_error = std::get<Response>(nested.body).error_code;
        }
        if (destroy_on_timer && host_ready && WaitForSingleObject(host_ready, 0) == WAIT_OBJECT_0) {
            destroy_on_timer = false;
            DestroyWindow(window);
            replacement = CreateWindowW(L"PulsePickerClientOwner", L"replacement", WS_OVERLAPPED,
                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            EnableWindow(replacement, FALSE);
            return 0;
        }
        if (cancellation && (!host_ready || WaitForSingleObject(host_ready, 0) == WAIT_OBJECT_0)) SetEvent(cancellation);
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
int FakeHost(const wchar_t* pipe, const wchar_t* parent_pid) {
    PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, wcstoul(parent_pid, nullptr, 10)));
    Channel channel;
    if (!parent || !Connect(pipe, parent.Get(), channel)) return 2;
    Message request;
    if (!ReceivePickerSessionRequest(channel, request, {nullptr, 5000})) return 3;
    const auto& body = std::get<Request>(request.body);
    if (body.filename == u"crash") return 7;
    if (body.filename == u"hang") {
        const auto ready_name = std::wstring(L"Local\\PulsePickerClientReady.") + parent_pid;
        PipeHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name.c_str()));
        if (ready) SetEvent(ready.Get());
        Sleep(60000); return 8;
    }
    Sleep(200);
    Response answer;
    answer.operation = body.operation;
    answer.status = body.filename == u"unsupported" ? Status::Unsupported :
        body.filename == u"cancel" ? Status::Cancelled : Status::Accepted;
    if (answer.status == Status::Accepted) answer.paths = {u"C:\\isolated-picker-result.txt"};
    if (answer.status == Status::Unsupported) {
        answer.error_source = ErrorSource::Win32;
        answer.error_code = ERROR_NOT_SUPPORTED;
    }
    Message response{request.request_id, answer};
    if (body.filename == u"wrong-id") ++response.request_id[0];
    return channel.Send(response, {nullptr, 5000}) ? 0 : 4;
}
constexpr UINT kExternalCommand = WM_APP + 93;
struct ExternalShared { volatile UINT_PTR window = 0, replacement = 0; };
ExternalShared* external_shared = nullptr;
LRESULT CALLBACK ExternalProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kExternalCommand) {
        if (wp == 1) EnableWindow(window, TRUE);
        if (wp == 2) {
            DestroyWindow(window);
            HWND replacement_window = CreateWindowW(L"PulsePickerExternalOwner", L"disabled replacement", WS_OVERLAPPED | WS_DISABLED,
                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (external_shared) external_shared->replacement = reinterpret_cast<UINT_PTR>(replacement_window);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
int ExternalOwnerChild(const wchar_t* mapping_name, const wchar_t* ready_name) {
    PipeHandle mapping(OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name));
    PipeHandle ready(OpenEventW(EVENT_MODIFY_STATE, FALSE, ready_name));
    if (!mapping || !ready) return 11;
    auto* data = static_cast<ExternalShared*>(MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExternalShared)));
    if (!data) return 12;
    external_shared = data;
    WNDCLASSW wc{}; wc.lpfnWndProc = ExternalProc; wc.lpszClassName = L"PulsePickerExternalOwner"; wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND window = CreateWindowW(wc.lpszClassName, L"owned child disabled window", WS_OVERLAPPED | WS_DISABLED,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    data->window = reinterpret_cast<UINT_PTR>(window);
    SetEvent(ready.Get());
    if (window) {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (IsWindow(window)) DestroyWindow(window);
    if (data->replacement && IsWindow(reinterpret_cast<HWND>(data->replacement))) DestroyWindow(reinterpret_cast<HWND>(data->replacement));
    external_shared = nullptr; UnmapViewOfFile(data);
    return window ? 0 : 13;
}
struct ExternalFixture {
    PipeHandle mapping, ready, process;
    ExternalShared* data = nullptr;
    DWORD thread = 0, pid = 0;
    bool started = false;
    explicit ExternalFixture(const wchar_t* executable) {
        static LONG serial = 0;
        const auto prefix = L"Local\\PulsePickerExternal." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(InterlockedIncrement(&serial));
        pulse::CurrentUserSecurityAttributes security;
        if (!security) return;
        mapping.Reset(CreateFileMappingW(INVALID_HANDLE_VALUE, security.get(), PAGE_READWRITE, 0, sizeof(ExternalShared), prefix.c_str()));
        const auto ready_name = prefix + L".ready";
        ready.Reset(CreateEventW(security.get(), TRUE, FALSE, ready_name.c_str()));
        if (!mapping || !ready) return;
        data = static_cast<ExternalShared*>(MapViewOfFile(mapping.Get(), FILE_MAP_ALL_ACCESS, 0, 0, sizeof(ExternalShared)));
        if (!data) return;
        *data = {};
        std::wstring command = L"\"" + std::wstring(executable) + L"\" --owner-fixture " + prefix + L" " + ready_name;
        wchar_t desktop_name[1024]{}; DWORD needed = 0;
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop_name, sizeof(desktop_name), &needed)) return;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop_name;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &info)) return;
        process.Reset(info.hProcess); PipeHandle initial_thread(info.hThread); thread = info.dwThreadId; pid = info.dwProcessId;
        started = WaitForSingleObject(ready.Get(), 5000) == WAIT_OBJECT_0 && data->window;
    }
    HWND Window() const { return data ? reinterpret_cast<HWND>(data->window) : nullptr; }
    ~ExternalFixture() {
        if (process) {
            PostThreadMessageW(thread, WM_QUIT, 0, 0);
            const DWORD waited = WaitForSingleObject(process.Get(), 2000);
            if (waited != WAIT_OBJECT_0) { TerminateProcess(process.Get(), 91); WaitForSingleObject(process.Get(), 5000); }
            DWORD exit = 0; GetExitCodeProcess(process.Get(), &exit);
            Check(waited == WAIT_OBJECT_0 && exit == 0, "owned external window process cleaned up without termination");
        }
        if (data) UnmapViewOfFile(data);
    }
};
Status StatusOf(const Message& message) { return std::get<Response>(message.body).status; }
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring_view(argv[1]) == L"--owner-fixture") return ExternalOwnerChild(argv[2], argv[3]);
    // The client appends the caller's owner HWND when it has one.
    if ((argc == 4 || argc == 5) && std::wstring_view(argv[1]) == L"--picker-host") return FakeHost(argv[2], argv[3]);
    const auto name = L"PulsePickerClientTest." + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop && SetThreadDesktop(desktop), "isolated desktop selected");
    if (!desktop) return 1;
    WNDCLASSW wc{}; wc.lpfnWndProc = OwnerProc; wc.lpszClassName = L"PulsePickerClientOwner";
    wc.hInstance = GetModuleHandleW(nullptr);
    RegisterClassW(&wc);
    HWND owner = CreateWindowW(wc.lpszClassName, L"isolated owner", WS_OVERLAPPED, 0, 0, 100, 100,
                               nullptr, nullptr, wc.hInstance, nullptr);
    Check(owner != nullptr, "caller window created");
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
    Message request; request.request_id[0] = 1;
    auto& body = std::get<Request>(request.body);
    SetTimer(owner, 1, 30, nullptr);
    if (argc == 2 && std::wstring_view(argv[1]) == L"--session") {
        for (const auto operation : {Operation::OpenFiles, Operation::SaveFile, Operation::PickFolder}) {
            body.operation = operation;
            body.required_capabilities = operation == Operation::OpenFiles ? Open : operation == Operation::SaveFile ? Save : Folder;
            const auto accepted = RunPickerClient(executable, request, owner);
            Check(StatusOf(accepted) == Status::Accepted && ValidateResponse(request, accepted),
                "negotiated client returns matching operation result");
            Check(IsWindowEnabled(owner), "negotiated client restores owner");
        }
        body.operation = Operation::OpenFiles; body.required_capabilities = Open;
        for (const auto* filename : {u"cancel", u"unsupported", u"wrong-id", u"crash"}) {
            body.filename = filename;
            const auto result = RunPickerClient(executable, request, owner);
            const auto expected = body.filename == u"cancel" ? Status::Cancelled :
                body.filename == u"unsupported" ? Status::Unsupported : Status::Failed;
            Check(StatusOf(result) == expected, "negotiated client preserves terminal outcome");
            Check(IsWindowEnabled(owner), "negotiated failure restores owner");
        }
        Check(ticks > 0, "negotiated client pumps caller messages");
        KillTimer(owner, 1);
        DestroyWindow(owner);
        CloseDesktop(desktop);
        std::cout << "Checks: " << checks << ", failures: " << failures << '\n';
        return failures ? 1 : 0;
    }
    const auto accepted = RunPickerClient(executable, request, owner);
    Check(StatusOf(accepted) == Status::Accepted && ValidateResponse(request, accepted), "real child result accepted");
    Check(IsWindowEnabled(owner), "enabled owner restored after acceptance");
    Check(ticks > 0, "caller messages pumped while waiting");
    EnableWindow(owner, FALSE);
    body.filename = u"cancel";
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Cancelled, "host cancellation preserved");
    Check(!IsWindowEnabled(owner), "already disabled owner remains disabled");
    EnableWindow(owner, TRUE);
    body.filename = u"unsupported";
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Unsupported, "unsupported returned without fallback");
    body.filename = u"wrong-id";
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Failed, "wrong response identity rejected");
    body.filename = u"crash";
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Failed, "host crash is failure not cancellation");
    Check(IsWindowEnabled(owner), "owner restored after host crash");
    PipeHandle cancel(CreateEventW(nullptr, TRUE, TRUE, nullptr));
    Check(StatusOf(RunPickerClient(executable, request, owner, cancel.Get())) == Status::Cancelled, "pre-cancelled request returns without host");
    ResetEvent(cancel.Get()); cancellation = cancel.Get();
    const auto ready_name = L"Local\\PulsePickerClientReady." + std::to_wstring(GetCurrentProcessId());
    PipeHandle ready(CreateEventW(nullptr, TRUE, FALSE, ready_name.c_str()));
    host_ready = ready.Get();
    body.filename = u"hang";
    const auto started = GetTickCount64();
    Check(StatusOf(RunPickerClient(executable, request, owner, cancel.Get())) == Status::Cancelled, "cancellation interrupts waiting host");
    cancellation = nullptr;
    host_ready = nullptr;
    Check(WaitForSingleObject(ready.Get(), 0) == WAIT_OBJECT_0, "cancellation exercised after request reached child");
    Check(GetTickCount64() - started < 10000 && IsWindowEnabled(owner), "cancel cleanup is bounded and restores owner");
    PostQuitMessage(71);
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Cancelled, "caller shutdown cancels host");
    MSG quit{};
    Check(PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && quit.wParam == 71, "WM_QUIT preserved for caller");
    Check(StatusOf(RunPickerClient(L"missing.exe", request, owner)) == Status::Failed, "relative host executable rejected");
    const auto missing = std::wstring(executable) + L".missing";
    Check(StatusOf(RunPickerClient(missing, request, owner)) == Status::Failed && IsWindowEnabled(owner), "launch failure restores owner");
    body.filename.clear();
    client_executable = executable;
    nested_request = request;
    reenter = true;
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Accepted &&
          nested_status == Status::Failed && nested_error == ERROR_BUSY,
          "message-pump reentry is rejected without launching a second modal host");
    HWND transient = CreateWindowW(wc.lpszClassName, L"destroy during disable", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
    destroy_on_disable = true;
    Check(StatusOf(RunPickerClient(executable, request, transient)) == Status::Cancelled && !IsWindow(transient),
          "synchronous WM_ENABLE owner destruction cancels before worker launch");
    ResetEvent(ready.Get());
    host_ready = ready.Get();
    destroy_on_timer = true;
    body.filename = u"hang";
    EnableWindow(owner, FALSE);
    Check(StatusOf(RunPickerClient(executable, request, owner)) == Status::Cancelled,
          "destruction of initially disabled owner cancels running host");
    host_ready = nullptr;
    Check(replacement && !IsWindowEnabled(replacement),
          "owner identity guard does not enable replacement window after destruction");
    if (replacement) DestroyWindow(replacement);
    {
        PipeHandle thread_ready(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        HWND thread_owner = nullptr; DWORD owner_thread = 0;
        std::thread owner_worker([&] {
            owner_thread = GetCurrentThreadId();
            if (SetThreadDesktop(desktop)) thread_owner = CreateWindowW(L"STATIC", L"other-thread disabled owner", WS_OVERLAPPED | WS_DISABLED,
                0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            MSG queued{}; PeekMessageW(&queued, nullptr, 0, 0, PM_NOREMOVE);
            SetEvent(thread_ready.Get());
            MSG message{};
            while (GetMessageW(&message, nullptr, 0, 0) > 0) { TranslateMessage(&message); DispatchMessageW(&message); }
            if (thread_owner) DestroyWindow(thread_owner);
        });
        const bool ready_thread = WaitForSingleObject(thread_ready.Get(), 5000) == WAIT_OBJECT_0;
        Check(ready_thread && thread_owner && owner_thread != GetCurrentThreadId(), "same-process different-thread owner fixture");
        if (ready_thread && thread_owner) {
            body.filename.clear();
            Check(StatusOf(RunPickerClient(executable, request, thread_owner)) == Status::Accepted && !IsWindowEnabled(thread_owner),
                "other-thread caller-managed disabled owner works without cross-thread EnableWindow");
        }
        PostThreadMessageW(owner_thread, WM_QUIT, 0, 0); owner_worker.join();
    }
    {
        ExternalFixture remote(executable);
        DWORD owner_pid = 0;
        const DWORD owner_thread = GetWindowThreadProcessId(remote.Window(), &owner_pid);
        Check(remote.started && owner_pid == remote.pid && owner_pid != GetCurrentProcessId() && owner_thread != GetCurrentThreadId(),
            "actual separate-process disabled owner fixture");
        if (remote.started) {
            const wchar_t* property = L"Pulse.PickerClient.OwnerGuard";
            const HANDLE existing_marker = reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(0x1234));
            const bool marked = SetPropW(remote.Window(), property, existing_marker) != FALSE;
            Check(marked, "seed pre-existing external window property");
            body.filename.clear();
            const auto busy = RunPickerClient(executable, request, remote.Window());
            Check(marked && StatusOf(busy) == Status::Failed && std::get<Response>(busy.body).error_code == ERROR_BUSY &&
                GetPropW(remote.Window(), property) == existing_marker && !IsWindowEnabled(remote.Window()),
                "failed guard cannot remove another owner's property or enable it");
            if (marked) RemovePropW(remote.Window(), property);
            body.filename.clear();
            const auto result = RunPickerClient(executable, request, remote.Window());
            Check(StatusOf(result) == Status::Accepted && ValidateResponse(request, result), "external disabled owner returns authenticated host result");
            Check(!IsWindowEnabled(remote.Window()), "accepted external owner remains caller-disabled");
            body.filename = u"crash";
            Check(StatusOf(RunPickerClient(executable, request, remote.Window())) == Status::Failed && !IsWindowEnabled(remote.Window()),
                "host crash does not enable external owner");
            body.filename = u"hang";
            ResetEvent(ready.Get()); ResetEvent(cancel.Get());
            std::thread cancel_after_ready([&] { WaitForSingleObject(ready.Get(), 5000); SetEvent(cancel.Get()); });
            const auto cancelled = RunPickerClient(executable, request, remote.Window(), cancel.Get());
            cancel_after_ready.join();
            Check(StatusOf(cancelled) == Status::Cancelled && !IsWindowEnabled(remote.Window()), "external owner cancellation retains caller modality");
            Check(PostMessageW(remote.Window(), kExternalCommand, 1, 0) != FALSE, "ask owned child thread to enable its own window asynchronously");
            const auto deadline = GetTickCount64() + 2000;
            while (!IsWindowEnabled(remote.Window()) && GetTickCount64() < deadline) Sleep(1);
            body.filename.clear();
            const auto enabled = RunPickerClient(executable, request, remote.Window());
            const auto enabled_error = std::get<Response>(enabled.body).error_code;
            std::cout << "[FACT] enabled external owner rejected error=" << enabled_error << '\n';
            Check(IsWindowEnabled(remote.Window()) && StatusOf(enabled) == Status::Failed && enabled_error == ERROR_NOT_SUPPORTED,
                "enabled external owner fails explicitly rather than unsafe synchronous modality");
        }
    }
    {
        ExternalFixture remote(executable);
        Check(remote.started, "owner destruction fixture started");
        if (remote.started) {
            body.filename = u"hang"; ResetEvent(ready.Get()); ResetEvent(cancel.Get());
            std::thread destroy_after_ready([&] {
                if (WaitForSingleObject(ready.Get(), 5000) == WAIT_OBJECT_0) PostMessageW(remote.Window(), kExternalCommand, 2, 0);
                else SetEvent(cancel.Get());
            });
            const auto destroyed = RunPickerClient(executable, request, remote.Window(), cancel.Get());
            destroy_after_ready.join();
            Check(StatusOf(destroyed) == Status::Cancelled && !IsWindow(remote.Window()), "external owner destruction cancels live host");
            const auto replacement_deadline = GetTickCount64() + 2000;
            while (!remote.data->replacement && GetTickCount64() < replacement_deadline) Sleep(1);
            const HWND other = reinterpret_cast<HWND>(remote.data->replacement);
            Check(other && !IsWindowEnabled(other), "replacement external window is never enabled by old guard");
        }
    }
    CloseDesktop(desktop);
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
