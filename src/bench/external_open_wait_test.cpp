#include "../app/external_open_wait.h"
#include "../app/external_open_notice.h"
#include "../app/single_instance_coordinator.h"
#include <cstdio>
using Coordinator = pulse::app::SingleInstanceCoordinator;
int wmain() {
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) { ++checks; failures += !ok; printf("[%s] %s\n", ok ? "PASS" : "FAIL", name); };
    using namespace pulse::app;
    uint64_t deadline = 0;
    check(AdvanceOpenWait(deadline, 100, true) == OpenWaitAction::Defer && deadline == 30100, "first busy sets independent thirty-second budget");
    check(AdvanceOpenWait(deadline, 200, true) == OpenWaitAction::Defer && deadline == 30100, "repeated busy does not renew budget");
    check(AdvanceOpenWait(deadline, 30099, false) == OpenWaitAction::Execute, "idle before deadline can execute");
    check(AdvanceOpenWait(deadline, 30100, false) == OpenWaitAction::Cancel, "idle at deadline cannot revive expired work");
    HWND host = CreateWindowExW(0, L"STATIC", L"owned wait test", 0, 0, 0, 1, 1, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
    check(host != nullptr, "own hidden message-only host");
    const auto now = GetTickCount64();
    {
        Coordinator reveal_queue;
        Coordinator::OpenRequest reveal;
        reveal.id[0] = 99;
        reveal.deadline = now + 10000;
        reveal.path = L"C:\\fixture\\folder";
        reveal.reveal = true;
        check(Coordinator::EncodeOpenRequest(reveal).empty(), "internal reveal cannot silently lose intent on v2 wire");
        check(reveal_queue.SubmitOpenRequest(reveal, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted,
            "folder reveal enters owned queue");
        auto open_same_id = reveal;
        open_same_id.reveal = false;
        check(reveal_queue.SubmitOpenRequest(open_same_id, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Invalid,
            "same ID with different reveal intent is rejected");
        check(reveal_queue.SubmitOpenRequest(reveal, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Duplicate,
            "same reveal retry remains duplicate");
        auto claimed = reveal_queue.TakeQueuedOpenRequest(now, true);
        check(claimed && claimed->reveal, "claim retains reveal intent");
        if (claimed) {
            AdvanceOpenWait(claimed->ui_wait_deadline, now, true);
            reveal_queue.FinishOpenClaim(std::move(*claimed), true);
        }
        auto resumed = reveal_queue.TakeQueuedOpenRequest(now + 1, true);
        check(resumed && resumed->reveal && resumed->path == reveal.path, "busy retry preserves reveal and full path");
        if (resumed) {
            Coordinator::CompleteDelivery(*resumed, true);
            reveal_queue.FinishOpenClaim(std::move(*resumed), false);
        }
    }
    auto make = [&](unsigned id) { Coordinator::OpenRequest r; r.id[0] = static_cast<unsigned char>(id); r.deadline = now + 1000;
        r.path = L"pulse:new-window"; r.delivery = std::make_shared<std::atomic<Coordinator::OpenDeliveryState>>(Coordinator::OpenDeliveryState::Pending); return r; };
    Coordinator coordinator;
    auto first = make(1);
    check(coordinator.SubmitOpenRequest(first, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted, "first request accepted");
    auto claim = coordinator.TakeQueuedOpenRequest(now, true);
    check(claim && claim->execution_started, "UI claims owned request before admission deadline");
    bool full = true;
    for (unsigned i = 2; i <= 64; ++i) full = full && coordinator.SubmitOpenRequest(make(i), now, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted;
    check(full && coordinator.SubmitOpenRequest(make(65), now, host, WM_APP + 125) == Coordinator::OpenSubmission::Retry, "claimed slot prevents concurrent producers exceeding sixty-four");
    if (claim) {
        AdvanceOpenWait(claim->ui_wait_deadline, now, true);
        coordinator.FinishOpenClaim(std::move(*claim), true);
    }
    check(first.delivery->load() == Coordinator::OpenDeliveryState::Queued, "deferral retains original identity and queued state");
    check(coordinator.SubmitOpenRequest(first, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Duplicate, "same ID remains duplicate during wait");
    auto waiting = coordinator.TakeQueuedOpenRequest(now + 1001, true);
    check(waiting && waiting->id == first.id, "expired admission requests removed while begun wait remains live");
    auto expired = coordinator.TakeOpenFailures();
    check(expired.timed_out == 63 && expired.failed == 0, "expired unbegun requests counted once without paths");
    if (waiting) {
        check(AdvanceOpenWait(waiting->ui_wait_deadline, now + 1001, false) == OpenWaitAction::Execute, "wait budget is independent of expired admission");
        Coordinator::CompleteDelivery(*waiting, true); coordinator.FinishOpenClaim(std::move(*waiting), false);
    }
    check(first.delivery->load() == Coordinator::OpenDeliveryState::Dispatched && !coordinator.TakeQueuedOpenRequest(now + 1001), "resumed request dispatches once");
    auto second = make(66);
    check(coordinator.SubmitOpenRequest(second, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted, "another explicit window ID is independently admitted");
    auto held = coordinator.TakeQueuedOpenRequest(now, true);
    coordinator.Release();
    check(second.delivery->load() == Coordinator::OpenDeliveryState::Cancelled, "release cancels live UI claim");
    if (held) coordinator.FinishOpenClaim(std::move(*held), true);
    check(!coordinator.TakeQueuedOpenRequest(now) && coordinator.TakeOpenFailures().timed_out == 0, "released claim cannot requeue or leave notifications");
    Coordinator timed;
    auto expiring = make(70);
    check(timed.SubmitOpenRequest(expiring, now, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted, "timed fixture admitted");
    auto timed_claim = timed.TakeQueuedOpenRequest(now, true);
    if (timed_claim) { AdvanceOpenWait(timed_claim->ui_wait_deadline, now, true); timed.FinishOpenClaim(std::move(*timed_claim), true); }
    auto later = make(71); later.deadline = now + 40000;
    check(timed.SubmitOpenRequest(later, now + 2000, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted &&
        expiring.delivery->load() == Coordinator::OpenDeliveryState::Queued && timed.TakeOpenFailures().timed_out == 0,
        "background admission preserves wait past original wire deadline");
    auto changed_deadline = expiring; changed_deadline.deadline = now + 40000;
    check(timed.SubmitOpenRequest(changed_deadline, now + 2000, host, WM_APP + 125) == Coordinator::OpenSubmission::Invalid,
        "wait does not authorize changing deadline of existing ID");
    auto newest = make(72); newest.deadline = now + 50000;
    check(timed.SubmitOpenRequest(newest, now + 30000, host, WM_APP + 125) == Coordinator::OpenSubmission::Accepted &&
        expiring.delivery->load() == Coordinator::OpenDeliveryState::Cancelled && timed.TakeOpenFailures().timed_out == 1,
        "background cleanup expires fixed wait and reports one timeout");
    auto next = timed.TakeQueuedOpenRequest(now + 30000);
    auto last = timed.TakeQueuedOpenRequest(now + 30000);
    check(next && last && next->id == later.id && last->id == newest.id,
        "different explicit window IDs retain independent FIFO delivery");
    if (next) Coordinator::CompleteDelivery(*next, true);
    if (last) Coordinator::CompleteDelivery(*last, true);
    timed.Release();
    ExternalOpenNotice notice;
    notice.TimedOut(false); notice.TimedOut(false); notice.Failed(false);
    check(!notice.Take(true, true, false, false) && !notice.Take(false, false, false, false) && !notice.Take(true, false, true, false), "busy hidden or existing toast retains summary");
    auto counts = notice.Take(true, false, false, false);
    check(counts.timed_out == 2 && counts.failed == 1 && !notice.Take(true, false, false, false), "multiple failures coalesce once and clear after display");
    check(FormatExternalOpenNotice(L"Timed out: {t}; failed: {f}. Retry.", counts) == L"Timed out: 2; failed: 1. Retry.", "mixed short text substitutes both counts");
    notice.Failed(false); notice.Take(false, true, true, true);
    check(!notice.pending && !notice.failed, "exit clears pending notice");
    DestroyWindow(host);
    printf("Checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
