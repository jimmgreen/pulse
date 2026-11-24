#include "../ops/ops_manager.h"
#if __has_include("../app/ops_notify_gate.h")
#include "../app/ops_notify_gate.h"
#include "../app/ops_notify_dispatch.h"
#define HAS_GATE 1
#endif
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

namespace pulse::ops {
struct OpsManagerAuditAccess {
    static void Connect(OpsManager& manager, std::function<void()> notify) { manager.notify_ = std::move(notify); }
    static void Publish(OpsManager& manager) { manager.NotifyStatusChanged(); }
};
}
namespace {
int failures = 0;
void Check(bool ok, const char* name) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
    failures += !ok;
}
#ifdef HAS_GATE
void ProtocolTests() {
    using pulse::app::OpsNotifyGate;
    OpsNotifyGate gate;
    int queued = 0, posts = 0, timers = 0;
    const auto post = [&] { ++queued; ++posts; return true; };
    const auto defer = [&] { ++timers; return true; };
    gate.Request(post);
    --queued;
    Check(gate.BeginDispatch(), "queued notification enters handler exactly once");
    Check(!gate.BeginDispatch(), "duplicate handler entry rejected");
    for (int i = 0; i < 1000; ++i) gate.Request(post);
    Check(queued == 0, "producer updates during handler do not post");
    Check(gate.EndDispatch(defer) && timers == 1 && queued == 0, "dirty handler defers rather than immediate repost");
    for (int i = 0; i < 1000; ++i) gate.Request(post);
    Check(queued == 0, "cooldown blocks continuous producer from starving frame gap");
    Check(gate.Resume(post) && queued == 1, "timer resumes exactly one dirty notification");
    Check(!gate.Resume(post) && queued == 1, "duplicate cooldown timer cannot add another notification");
    --queued; gate.BeginDispatch(); gate.EndDispatch(defer);
    const auto before = posts;
    Check(gate.Resume(post) && queued == 0 && posts == before, "clean cooldown returns idle without extra wake");

    gate.Request(post); --queued; gate.BeginDispatch(); gate.EndDispatch(defer);
    // A terminal/authorization/cancel publication after a clean handler must
    // stay dirty until the timer. Persistent model records carry event details.
    unsigned durable_events = 0;
    for (unsigned event : {1u, 2u, 4u}) { durable_events |= event; gate.Request(post); }
    Check(queued == 0 && gate.Resume(post) && queued == 1, "completion authorization cancellation survive cooling");
    --queued; gate.BeginDispatch();
    Check(durable_events == 7, "deferred dispatch observes every durable terminal interaction record");
    gate.EndDispatch(defer); gate.Resume(post);

    int max_queued = 0;
    const auto counted_post = [&] { ++queued; ++posts; max_queued = (std::max)(max_queued, queued); return true; };
    gate.Request(counted_post);
    const int posts_before = posts;
    for (int frame = 0; frame < 240; ++frame) {
        --queued; gate.BeginDispatch();
        for (int i = 0; i < 100; ++i) gate.Request(counted_post);
        gate.EndDispatch(defer);
        for (int i = 0; i < 100; ++i) gate.Request(counted_post);
        if (queued != 0) ++failures;
        gate.Resume(counted_post);
    }
    Check(posts - posts_before == 240 && max_queued == 1, "48000 continuous requests produce at most one wake per simulated frame");
    --queued; gate.BeginDispatch(); gate.EndDispatch(defer); gate.Resume(post);

    OpsNotifyGate failed;
    int retry_posts = 0;
    const auto retry_post = [&] { ++retry_posts; return true; };
    Check(!failed.Request([] { return false; }) && failed.Retry(retry_post) && retry_posts == 1,
        "PostMessage failure retains retry obligation without wedging");
    failed.BeginDispatch();
    Check(!failed.EndDispatch([] { return false; }) && failed.Retry(retry_post) && retry_posts == 2,
        "SetTimer failure retains retry obligation without synchronous spin");
    failed.Close();
    Check(!failed.BeginDispatch() && !failed.Request(retry_post) && !failed.Resume(retry_post) &&
        !failed.Retry(retry_post) && retry_posts == 2, "closed gate rejects queued stale messages and later producers");
}
void ConcurrentTest() {
    pulse::app::OpsNotifyGate gate;
    std::atomic<int> queued{0}, maximum{0}, generation{0}, done{0};
    const auto post = [&] {
        const auto count = ++queued;
        auto prior = maximum.load();
        while (prior < count && !maximum.compare_exchange_weak(prior, count)) {}
        return true;
    };
    std::vector<std::thread> producers;
    for (int i = 0; i < 4; ++i) producers.emplace_back([&] {
        for (int n = 0; n < 5000; ++n) { ++generation; gate.Request(post); if (n % 13 == 0) std::this_thread::yield(); }
        ++done;
    });
    bool timer = false;
    int seen = 0;
    while (done.load() < 4 || queued.load() || timer) {
        if (queued.load()) {
            --queued;
            if (gate.BeginDispatch()) {
                seen = generation.load();
                std::this_thread::yield();
                gate.EndDispatch([&] { timer = true; return true; });
            }
        } else if (timer) {
            std::this_thread::yield();
            timer = false; gate.Resume(post);
        } else std::this_thread::yield();
    }
    for (auto& thread : producers) thread.join();
    Check(maximum.load() == 1 && seen == 20000 && queued.load() == 0,
        "four concurrent producers preserve final generation with maximum queue depth one");
    gate.Close();
}
template<class Gate> void SuspendNoTarget(Gate& gate) {
    if constexpr (requires { gate.SuspendDispatch(); }) gate.SuspendDispatch();
    else gate.EndDispatch([] { return false; });
}
template<class Dispatch = pulse::app::OpsNotifyDispatch>
std::unique_ptr<Dispatch> MakeNativeDispatch(std::shared_ptr<pulse::app::OpsNotifyGate> gate, HWND host, ULONGLONG& due) {
    if constexpr (std::is_constructible_v<Dispatch, decltype(gate), HWND, ULONGLONG&>)
        return std::make_unique<Dispatch>(std::move(gate), host, due);
    else return std::make_unique<Dispatch>(std::move(gate), host);
}
template<class Gate> void NativeResume(Gate& gate, HWND host, ULONGLONG& due, bool high_priority) {
    if constexpr (requires { ResumeOpsNotify(gate, host, due); }) ResumeOpsNotify(gate, host, due);
    else if (!high_priority) ResumeOpsNotify(gate, host);
}
struct NativeGateFixture {
    std::shared_ptr<pulse::app::OpsNotifyGate> gate = std::make_shared<pulse::app::OpsNotifyGate>();
    HWND host = nullptr, target = nullptr;
    std::atomic<int> generation{0};
    ULONGLONG due = 0, last_dispatch = 0, minimum_gap = MAXULONGLONG;
    bool starvation = false, frame_pump = false;
    int frames = 0, op_timers = 0;
    int observed = 0, dispatches = 0, no_target = 0, timers = 0, paints = 0, maximum_queue = 0;
    static LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
        auto* self = reinterpret_cast<NativeGateFixture*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (message == WM_NCCREATE) {
            self = static_cast<NativeGateFixture*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (!self) return DefWindowProcW(window, message, wp, lp);
        if (message == WM_APP + 99) {
            ++self->frames;
            NativeResume(*self->gate, self->host, self->due, true);
            self->gate->Retry([&] { return PostMessageW(self->host, pulse::WM_OPS_NOTIFY, 0, 0) != FALSE; });
            if (self->frame_pump) PostMessageW(window, WM_APP + 99, 0, 0);
            return 0;
        }
        if (message == pulse::WM_OPS_NOTIFY) {
            if (window == self->host) {
                ++self->dispatches;
                const auto tick = GetTickCount64();
                if (self->last_dispatch) self->minimum_gap = (std::min)(self->minimum_gap, tick - self->last_dispatch);
                self->last_dispatch = tick;
                int depth = 1;
                MSG extra{};
                while (PeekMessageW(&extra, window, pulse::WM_OPS_NOTIFY, pulse::WM_OPS_NOTIFY, PM_REMOVE)) ++depth;
                self->maximum_queue = (std::max)(self->maximum_queue, depth);
                if (self->target) return SendMessageW(self->target, message, wp, lp);
                ++self->no_target;
                if (self->gate->BeginDispatch()) SuspendNoTarget(*self->gate);
            } else if (self->gate->BeginDispatch()) {
                auto dispatch = MakeNativeDispatch(self->gate, self->host, self->due);
                self->observed = self->generation.load();
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }
        if (message == WM_TIMER) {
            if (wp == pulse::app::kOpsNotifyTimer) { ++self->op_timers; NativeResume(*self->gate, window, self->due, false); }
            else {
                ++self->timers;
                if (self->target) {
                    self->gate->Retry([&] { return PostMessageW(self->host, pulse::WM_OPS_NOTIFY, 0, 0) != FALSE; });
                    InvalidateRect(self->target, nullptr, FALSE);
                }
            }
            return 0;
        }
        if (message == WM_PAINT) {
            PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint);
            ++self->paints; return 0;
        }
        return DefWindowProcW(window, message, wp, lp);
    }
};
void NativeWindowTest(bool starvation = false) {
    // A private desktop is never switched to: visible receiver windows here do
    // not appear on the user's desktop or take its foreground/focus.
    const std::wstring name = L"PulseOpsGate-" + std::to_wstring(GetCurrentProcessId());
    const auto caller_desktop = GetThreadDesktop(GetCurrentThreadId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    Check(desktop != nullptr, "private desktop created without switching user desktop");
    if (!desktop) { std::printf("desktop_error=%lu\n", GetLastError()); return; }
    Check(SetThreadDesktop(caller_desktop) != FALSE, "creator thread restores original desktop association");
    std::thread ui([&] {
        const auto original_desktop = GetThreadDesktop(GetCurrentThreadId());
        if (!SetThreadDesktop(desktop)) { Check(false, "test UI thread selects private desktop"); return; }
        NativeGateFixture state;
        state.starvation = starvation;
        state.frame_pump = starvation;
        WNDCLASSW wc{}; wc.lpfnWndProc = NativeGateFixture::Proc; wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"PulseOpsNotifyPrivateTest";
        const ATOM atom = RegisterClassW(&wc);
        state.host = CreateWindowExW(0, wc.lpszClassName, L"host", WS_POPUP, 0, 0, 32, 32, nullptr, nullptr, wc.hInstance, &state);
        const auto create_target = [&] {
            return CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"receiver", WS_POPUP | WS_VISIBLE,
                0, 0, 64, 64, nullptr, nullptr, wc.hInstance, &state);
        };
        state.target = create_target();
        Check(atom && state.host && state.target, "private host and visible receiver created");
        if (!state.host || !state.target) return;
        SetTimer(state.host, 19, 16, nullptr);
        if (starvation) PostMessageW(state.host, WM_APP + 99, 0, 0);
        std::atomic<bool> stop{false};
        std::thread producer([&] {
            while (!stop.load()) {
                for (int i = 0; i < 128; ++i) {
                    ++state.generation;
                    state.gate->Request([&] { return PostMessageW(state.host, pulse::WM_OPS_NOTIFY, 0, 0) != FALSE; });
                }
                Sleep(1);
            }
        });
        const auto start = GetTickCount64();
        bool removed = false, replaced = false;
        while (GetTickCount64() - start < 1000) {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
            const auto elapsed = GetTickCount64() - start;
            if (!starvation && !removed && elapsed >= 200) { const auto old = state.target; state.target = nullptr; DestroyWindow(old); removed = true; }
            if (!starvation && !replaced && elapsed >= 450) { state.target = create_target(); replaced = state.target != nullptr; }
        }
        stop = true; producer.join();
        const auto final_generation = state.generation.load();
        if (starvation) {
            const auto settle = GetTickCount64() + 100;
            while (GetTickCount64() < settle) {
                MSG message{};
                if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            }
            std::printf("WIN32_STARVED final=%d observed=%d frames=%d op_timers=%d heartbeat=%d notifications=%d\n",
                final_generation, state.observed, state.frames, state.op_timers, state.timers, state.dispatches);
            Check(state.frames > 100 && state.op_timers == 0 && state.timers == 0,
                "continuous real posted messages defer actual WM_TIMER delivery");
            Check(state.observed == final_generation && state.dispatches > 5 && state.dispatches < 40,
                "deadline UI ticks deliver final update with zero timer messages and bounded dispatch rate");
            std::printf("WIN32 minimum_notification_gap_ms=%llu\n", state.minimum_gap);
            Check(state.minimum_gap >= 33, "real high-priority ticks cannot bypass 33ms cooldown deadline");
            state.frame_pump = false;
        }
        const auto deadline = GetTickCount64() + 2000;
        while (state.observed != final_generation && GetTickCount64() < deadline) {
            MSG message{};
            if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
            else MsgWaitForMultipleObjects(0, nullptr, FALSE, 2, QS_ALLINPUT);
        }
        std::printf("WIN32 generation=%d observed=%d notifications=%d no_target=%d timers=%d paints=%d max_queue=%d\n",
            final_generation, state.observed, state.dispatches, state.no_target, state.timers, state.paints, state.maximum_queue);
        Check(state.maximum_queue == 1 && state.dispatches < 40, "real SetTimer bounds continuous producer notification queue and rate");
        if (!starvation) Check(state.timers >= 10 && state.paints >= 5, "real WM_TIMER and WM_PAINT receive service during publication");
        if (!starvation) Check(removed && replaced && state.no_target <= 1 && state.observed == final_generation,
            "closed receiver suspends host wakes and replacement receives final generation");
        state.gate->Close();
        KillTimer(state.host, pulse::app::kOpsNotifyTimer); KillTimer(state.host, 19);
        Check(!state.gate->Request([&] { return PostMessageW(state.host, pulse::WM_OPS_NOTIFY, 0, 0) != FALSE; }),
            "native teardown rejects notifications after close");
        DestroyWindow(state.target); DestroyWindow(state.host);
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        Check(SetThreadDesktop(original_desktop) != FALSE, "test UI thread detaches private desktop");
    });
    ui.join();
    const bool closed = CloseDesktop(desktop) != FALSE;
    if (!closed) std::printf("CloseDesktop error=%lu\n", GetLastError());
    Check(closed, "private desktop closed after receiver and host teardown");
}

#endif
}
int main(int argc, char** argv) {
#ifdef HAS_GATE
    if (argc == 2 && std::string(argv[1]) == "--win32-starvation-only") { NativeWindowTest(true); return failures ? 1 : 0; }
    if (argc == 2 && std::string(argv[1]) == "--win32-only") { NativeWindowTest(); return failures ? 1 : 0; }
#endif
    if (argc != 1) return 2;
    pulse::ops::OpsManager manager;
    int queued = 0;
    const auto post = [&] { ++queued; return true; };
#ifdef HAS_GATE
    pulse::app::OpsNotifyGate gate;
    pulse::ops::OpsManagerAuditAccess::Connect(manager, [&] { gate.Request(post); });
#else
    pulse::ops::OpsManagerAuditAccess::Connect(manager, [&] { post(); });
#endif
    for (int i = 0; i < 1000; ++i) {
        (void)manager.Status();
        pulse::ops::OpsManagerAuditAccess::Publish(manager);
    }
    const bool ok = queued == 1;
    std::printf("[%s] actual OpsManager Status polls never release posted-message ownership: queued=%d expected=1\n", ok ? "PASS" : "FAIL", queued);
    failures += !ok;
#ifdef HAS_GATE
    ProtocolTests();
    ConcurrentTest();
#endif
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}

