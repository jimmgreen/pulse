#include "../app/explorer_handoff.h"
#include "../app/single_instance_coordinator.h"

#include <cstdio>

namespace {
int failures = 0;
void Check(bool condition, const char* name) {
    std::printf("[%s] %s\n", condition ? "PASS" : "FAIL", name);
    if (!condition) ++failures;
}
}

int main() {
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
        Check(!lease.Complete(lease.path, 41, true, lease.names, 20) && handoff->state == S::Pending,
              "stale generation cannot acknowledge navigation");
        Check(lease.Complete(lease.path, 42, true, {L"a.txt", L"b.txt"}, 30),
              "fresh enumeration and exact restored selection acknowledge navigation");
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
        Check(!lease.Complete(lease.path, 1, true, {}, 10) && handoff->state == S::Cancelled,
              "missing selected item preserves Explorer");
    }
    handoff = std::make_shared<ExplorerHandoff>(1000);
    {
        ExplorerNavigationLease lease;
        lease.handoff = handoff;
        lease.path = L"C:\\source";
        lease.generation = 1;
        Check(!lease.Complete(lease.path, 1, false, {}, 10) && !handoff->Ready(20),
              "enumeration failure cannot be revived by late success");
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
        Check(!lease.Complete(L"C:\\elsewhere", 1, true, {}, 10) && handoff->state == S::Cancelled,
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
    return failures ? 1 : 0;
}
