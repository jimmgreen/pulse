#include "../ops/ops_manager.h"
#include "../app/extensions/transfer_snapshot.h"
#include "../app/extensions/transfer_status.h"
#include "../common/localization.h"
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <limits>
#include <string_view>
using namespace pulse;
namespace pulse::ops {
struct OpsManagerAuditAccess {
 static std::wstring Journal(OpsManager& manager) {std::lock_guard lock(manager.mutex_);return manager.JournalJsonLocked();}
 static size_t Retained(OpsManager& manager) {
  std::lock_guard lock(manager.mutex_);
  size_t count=manager.active_item_&&manager.active_item_->controlled&&manager.active_item_->controlled->retained?1:0;
  for(const auto& item:manager.queue_)if(item.controlled&&item.controlled->retained)++count;
  for(const auto& item:manager.controlled_finished_)if(item->retained)++count;
  return count;
 }
};
}
namespace {
int passed=0,failed=0;
void Check(bool ok,const char* label){std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);ok?++passed:++failed;}
struct Fixture {
 std::filesystem::path root;
 extensions::authorization::Registry registry;
 extensions::authorization::PackageIdentity package{0,"ops.fixture","revision",1};
 extensions::authorization::OwnerHandle owner;
 extensions::authorization::PrincipalBinding binding;
 std::mutex gate_mutex;std::condition_variable gate_cv;bool gate=false,entered=false,released=false;
 const std::thread::id main_thread=std::this_thread::get_id();
 extensions::transfer_plan::Store store;
 ops::OpsManager manager;
 extensions::authorization::OwnerHandle Setup(){registry.SyncPackages({{package,true}});return *registry.RegisterOwner(1);}
 explicit Fixture(size_t plan_limit=32):owner(Setup()),binding(*registry.CaptureBinding(package,owner)),store(registry,{plan_limit},[this]{
   std::unique_lock lock(gate_mutex);if(gate&&std::this_thread::get_id()!=main_thread){entered=true;gate_cv.notify_all();gate_cv.wait(lock,[&]{return released;});}return uint64_t{100};}) {
  root=std::filesystem::absolute(std::filesystem::path("bench_data")/("controlled-ops-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64())));
  if(!std::filesystem::create_directory(root))throw std::runtime_error("unique fixture failed");
  std::filesystem::create_directory(root/"from");std::filesystem::create_directory(root/"to");manager.Start([]{},false);
 }
 ~Fixture(){Release();manager.Stop();if(root.parent_path()==std::filesystem::absolute("bench_data")&&root.filename().wstring().starts_with(L"controlled-ops-")){std::error_code error;std::filesystem::remove_all(root,error);}}
 void Release(){std::lock_guard lock(gate_mutex);released=true;gate_cv.notify_all();}
 bool WaitGate(){std::unique_lock lock(gate_mutex);return gate_cv.wait_for(lock,std::chrono::seconds(10),[&]{return entered;});}
 extensions::transfer_plan::ExecutionPermit Permit(const std::string& name,bool move=false,bool readonly=false){
  auto path=root/"from"/name;{std::ofstream out(path,std::ios::binary);out<<std::string(8192,'x');}
  if(readonly&&!SetFileAttributesW(path.c_str(),FILE_ATTRIBUTE_READONLY))throw std::runtime_error("readonly fixture failed");
  std::atomic<bool> cancel=false;auto source=extensions::transfer_snapshot::CaptureSource(path.wstring(),cancel);auto target=extensions::transfer_snapshot::CaptureTarget((root/"to").wstring(),cancel);
  if(!source||!target)throw std::runtime_error("capture failed");extensions::transfer_plan::Draft draft;draft.operation=move?extensions::transfer_plan::Operation::Move:extensions::transfer_plan::Operation::Copy;draft.sources.push_back(source.value->snapshot);draft.target=target.value->snapshot;
  auto plan=store.Create(binding,draft);if(!plan)throw std::runtime_error("plan failed");auto receipt=store.Confirm(*plan.value,binding);if(!receipt)throw std::runtime_error("confirm failed");auto permit=store.Consume(*receipt.value,binding);if(!permit)throw std::runtime_error("consume failed");return *permit.value;
 }
 ops::ControlledTaskStatus Wait(uint64_t id){for(int i=0;i<1000;++i){auto state=manager.QueryControlled(id,binding);if(state.result&&manager.FinalStatus(id))return state;std::this_thread::sleep_for(std::chrono::milliseconds(10));}return {};}
};
void RetentionChecks() {
 Fixture f(128);
 const auto first=f.manager.SubmitControlled(f.Permit("unread-native.txt"),f.registry,true);
 Check(first&&f.Wait(first).result.has_value(),"retained controlled request reaches terminal state");
 uint64_t legacy=0;
 for(int i=0;i<40;++i){
  const auto id=f.manager.SubmitControlled(f.Permit("later-"+std::to_string(i)+".txt"),f.registry);
  if(!i)legacy=id;
  if(!id||!f.Wait(id).result)throw std::runtime_error("later transfer timeout");
  f.manager.DrainFinishedTasks();
 }
 Check(f.manager.QueryControlled(first,f.binding).result.has_value(),"unread native terminal survives forty later ordinary transfers");
 Check(f.manager.QueryControlled(legacy,f.binding).error==extensions::transfer_plan::Error::NotFound,
  "legacy controlled requests still use the ordinary sixteen-result history");
 auto other=f.package;other.slot=1;other.id="other.retention";
 f.registry.SyncPackages({{f.package,true},{other,true}});
 const auto foreign=*f.registry.CaptureBinding(other,f.owner);
 const auto other_owner=*f.registry.RegisterOwner(2);
 const auto foreign_owner=*f.registry.CaptureBinding(f.package,other_owner);
 Check(f.manager.QueryControlled(first,foreign).error==extensions::transfer_plan::Error::NotFound&&
  !f.manager.ReleaseControlled(first,foreign)&&!f.manager.ReleaseControlled(first,foreign_owner)&&
  f.manager.QueryControlled(first,f.binding).result.has_value(),"other packages and owners cannot read or release a retained terminal");
 Check(f.manager.ReleaseControlled(first,f.binding)&&ops::OpsManagerAuditAccess::Retained(f.manager)==0&&
  f.manager.QueryControlled(first,f.binding).error==extensions::transfer_plan::Error::NotFound,
  "consumption releases and removes the exact retained terminal result");
 std::vector<uint64_t> retained;
 for(int i=0;i<32;++i){
  const auto id=f.manager.SubmitControlled(f.Permit("bounded-"+std::to_string(i)+".txt"),f.registry,true);
  if(!id||!f.Wait(id).result)throw std::runtime_error("retained transfer timeout");
  retained.push_back(id);f.manager.DrainFinishedTasks();
 }
 Check(ops::OpsManagerAuditAccess::Retained(f.manager)==32&&
  !f.manager.SubmitControlled(f.Permit("over-limit.txt"),f.registry,true),"one principal can retain at most thirty-two controlled requests");
 Check(f.manager.ReleaseControlled(retained.front(),f.binding),"consumption releases a full retention slot");
 const auto replacement=f.manager.SubmitControlled(f.Permit("replacement.txt"),f.registry,true);
 Check(replacement&&f.Wait(replacement).result&&ops::OpsManagerAuditAccess::Retained(f.manager)==32,
  "a consumed retention slot accepts a replacement request");
 f.registry.SyncPackages({{f.package,false},{other,true}});
 for(const auto id:retained)f.manager.ReleaseControlled(id,f.binding);
 f.manager.ReleaseControlled(replacement,f.binding);
 Check(ops::OpsManagerAuditAccess::Retained(f.manager)==0,"revocation still permits exact host cleanup of retained results");

 Fixture active;
 active.gate=true;
 const auto running=active.manager.SubmitControlled(active.Permit("retained-active.txt"),active.registry,true);
 Check(running&&active.WaitGate(),"retained cancellation fixture reaches worker gate");
 const auto queued=active.manager.SubmitControlled(active.Permit("retained-queued.txt"),active.registry,true);
 Check(active.manager.CancelControlled(queued,active.binding)&&
  active.manager.ReleaseControlled(queued,active.binding)&&
  active.manager.QueryControlled(running,active.binding).cancel_requested==false&&
  ops::OpsManagerAuditAccess::Retained(active.manager)==1,"queued cancellation and release preserve the active retention lease");
 active.registry.UnregisterOwner(active.owner);
 Check(active.manager.CancelTask(running)&&active.manager.ReleaseControlled(running,active.binding)&&
  ops::OpsManagerAuditAccess::Retained(active.manager)==0,"owner closure cancels and releases only its exact retained active task");
 active.Release();active.manager.Stop();
 Check(!std::filesystem::exists(active.root/"to"/"retained-active.txt")&&
  !std::filesystem::exists(active.root/"to"/"retained-queued.txt"),"released active and queued cancellation never commit their source files");
}
}
int main(int argc,char** argv){pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");std::setvbuf(stdout,nullptr,_IONBF,0);
 if(argc==2&&std::string_view(argv[1])=="--retention"){
  try{RetentionChecks();}catch(const std::exception& error){std::printf("[FAIL] exception: %s\n",error.what());++failed;}
  std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
 }
 try{
 {
 ops::ControlledTransferResult result; result.complete=true;
 Check(extensions::ClassifyTransferResult(result)==extensions::TransferDisplayState::Failed,"empty result cannot claim completion");
 result.complete=false; result.items.resize(2); result.items[0].state=ops::ControlledTransferState::Failed; result.items[1].state=ops::ControlledTransferState::Cancelled;
 Check(extensions::ClassifyTransferResult(result)==extensions::TransferDisplayState::Failed,"mixed failure and cancellation preserves failure");
 result.items[0].state=ops::ControlledTransferState::Copied;
 Check(extensions::ClassifyTransferResult(result)==extensions::TransferDisplayState::Partial,"cancellation after a committed item remains partial");
 result.items.resize(1); result.complete=true; result.error=ERROR_WRITE_FAULT;
 Check(extensions::ClassifyTransferResult(result)==extensions::TransferDisplayState::Partial,"error alongside completion flag cannot claim full success");
 }
 {Fixture f;auto id=f.manager.SubmitControlled(f.Permit("readonly.txt",true,true),f.registry);auto result=f.Wait(id);
 Check(result.result&&!result.result->complete&&result.result->items[0].state==ops::ControlledTransferState::DestinationCommittedSourceRetained,"readonly move preserves actual destination and retained source outcome");
 Check(result.result && extensions::ClassifyTransferResult(*result.result) == extensions::TransferDisplayState::Partial,"retained source is displayed as partial, never completed");
 Check(!result.progress.last_error.empty()&&result.progress.last_error.find(L"{retained}")==std::wstring::npos&&result.progress.last_error.find(L"1")!=std::wstring::npos,"partial move publishes resolved user-visible counts and error");
 SetFileAttributesW((f.root/"from"/"readonly.txt").c_str(),FILE_ATTRIBUTE_NORMAL);SetFileAttributesW((f.root/"to"/"readonly.txt").c_str(),FILE_ATTRIBUTE_NORMAL);}
 {Fixture f;extensions::transfer_plan::Draft draft;draft.target.path=(f.root/"to").wstring();draft.sources.push_back({(f.root/"from"/"huge1").wstring(),{0,{},(std::numeric_limits<uint64_t>::max)(),0},{}});draft.sources.push_back({(f.root/"from"/"huge2").wstring(),{0,{},1,0},{}});
 auto plan=f.store.Create(f.binding,draft);auto receipt=f.store.Confirm(*plan.value,f.binding);auto permit=f.store.Consume(*receipt.value,f.binding);auto id=f.manager.SubmitControlled(*permit.value,f.registry);auto result=f.Wait(id);
 Check(result.result&&result.result->error==ERROR_ARITHMETIC_OVERFLOW&&result.result->permit_error==extensions::transfer_plan::Error::InvalidDraft&&result.progress.total_bytes==0,"host-only overflow plan fails before execution without wrapped progress");}
 {Fixture f;f.gate=true;auto first=f.manager.SubmitControlled(f.Permit("active.txt"),f.registry);Check(first&&f.WaitGate(),"controlled task reaches real worker start gate");
 auto queued=f.manager.SubmitControlled(f.Permit("queued.txt"),f.registry);auto next=f.manager.SubmitControlled(f.Permit("next.txt"),f.registry);
 auto other=f.package;other.slot=1;other.id="other";f.registry.SyncPackages({{f.package,true},{other,true}});auto foreign=*f.registry.CaptureBinding(other,f.owner);
 Check(f.manager.QueryControlled(first,foreign).error==extensions::transfer_plan::Error::NotFound&&!f.manager.CancelControlled(first,foreign),"foreign caller cannot query or cancel active task");
 Check(f.manager.CancelControlled(queued,f.binding)&&!f.manager.QueryControlled(first,f.binding).cancel_requested,"queued cancellation does not cancel active task");
 Check(f.manager.CancelControlled(first,f.binding)&&!f.manager.QueryControlled(next,f.binding).cancel_requested,"active cancellation does not cancel next task");
 auto journal=ops::OpsManagerAuditAccess::Journal(f.manager);Check(journal.find(L"active.txt")==std::wstring::npos&&journal.find(L"queued.txt")==std::wstring::npos,"active and queued controlled tasks excluded from recovery journal");
 f.manager.PauseCurrent();Check(!f.manager.Status().can_pause&&!f.manager.Status().pause_requested,"controlled progress does not advertise pause");f.Release();
 auto a=f.Wait(first),b=f.Wait(queued),c=f.Wait(next);
 Check(a.result&&!a.result->complete&&b.result&&!b.result->complete,"active and queued cancellation both publish detailed terminal results");
 Check(a.result && b.result && extensions::ClassifyTransferResult(*a.result) == extensions::TransferDisplayState::Cancelled && extensions::ClassifyTransferResult(*b.result) == extensions::TransferDisplayState::Cancelled,"active and queued cancellations have cancelled display status");
 Check(c.result && extensions::ClassifyTransferResult(*c.result) == extensions::TransferDisplayState::Completed,"completed worker result has completed display status");
 Check(c.result&&c.result->complete&&std::filesystem::exists(f.root/"to"/"next.txt"),"following task executes normally after scoped cancellation");
 Check(!std::filesystem::exists(f.root/"to"/"active.txt")&&!std::filesystem::exists(f.root/"to"/"queued.txt"),"cancelled tasks do not commit files");
 Check(f.manager.QueryControlled(next,foreign).error==extensions::transfer_plan::Error::NotFound,"foreign caller cannot read successful terminal result");
 Check(f.manager.RetryFailedItems(first).empty()&&!f.manager.RetryLockedOperation(first,false)&&!f.manager.CanUndoTask(next),"controlled tasks cannot enter path-only retry or undo");
 auto finished=f.manager.DrainFinishedTasks();Check(finished.size()==3&&finished[0].controlled_result&&finished[1].controlled_result&&finished[2].controlled_result,"normal finished-task drain preserves controlled details");
 Check(f.manager.RetainTask(next),"exact task can retain its terminal snapshot for async native request");
 for(int i=0;i<16;++i){auto id=f.manager.SubmitControlled(f.Permit("retained"+std::to_string(i)+".txt"),f.registry);if(!f.Wait(id).result)throw std::runtime_error("retention task timeout");}
 Check(f.manager.QueryControlled(next,f.binding).error==extensions::transfer_plan::Error::NotFound,"evicted terminal task explicitly reports not found");
 const auto retained=f.manager.QueryTask(next);Check(retained&&!retained->active&&retained->phase==ops::OpPhase::Completed,"retained exact task survives ordinary terminal history eviction");
 f.manager.ReleaseTask(next);Check(!f.manager.QueryTask(next),"release removes retained snapshot after ordinary history eviction");}
 {
     Fixture f;
     f.gate = true;
     const auto active = f.manager.SubmitControlled(f.Permit("exact-active.txt"), f.registry);
     Check(active && f.WaitGate(), "exact task cancellation fixture reaches worker gate");
     const auto source = f.root / "from" / "cancel-recycle.txt";
     std::ofstream(source) << "must survive queued cancellation\n";
     ops::OpRequest deletion;
     deletion.type = ops::OpType::RecycleDelete;
     deletion.sources = {source.wstring()};
     const auto queued = f.manager.Submit(std::move(deletion));
     const auto following = f.manager.SubmitControlled(f.Permit("exact-following.txt"), f.registry);
     const auto queued_status = f.manager.QueryTask(queued);
     Check(queued_status && queued_status->task_id == queued && queued_status->phase == ops::OpPhase::Queued,
         "exact lookup identifies queued ordinary delete without adopting active task");
     Check(f.manager.CancelTask(queued) && !f.manager.QueryControlled(active, f.binding).cancel_requested,
         "exact queued delete cancellation leaves active task untouched");
     Check(f.manager.CancelTask(active) && !f.manager.QueryControlled(following, f.binding).cancel_requested,
         "exact active cancellation leaves following task untouched");
     Check(!f.manager.CancelTask(0) && !f.manager.CancelTask(UINT64_MAX) && !f.manager.QueryTask(UINT64_MAX),
         "unknown exact task IDs are rejected");
     f.Release();
     f.Wait(active);
     const auto completed = f.Wait(following);
     const auto deleted = f.manager.QueryTask(queued);
     Check(deleted && !deleted->active && deleted->phase == ops::OpPhase::Failed && deleted->completed_items == 0 &&
         std::filesystem::exists(source), "cancelled queued delete reaches terminal result without touching source");
     Check(completed.result && completed.result->complete && std::filesystem::exists(f.root / "to" / "exact-following.txt"),
         "following task still commits after exact cancellations");
 }
 {Fixture f;f.gate=true;auto active=f.manager.SubmitControlled(f.Permit("global.txt"),f.registry);Check(active&&f.WaitGate(),"global cancellation fixture active");f.manager.CancelCurrent();Check(f.manager.QueryControlled(active,f.binding).cancel_requested,"global cancel reaches controlled task flag");f.Release();Check(f.Wait(active).result.has_value(),"global cancellation publishes terminal result");}
 {Fixture f;f.gate=true;auto active=f.manager.SubmitControlled(f.Permit("stop-active.txt"),f.registry);Check(active&&f.WaitGate(),"stop fixture active");auto queued=f.manager.SubmitControlled(f.Permit("stop-queued.txt"),f.registry);
 std::thread stopper([&]{f.manager.Stop();});for(int i=0;i<1000&&!f.manager.QueryControlled(active,f.binding).cancel_requested;++i)std::this_thread::sleep_for(std::chrono::milliseconds(1));f.Release();stopper.join();
 auto q=f.manager.QueryControlled(queued,f.binding);Check(q.result&&q.result->permit_error==extensions::transfer_plan::Error::Cancelled&&!q.result->items.empty(),"stop retains explicit queued cancellation details");}
 }catch(const std::exception& error){std::printf("[FAIL] exception: %s\n",error.what());++failed;}
 std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;}
