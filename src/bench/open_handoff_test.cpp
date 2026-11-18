#include "../app/explorer_initial_arm.h"
#include "../app/explorer_handoff.h"
#include "../app/single_instance_coordinator.h"
#include "../app/win_e_chord.h"
#include "../app/win_e_input.h"
#include "../app/win_e_foreground.h"
#include "../app/explorer_source_lifetime.h"

#include <cstdio>
#include <cstring>
#include <thread>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
using Coordinator = pulse::app::SingleInstanceCoordinator;
constexpr UINT queue_wake = WM_APP + 931;
HWND QueueWindow() { return CreateWindowExW(0, L"STATIC", L"owned queue test", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr); }
Coordinator::OpenRequest QueueRequest(unsigned id) {
    Coordinator::OpenRequest request;
    request.id[0] = static_cast<unsigned char>(id); request.id[1] = static_cast<unsigned char>(id >> 8);
    request.path = L"pulse:win-e"; request.deadline = 1000;
    request.delivery = std::make_shared<std::atomic<Coordinator::OpenDeliveryState>>(Coordinator::OpenDeliveryState::Pending);
    return request;
}
BOOL WINAPI DestroyBeforePost(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    DestroyWindow(window); // Deterministic real HWND invalidation between validation and Post.
    const BOOL posted = PostMessageW(window, message, wp, lp);
    Check(!posted && GetLastError() == ERROR_INVALID_WINDOW_HANDLE, "actual PostMessage rejects a just-destroyed own host");
    return posted;
}
void QueueTests() {
    {
        const RECT monitor{-1920, 0, 0, 1080};
        using Kind = pulse::app::WinEForegroundKind;
        Check(pulse::app::ClassifyWinEForeground(true, monitor, monitor) == Kind::Fullscreen &&
              pulse::app::ClassifyWinEForeground(true, {-1928, -8, 8, 1088}, monitor) == Kind::Fullscreen,
              "fullscreen and oversized geometry remains available for diagnostics");
        Check(pulse::app::ClassifyWinEForeground(true, {-1920, 0, 0, 1040}, monitor) == Kind::Windowed &&
              pulse::app::ClassifyWinEForeground(true, {-1800, 100, -100, 900}, monitor) == Kind::Windowed,
              "ordinary and work-area geometry is classified for diagnostics");
        Check(pulse::app::ClassifyWinEForeground(false, {}, monitor) == Kind::Unknown,
              "missing geometry is recorded as unknown");
        const HWND desktop = GetShellWindow();
        Check(desktop && pulse::app::WinEIsShellDesktop(desktop) &&
              pulse::app::ClassifyWinEForeground(desktop) == Kind::Windowed,
              "actual shell desktop keeps the direct WinE route despite covering the monitor");
        MONITORINFO display{sizeof(display)};
        const bool display_known = GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &display);
        Check(display_known, "read native monitor for isolated fullscreen fixture");
        if (display_known) {
            const auto& area = display.rcMonitor;
            WNDCLASSW fake_desktop{};
            fake_desktop.lpfnWndProc = DefWindowProcW;
            fake_desktop.hInstance = GetModuleHandleW(nullptr);
            fake_desktop.lpszClassName = L"WorkerW";
            const ATOM fake_class = RegisterClassW(&fake_desktop);
            Check(fake_class != 0, "register isolated shell-class spoof fixture");
            if (fake_class) {
                HWND spoof = CreateWindowExW(0, fake_desktop.lpszClassName, L"private desktop class spoof", WS_POPUP,
                    area.left, area.top, area.right - area.left, area.bottom - area.top,
                    nullptr, nullptr, fake_desktop.hInstance, nullptr);
                Check(spoof && !pulse::app::WinEIsShellDesktop(spoof) && pulse::app::ClassifyWinEForeground(spoof) == Kind::Fullscreen,
                      "fullscreen WorkerW class from another process cannot impersonate shell desktop");
                if (spoof) DestroyWindow(spoof);
                UnregisterClassW(fake_desktop.lpszClassName, fake_desktop.hInstance);
            }
            HWND fullscreen = CreateWindowExW(0, L"STATIC", L"private fullscreen routing fixture", WS_POPUP,
                area.left, area.top, area.right - area.left, area.bottom - area.top,
                nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
            Check(fullscreen && pulse::app::ClassifyWinEForeground(fullscreen) == Kind::Fullscreen,
                  "native borderless HWND records fullscreen geometry");
            if (fullscreen) {
                SetWindowPos(fullscreen, nullptr, area.left + 20, area.top + 20, 400, 300, SWP_NOZORDER | SWP_NOACTIVATE);
                Check(pulse::app::ClassifyWinEForeground(fullscreen) == Kind::Windowed,
                      "same native HWND resized records windowed geometry");
                DestroyWindow(fullscreen);
            }
        }
        pulse::app::WinEChord chord;
        Check(chord.Press(true) == pulse::app::WinEChord::Action::Submit &&
              chord.Press(true) == pulse::app::WinEChord::Action::Pass && !chord.Release(),
              "rejected submission keeps repeats and release native");
        HWND source = QueueWindow();
        Check(source != nullptr, "create isolated source lifetime fixture");
        if (source) {
            DWORD pid = 0;
            const DWORD tid = GetWindowThreadProcessId(source, &pid);
            SetPropW(source, L"Pulse.Test.SourceIdentity", reinterpret_cast<HANDLE>(1));
            RemovePropW(source, L"Pulse.Test.SourceIdentity");
            Check(!pulse::app::ExplorerSourceDestroyed(source, pid, tid),
                  "removed source marker never confirms a still-open native window");
            Check(pulse::app::ExplorerSourceDestroyed(source, pid + 1, tid),
                  "changed source process confirms original HWND lifetime ended");
            DestroyWindow(source);
            Check(pulse::app::ExplorerSourceDestroyed(source, pid, tid),
                  "actual native HWND destruction confirms handoff");
        }
    }
    {
        pulse::app::WinEInputState input;
        input.ObserveModifier(VK_LWIN, true);
        Check(!input.Eligible(0), "lost Win release cannot turn plain E into a captured shortcut");
        input.ObserveModifier(VK_RWIN, true);
        input.ObserveModifier(VK_LWIN, false);
        Check(input.Eligible(2), "other held Win key remains eligible through the current snapshot");
        for (const auto key : {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT}) {
            input.ObserveModifier(key, true);
            Check(!input.Eligible(2 | pulse::app::WinEInputState::Modifier(key)), "current additional modifier never becomes plain WinE");
            Check(input.Eligible(2), "lost extra-modifier release cannot permanently disable WinE");
            input.ObserveModifier(key, false);
        }
        input.ObserveModifier(VK_RWIN, false);
        Check(!input.Eligible(0), "released modifiers do not capture ordinary E");
        Check(input.Eligible(1) && !input.Eligible(1 | 4), "snapshot covers keys held before hook startup");
        Check(input.chord.Press(input.Eligible(1)) == pulse::app::WinEChord::Action::Submit,
            "valid WinE enters the same submission state machine");
        input.chord.accepted = true;
        Check(input.chord.Press(input.Eligible(0)) == pulse::app::WinEChord::Action::Swallow && input.chord.Release(),
            "accepted E ownership survives modifier release and hook refresh until E release");
    }
    {
        pulse::app::WinEChord chord;
        using Action = pulse::app::WinEChord::Action;
        Check(chord.Press(false) == Action::Pass && chord.Press(true) == Action::Pass && !chord.Release(),
              "ordinary E or modified chord cannot become WinE on key repeat");
        Check(chord.Press(true) == Action::Submit && chord.Press(true) == Action::Pass && !chord.Release(),
              "rejected WinE passes the whole press and never retries while held");
        Check(chord.Press(true) == Action::Submit, "fresh WinE can submit after release");
        chord.accepted = true;
        Check(chord.Press(true) == Action::Swallow && chord.Press(false) == Action::Swallow && chord.Release() &&
              !chord.Release(), "accepted WinE consumes repeats and its keyup exactly once");
    }
    using C = Coordinator; using R = C::OpenSubmission; using S = C::OpenDeliveryState;
    HWND host = QueueWindow(); Check(host != nullptr, "create private message-only queue host"); if (!host) return;
    {
        Coordinator keyboard;
        auto first = QueueRequest(500), next = QueueRequest(501);
        const auto delivery = first.delivery;
        Check(keyboard.TrySubmitKeyboardOpen(first, 100, host, queue_wake) &&
              delivery->load() == Coordinator::OpenDeliveryState::Queued &&
              !keyboard.TrySubmitKeyboardOpen(next, 101, host, queue_wake),
              "keyboard fixed slot admits once and rejects overlap for native fallback");
        auto received = keyboard.TakeQueuedOpenRequest(102, true);
        Check(received && received->path == L"pulse:win-e" && received->id[0] == static_cast<unsigned char>(500) &&
              received->deadline == 10100, "keyboard request reaches existing route with fresh deadline and unchanged identity");
        if (received) {
            Coordinator::CompleteDelivery(*received, true);
            keyboard.FinishOpenClaim(std::move(*received), false);
        }
        Check(delivery->load() == Coordinator::OpenDeliveryState::Dispatched, "keyboard delivery keeps actual route outcome");
        auto failed = QueueRequest(502);
        HWND doomed = QueueWindow();
        Check(!keyboard.TrySubmitKeyboardOpen(failed, 103, doomed, queue_wake, DestroyBeforePost) &&
              failed.path == L"pulse:win-e" && failed.delivery->load() == Coordinator::OpenDeliveryState::Pending,
              "keyboard failed wake leaves no accepted intent and preserves native fallback");
        const auto cancelled = next.delivery;
        Check(keyboard.TrySubmitKeyboardOpen(next, 104, host, queue_wake), "keyboard slot reusable after failed wake");
        keyboard.Release();
        Check(cancelled->load() == Coordinator::OpenDeliveryState::Cancelled &&
              !keyboard.TrySubmitKeyboardOpen(failed, 105, host, queue_wake), "stop cancels keyboard ownership and rejects new chords");
        MSG wake{};
        while (PeekMessageW(&wake, host, queue_wake, queue_wake, PM_REMOVE)) {}
    }
    {
        Coordinator keyboard;
        auto intent = QueueRequest(503);
        const auto delivery = intent.delivery;
        Check(keyboard.TrySubmitKeyboardOpen(intent, 100, host, queue_wake), "keyboard disable race fixture admitted");
        auto queued = Coordinator::OpenDeliveryState::Queued;
        Check(delivery->compare_exchange_strong(queued, Coordinator::OpenDeliveryState::Cancelled) &&
              !keyboard.TakeQueuedOpenRequest(101, true), "disable cancels queued keyboard intent before it can open Pulse");
        MSG wake{};
        while (PeekMessageW(&wake, host, queue_wake, queue_wake, PM_REMOVE)) {}
    }
    {
        C queue; auto one = QueueRequest(1), two = QueueRequest(2);
        Check(queue.SubmitOpenRequest(one, 100, host, queue_wake) == R::Accepted &&
            queue.SubmitOpenRequest(one, 101, host, queue_wake) == R::Duplicate, "atomic enqueue commits once and acknowledges same-ID retry");
        auto conflict = one; conflict.path = L"pulse:this-pc";
        Check(queue.SubmitOpenRequest(conflict, 101, host, queue_wake) == R::Invalid, "same-ID changed raw path rejected");
        Check(queue.SubmitOpenRequest(two, 102, host, queue_wake) == R::Accepted, "second request queued");
        MSG message{}; unsigned wakes = 0;
        while (PeekMessageW(&message, host, queue_wake, queue_wake, PM_REMOVE)) { ++wakes; Check(message.wParam == 0 && message.lParam == 0, "wake has no owning pointer payload"); }
        Check(wakes == 2, "duplicate does not post a second execution wake");
        auto first = queue.TakeQueuedOpenRequest(200), second = queue.TakeQueuedOpenRequest(200);
        Check(first && second && first->id == one.id && second->id == two.id && !queue.TakeQueuedOpenRequest(200), "owned queue drains FIFO exactly once");
        if (first) C::CompleteDelivery(*first, true);
        if (second) C::CompleteDelivery(*second, false); // No target window.
        Check(one.delivery->load() == S::Dispatched && two.delivery->load() == S::Cancelled, "delivery distinguishes dispatched from absent-target cancellation, never Presented");
        queue.Release(); Check(one.delivery->load() == S::Dispatched, "Release cannot revive or cancel completed delivery");
    }
    {
        C queue; auto request = QueueRequest(3);
        Check(queue.SubmitOpenRequest(request, 100, nullptr, queue_wake) == R::Retry && request.delivery.use_count() == 1 && !queue.TakeQueuedOpenRequest(100), "invalid HWND leaves no queue/accepted ownership");
        HWND doomed = QueueWindow(); Check(doomed != nullptr, "create owned Post-failure host");
        Check(queue.SubmitOpenRequest(request, 100, doomed, queue_wake, DestroyBeforePost) == R::Retry &&
            request.delivery.use_count() == 1 && request.delivery->load() == S::Pending && !queue.TakeQueuedOpenRequest(100), "real Post failure rolls back accepted ID and all owned payload references");
        Check(queue.SubmitOpenRequest(request, 101, host, queue_wake) == R::Accepted, "same ID/deadline retry succeeds after Post rollback");
        queue.Release(); Check(request.delivery->load() == S::Cancelled && request.delivery.use_count() == 1 && !queue.TakeQueuedOpenRequest(200), "Release cancels and releases every pending request");
        Check(queue.SubmitOpenRequest(QueueRequest(4), 200, host, queue_wake) == R::Invalid, "submission after Release is rejected");
    }
    {
        C queue; std::vector<C::OpenRequest> requests;
        for (size_t i = 0; i < C::OpenQueueCapacity; ++i) { requests.push_back(QueueRequest(static_cast<unsigned>(i + 10))); Check(queue.SubmitOpenRequest(requests.back(), 100, host, queue_wake) == R::Accepted, "bounded queue accepts available slot"); }
        auto extra = QueueRequest(100);
        Check(queue.SubmitOpenRequest(extra, 100, host, queue_wake) == R::Retry && extra.delivery.use_count() == 1, "capacity rejection does not reserve ID or retain payload");
        auto first = queue.TakeQueuedOpenRequest(101); if (first) C::CompleteDelivery(*first, true);
        Check(queue.SubmitOpenRequest(extra, 101, host, queue_wake) == R::Accepted, "capacity retry uses same ID once a slot is available");
        queue.Release(); bool cancelled = extra.delivery->load() == S::Cancelled;
        for (size_t i = 1; i < requests.size(); ++i) cancelled &= requests[i].delivery->load() == S::Cancelled && requests[i].delivery.use_count() == 1;
        Check(cancelled, "exit cancels and releases full pending queue");
    }
    {
        C queue; std::vector<C::OpenRequest> expired;
        bool filled = true;
        for (size_t i = 0; i < C::OpenQueueCapacity; ++i) {
            expired.push_back(QueueRequest(static_cast<unsigned>(300 + i)));
            filled &= queue.SubmitOpenRequest(expired.back(), 100, host, queue_wake) == R::Accepted;
        }
        auto fresh = QueueRequest(500); fresh.deadline = 2000;
        Check(filled && queue.SubmitOpenRequest(fresh, 1000, host, queue_wake) == R::Accepted,
            "expired full queue reclaims admission capacity without UI Take");
        bool cancelled = true;
        for (const auto& old : expired) cancelled &= old.delivery->load() == S::Cancelled && old.delivery.use_count() == 1;
        Check(cancelled, "admission expiry cancels and releases every old owned delivery");
        auto only = queue.TakeQueuedOpenRequest(1001);
        Check(only && only->id == fresh.id && !queue.TakeQueuedOpenRequest(1001) &&
            queue.SubmitOpenRequest(expired.front(), 1001, host, queue_wake) == R::Invalid,
            "only fresh request can dispatch after admission expiry; expired retries stay invalid");
        if (only) C::CompleteDelivery(*only, true);
    }
    {
        C queue; auto expired = QueueRequest(200);
        Check(queue.SubmitOpenRequest(expired, 100, host, queue_wake) == R::Accepted && !queue.TakeQueuedOpenRequest(1000) &&
            expired.delivery->load() == S::Cancelled && expired.delivery.use_count() == 1, "deadline expiration cancels at dequeue without dispatch or retained acceptance");
        auto synchronous = QueueRequest(201);
        Check(queue.AcceptOpenRequest(synchronous, 100) == C::OpenAcceptance::New &&
            queue.SubmitOpenRequest(synchronous, 100, host, queue_wake) == R::Duplicate && !queue.TakeQueuedOpenRequest(100), "legacy synchronous acceptance remains deduplicated without queued replay");
    }
    {
        C queue; auto request = QueueRequest(202); R submitted = R::Invalid;
        std::thread producer([&] { submitted = queue.SubmitOpenRequest(request, 100, host, queue_wake); });
        queue.Release(); producer.join();
        Check((submitted == R::Accepted && request.delivery->load() == S::Cancelled) ||
            (submitted == R::Invalid && request.delivery->load() == S::Pending), "Release and producer serialize: accepted work cancelled or admission rejected");
        Check(!queue.TakeQueuedOpenRequest(200) && request.delivery.use_count() == 1, "exit race retains no payload or accepted record");
    }
    MSG message{}; while (PeekMessageW(&message, host, queue_wake, queue_wake, PM_REMOVE)) {}
    DestroyWindow(host);
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::strcmp(argv[1], "--keyboard-queue") == 0) {
        QueueTests();
        return failures ? 1 : 0;
    }
    using pulse::app::HandoffDiagnostics;
    using pulse::app::HandoffPhase;
    static std::atomic<unsigned> diagnostic_calls{0};
    HandoffDiagnostics::sink.store(+[](uint64_t, HandoffPhase, uint64_t, uint64_t, uint64_t) noexcept {
        ++diagnostic_calls;
    });
    {
        HandoffDiagnostics trace;
        std::thread first([&] { for (unsigned i = 0; i < 1000; ++i) trace.Emit(HandoffPhase::Prepared); });
        std::thread second([&] { for (unsigned i = 0; i < 1000; ++i) trace.Emit(HandoffPhase::Prepared); });
        first.join(); second.join();
        HandoffDiagnostics other;
        Check(trace.id != other.id && diagnostic_calls == 3,
              "handoff diagnostics correlate distinct requests and deduplicate concurrent phase retries");
    }
    HandoffDiagnostics::sink.store(nullptr);

    QueueTests();
    using namespace pulse::app;
    using S = HandoffState;
    using C = SingleInstanceCoordinator;
    C coordinator;
    C::OpenRequest request;
    request.id[0] = 1;
    request.deadline = 1000;
    request.path = L"\\\\server\\share\\folder";
    auto bytes = C::EncodeOpenRequest(request);
    COPYDATASTRUCT data{C::OpenRequestMessageId(), static_cast<DWORD>(bytes.size()), bytes.data()};
    C::OpenRequest decoded;
    Check(C::DecodeOpenRequest(&data, decoded) && decoded.path == request.path && decoded.id == request.id,
          "versioned request round trip preserves UNC path and ID");
    Check(coordinator.AcceptOpenRequest(decoded, 100) == C::OpenAcceptance::New,
          "first request accepted");
    Check(coordinator.AcceptOpenRequest(decoded, 200) == C::OpenAcceptance::Duplicate,
          "retry of timed-out send cannot open a second tab");
    decoded.path += L"other";
    Check(coordinator.AcceptOpenRequest(decoded, 300) == C::OpenAcceptance::Invalid,
          "ID cannot be reused with a different path");
    Check(coordinator.AcceptOpenRequest(request, 1000) == C::OpenAcceptance::Invalid,
          "late delivery after deadline rejected even after dedup cache expiry");
    C full;
    for (unsigned i = 0; i < 256; ++i) {
        auto item = request;
        item.id[0] = static_cast<unsigned char>(i);
        item.id[1] = 1;
        if (full.AcceptOpenRequest(item, 100) != C::OpenAcceptance::New) ++failures;
    }
    auto extra = request;
    extra.id[1] = 2;
    auto first = request;
    first.id[0] = 0;
    first.id[1] = 1;
    Check(full.AcceptOpenRequest(extra, 200) == C::OpenAcceptance::Invalid &&
          full.AcceptOpenRequest(first, 200) == C::OpenAcceptance::Duplicate,
          "full dedup cache rejects new IDs without evicting accepted live IDs");
    extra.deadline = 2000;
    Check(full.AcceptOpenRequest(extra, 1001) == C::OpenAcceptance::New &&
          full.AcceptOpenRequest(first, 1001) == C::OpenAcceptance::Invalid,
          "expired capacity is reusable but old late requests remain rejected");
    --data.cbData;
    Check(!C::DecodeOpenRequest(&data, decoded), "truncated payload rejected");
    ++data.cbData;
    bytes[0] = 9;
    Check(!C::DecodeOpenRequest(&data, decoded), "unknown protocol rejected");

    auto handoff = std::make_shared<ExplorerHandoff>(1000);
    Check(handoff->Receive(10) && !handoff->Receive(11), "Explorer request accepted only once");
    {
        ExplorerNavigationLease lease;
        lease.handoff = handoff;
        lease.path = L"C:\\source";
        lease.generation = 42;
        lease.names = {L"b.txt", L"a.txt"};
        Check(!lease.RecordListing(lease.path, 41, true, lease.names, 20) && handoff->state == S::Pending,
              "stale generation cannot acknowledge navigation");
        Check(lease.RecordListing(lease.path, 42, true, {L"a.txt", L"b.txt"}, 30),
              "fresh listing records exact selection");
        Check(handoff->state == S::Pending && !handoff->ClaimClose(31), "enumeration alone cannot authorize source close");
        Check(!lease.Presented(lease.path, 0, 42, lease.names, true, false, 32) && handoff->state == S::Pending,
              "failed or occluded frame cannot authorize source close");
        Check(!lease.Presented(lease.path, 0, 42, lease.names, true, true, 33) && handoff->state == S::Pending,
              "drawing before source close cannot commit or prepare a handoff");
        Check(lease.Prepared(lease.path, 0, 42, lease.names, 34), "matching background listing prepares handoff without presentation");
    }
    Check(handoff->state == S::Cancelled && !handoff->ClaimClose(40),
          "closing tab after acknowledgement cancels unconsumed close");
    handoff = std::make_shared<ExplorerHandoff>(1000);
    {
        ExplorerNavigationLease lease;
        lease.handoff = handoff;
        lease.path = L"C:\\source";
        lease.generation = 1;
        lease.names = {L"missing.txt"};
        Check(!lease.RecordListing(lease.path, 1, true, {}, 10) && handoff->state == S::Cancelled,
              "missing selected item preserves Explorer");
    }
    handoff = std::make_shared<ExplorerHandoff>(1000);
    {
        ExplorerNavigationLease lease;
        lease.handoff = handoff;
        lease.path = L"C:\\source";
        lease.generation = 1;
        Check(!lease.RecordListing(lease.path, 1, false, {}, 10) && !handoff->Ready(20),
              "enumeration failure cannot be revived by late success");
    }
    Check(ExplorerNavigationLease::FrameSucceeded(S_OK, S_OK, false) &&
          !ExplorerNavigationLease::FrameSucceeded(E_FAIL, S_OK, false) &&
          !ExplorerNavigationLease::FrameSucceeded(S_OK, static_cast<int32_t>(0x087a0001), false) &&
          !ExplorerNavigationLease::FrameSucceeded(S_OK, S_OK, true),
          "actual HRESULT gate rejects failed drawing occlusion and hidden capture");
    for (int boundary = 0; boundary < 6; ++boundary) {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7; lease.names = {L"selected.txt"};
        Check(lease.RecordListing(lease.path, 7, true, lease.names, 10), "boundary has a validated listing before frame");
        const bool visible = boundary != 0;
        const uint64_t pending = boundary == 1 ? 8 : 0;
        const uint64_t applied = boundary == 2 ? 8 : 7;
        const auto selected = boundary == 3 ? std::vector<std::wstring>{L"changed.txt"} : lease.names;
        const auto path = boundary == 4 ? std::wstring(L"C:\\other") : lease.path;
        Check(!lease.Presented(path, pending, applied, selected, visible, true, boundary == 5 ? 100 : 20) &&
              !observed->ClaimClose(21), "hidden pane replacement selection path or expiry cannot close source");
    }
    {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L""; lease.generation = 7;
        lease.RecordListing(lease.path, 7, true, {}, 10);
        Check(lease.Prepared(lease.path, 0, 7, {}, 20) && observed->ClaimClose(21),
              "This PC empty path accepts matching background prepared generation");
        Check(!lease.Presented(lease.path, 0, 7, {}, true, true, 22), "Closing cannot report a committed presentation");
        Check(observed->ConfirmClose(true) && !lease.Presented(lease.path, 0, 7, {}, true, false, 23) &&
              lease.Presented(lease.path, 0, 7, {}, true, true, 101),
              "actual successful frame after confirmed close is presentation even past preparation deadline");
    }
    for (int boundary = 0; boundary < 5; ++boundary) {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7; lease.names = {L"selected.txt"};
        lease.RecordListing(lease.path, 7, true, lease.names, 10);
        Check(!lease.Prepared(boundary == 0 ? L"C:\\other" : lease.path,
                  boundary == 1 ? 8 : 0, boundary == 2 ? 8 : 7,
                  boundary == 3 ? std::vector<std::wstring>{} : lease.names, boundary == 4 ? 100 : 20) &&
              !observed->ClaimClose(21), "background preparation rejects changed path pending generation selection or expiry");
    }
    {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7; lease.names = {L"selected.txt"};
        lease.RecordListing(lease.path, 7, true, lease.names, 10);
        lease.Prepared(lease.path, 0, 7, lease.names, 20);
        observed->ClaimClose(21);
        Check(lease.RetainCommittedRefresh(lease.path, true), "in-flight irreversible close also retains same-path background refresh");
        observed->ConfirmClose(true);
        Check(lease.RetainCommittedRefresh(lease.path, true) &&
              !lease.RetainCommittedRefresh(lease.path, false) && !lease.RetainCommittedRefresh(L"C:\\other", true),
              "committed page retains only same-path automatic refresh before its first real frame");
        lease.listing_complete = false; lease.generation = 8;
        Check(!lease.RecordListing(lease.path, 7, true, lease.names, 110) &&
              !lease.Presented(lease.path, 8, 7, lease.names, true, true, 111),
              "pre-refresh listing and frame cannot prove the committed replacement generation");
        Check(lease.RecordListing(lease.path, 8, true, lease.names, 112) &&
              !lease.Presented(lease.path, 0, 8, {}, true, true, 113) &&
              !lease.Presented(lease.path, 0, 8, lease.names, true, false, 114) &&
              lease.Presented(lease.path, 0, 8, lease.names, true, true, 115) &&
              !lease.RetainCommittedRefresh(lease.path, true),
              "committed refresh still requires fresh exact selection and successful real frame, then releases retention");
    }
    {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7;
        Check(!lease.Prepared(lease.path, 0, 7, {}, 10) && !observed->ClaimClose(11),
              "preparation without completed listing cannot authorize close");
        lease.RecordListing(lease.path, 7, true, {}, 12);
        observed->Cancel();
        Check(!lease.Prepared(lease.path, 0, 7, {}, 13) && !observed->ClaimClose(14),
              "late preparation cannot resurrect cancelled handoff");
    }
    Check(pulse::app::ExplorerArmOnlyRefresh(true, false) && !pulse::app::ExplorerArmOnlyRefresh(true, true) &&
          !pulse::app::ExplorerArmOnlyRefresh(false, true), "arm-only excludes mixed real dirty events");
    Check(pulse::app::CanTransferExplorerInitialArm(true, true, false, false, true, true, true, 10, 100),
          "initial arm may transfer unlisted pending matching lease");
    for (int blocked = 0; blocked < 8; ++blocked)
        Check(!pulse::app::CanTransferExplorerInitialArm(blocked != 0, blocked != 1, blocked == 2, blocked == 3,
                  blocked != 4, blocked != 5, blocked != 6, blocked == 7 ? 100 : 10, 100),
              "real refresh ready listed repeat path generation selection or deadline rejects transfer");
    {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7; lease.names = {L"selected.txt"};
        if (pulse::app::CanTransferExplorerInitialArm(true, true, false, false, true, true, true, 10, observed->deadline_tick)) {
            lease.generation = 8; lease.initial_arm_transferred = true;
        }
        Check(!lease.RecordListing(lease.path, 7, true, lease.names, 12) &&
              !lease.Presented(lease.path, 8, 7, lease.names, true, true, 13) && !observed->ClaimClose(14),
              "old listing and frame cannot authorize after arm transfer");
        Check(lease.RecordListing(lease.path, 8, true, lease.names, 15) && observed->state == S::Pending &&
              lease.Prepared(lease.path, 0, 8, lease.names, 16) && observed->ClaimClose(17) && observed->deadline_tick == 100,
              "replacement background listing retains deadline and selection authorization");
    }
    ExplorerHandoff expired(50);
    Check(!expired.Ready(50) && !expired.ClaimClose(51) && expired.state == S::Expired,
          "timeout blocks late success and close");
    handoff = std::make_shared<ExplorerHandoff>(1000);
    {
        ExplorerNavigationLease lease;
        lease.handoff = handoff;
        lease.path = L"C:\\source";
        lease.generation = 1;
        Check(!lease.RecordListing(L"C:\\elsewhere", 1, true, {}, 10) && handoff->state == S::Cancelled,
              "navigation replacement cannot acknowledge the old source");
    }
    ExplorerHandoff ready_expired(50);
    Check(ready_expired.Ready(20) && !ready_expired.ClaimClose(50) && ready_expired.state == S::Expired,
          "acknowledgement received before timeout cannot close after deadline");
    ExplorerHandoff stopped(1000);
    stopped.Ready(20);
    stopped.Cancel();
    Check(!stopped.ClaimClose(30), "disable or source change cancels ready close");
    ExplorerHandoff ready(1000);
    Check(ready.Ready(20) && ready.ClaimClose(30) && !ready.ClaimClose(31),
          "successful handoff can close at most once");
    Check(ready.state == S::Closing && !ready.ConfirmClose(false),
          "accepted close does not claim the source window disappeared");
    ready.Cancel();
    Check(ready.state == S::Closing && ready.ConfirmClose(true) && ready.state == S::Closed,
          "cancellation after Quit waits for observed source destruction");
    Check(!ready.FailClose() && !ready.ConfirmClose(true), "confirmed close is terminal");
    ExplorerHandoff ignored_close(1000);
    ignored_close.Ready(20); ignored_close.ClaimClose(30);
    Check(!ignored_close.ConfirmClose(false) && ignored_close.FailClose(S::Expired) &&
          ignored_close.state == S::Expired && !ignored_close.ConfirmClose(true),
          "source still open at deadline fails without committing Pulse");
    ExplorerHandoff failed_close(1000);
    failed_close.Ready(20); failed_close.ClaimClose(30);
    Check(failed_close.FailClose() && failed_close.state == S::Cancelled && !failed_close.ClaimClose(40),
          "failed close cannot be retried or reported closed");
    // 设为默认文件管理器: the desktop Recycle Bin verb passes its parsing name.
    std::wstring launch;
    Check(C::NormalizeLaunchPath(L"::{645FF040-5081-101B-9F08-00AA002F954E}", launch) && launch == L"pulse:recycle",
          "Recycle Bin verb argument opens Pulse's recycle view");
    Check(C::NormalizeLaunchPath(L"\"shell:RecycleBinFolder\"", launch) && launch == L"pulse:recycle" &&
          C::NormalizeLaunchPath(L"shell:::{645ff040-5081-101b-9f08-00aa002f954e}", launch) && launch == L"pulse:recycle",
          "shell:RecycleBinFolder and lower-case shell parsing names are accepted");
    Check(C::NormalizeLaunchPath(L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", launch) &&
          launch == L"::{20D04FE0-3AEA-1069-A2D8-08002B30309D}", "This PC argument is unchanged");
    // The Drive verb's "%1" = "C:\" reaches argv as C:" (\" is an escaped
    // quote). Resolved as is, it named the drive's current directory instead.
    wchar_t cwd[MAX_PATH]{};
    GetCurrentDirectoryW(MAX_PATH, cwd);
    const std::wstring drive = std::wstring(1, cwd[0]) + L":";
    Check(C::NormalizeLaunchPath(drive + L"\"", launch) && launch == drive + L"\\",
          "a drive root with the stray quote of \"%1\" opens the root, not the current directory");
    Check(C::NormalizeLaunchPath(L"\"D:\\Data\"", launch) && launch == L"D:\\Data",
          "quotes around a folder are dropped");
    return failures ? 1 : 0;
}
