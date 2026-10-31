#include "../app/single_instance_coordinator.h"
#include "../app/explorer_submission_port.h"
#include <cstdio>
#include <thread>
#include <latch>
using namespace pulse::app;
using C = SingleInstanceCoordinator;
namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) { ++checks; if (!ok) ++failures; std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text); }
constexpr UINT wake = WM_APP + 945;
HWND Host() { return CreateWindowExW(0,L"STATIC",L"queue fixture",0,0,0,0,0,HWND_MESSAGE,nullptr,GetModuleHandleW(nullptr),nullptr); }
auto Context() { return MakeExplorerOpenContext(L"C:\\fixture", {L"selected.txt"}, std::make_shared<ExplorerHandoff>(GetTickCount64()+10000), true); }
// Mirrors Watcher::Take: only a newly submitted source is cancelled on rejection.
bool SubmitNew(const std::shared_ptr<ExplorerSubmissionPort>& port, const std::shared_ptr<const ExplorerOpenContext>& source) {
    if (port && port->Submit(source)) return true;
    source->handoff->Cancel(); return false;
}
C::OpenRequest Ordinary(unsigned id) {
    C::OpenRequest r; r.id[0]=static_cast<unsigned char>(id); r.id[1]=static_cast<unsigned char>(id>>8);
    r.path=L"pulse:win-e"; r.deadline=GetTickCount64()+10000; return r;
}
}
int main() {
    HWND host=Host(); Check(host!=nullptr,"private message-only host"); if (!host) return 1;
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context();
        Check(port && source,"port and immutable source creation"); if (!port || !source) return 1;
        Check(port->Submit(source) && port->Submit(source),"same internal identity accepted then duplicate");
        auto r=c.TakeQueuedOpenRequest(GetTickCount64(),true);
        Check(r && r->explorer==source && !r->explorer_received,"first claim owns original context");
        if (r) {
            Check(source->handoff->Receive(GetTickCount64()),"first UI Receive succeeds"); r->explorer_received=true;
            r->ui_wait_deadline=GetTickCount64()+30000;
            c.FinishOpenClaim(std::move(*r),true);
            r=c.TakeQueuedOpenRequest(GetTickCount64(),true);
            Check(r && r->explorer_received && r->deadline==source->handoff->deadline_tick,"busy retry preserves Receive marker and fixed source deadline");
            if (r) { C::CompleteDelivery(*r,true); c.FinishOpenClaim(std::move(*r),false); }
            Check(source->handoff->state==HandoffState::Pending,"dispatch transfers lease without Ready or cancellation");
        }
        Check(!c.TakeQueuedOpenRequest(),"duplicate has no second queued execution");
    }
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context();
        C::OpenRequest raw; raw.id=source->id; raw.path=source->folder; raw.deadline=source->handoff->deadline_tick;
        Check(c.SubmitOpenRequest(raw,GetTickCount64(),host,wake)==C::OpenSubmission::Accepted && port->Submit(source),"wire and internal same ID have distinct identities");
        auto first=c.TakeQueuedOpenRequest(), second=c.TakeQueuedOpenRequest();
        Check(first && second && !first->explorer && second->explorer==source,"mixed FIFO is shared and source namespace preserved");
        auto forged=raw; forged.explorer=Context();
        Check(C::EncodeOpenRequest(forged).empty(),"internal payload cannot encode on RawV2");
        Check(c.SubmitOpenRequest(forged,GetTickCount64(),host,wake)==C::OpenSubmission::Invalid && forged.explorer->handoff->state==HandoffState::Pending,"public submission rejects internal payload without stealing producer ownership");
        const auto bytes=C::EncodeOpenRequest(raw); COPYDATASTRUCT data{C::OpenRequestMessageId(),static_cast<DWORD>(bytes.size()),const_cast<unsigned char*>(bytes.data())};
        Check(C::DecodeOpenRequest(&data,forged) && !forged.explorer && !forged.explorer_received,"RawV2 decode clears all internal context");
    }
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context(); port->Submit(source);
        auto r=c.TakeQueuedOpenRequest(GetTickCount64(),true);
        if (r) { r->explorer_received=source->handoff->Receive(GetTickCount64()); r->ui_wait_deadline=UINT64_MAX; c.FinishOpenClaim(std::move(*r),true); }
        Check(!c.TakeQueuedOpenRequest(source->handoff->deadline_tick,true) && source->handoff->state==HandoffState::Expired,"30 second ordinary budget cannot extend Explorer deadline");
        const auto notices=c.TakeOpenFailures(); Check(notices.timed_out==0 && notices.failed==0,"Explorer expiry never becomes ordinary toast");
        auto cancelled=Context(); port->Submit(cancelled); cancelled->handoff->Cancel();
        Check(!c.TakeQueuedOpenRequest() && cancelled->handoff->state==HandoffState::Cancelled,"source cancellation discards queued request");
    }
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context(); port->Submit(source);
        auto claim=c.TakeQueuedOpenRequest(GetTickCount64(),true);
        bool filled=true; for (unsigned i=1;i<64;++i) filled &= c.SubmitOpenRequest(Ordinary(i),GetTickCount64(),host,wake)==C::OpenSubmission::Accepted;
        auto overflow=Context(); Check(filled && !SubmitNew(port,overflow) && overflow->handoff->state==HandoffState::Cancelled,"claim reserves original slot; full shared queue cancels rejected source");
        if (claim) c.FinishOpenClaim(std::move(*claim),true);
        unsigned count=0; while (auto r=c.TakeQueuedOpenRequest()) { ++count; C::CompleteDelivery(*r,false); }
        Check(count==64,"busy defer retains reserved slot amid concurrent ordinary admission");
    }
    {
        auto c=std::make_unique<C>(); auto old=c->CreateExplorerSubmissionPort(host,wake);
        auto fresh=c->CreateExplorerSubmissionPort(host,wake); auto late=Context();
        Check(!SubmitNew(old,late) && late->handoff->state==HandoffState::Cancelled,"replacement revokes previous Watcher port");
        auto source=Context(); Check(fresh->Submit(source),"new Watcher port remains usable");
        auto claim=c->TakeQueuedOpenRequest(GetTickCount64(),true); c->Release();
        Check(source->handoff->state==HandoffState::Cancelled && claim && claim->delivery->load()==C::OpenDeliveryState::Cancelled,"Release cancels outstanding claim and handoff");
        c.reset(); std::latch go(1); auto after=Context(); bool submitted=true;
        std::thread worker([&]{go.wait();submitted=SubmitNew(fresh,after);}); go.count_down(); worker.join();
        Check(!submitted && after->handoff->state==HandoffState::Cancelled,"late worker retained port cannot access destroyed coordinator");
    }
    {
        C c; HWND doomed=Host(); auto port=c.CreateExplorerSubmissionPort(doomed,wake); DestroyWindow(doomed);
        auto source=Context(); Check(port && !SubmitNew(port,source) && source->handoff->state==HandoffState::Cancelled && !c.TakeQueuedOpenRequest(),"destroyed wake host leaves no accepted queue payload");
        auto replacement=c.CreateExplorerSubmissionPort(host,wake); auto next=Context();
        Check(replacement && replacement->Submit(next),"failed old host does not poison new Watcher");
    }
    {
        // Fill only this disposable thread's message queue. This forces the real
        // PostMessage failure after HWND validation without touching user windows.
        bool quota=false, rejected=false, empty=false, recovered=false;
        std::thread worker([&] {
            HWND own=Host(); C c; auto port=c.CreateExplorerSubmissionPort(own,wake);
            unsigned posted=0;
            while (posted<20000 && PostMessageW(own,wake,0,0)) ++posted;
            quota=posted<20000 && GetLastError()==ERROR_NOT_ENOUGH_QUOTA;
            if (quota && port) {
                auto source=Context(); rejected=!SubmitNew(port,source) && source->handoff->state==HandoffState::Cancelled;
                empty=!c.TakeQueuedOpenRequest();
                MSG message{}; while (PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {}
                auto next=Context(); recovered=port->Submit(next);
            }
            DestroyWindow(own);
        }); worker.join();
        Check(quota,"private thread reaches real PostMessage queue quota");
        Check(rejected && empty,"Post failure cancels Explorer and rolls back acceptance plus payload");
        Check(recovered,"drained wake queue accepts a fresh source");
    }
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context();
        std::latch start(1); bool accepted=false;
        std::thread worker([&]{start.wait(); accepted=port->Submit(source);});
        start.count_down(); port->Revoke(); worker.join();
        auto request=c.TakeQueuedOpenRequest();
        Check(accepted ? bool(request) : !request,"Submit versus Revoke is one atomic ownership boundary");
        if (request) C::CompleteDelivery(*request,false);
        auto after=Context(); Check(!port->Submit(after),"no submission is admitted after Revoke returns");
    }
    for (bool ready : {false,true}) {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context();
        Check(port->Submit(source),"ownership fixture source accepted");
        auto request=c.TakeQueuedOpenRequest(GetTickCount64(),true);
        Check(bool(request),"ownership fixture source claimed");
        if (!request) continue;
        C::CompleteDelivery(*request,true);
        if (ready) Check(source->handoff->Ready(GetTickCount64()),"ownership fixture publishes Ready");
        const auto expected=ready?HandoffState::Ready:HandoffState::Pending;
        Check(port->Submit(source) && source->handoff->state==expected,"dispatched Pending/Ready retry is duplicate without cancelling lease");
        Check(!c.TakeQueuedOpenRequest(),"dispatched retry does not enqueue a second delivery");
        port->Revoke();
        Check(!port->Submit(source) && source->handoff->state==expected,"revoked port rejection preserves dispatched Pending/Ready lease");
        c.Release();
        Check(source->handoff->state==expected && request->delivery->load()==C::OpenDeliveryState::Dispatched,"Release between dispatch and Finish preserves transferred lease");
        c.FinishOpenClaim(std::move(*request),false);
        Check(source->handoff->state==expected,"late Finish after Release does not cancel transferred lease");
        source->handoff->Cancel(); // Simulate final UI lease destruction.
    }
    {
        C c; auto port=c.CreateExplorerSubmissionPort(host,wake); auto source=Context();
        source->handoff->Ready(GetTickCount64());
        Check(!port->Submit(source) && !c.TakeQueuedOpenRequest(),"new Ready context cannot use committed-duplicate exception");
        source->handoff->Cancel();
        auto expired=MakeExplorerOpenContext(L"C:\\fixture",{},std::make_shared<ExplorerHandoff>(GetTickCount64()),true);
        Check(!SubmitNew(port,expired) && expired->handoff->state==HandoffState::Cancelled,"producer cancels expired unaccepted source");
    }
    DestroyWindow(host);
    std::printf("Checks: %d, failures: %d\n",checks,failures); return failures?1:0;
}
