#include "picker_event_client.h"

#include <objbase.h>
#include <thread>

namespace pulse::ipc::picker {
namespace {
uint32_t Error(DWORD error) { return static_cast<uint32_t>(HRESULT_FROM_WIN32(error)); }
constexpr uint32_t kUnexpected = 0x8000ffff;
constexpr uint32_t kInvalidData = 0x8007000d;
bool Failed(uint32_t result) { return (result & 0x80000000u) != 0; }
}

PickerEventContext::~PickerEventContext() {
    if (quit_) PostQuitMessage(quit_code_);
}

TransportResult PickerEventContext::Io(const std::function<TransportResult(HANDLE)>& operation) {
    if (io_active_) return {ERROR_BUSY};
    if (quit_) return {ERROR_CANCELLED};
    PipeHandle done(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    PipeHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!done || !stop) return {GetLastError()};
    io_active_ = true;
    struct Reset { bool& flag; ~Reset() { flag = false; } } reset{io_active_};
    TransportResult result{ERROR_GEN_FAILURE};
    std::thread worker;
    try {
        worker = std::thread([&] {
            try { result = operation(stop.Get()); }
            catch (...) { result = {ERROR_NOT_ENOUGH_MEMORY}; }
            SetEvent(done.Get());
        });
    } catch (...) { return {ERROR_NOT_ENOUGH_MEMORY}; }
    DWORD error = ERROR_SUCCESS;
    for (;;) {
        const HANDLE handles[]{done.Get(), cancel_};
        const DWORD count = cancel_ ? 2 : 1;
        const DWORD waited = MsgWaitForMultipleObjectsEx(count, handles, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (waited == WAIT_OBJECT_0) break;
        if (waited == WAIT_FAILED) { error = GetLastError(); break; }
        if (count == 2 && waited == WAIT_OBJECT_0 + 1) { error = ERROR_CANCELLED; break; }
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                quit_ = true; quit_code_ = static_cast<int>(message.wParam);
                error = ERROR_CANCELLED;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (cancel_ && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) {
                error = ERROR_CANCELLED;
                break;
            }
            if (WaitForSingleObject(done.Get(), 0) == WAIT_OBJECT_0) break;
        }
        if (error) break;
    }
    if (error) SetEvent(stop.Get());
    worker.join();
    MSG pending_quit{};
    if (!quit_ && PeekMessageW(&pending_quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) {
        quit_ = true; quit_code_ = static_cast<int>(pending_quit.wParam);
    }
    if (quit_) error = ERROR_CANCELLED;
    // Cancellation wins even if a reply raced the signalled event.
    if (cancel_ && WaitForSingleObject(cancel_, 0) == WAIT_OBJECT_0) error = ERROR_CANCELLED;
    return error ? TransportResult{error} : result;
}

bool PickerEventContext::WindowMatches(const EventSnapshot& snapshot) const {
    if (!snapshot.window || snapshot.window > UINTPTR_MAX || !host_pid_) return false;
    DWORD pid = 0;
    const auto window = reinterpret_cast<HWND>(static_cast<uintptr_t>(snapshot.window));
    return GetWindowThreadProcessId(window, &pid) && pid == host_pid_ &&
        (!snapshot_.window || snapshot_.window == snapshot.window);
}

uint32_t PickerEventContext::Command(CommandKind kind, uint64_t generation, uint32_t hresult, EventSnapshot* output) {
    if (GetCurrentThreadId() != thread_) return static_cast<uint32_t>(RPC_E_WRONG_THREAD);
    if (!in_callback_) return Error(ERROR_INVALID_STATE);
    if (io_active_) return static_cast<uint32_t>(RPC_E_CALL_REJECTED);
    if (failure_) return failure_;
    if (command_id_ == UINT64_MAX) return failure_ = kUnexpected;
    SessionCommand command{++command_id_, kind, generation, hresult};
    Message reply;
    const auto result = Io([&](HANDLE stop) {
        auto sent = channel_.Send({id_, command}, {stop, 5000});
        return sent ? channel_.Receive(reply, {stop, 10000}) : sent;
    });
    if (!result) return failure_ = Error(result.error);
    const auto* response = std::get_if<CommandReply>(&reply.body);
    if (!response || reply.request_id != id_ || response->command_id != command.command_id || response->kind != kind)
        return failure_ = kInvalidData;
    if (Failed(response->hresult)) return response->hresult;
    if (kind == CommandKind::Close) {
        if (!close_requested_) { close_requested_ = true; close_result_ = hresult; }
    } else {
        if (!WindowMatches(response->snapshot) || response->snapshot.generation < snapshot_.generation ||
            (response->snapshot.generation == snapshot_.generation && response->snapshot != snapshot_) ||
            (generation && response->snapshot.generation != generation)) return failure_ = kInvalidData;
        snapshot_ = response->snapshot;
        if (output) *output = snapshot_;
    }
    return response->hresult;
}

uint32_t PickerEventContext::Query(uint64_t expected_generation, EventSnapshot& output) {
    return Command(CommandKind::Query, expected_generation, 0, &output);
}
uint32_t PickerEventContext::GetWindow(uint64_t& output) {
    EventSnapshot snapshot;
    const auto result = Command(CommandKind::GetWindow, 0, 0, &snapshot);
    if (!result) output = snapshot.window;
    return result;
}
uint32_t PickerEventContext::Close(uint32_t hresult) { return Command(CommandKind::Close, 0, hresult, nullptr); }

SessionTerminal RunPickerEventClient(Channel channel, RequestId request_id, DWORD host_pid,
                                    const PickerEventCallback& callback, HANDLE cancel) {
    PickerEventContext context(channel, request_id, host_pid, cancel);
    const auto failure = [&](uint32_t result) {
        SessionTerminal terminal;
        terminal.status = result == Error(ERROR_CANCELLED) ? TerminalKind::Cancelled : TerminalKind::Failed;
        terminal.hresult = result;
        terminal.ui_shown = context.snapshot_.window ? 1u : 0u;
        terminal.generation = context.snapshot_.generation;
        return terminal;
    };
    APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
    if (FAILED(CoGetApartmentType(&apartment, &qualifier)) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA))
        return failure(static_cast<uint32_t>(RPC_E_WRONG_THREAD));
    if (!callback || !host_pid) return failure(static_cast<uint32_t>(E_INVALIDARG));
    uint64_t event_id = 0, accepted_generation = 0;
    uint32_t accepted_filter = 0, callback_failure = 0;
    std::vector<std::u16string> accepted_paths;
    for (;;) {
        Message incoming;
        const auto received = context.Io([&](HANDLE stop) { return channel.Receive(incoming, {stop, INFINITE}); });
        if (!received) return failure(Error(received.error));
        if (incoming.request_id != request_id) return failure(kInvalidData);
        if (const auto* terminal = std::get_if<SessionTerminal>(&incoming.body)) {
            if (terminal->ui_shown != (event_id ? 1u : 0u) || terminal->generation < context.snapshot_.generation)
                return failure(kInvalidData);
            if (context.close_requested_ && (terminal->status != TerminalKind::Closed || terminal->hresult != context.close_result_))
                return failure(kInvalidData);
            if (terminal->status == TerminalKind::Closed && !context.close_requested_) return failure(kInvalidData);
            if (callback_failure && !context.close_requested_ &&
                (terminal->status != TerminalKind::Failed || terminal->hresult != callback_failure)) return failure(kInvalidData);
            if (terminal->status == TerminalKind::Accepted && (!accepted_generation ||
                terminal->generation != accepted_generation || terminal->filter_index != accepted_filter || terminal->paths != accepted_paths))
                return failure(kInvalidData);
            return *terminal;
        }
        const auto* event = std::get_if<SessionEvent>(&incoming.body);
        if (!event || context.close_requested_ || callback_failure || event->event_id <= event_id ||
            ((!event_id) != (event->kind == EventKind::Ready)) ||
            event->snapshot.generation < context.snapshot_.generation ||
            (event_id && event->snapshot.generation == context.snapshot_.generation && event->snapshot != context.snapshot_) ||
            !context.WindowMatches(event->snapshot))
            return failure(kInvalidData);
        context.snapshot_ = event->snapshot;
        event_id = event->event_id;
        accepted_generation = 0; accepted_paths.clear();
        uint32_t result = kUnexpected;
        context.in_callback_ = true;
        try { result = callback(context, *event); } catch (...) {}
        context.in_callback_ = false;
        if (context.failure_) return failure(context.failure_);
        if (result && !(result == 1 && (event->kind == EventKind::FileOk || event->kind == EventKind::FolderChanging))) {
            if (!Failed(result)) result = kUnexpected;
            if (context.snapshot_.generation == event->snapshot.generation) callback_failure = result;
        }
        if (cancel && WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) return failure(Error(ERROR_CANCELLED));
        if (event->kind == EventKind::FileOk && !result && !context.close_requested_ &&
            context.snapshot_.generation == event->snapshot.generation) {
            accepted_generation = event->snapshot.generation;
            accepted_filter = event->snapshot.filter_index;
            accepted_paths = event->snapshot.candidates;
        }
        const auto sent = context.Io([&](HANDLE stop) {
            return channel.Send({request_id, EventReply{event->event_id, event->snapshot.generation, result}}, {stop, 5000});
        });
        if (!sent) return failure(Error(sent.error));
    }
}

} // namespace pulse::ipc::picker
