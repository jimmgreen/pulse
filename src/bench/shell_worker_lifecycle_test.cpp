// Same translation unit as the host's real private worker-acquisition code.
// wWinMain is never called. No registry providers, Shell verbs or host pipe.
#include "../shell_host/main.cpp"
#include <iostream>
#include <thread>

namespace {
int failures = 0;
void Check(bool ok, const char* label) {
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
    if (!ok) ++failures;
}
struct AllocationCase {
    int fail_event = 0;
    bool fail_thread = false;
    int event_calls = 0;
    int thread_calls = 0;
    std::atomic<int> ready_workers{0};
    bool correct_entry = true;
    std::vector<HANDLE> handles;
};
AllocationCase* active = nullptr;

HANDLE WINAPI EventProbe(LPSECURITY_ATTRIBUTES a, BOOL manual, BOOL signalled, LPCWSTR name) {
    if (++active->event_calls == active->fail_event) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    HANDLE h = ::CreateEventW(a, manual, signalled, name);
    if (h) active->handles.push_back(h);
    return h;
}
DWORD WINAPI ControlledWorker(LPVOID argument) {
    auto* worker = static_cast<HandlerWorker*>(argument);
    if (worker->done_event && worker->exit_event) ++active->ready_workers;
    if (worker->done_event) SetEvent(worker->done_event);
    return 0;
}
HANDLE WINAPI ThreadProbe(LPSECURITY_ATTRIBUTES a, SIZE_T stack, LPTHREAD_START_ROUTINE entry,
                          LPVOID argument, DWORD flags, LPDWORD id) {
    ++active->thread_calls;
    active->correct_entry &= entry == HandlerWorkerThread;
    if (active->fail_thread) {
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        return nullptr;
    }
    // A real, harmless Windows thread. Keep the owner alive until it exits,
    // including on the buggy baseline; do not deliberately execute freed data.
    HANDLE h = ::CreateThread(a, stack, ControlledWorker, argument, flags, id);
    if (h) active->handles.push_back(h);
    return h;
}
bool IsOpen(HANDLE h) {
    DWORD flags = 0;
    return GetHandleInformation(h, &flags) != FALSE;
}
void RunCase(int fail_event, bool fail_thread) {
    AllocationCase state;
    state.fail_event = fail_event;
    state.fail_thread = fail_thread;
    state.handles.reserve(4);
    active = &state;
    auto worker = std::make_unique<HandlerWorker>();
    const HandlerWorkerApi api{EventProbe, ThreadProbe};
    const bool started = StartHandlerWorker(*worker, api);
    bool joined = !worker->thread || WaitForSingleObject(worker->thread, 5000) == WAIT_OBJECT_0;
    Check(joined, "M04-007 controlled thread has exited before any owner destruction");
    if (!joined) {
        (void)worker.release(); // never free data still used by a live thread
        ExitProcess(2);
    }
    const bool expected = fail_event == 0 && !fail_thread;
    Check(started == expected, "M04-007 allocation outcome reported correctly");
    Check(state.thread_calls == (fail_event ? 0 : 1),
          "M04-007 no thread creation is attempted until both events exist");
    if (expected) {
        Check(state.ready_workers == 1 && state.correct_entry,
              "M04-007 successful worker receives both events and the production entry contract");
        std::vector<std::unique_ptr<HandlerWorker>> workers;
        workers.push_back(std::move(worker));
        JoinHandlerWorkers(workers);
        Check(workers.empty(), "M04-007 successful worker is joined and ownership cleared");
    } else {
        worker.reset();
    }
    bool all_closed = true;
    for (HANDLE h : state.handles) all_closed &= !IsOpen(h);
    Check(all_closed, "M04-007 every acquired event and thread handle is released");
    // Clean only known test handles leaked by the unmodified baseline.
    for (HANDLE h : state.handles) if (IsOpen(h)) CloseHandle(h);
    active = nullptr;
}
}

namespace {
struct CloseControl {
    HANDLE entered = nullptr;
    HANDLE release_query = nullptr;
    HANDLE final_phase = nullptr;
    bool empty_handlers = false;
    std::atomic<int> enumerate_calls{0};
    HANDLE worker_done = nullptr;
    HANDLE worker_exit = nullptr;
    HANDLE worker_thread = nullptr;
    std::atomic<bool> exited_by_event{false};
    std::atomic<int> fallback_calls{0};
};
CloseControl* close_control = nullptr;
std::vector<pulse::shell::CtxHandlerDesc> OneControlledHandler(
        bool, const std::wstring&, const std::vector<std::wstring>&) {
    ++close_control->enumerate_calls;
    if (close_control->empty_handlers) return {};
    pulse::shell::CtxHandlerDesc desc;
    desc.clsid_text = L"review-controlled-worker";
    return {desc};
}
HRESULT NoFallback(const CtxSessionData&, IContextMenu** menu, HMENU* hmenu,
                   std::vector<CtxItemOut>&) {
    ++close_control->fallback_calls;
    *menu = nullptr; *hmenu = nullptr;
    return E_FAIL;
}
void NoUserLog(const wchar_t*) {
    if (close_control->final_phase) SetEvent(close_control->final_phase);
}
DWORD WINAPI ControlledQuery(LPVOID argument) {
    auto* worker = static_cast<HandlerWorker*>(argument);
    SetEvent(close_control->entered);
    WaitForSingleObject(close_control->release_query, 5000);
    SetEvent(worker->done_event);
    close_control->exited_by_event =
        WaitForSingleObject(worker->exit_event, 5000) == WAIT_OBJECT_0;
    return 0;
}
HANDLE WINAPI QueryThreadProbe(LPSECURITY_ATTRIBUTES a, SIZE_T stack,
        LPTHREAD_START_ROUTINE, LPVOID argument, DWORD flags, LPDWORD id) {
    auto* worker = static_cast<HandlerWorker*>(argument);
    close_control->worker_done = worker->done_event;
    close_control->worker_exit = worker->exit_event;
    HANDLE thread = ::CreateThread(a, stack, ControlledQuery, argument, flags, id);
    close_control->worker_thread = thread;
    return thread;
}
DWORD WINAPI ControlledSession(LPVOID argument) {
    CtxSessionApi api;
    api.enumerate = OneControlledHandler;
    api.fallback = NoFallback;
    api.log = NoUserLog;
    api.worker.create_thread = QueryThreadProbe;
    return CtxSessionThreadImpl(argument, api);
}
void RunCloseCase(uint32_t sid) {
    CloseControl control;
    control.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    control.release_query = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    Check(control.entered && control.release_query, "M04-006 isolated control events created");
    if (!control.entered || !control.release_query) ExitProcess(2);
    close_control = &control;
    auto data = std::make_unique<CtxSessionData>();
    data->session_id = sid;
    data->paths = {L"."}; // only GetFileAttributes; no file modifications or provider binding
    {
        std::lock_guard lock(g_ctx_mutex);
        g_ctx_sessions.emplace(sid, CtxSlot{});
    }
    HANDLE session = CreateThread(nullptr, 0, ControlledSession, data.get(), 0, nullptr);
    if (!session) ExitProcess(2);
    data.release();
    Check(WaitForSingleObject(control.entered, 3000) == WAIT_OBJECT_0,
          "M04-006 real coordinator reaches a controlled query still in progress");
    PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0);
    const ULONGLONG deadline = GetTickCount64() + 1500;
    while (!SessionCloseRequested(sid) && GetTickCount64() < deadline) Sleep(1);
    Check(SessionCloseRequested(sid), "M04-006 first Close is consumed during the query phase");
    SetEvent(control.release_query);
    const bool timely = WaitForSingleObject(session, 1200) == WAIT_OBJECT_0;
    Check(timely, "M04-006 consumed Close exits without entering the 120-second session wait");
    // Rescue only a failing baseline, after recording the failed assertion.
    // A second Close avoids deliberately waiting 120 seconds or killing a thread.
    if (!timely) PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0);
    if (WaitForSingleObject(session, 5000) != WAIT_OBJECT_0) ExitProcess(2);
    Check(control.exited_by_event && control.fallback_calls == 0,
          "M04-006 coordinator signals worker exit without invoking a fallback provider");
    bool retired = false;
    {
        std::lock_guard lock(g_ctx_mutex);
        retired = !g_ctx_sessions.contains(sid) && g_abandoned_threads.empty();
    }
    Check(retired, "M04-006 session is retired and no controlled worker is abandoned");
    const bool worker_handles_closed = control.worker_done && control.worker_exit && control.worker_thread &&
        !IsOpen(control.worker_done) && !IsOpen(control.worker_exit) && !IsOpen(control.worker_thread);
    CloseHandle(session);
    CloseHandle(control.entered);
    CloseHandle(control.release_query);
    Check(worker_handles_closed && !IsOpen(session) && !IsOpen(control.entered) && !IsOpen(control.release_query),
          "M04-006 session, worker and control handles are all closed");
    close_control = nullptr;
}
// Cover neighboring contracts: Close after query collection, pre-start Close,
// and an empty/failing fallback. None of these calls installed COM providers.
void RunNeighborCase(uint32_t sid, int mode) {
    CloseControl control;
    control.entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    control.release_query = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    control.final_phase = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    control.empty_handlers = mode == 2;
    Check(control.entered && control.release_query && control.final_phase,
          "M04-006 neighboring-case control events created");
    if (!control.entered || !control.release_query || !control.final_phase) ExitProcess(2);
    close_control = &control;
    auto data = std::make_unique<CtxSessionData>();
    data->session_id = sid;
    data->paths = {L"."};
    {
        std::lock_guard lock(g_ctx_mutex);
        CtxSlot slot;
        slot.close_requested = mode == 1;
        g_ctx_sessions.emplace(sid, slot);
    }
    HANDLE session = CreateThread(nullptr, 0, ControlledSession, data.get(), 0, nullptr);
    if (!session) ExitProcess(2);
    data.release();
    if (mode == 0) {
        Check(WaitForSingleObject(control.final_phase, 3000) == WAIT_OBJECT_0,
              "M04-006 normal query finishes collection before Close is sent");
        PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0);
    }
    const bool timely = WaitForSingleObject(session, 1200) == WAIT_OBJECT_0;
    Check(timely, "M04-006 normal-close, pre-start-close or empty fallback exits promptly");
    if (!timely) PostCtxMessage(sid, WM_CTX_CLOSE, 0, 0);
    if (WaitForSingleObject(session, 5000) != WAIT_OBJECT_0) ExitProcess(2);
    bool contract = false;
    if (mode == 0) contract = control.enumerate_calls == 1 && control.fallback_calls == 0 && control.exited_by_event;
    if (mode == 1) contract = control.enumerate_calls == 0 && control.fallback_calls == 0 && !control.worker_thread;
    if (mode == 2) contract = control.enumerate_calls == 1 && control.fallback_calls == 1 && !control.worker_thread;
    Check(contract, "M04-006 neighboring path preserves its worker/fallback ownership contract");
    bool clean = false;
    {
        std::lock_guard lock(g_ctx_mutex);
        clean = !g_ctx_sessions.contains(sid) && g_abandoned_threads.empty();
    }
    for (HANDLE h : {control.worker_done, control.worker_exit, control.worker_thread})
        if (h) clean &= !IsOpen(h);
    for (HANDLE h : {session, control.entered, control.release_query, control.final_phase}) {
        CloseHandle(h);
        clean &= !IsOpen(h);
    }
    Check(clean, "M04-006 neighboring-case session and all owned handles are retired");
    close_control = nullptr;
}

int RunSessionCloseTests() {
    std::cout << "[INFO] real host coordinator and Windows message queue; only controlled workers, no providers\n";
    for (uint32_t sid = 91001; sid != 91004; ++sid) RunCloseCase(sid);
    for (int mode = 0; mode != 3; ++mode) RunNeighborCase(92001 + mode, mode);
    return failures ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring(argv[1]) == L"--diagnostic-log") {
        HostLog(L"中文菜单诊断");
        std::vector<std::thread> writers;
        for (uint32_t worker = 0; worker < 4; ++worker) {
            writers.emplace_back([worker] {
                for (uint32_t row = 0; row < 20; ++row)
                    ContextDiagnostic(9000 + worker, L"concurrent row=" + std::to_wstring(row));
            });
        }
        for (auto& writer : writers) writer.join();
        std::cout << "[PASS] diagnostic log writers completed; verify isolated UTF-8 log contents\n";
        return 0;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--session-close") return RunSessionCloseTests();
    std::cout << "[INFO] isolated worker allocation failure injection; no host or Shell provider startup\n";
    DWORD before = 0, after = 0;
    const bool have_before = GetProcessHandleCount(GetCurrentProcess(), &before) != FALSE;
    RunCase(1, false);
    RunCase(2, false);
    RunCase(0, true);
    RunCase(0, false);
    const bool have_after = GetProcessHandleCount(GetCurrentProcess(), &after) != FALSE;
    Check(have_before && have_after && after <= before,
          "M04-007 test-owned handle count returns to baseline after all cases");
    return failures ? 1 : 0;
}