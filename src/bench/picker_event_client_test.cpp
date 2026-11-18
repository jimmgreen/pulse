#include "../ipc/picker_event_client.h"
#include "../ipc/picker_event_state.h"

#include <objbase.h>
#include <iostream>
#include <string>
#include <thread>

using namespace pulse::ipc::picker;
namespace {
int checks = 0, failures = 0;
HANDLE pump_event = nullptr;
constexpr UINT kPumpMessage = WM_APP + 17;
constexpr UINT kFloodMessage = WM_APP + 18;
bool flooding = false;
int flood_count = 0;
HANDLE flood_cancel = nullptr;
constexpr uint32_t kCloseResult = 0x81234567;
constexpr WaitOptions kWait{nullptr, 5000};
RequestId Id() { RequestId id{}; id[0] = 73; return id; }
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << name << std::endl;
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    if (message == kPumpMessage) { if (pump_event) SetEvent(pump_event); return 0; }
    if (message == kFloodMessage) {
        if (flooding) {
            if (++flood_count >= 8) SetEvent(flood_cancel);
            PostMessageW(window, kFloodMessage, 0, 0);
        }
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
HWND MakeWindow() {
    WNDCLASSW cls{}; cls.hInstance = GetModuleHandleW(nullptr); cls.lpfnWndProc = WindowProc;
    cls.lpszClassName = L"PulseEventTransportFixture";
    RegisterClassW(&cls);
    return CreateWindowW(cls.lpszClassName, L"event transport fixture", WS_POPUP, 0, 0, 100, 100,
        nullptr, nullptr, cls.hInstance, nullptr);
}
std::wstring PumpName(DWORD pid) { return L"Local\\PulseEventPump." + std::to_wstring(pid); }
int Drain(Channel& channel) {
    Message discarded;
    return channel.Receive(discarded, kWait) ? 29 : 0;
}
int Host(const wchar_t* pipe, DWORD parent_pid, const std::wstring& mode, uint64_t foreign_window) {
    PipeHandle parent(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, parent_pid));
    Channel channel;
    if (!parent || !Connect(pipe, parent.Get(), channel, kWait)) return 10;
    HWND window = MakeWindow();
    if (!window) return 11;
    struct Destroy { HWND window; ~Destroy() { DestroyWindow(window); } } destroy{window};
    EventSnapshot snapshot{1, reinterpret_cast<uintptr_t>(window), 0, u"C:\\fixture", u"one.txt", {}, {}};
    SessionEvent event{1, EventKind::Ready, snapshot, {}};
    if (mode == L"foreign-window") event.snapshot.window = foreign_window;
    Message first{Id(), event};
    if (mode == L"wrong-id") first.request_id[1] = 1;
    PickerEventState state(Id());
    if (!state.Begin({Id(), event}) || !channel.Send(first, kWait)) return 12;
    if (mode == L"foreign-window" || mode == L"wrong-id") return Drain(channel);
    bool refreshed = false;
    for (;;) {
        Message message;
        if (!channel.Receive(message, kWait))
            return mode == L"cancel" || mode == L"flood-cancel" || mode == L"quit" || mode == L"disconnect" ? 0 : 13;
        if (const auto* command = std::get_if<SessionCommand>(&message.body)) {
            if (mode == L"disconnect") return 0;
            if (mode == L"nested") {
                auto nested = event; nested.event_id = 99; nested.kind = EventKind::SelectionChanged;
                if (!channel.Send({Id(), nested}, kWait)) return 14;
                return Drain(channel);
            }
            if ((mode == L"stale" || mode == L"stale-terminal") && event.kind == EventKind::FileOk && !refreshed) {
                auto update = state.Snapshot(); ++update.generation;
                update.filename = u"two.txt"; update.candidates = {u"C:\\fixture\\two.txt"};
                if (!state.Refresh(update)) return 15;
                refreshed = true;
            }
            if (mode == L"success" && command->kind == CommandKind::Query) {
                PipeHandle signal(OpenEventW(SYNCHRONIZE, FALSE, PumpName(parent_pid).c_str()));
                if (!signal || WaitForSingleObject(signal.Get(), 5000) != WAIT_OBJECT_0) return 16;
            }
            Message answer;
            if (!state.Command(message, answer) || !channel.Send(answer, kWait)) return 17;
            continue;
        }
        const auto outcome = state.Apply(message);
        if (outcome == ReplyOutcome::Invalid) return 18;
        if (outcome == ReplyOutcome::Continue || outcome == ReplyOutcome::Veto || outcome == ReplyOutcome::Retry) {
            if (mode == L"flood-cancel") return Drain(channel);
            if (mode == L"stale-terminal" && outcome == ReplyOutcome::Retry) {
                SessionTerminal terminal{TerminalKind::Accepted, 0, 1, event.snapshot.generation, 0, event.snapshot.candidates};
                if (!channel.Send({Id(), terminal}, kWait)) return 19;
                return Drain(channel);
            }
            snapshot = state.Snapshot(); ++snapshot.generation;
            snapshot.selections = {refreshed ? u"C:\\fixture\\two.txt" : u"C:\\fixture\\one.txt"};
            snapshot.candidates = snapshot.selections;
            event = {event.event_id + 1, EventKind::FileOk, snapshot, {}};
            if (!state.Begin({Id(), event}) || !channel.Send({Id(), event}, kWait)) return 20;
            continue;
        }
        SessionTerminal terminal;
        terminal.ui_shown = 1; terminal.generation = state.Snapshot().generation;
        if (outcome == ReplyOutcome::Closed) {
            terminal.status = TerminalKind::Closed; terminal.hresult = state.Result();
        } else if (outcome == ReplyOutcome::Failed) {
            terminal.status = TerminalKind::Failed; terminal.hresult = state.Result();
        } else if (outcome == ReplyOutcome::Accept) {
            terminal.status = TerminalKind::Accepted; terminal.hresult = 0;
            terminal.paths = state.AcceptedCandidates(); terminal.filter_index = state.Snapshot().filter_index;
            if (mode == L"wrong-result") terminal.paths = {u"C:\\fixture\\substituted.txt"};
        } else return 21;
        if (!channel.Send({Id(), terminal}, kWait)) return 22;
        return Drain(channel);
    }
}
struct Child {
    PipeHandle process;
    DWORD pid = 0;
    bool Start(const std::wstring& arguments) {
        wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, 32768);
        std::wstring command = L"\"" + std::wstring(executable) + L"\" " + arguments;
        wchar_t desktop[1024]{}; DWORD needed = 0;
        if (!GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()), UOI_NAME, desktop, sizeof(desktop), &needed)) return false;
        STARTUPINFOW startup{}; startup.cb = sizeof(startup); startup.lpDesktop = desktop;
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info)) return false;
        process.Reset(info.hProcess); pid = info.dwProcessId; CloseHandle(info.hThread);
        return true;
    }
    bool Finish() {
        if (!process || WaitForSingleObject(process.Get(), 6000) != WAIT_OBJECT_0) return false;
        DWORD code = 1;
        return GetExitCodeProcess(process.Get(), &code) && code == 0;
    }
    ~Child() {
        if (process && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT) {
            TerminateProcess(process.Get(), 99); WaitForSingleObject(process.Get(), 5000);
        }
    }
};
void Case(const std::wstring& mode, HWND caller) {
    std::wcout << L"[CASE] " << mode << std::endl;
    ResetEvent(pump_event);
    Server server; Child child; Channel channel;
    const bool connected = Server::Create(server) && child.Start(L"--host " + server.Name() + L" " +
        std::to_wstring(GetCurrentProcessId()) + L" " + mode + L" " + std::to_wstring(reinterpret_cast<uintptr_t>(caller))) &&
        server.Accept(child.process.Get(), channel, kWait);
    Check(connected, "authenticated child connected");
    if (!connected) return;
    PipeHandle cancel(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    const auto start = GetTickCount64();
    const DWORD sta = GetCurrentThreadId();
    int callbacks = 0, file_ok = 0;
    const auto terminal = RunPickerEventClient(std::move(channel), Id(), child.pid,
        [&](PickerEventContext& context, const SessionEvent& event) -> uint32_t {
            ++callbacks;
            Check(GetCurrentThreadId() == sta, "callback runs on original STA");
            if (mode == L"cancel") { SetEvent(cancel.Get()); return 0; }
            if (mode == L"flood-cancel") {
                flood_cancel = cancel.Get(); flood_count = 0; flooding = true;
                PostMessageW(caller, kFloodMessage, 0, 0);
                return 0;
            }
            if (mode == L"quit") { PostQuitMessage(73); return 0; }
            if (mode == L"failure") return 0x80070005;
            if (mode == L"throw") throw 1;
            if (mode == L"bad-success") return 2;
            if (mode == L"close" || mode == L"close-ok") {
                const uint32_t hr = mode == L"close" ? kCloseResult : 0;
                Check(context.Close(hr) == 0, "callback Close command acknowledged");
                Check(context.Close(0x80004005) == 0, "repeat Close acknowledged without replacing first result");
                EventSnapshot untouched{88, 99};
                const auto sentinel = untouched;
                Check(context.Query(0, untouched) == 0x8007139f && untouched == sentinel, "query after Close preserves output");
                return 0x80070005; // Close wins over this callback failure.
            }
            if (event.kind == EventKind::Ready) {
                if (mode == L"success") {
                    uint32_t wrong_thread = 0;
                    std::thread other([&] { EventSnapshot unused; wrong_thread = context.Query(0, unused); });
                    other.join();
                    Check(wrong_thread == static_cast<uint32_t>(RPC_E_WRONG_THREAD), "off-STA command rejected before channel access");
                }
                if (mode == L"success") PostMessageW(caller, kPumpMessage, 0, 0);
                EventSnapshot snapshot;
                const auto query = context.Query(event.snapshot.generation, snapshot);
                if (mode == L"nested" || mode == L"disconnect") return query;
                Check(query == 0 && snapshot == event.snapshot, "Query returns coherent callback snapshot");
                uint64_t window = 0;
                Check(context.GetWindow(window) == 0 && window == event.snapshot.window, "GetWindow returns verified host HWND");
                auto sentinel = snapshot;
                Check(context.Query(9999, snapshot) == 0x800704d5 && snapshot == sentinel, "stale Query returns retry without output mutation");
            } else if (event.kind == EventKind::FileOk) {
                ++file_ok;
                if (mode == L"veto" && file_ok == 1) return 1;
                if ((mode == L"stale" || mode == L"stale-terminal") && file_ok == 1) {
                    EventSnapshot current;
                    Check(context.Query(0, current) == 0 && current.generation > event.snapshot.generation,
                        "reentrant Query observes a new generation");
                }
            }
            return 0;
        }, cancel.Get());
    flooding = false; flood_cancel = nullptr;
    if (mode == L"flood-cancel") {
        MSG remaining{};
        while (PeekMessageW(&remaining, caller, kFloodMessage, kFloodMessage, PM_REMOVE)) {}
    }
    if (mode == L"success" || mode == L"veto" || mode == L"stale") {
        Check(terminal.status == TerminalKind::Accepted && terminal.hresult == 0 && terminal.paths.size() == 1 &&
            terminal.paths[0] == (mode == L"stale" ? u"C:\\fixture\\two.txt" : u"C:\\fixture\\one.txt"), "accepted terminal binds last validated FileOk");
        Check(file_ok == (mode == L"success" ? 1 : 2), "veto or stale candidate requires another FileOk");
        if (mode == L"success") Check(WaitForSingleObject(pump_event, 0) == WAIT_OBJECT_0, "caller message pump serviced host-blocking message");
    } else if (mode == L"close" || mode == L"close-ok") {
        Check(terminal.status == TerminalKind::Closed && terminal.hresult == (mode == L"close" ? kCloseResult : 0) && terminal.paths.empty(),
            "first Close HRESULT wins and cannot fabricate accepted paths");
    } else if (mode == L"cancel" || mode == L"flood-cancel" || mode == L"quit") {
        Check(terminal.status == TerminalKind::Cancelled && terminal.paths.empty(), "cancellation wins over callback result");
        if (mode == L"flood-cancel")
            Check(flood_count >= 8 && GetTickCount64() - start < 5000, "continuous messages cannot starve cancellation");
        if (mode == L"quit") {
            MSG message{};
            Check(PeekMessageW(&message, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE) && message.wParam == 73, "WM_QUIT code preserved");
        }
    } else {
        Check(terminal.status == TerminalKind::Failed && terminal.paths.empty(), "invalid or failed session produces no selection");
        if (mode == L"failure") Check(terminal.hresult == 0x80070005, "callback failure HRESULT preserved");
        if (mode == L"throw" || mode == L"bad-success") Check(terminal.hresult == 0x8000ffff, "exception or invalid success becomes E_UNEXPECTED");
        if (mode == L"foreign-window" || mode == L"wrong-id") Check(callbacks == 0, "invalid identity never reaches callback");
        if (mode == L"nested") Check(callbacks == 1, "reentrant command wait never dispatches nested events");
    }
    Check(child.Finish(), "owned host exits after channel closure");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 6 && std::wstring_view(argv[1]) == L"--host")
        return Host(argv[2], wcstoul(argv[3], nullptr, 10), argv[4], _wcstoui64(argv[5], nullptr, 10));
    const auto name = L"PulseEventClient." + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop && SetThreadDesktop(desktop), "isolated non-active desktop selected");
    if (!desktop) return 1;
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Check(SUCCEEDED(initialized), "caller STA initialized");
    if (FAILED(initialized)) return 1;
    HWND caller = MakeWindow();
    PipeHandle pump(CreateEventW(nullptr, TRUE, FALSE, PumpName(GetCurrentProcessId()).c_str()));
    pump_event = pump.Get();
    Check(caller && pump, "caller message fixture created");
    if (caller && pump) {
        PipeHandle finished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        std::thread watchdog([&] {
            if (WaitForSingleObject(finished.Get(), 30000) != WAIT_OBJECT_0) {
                std::cerr << "[FAIL] event client watchdog expired" << std::endl;
                TerminateProcess(GetCurrentProcess(), 99);
            }
        });
        for (const auto* mode : {L"success", L"veto", L"stale", L"close", L"close-ok", L"failure", L"throw", L"bad-success",
                 L"foreign-window", L"wrong-id", L"nested", L"wrong-result", L"stale-terminal", L"disconnect", L"cancel", L"flood-cancel", L"quit"})
            Case(mode, caller);
        SetEvent(finished.Get());
        watchdog.join();
    }
    if (caller) DestroyWindow(caller);
    pump_event = nullptr;
    CoUninitialize();
    CloseDesktop(desktop);
    std::cout << "Checks: " << checks << ", failures: " << failures << std::endl;
    return failures ? 1 : 0;
}
