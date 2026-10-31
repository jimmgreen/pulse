#include "../app/explorer_initial_arm.h"
#include "../app/explorer_handoff.h"
#include "../app/single_instance_coordinator.h"

#include <cstdio>
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
    using C = Coordinator; using R = C::OpenSubmission; using S = C::OpenDeliveryState;
    HWND host = QueueWindow(); Check(host != nullptr, "create private message-only queue host"); if (!host) return;
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

int main() {
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
        Check(lease.Presented(lease.path, 0, 42, lease.names, true, true, 33), "successful matching visible frame authorizes handoff");
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
        Check(lease.Presented(lease.path, 0, 7, {}, true, true, 20) && observed->ClaimClose(21),
              "This PC empty path requires and accepts matching actual presented generation");
    }
    {
        auto observed = std::make_shared<ExplorerHandoff>(100);
        ExplorerNavigationLease lease;
        lease.handoff = observed; lease.path = L"C:\\source"; lease.generation = 7;
        Check(!lease.Presented(lease.path, 0, 7, {}, true, true, 10) && !observed->ClaimClose(11),
              "successful present without completed listing cannot authorize close");
        lease.RecordListing(lease.path, 7, true, {}, 12);
        observed->Cancel();
        Check(!lease.Presented(lease.path, 0, 7, {}, true, true, 13) && !observed->ClaimClose(14),
              "late successful frame cannot resurrect cancelled handoff");
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
              lease.Presented(lease.path, 0, 8, lease.names, true, true, 16) && observed->ClaimClose(17) && observed->deadline_tick == 100,
              "replacement listing and frame retain deadline and selection authorization");
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
