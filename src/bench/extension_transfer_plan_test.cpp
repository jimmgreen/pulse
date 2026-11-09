#include "../app/extensions/transfer_plan.h"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <thread>
#include <type_traits>
using namespace pulse::extensions;
using namespace transfer_plan;
namespace {
int passed=0, failed=0;
void Check(bool ok,const char* label) { std::printf("[%s] %s\n",ok?"PASS":"FAIL",label); ok?++passed:++failed; }
struct Fixture {
 uint64_t now=100; uint8_t nonce=1; bool random_ok=true, repeat=false;
 authorization::Registry registry;
 authorization::PackageIdentity package{0,"test.package","revision",1};
 authorization::OwnerHandle owner;
 authorization::PrincipalBinding binding;
 Store store;
 Fixture(Limits limits={}) : registry({},[this]{return now;}), owner(Setup()),
 binding(*registry.CaptureBinding(package,owner)), store(registry,limits,[this]{return now;},[this](std::span<uint8_t> out){
 std::fill(out.begin(),out.end(),nonce); if(!repeat) ++nonce; return random_ok;}) {}
 authorization::OwnerHandle Setup(){registry.SyncPackages({{package,true}});return *registry.RegisterOwner(7);}
};
Draft DraftValue(){Draft d;d.sources.push_back({L"C:\\fixture\\source.txt",{9,{1},10,20},{9,{3}}});d.target={L"C:\\fixture\\dest",{9,{2},0,30}};return d;}
}
int main(){
 static_assert(!std::is_default_constructible_v<ExecutionPermit>);
 static_assert(!std::is_constructible_v<ExecutionPermit,Store::FrozenPlan>);
 {Fixture f;auto draft=DraftValue();auto id=*f.store.Create(f.binding,draft).value;auto frozen=*f.store.Read(id,f.binding).value;
 draft.sources[0].parent.volume=999;draft.sources[0].identity.size=999;draft.sources[0].path=L"changed";draft.sources.clear();draft.target.path=L"other";
 Check(frozen->transfer.sources.size()==1&&frozen->transfer.sources[0].identity.size==10&&frozen->transfer.sources[0].parent.volume==9&&frozen->transfer.target.path==L"C:\\fixture\\dest","draft mutation cannot change frozen plan");
 Check(frozen->digest.size()==64,"plan has SHA256 digest");auto receipt=*f.store.Confirm(id,f.binding).value;
 Check(*f.store.Confirm(id,f.binding).value==receipt,"repeat confirmation reuses one receipt");
 auto permit=*f.store.Consume(receipt,f.binding).value;
 Check(&permit.Plan()==frozen.get()&&permit.Principal().Package()==f.package,"consume returns permit for identical frozen plan and principal");
 Check(permit.TryStart(f.registry,f.binding)==Error::None&&permit.TryStart(f.registry,f.binding)==Error::AlreadyStarted,"permit start succeeds only once");
 Check(f.store.Consume(receipt,f.binding).error==Error::AlreadyConsumed,"receipt only consumed once");
 Check(f.store.Consume(std::string(80,'f'),f.binding).error==Error::NotFound,"unknown receipt rejected");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;
 auto other=f.package;other.slot=1;other.id="other";f.registry.SyncPackages({{f.package,true},{other,true}});
 auto foreign=*f.registry.CaptureBinding(other,f.owner);auto other_owner=*f.registry.RegisterOwner(8);auto foreign_owner=*f.registry.CaptureBinding(f.package,other_owner);
 Check(f.store.Read(id,foreign).error==Error::NotFound&&f.store.Confirm(id,foreign).error==Error::NotFound&&f.store.Consume(receipt,foreign).error==Error::NotFound,"cross-package records hidden");
 Check(f.store.Consume(receipt,foreign_owner).error==Error::NotFound,"cross-owner receipt rejected");
 f.registry.SyncPackages({{f.package,false}});f.registry.SyncPackages({{f.package,true}});auto rebound=*f.registry.CaptureBinding(f.package,f.owner);
 Check(f.store.Consume(receipt,f.binding).error==Error::Revoked&&f.store.Consume(receipt,rebound).error==Error::Revoked,"disable-enable cannot revive plan with old or new binding");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto changed=f.package;++changed.generation;f.registry.SyncPackages({{changed,true}});
 Check(f.store.Read(id,f.binding).error==Error::Revoked,"reload revokes plan");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;f.registry.RegisterOwner(f.owner.key);
 Check(f.store.Read(id,f.binding).error==Error::Revoked,"owner key reuse revokes plan");}
 {Limits l;l.lifetime_ms=10;Fixture f(l);auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;f.now+=10;
 Check(f.store.Confirm(id,f.binding).error==Error::Expired&&f.store.Consume(receipt,f.binding).error==Error::Expired,"exact expiration rejects confirm and consume");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;
 Check(f.store.Cancel(id,f.binding)==Error::None&&f.store.Consume(receipt,f.binding).error==Error::Cancelled,"cancel invalidates confirmed receipt");}
 {Fixture f;f.random_ok=false;Check(f.store.Create(f.binding,DraftValue()).error==Error::RandomFailed,"creation fails closed without entropy");f.random_ok=true;auto id=*f.store.Create(f.binding,DraftValue()).value;f.random_ok=false;
 Check(f.store.Confirm(id,f.binding).error==Error::RandomFailed,"confirmation fails closed without entropy");f.random_ok=true;Check(bool(f.store.Confirm(id,f.binding)),"failed confirmation leaves pending plan retryable");}
 {Fixture f;f.repeat=true;auto id=*f.store.Create(f.binding,DraftValue()).value;Check(f.store.Confirm(id,f.binding).error==Error::RandomFailed,"duplicate random nonce rejected");}
 {Limits l;l.plans=1;l.lifetime_ms=10;Fixture f(l);auto id=f.store.Create(f.binding,DraftValue());Check(f.store.Create(f.binding,DraftValue()).error==Error::Quota,"live record quota enforced");f.now+=10;Check(bool(f.store.Create(f.binding,DraftValue())),"expired record cleanup frees quota");}
 {Fixture f;auto d=DraftValue();d.sources.clear();Check(f.store.Create(f.binding,d).error==Error::InvalidDraft,"empty source rejected");d=DraftValue();d.operation=static_cast<Operation>(9);Check(f.store.Create(f.binding,d).error==Error::InvalidDraft,"unknown operation rejected");d=DraftValue();d.target.path.push_back(0);Check(f.store.Create(f.binding,d).error==Error::InvalidDraft,"embedded NUL rejected");}
 {Limits l;l.plan_bytes=10;Fixture f(l);Check(f.store.Create(f.binding,DraftValue()).error==Error::InvalidDraft,"complete canonical size bounded");}
 {auto d=DraftValue();Limits l;l.plan_bytes=21+32+48+72+8*(d.sources[0].path.size()+d.target.path.size());Fixture exact(l);
 Check(bool(exact.store.Create(exact.binding,d)),"exact canonical byte limit accepted");--l.plan_bytes;Fixture short_limit(l);
 Check(short_limit.store.Create(short_limit.binding,d).error==Error::InvalidDraft,"one byte below canonical size rejected");}
 {Fixture f;auto d=DraftValue();d.sources.push_back(d.sources[0]);
 Check(f.store.Create(f.binding,d).error==Error::InvalidDraft,"duplicate exact source path rejected");
 d.sources[1].path=L"C:\\fixture\\hardlink.txt";
 Check(bool(f.store.Create(f.binding,d)),"distinct hardlink name with same file identity accepted");}
 {Limits l;l.sources=1;Fixture f(l);auto d=DraftValue();d.sources.push_back(d.sources[0]);Check(f.store.Create(f.binding,d).error==Error::InvalidDraft,"source quota enforced");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;std::atomic<int> successes=0,duplicates=0;std::vector<std::thread> workers;
 for(int i=0;i<8;++i)workers.emplace_back([&]{auto r=f.store.Consume(receipt,f.binding);if(r)++successes;else if(r.error==Error::AlreadyConsumed)++duplicates;});for(auto& t:workers)t.join();Check(successes==1&&duplicates==7,"concurrent consumption succeeds exactly once");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;
 Check(f.store.Consume(id,f.binding).error==Error::NotFound,"unconfirmed plan identifier cannot obtain permit");
 auto receipt=*f.store.Confirm(id,f.binding).value;auto permit=*f.store.Consume(receipt,f.binding).value;
 auto other=f.package;other.slot=1;other.id="other";f.registry.SyncPackages({{f.package,true},{other,true}});auto foreign=*f.registry.CaptureBinding(other,f.owner);
 Check(permit.TryStart(f.registry,foreign)==Error::NotFound,"permit rejects cross-package binding");
 auto owner=*f.registry.RegisterOwner(8);auto foreign_owner=*f.registry.CaptureBinding(f.package,owner);
 Check(permit.TryStart(f.registry,foreign_owner)==Error::NotFound,"permit rejects cross-owner binding");
 auto copy=permit;std::atomic<int> successes=0,duplicates=0;std::vector<std::thread> workers;
 for(int i=0;i<8;++i)workers.emplace_back([&,copy]{auto result=copy.TryStart(f.registry,f.binding);if(result==Error::None)++successes;else if(result==Error::AlreadyStarted)++duplicates;});
 for(auto& t:workers)t.join();Check(successes==1&&duplicates==7,"copied permits share concurrent once-start gate");}
 {Limits l;l.lifetime_ms=10;Fixture f(l);auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;auto permit=*f.store.Consume(receipt,f.binding).value;f.now+=10;
 Check(permit.TryStart(f.registry,f.binding)==Error::Expired,"consumed permit expires before start");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;auto permit=*f.store.Consume(receipt,f.binding).value;
 f.registry.SyncPackages({{f.package,false}});f.registry.SyncPackages({{f.package,true}});auto rebound=*f.registry.CaptureBinding(f.package,f.owner);
 Check(permit.TryStart(f.registry,rebound)==Error::Revoked,"permit cannot start after disable-enable even with fresh binding");}
 {Fixture f;auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;auto permit=*f.store.Consume(receipt,f.binding).value;f.registry.UnregisterOwner(f.owner);
 Check(permit.TryStart(f.registry,f.binding)==Error::Revoked,"owner destruction revokes queued permit");}
 {Limits l;l.lifetime_ms=10;Fixture f(l);auto id=*f.store.Create(f.binding,DraftValue()).value;auto receipt=*f.store.Confirm(id,f.binding).value;auto permit=*f.store.Consume(receipt,f.binding).value;
 const auto started=permit.TryStart(f.registry,f.binding);f.now+=10;Check(started==Error::None&&permit.TryStart(f.registry,f.binding)==Error::AlreadyStarted,"expiry does not reset a started permit");}
 {authorization::Registry registry;authorization::PackageIdentity p{0,"production","revision",1};registry.SyncPackages({{p,true}});auto owner=*registry.RegisterOwner(99);auto binding=*registry.CaptureBinding(p,owner);Store store(registry);auto d=DraftValue();d.operation=Operation::Move;auto id=store.Create(binding,d);auto receipt=id?store.Confirm(*id.value,binding):Result<std::string>{};auto permit=receipt?store.Consume(*receipt.value,binding):Result<ExecutionPermit>{};Check(permit&&permit.value->TryStart(registry,binding)==Error::None,"production BCrypt copy/move model path");}
 std::printf("%d passed, %d failed\n",passed,failed);return failed?1:0;
}

