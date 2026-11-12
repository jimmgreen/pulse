#include "../common/dialog_admin_auto_task.h"
#include <taskschd.h>
#include <sddl.h>
#include <wrl/client.h>
#include <cstdio>
using namespace pulse::dialog_admin_auto;
int checks=0,failures=0;
void Check(bool good,const char* label){++checks;printf("[%s] %s\n",good?"PASS":"FAIL",label);if(!good)++failures;}
int wmain(){
 const Identity identity{L"S-1-5-21-1-2-3-1001",1};TaskPlan plan;
 const std::wstring root=L"C:\\Program Files\\Pulse",pf=L"C:\\Program Files";
 const auto helper=root+L"\\DialogBridge\\"+std::wstring(64,L'a')+L"\\PulseDialogAdminAuto.exe";
 Check(BuildTaskPlan(root,pf,identity,helper,plan)==S_OK,"fixed immutable helper plan");
 Check(plan.arguments==L"--installed-root \"C:\\Program Files\\Pulse\"","fixed root argument only");
 Check(plan.xml.find(L"HighestAvailable")!=plan.xml.npos&&plan.xml.find(L"InteractiveToken")!=plan.xml.npos&&plan.xml.find(L"LogonTrigger")!=plan.xml.npos,"highest interactive-user logon definition");
 Check(plan.xml.find(L"IgnoreNew")!=plan.xml.npos&&plan.xml.find(L"PT0S")!=plan.xml.npos,"single unlimited-lived helper, not arbitrary admin app");
 TaskPlan ignored;
 Check(FAILED(BuildTaskPlan(L"C:\\Program FilesOther\\Pulse",pf,identity,helper,ignored)),"ProgramFiles prefix confusion rejected");
 Check(FAILED(BuildTaskPlan(root+L"\\..\\Other",pf,identity,helper,ignored)),"parent traversal rejected");
 Check(FAILED(BuildTaskPlan(root+L"$(Arg0)",pf,identity,helper,ignored)),"scheduler argument substitution rejected");
 Check(FAILED(BuildTaskPlan(root,pf,identity,root+L"\\other.exe",ignored)),"arbitrary/root executable rejected");
 Check(FAILED(BuildTaskPlan(root,pf,identity,root+L"\\DialogBridge\\x\\PulseDialogAdminAuto.exe",ignored)),"non-digest bundle rejected");
 Check(MatchInteractiveIdentity(identity,identity)==S_OK,"same interactive SID/session accepted");
 Check(FAILED(MatchInteractiveIdentity({L"S-1-5-21-1-2-3-1002",1},identity)),"alternate credential elevation rejected");
 Check(FAILED(MatchInteractiveIdentity({identity.sid,2},identity)),"different interactive session rejected");
 PSECURITY_DESCRIPTOR descriptor=nullptr;Check(ConvertStringSecurityDescriptorToSecurityDescriptorW(plan.sddl.c_str(),SDDL_REVISION_1,&descriptor,nullptr)!=FALSE,"task ACL parses");
 if(descriptor){PSID owner=nullptr;BOOL defaulted=FALSE,present=FALSE;PACL acl=nullptr;GetSecurityDescriptorOwner(descriptor,&owner,&defaulted);GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted);BYTE admins[SECURITY_MAX_SID_SIZE]{};DWORD size=sizeof(admins);CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&size);Check(EqualSid(owner,admins)!=FALSE,"task owner is Administrators, not caller");void* raw=nullptr;GetAce(acl,2,&raw);auto ace=static_cast<ACCESS_ALLOWED_ACE*>(raw);Check(ace->Mask==(GENERIC_READ|GENERIC_EXECUTE),"interactive user has read/execute only");LocalFree(descriptor);}
 for(int scenario=0;scenario<6;++scenario){
  Snapshot before;before.exists=scenario>=2;before.running=before.exists;before.root=root;before.xml=L"old";int puts=0,starts=0,stops=0,deletes=0;std::wstring stored=before.xml;
  Backend backend{[&](Snapshot& out){out=before;return S_OK;},[&]{++stops;return scenario==5?E_ACCESSDENIED:S_OK;},[&]{++deletes;stored.clear();return S_OK;},[&]{++starts;return (scenario==1||scenario==3)&&starts==1?E_FAIL:S_OK;},[&](const std::wstring& xml){++puts;if(scenario==4&&puts==1)return E_FAIL;stored=xml;return S_OK;}};
  const auto result=ApplyTaskPlan(plan,true,backend);
  if(scenario==0)Check(SUCCEEDED(result.error)&&result.running&&stored==plan.xml,"new registration starts verified helper");
  if(scenario==1)Check(FAILED(result.error)&&deletes==1&&!result.registered,"failed new start deletes created task");
  if(scenario==2)Check(SUCCEEDED(result.error)&&stops==1&&stored==plan.xml,"upgrade stops old task and updates action");
  if(scenario==3)Check(FAILED(result.error)&&SUCCEEDED(result.rollback_error)&&stored==L"old"&&starts==2,"failed upgrade start restores prior task and running state");
  if(scenario==4)Check(FAILED(result.error)&&stored==L"old"&&starts==1,"failed registration restores prior task");
  if(scenario==5)Check(FAILED(result.error)&&puts==0,"failed stop prevents task mutation");
 }
 // Native task-definition parser only: no RegisterTask/DeleteTask/Run calls.
 const HRESULT initialized=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
 Microsoft::WRL::ComPtr<ITaskService> service;HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service));
 if(SUCCEEDED(hr)){VARIANT empty{};hr=service->Connect(empty,empty,empty,empty);}
 Microsoft::WRL::ComPtr<ITaskDefinition> definition;if(SUCCEEDED(hr))hr=service->NewTask(0,&definition);
 BSTR xml=SysAllocString(plan.xml.c_str());if(SUCCEEDED(hr))hr=definition->put_XmlText(xml);SysFreeString(xml);Check(SUCCEEDED(hr),"native Task Scheduler accepts in-memory XML without registration");
 definition.Reset();service.Reset();if(SUCCEEDED(initialized))CoUninitialize();
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
