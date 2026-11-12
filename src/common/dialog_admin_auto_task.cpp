#include "dialog_admin_auto_task.h"
#include "dialog_admin_trust.h"
#include "dialog_bundle.h"
#include "../dialog_launcher/admin_auto_attach.h"
#include <taskschd.h>
#include <shlobj.h>
#include <sddl.h>
#include <wrl/client.h>
#include <vector>
#include <memory>
#include <algorithm>

namespace pulse::dialog_admin_auto {
namespace {
using Microsoft::WRL::ComPtr;
struct Handle { HANDLE h=nullptr; ~Handle(){if(h&&h!=INVALID_HANDLE_VALUE)CloseHandle(h);} };
struct Bstr { BSTR p=nullptr; Bstr()=default; explicit Bstr(const std::wstring& s):p(SysAllocStringLen(s.data(),static_cast<UINT>(s.size()))){} ~Bstr(){SysFreeString(p);} };
struct Variant { VARIANT v{}; Variant(){VariantInit(&v);} explicit Variant(const std::wstring& s){VariantInit(&v);v.vt=VT_BSTR;v.bstrVal=SysAllocString(s.c_str());} ~Variant(){VariantClear(&v);} };
struct Com { HRESULT hr=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED); ~Com(){if(SUCCEEDED(hr))CoUninitialize();} };
HRESULT Error(){return HRESULT_FROM_WIN32(GetLastError());}
std::wstring Xml(const std::wstring& s){std::wstring r;for(wchar_t c:s){switch(c){case L'&':r+=L"&amp;";break;case L'<':r+=L"&lt;";break;case L'>':r+=L"&gt;";break;case L'\"':r+=L"&quot;";break;default:r+=c;}}return r;}
bool Same(const std::wstring& a,const std::wstring& b){return CompareStringOrdinal(a.c_str(),-1,b.c_str(),-1,TRUE)==CSTR_EQUAL;}
std::wstring ProgramFiles(){PWSTR value=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_ProgramFiles,0,nullptr,&value)))return {};std::wstring result=value;CoTaskMemFree(value);return result;}
HRESULT SidFor(HANDLE process,std::wstring& sid,DWORD& session){
 Handle token;if(!OpenProcessToken(process,TOKEN_QUERY,&token.h))return Error();DWORD size=0;GetTokenInformation(token.h,TokenUser,nullptr,0,&size);std::vector<BYTE> bytes(size);
 if(!size||!GetTokenInformation(token.h,TokenUser,bytes.data(),size,&size))return Error();
 LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))return Error();sid=text;LocalFree(text);
 if(!GetTokenInformation(token.h,TokenSessionId,&session,sizeof(session),&size))return Error();return S_OK;
}
HRESULT Plan(const std::wstring& root,bool elevated,bool helper,TaskPlan& plan){Identity identity;HRESULT hr=InteractiveIdentity(identity,elevated);if(FAILED(hr))return hr;if(!helper){plan.root=root;plan.sid=identity.sid;plan.session=identity.session;plan.name=L"PulseDialogAdminAuto-"+identity.sid;return S_OK;}hr=ValidateInstalledRoot(root,helper);if(FAILED(hr))return hr;dialog_bundle::BundleLease lease;
 std::wstring executable=root+L"\\DialogBridge\\"+std::wstring(64,L'0')+L"\\"+kHelperName;
 if(helper){DWORD error=dialog_bundle::ResolveInstalledBundle(root,lease);if(error)return HRESULT_FROM_WIN32(error);executable=lease.AdminAutoHelper();if(executable.empty())return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);error=dialog_admin::VerifyProtectedPath(executable,false);if(error)return HRESULT_FROM_WIN32(error);}
 return BuildTaskPlan(root,ProgramFiles(),identity,executable,plan);}
HRESULT Ready(const std::wstring& root,DWORD session,WORD machine,const std::wstring& executable) {
 using dialog_launcher::admin_auto::HelperStatusV1;
 std::wstring control;DWORD error=dialog_admin::AdminControlRoot(root,false,control);if(error)return HRESULT_FROM_WIN32(error);
 const auto path=control+L"\\auto-helper-"+std::to_wstring(session)+L"-"+std::to_wstring(machine)+L".bin";
 error=dialog_admin::VerifyProtectedPath(path,false);if(error)return HRESULT_FROM_WIN32(error);
 Handle file{CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr)};
 if(file.h==INVALID_HANDLE_VALUE)return Error();HelperStatusV1 status{};DWORD read=0;LARGE_INTEGER size{};
 if(!GetFileSizeEx(file.h,&size)||size.QuadPart!=sizeof(status)||!ReadFile(file.h,&status,sizeof(status),&read,nullptr)||read!=sizeof(status))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
 if(status.magic!=dialog_launcher::admin_auto::kStatusMagic||status.version!=1||status.size!=sizeof(status)||status.reserved||status.session_id!=session||status.machine!=machine||!status.generation||status.ready!=1||status.error)return HRESULT_FROM_WIN32(status.error?status.error:ERROR_NOT_READY);
 Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION|SYNCHRONIZE,FALSE,status.pid)};if(!process.h)return Error();
 FILETIME created{},exit{},kernel{},user{};DWORD actual_session=0;if(!GetProcessTimes(process.h,&created,&exit,&kernel,&user)||!ProcessIdToSessionId(status.pid,&actual_session))return Error();
 if(CompareFileTime(&created,&status.created)||actual_session!=session||WaitForSingleObject(process.h,0)!=WAIT_TIMEOUT)return HRESULT_FROM_WIN32(ERROR_NOT_READY);
 wchar_t path_buffer[32768]{};DWORD length=32768;if(!QueryFullProcessImageNameW(process.h,0,path_buffer,&length))return Error();
 return Same(path_buffer,executable)?S_OK:HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
}
class Scheduler {
public:
 ComPtr<ITaskService> service;ComPtr<ITaskFolder> folder;TaskPlan plan;
 HRESULT Open(const TaskPlan& p){plan=p;HRESULT hr=CoCreateInstance(CLSID_TaskScheduler,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&service));if(FAILED(hr))return hr;Variant empty;hr=service->Connect(empty.v,empty.v,empty.v,empty.v);if(FAILED(hr))return hr;Bstr root(L"\\");return service->GetFolder(root.p,&folder);}
 HRESULT Get(ComPtr<IRegisteredTask>& task){Bstr name(plan.name);return folder->GetTask(name.p,&task);}
 HRESULT InspectTask(Snapshot& out){
  out={};ComPtr<IRegisteredTask> task;HRESULT hr=Get(task);if(hr==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND))return S_OK;if(FAILED(hr))return hr;
  ComPtr<ITaskDefinition> definition;hr=task->get_Definition(&definition);if(FAILED(hr))return hr;
  ComPtr<IPrincipal> principal;hr=definition->get_Principal(&principal);if(FAILED(hr))return hr;Bstr sid;hr=principal->get_UserId(&sid.p);if(FAILED(hr))return hr;
  TASK_LOGON_TYPE logon;TASK_RUNLEVEL_TYPE level;if(FAILED(hr=principal->get_LogonType(&logon))||FAILED(hr=principal->get_RunLevel(&level)))return hr;
  if(!sid.p||!Same(sid.p,plan.sid)||logon!=TASK_LOGON_INTERACTIVE_TOKEN||level!=TASK_RUNLEVEL_HIGHEST)return E_ACCESSDENIED;
  ComPtr<IActionCollection> actions;hr=definition->get_Actions(&actions);if(FAILED(hr))return hr;LONG count=0;hr=actions->get_Count(&count);if(FAILED(hr)||count!=1)return E_ACCESSDENIED;
  ComPtr<IAction> action;hr=actions->get_Item(1,&action);if(FAILED(hr))return hr;ComPtr<IExecAction> exec;hr=action.As(&exec);if(FAILED(hr))return E_ACCESSDENIED;
  Bstr path,args;hr=exec->get_Path(&path.p);if(FAILED(hr))return hr;hr=exec->get_Arguments(&args.p);if(FAILED(hr))return hr;
  std::wstring executable=path.p?path.p:L"";const auto slash=executable.find_last_of(L'\\');if(slash==executable.npos||!Same(executable.substr(slash+1),kHelperName))return E_ACCESSDENIED;
  const auto bridge=executable.rfind(L"\\DialogBridge\\");if(bridge==executable.npos)return E_ACCESSDENIED;
  out.root=executable.substr(0,bridge);out.executable=executable;TaskPlan previous;hr=BuildTaskPlan(out.root,ProgramFiles(),{plan.sid,plan.session},executable,previous);if(FAILED(hr)||!args.p||previous.arguments!=args.p)return E_ACCESSDENIED;
  hr=ValidateInstalledRoot(out.root,false);if(FAILED(hr))return hr;
  // Never trust a user-writable highest-privilege task, including during upgrade.
  Bstr security;hr=task->GetSecurityDescriptor(OWNER_SECURITY_INFORMATION|DACL_SECURITY_INFORMATION,&security.p);if(FAILED(hr))return hr;
  PSECURITY_DESCRIPTOR sd=nullptr;if(!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.p,SDDL_REVISION_1,&sd,nullptr))return Error();
  PSID owner=nullptr;BOOL defaulted=FALSE,present=FALSE;PACL acl=nullptr;bool secure=GetSecurityDescriptorOwner(sd,&owner,&defaulted)&&GetSecurityDescriptorDacl(sd,&present,&acl,&defaulted)&&present&&acl;
  BYTE admins[SECURITY_MAX_SID_SIZE]{},system[SECURITY_MAX_SID_SIZE]{};DWORD n=sizeof(admins),m=sizeof(system);CreateWellKnownSid(WinBuiltinAdministratorsSid,nullptr,admins,&n);CreateWellKnownSid(WinLocalSystemSid,nullptr,system,&m);
  secure=secure&&owner&&(EqualSid(owner,admins)||EqualSid(owner,system));
  for(DWORD i=0;secure&&i<acl->AceCount;++i){void* value=nullptr;if(!GetAce(acl,i,&value)){secure=false;break;}auto* header=static_cast<ACE_HEADER*>(value);if(header->AceType==ACCESS_DENIED_ACE_TYPE)continue;if(header->AceType!=ACCESS_ALLOWED_ACE_TYPE){secure=false;break;}auto* ace=static_cast<ACCESS_ALLOWED_ACE*>(value);PSID who=&ace->SidStart;
   if(!EqualSid(who,admins)&&!EqualSid(who,system)&&(ace->Mask&(GENERIC_ALL|GENERIC_WRITE|WRITE_DAC|WRITE_OWNER|DELETE|FILE_WRITE_DATA|FILE_APPEND_DATA)))secure=false;
  }LocalFree(sd);if(!secure)return E_ACCESSDENIED;
  Bstr xml;hr=task->get_Xml(&xml.p);if(FAILED(hr))return hr;out.xml=xml.p?xml.p:L"";TASK_STATE state{};hr=task->get_State(&state);if(FAILED(hr))return hr;LONG last=0;hr=task->get_LastTaskResult(&last);if(FAILED(hr))return hr;out.last_result=static_cast<DWORD>(last);out.exists=true;out.running=state==TASK_STATE_RUNNING;return S_OK;
 }
 HRESULT Stop(){ComPtr<IRegisteredTask> task;HRESULT hr=Get(task);if(FAILED(hr))return hr;hr=task->Stop(0);if(FAILED(hr)&&hr!=SCHED_E_TASK_NOT_RUNNING)return hr;const auto end=GetTickCount64()+5000;do{TASK_STATE state{};hr=task->get_State(&state);if(FAILED(hr))return hr;if(state!=TASK_STATE_RUNNING&&state!=TASK_STATE_QUEUED)return S_OK;Sleep(50);}while(GetTickCount64()<end);return HRESULT_FROM_WIN32(ERROR_TIMEOUT);}
 HRESULT Put(const std::wstring& xml){Bstr name(plan.name),document(xml);Variant user(plan.sid),empty,security(plan.sddl);ComPtr<IRegisteredTask> task;return folder->RegisterTask(name.p,document.p,TASK_CREATE_OR_UPDATE|TASK_IGNORE_REGISTRATION_TRIGGERS,user.v,empty.v,TASK_LOGON_INTERACTIVE_TOKEN,security.v,&task);}
 HRESULT Delete(){Bstr name(plan.name);return folder->DeleteTask(name.p,0);}
 HRESULT Start(){
  dialog_bundle::BundleLease primary,companion;
  DWORD error=dialog_bundle::ResolveInstalledBundle(plan.root,primary);if(error)return HRESULT_FROM_WIN32(error);
  if(!Same(primary.AdminAutoHelper(),plan.executable))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);
  SYSTEM_INFO system{};GetNativeSystemInfo(&system);const bool dual=system.wProcessorArchitecture==PROCESSOR_ARCHITECTURE_AMD64;
  if(dual){error=dialog_bundle::ResolveInstalledBundleForMachine(plan.root,IMAGE_FILE_MACHINE_I386,companion);if(error)return HRESULT_FROM_WIN32(error);if(companion.AdminAutoHelper().empty())return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);}
  ComPtr<IRegisteredTask> task;HRESULT hr=Get(task);if(FAILED(hr))return hr;Variant empty;ComPtr<IRunningTask> running;
  Bstr user(plan.sid);hr=task->RunEx(empty.v,TASK_RUN_USE_SESSION_ID|TASK_RUN_USER_SID,static_cast<LONG>(plan.session),user.p,&running);if(FAILED(hr))return hr;
  const auto end=GetTickCount64()+15000;
  do{TASK_STATE state{};hr=running->Refresh();if(FAILED(hr))return hr;hr=running->get_State(&state);if(FAILED(hr))return hr;
   if(state==TASK_STATE_RUNNING){hr=Ready(plan.root,plan.session,static_cast<WORD>(primary.Machine()),primary.AdminAutoHelper());if(SUCCEEDED(hr)&&dual)hr=Ready(plan.root,plan.session,IMAGE_FILE_MACHINE_I386,companion.AdminAutoHelper());if(SUCCEEDED(hr))return S_OK;}
   if(state==TASK_STATE_READY)return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);Sleep(100);
  }while(GetTickCount64()<end);return FAILED(hr)?hr:HRESULT_FROM_WIN32(ERROR_TIMEOUT);
 }
 Backend Api(){return {[&](Snapshot& s){return InspectTask(s);},[&]{return Stop();},[&]{return Delete();},[&]{return Start();},[&](const std::wstring& xml){return Put(xml);}};}
};
}
HRESULT BuildTaskPlan(const std::wstring& root,const std::wstring& pf,const Identity& identity,const std::wstring& helper,TaskPlan& out) noexcept try {
 if(pf.size()<3||root.size()<=pf.size()+1||!Same(root.substr(0,pf.size()),pf)||root[pf.size()]!=L'\\'||root.back()==L'\\'||root.find_first_of(L"\"<>|?*/%$")!=root.npos||root.find(L'\0')!=root.npos||root.find(L':',2)!=root.npos)return E_INVALIDARG;
 for(size_t i=3;i<root.size();){auto end=root.find(L'\\',i);if(end==root.npos)end=root.size();auto part=root.substr(i,end-i);if(part.empty()||part==L"."||part==L".."||part.back()==L'.'||part.back()==L' ')return E_INVALIDARG;i=end+1;}
 if(identity.sid.find(L'\0')!=identity.sid.npos||std::any_of(root.begin(),root.end(),[](wchar_t c){return c<32;}))return E_INVALIDARG;
 PSID sid=nullptr;if(!ConvertStringSidToSidW(identity.sid.c_str(),&sid))return E_INVALIDARG;const bool valid=IsValidSid(sid)!=FALSE;LocalFree(sid);if(!valid||identity.session==0)return E_INVALIDARG;
 const std::wstring prefix=root+L"\\DialogBridge\\";
 if(!helper.starts_with(prefix)||helper.size()!=prefix.size()+64+1+wcslen(kHelperName)||helper.substr(prefix.size(),64).find_first_not_of(L"0123456789abcdef")!=std::wstring::npos||helper.substr(prefix.size()+64)!=L"\\"+std::wstring(kHelperName))return E_INVALIDARG;
 TaskPlan plan;plan.root=root;plan.sid=identity.sid;plan.session=identity.session;plan.name=L"PulseDialogAdminAuto-"+identity.sid;plan.executable=helper;plan.arguments=L"--installed-root \""+root+L"\"";
 plan.sddl=L"O:BAG:BAD:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGX;;;"+identity.sid+L")";
 plan.xml=L"<?xml version=\"1.0\"?><Task version=\"1.2\" xmlns=\"http://schemas.microsoft.com/windows/2004/02/mit/task\"><RegistrationInfo><Author>Pulse</Author></RegistrationInfo><Triggers><LogonTrigger><Enabled>true</Enabled><UserId>"+Xml(identity.sid)+L"</UserId></LogonTrigger></Triggers><Principals><Principal id=\"PulseUser\"><UserId>"+Xml(identity.sid)+L"</UserId><LogonType>InteractiveToken</LogonType><RunLevel>HighestAvailable</RunLevel></Principal></Principals><Settings><MultipleInstancesPolicy>IgnoreNew</MultipleInstancesPolicy><DisallowStartIfOnBatteries>false</DisallowStartIfOnBatteries><StopIfGoingOnBatteries>false</StopIfGoingOnBatteries><AllowHardTerminate>true</AllowHardTerminate><StartWhenAvailable>true</StartWhenAvailable><RunOnlyIfNetworkAvailable>false</RunOnlyIfNetworkAvailable><AllowStartOnDemand>true</AllowStartOnDemand><Enabled>true</Enabled><Hidden>false</Hidden><ExecutionTimeLimit>PT0S</ExecutionTimeLimit></Settings><Actions Context=\"PulseUser\"><Exec><Command>"+Xml(plan.executable)+L"</Command><Arguments>"+Xml(plan.arguments)+L"</Arguments><WorkingDirectory>"+Xml(root)+L"</WorkingDirectory></Exec></Actions></Task>";
 out=std::move(plan);return S_OK;
} catch(...){return E_OUTOFMEMORY;}
HRESULT MatchInteractiveIdentity(const Identity& caller,const Identity& interactive) noexcept {
 return !caller.sid.empty()&&caller.session!=0&&caller.session==interactive.session&&caller.sid==interactive.sid?S_OK:HRESULT_FROM_WIN32(ERROR_LOGON_TYPE_NOT_GRANTED);
}
HRESULT InteractiveIdentity(Identity& identity,bool require_elevated) noexcept try {
 if(require_elevated&&!dialog_admin::IsElevated())return HRESULT_FROM_WIN32(ERROR_ELEVATION_REQUIRED);
 DWORD pid=0;GetWindowThreadProcessId(GetShellWindow(),&pid);if(!pid)return HRESULT_FROM_WIN32(ERROR_NO_SUCH_LOGON_SESSION);
 Handle shell{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,pid)};if(!shell.h)return Error();Identity own,interactive;HRESULT hr=SidFor(GetCurrentProcess(),own.sid,own.session);if(FAILED(hr))return hr;hr=SidFor(shell.h,interactive.sid,interactive.session);if(FAILED(hr))return hr;
 hr=MatchInteractiveIdentity(own,interactive);if(FAILED(hr))return hr;
 identity=std::move(interactive);return S_OK;
} catch(...){return E_OUTOFMEMORY;}
HRESULT ValidateInstalledRoot(const std::wstring& root,bool helper) noexcept try {
 TaskPlan ignored;HRESULT hr=BuildTaskPlan(root,ProgramFiles(),{L"S-1-5-21-1-2-3-1001",1},root+L"\\DialogBridge\\"+std::wstring(64,L'0')+L"\\"+kHelperName,ignored);if(FAILED(hr))return hr;
 DWORD error=dialog_admin::VerifyProtectedPath(root,true);
 if(!error&&helper){dialog_bundle::BundleLease lease;error=dialog_bundle::ResolveInstalledBundle(root,lease);if(!error&&lease.AdminAutoHelper().empty())error=ERROR_NOT_SUPPORTED;if(!error)error=dialog_admin::VerifyProtectedPath(lease.AdminAutoHelper(),false);}
 return HRESULT_FROM_WIN32(error);
} catch(...){return E_OUTOFMEMORY;}
Result ApplyTaskPlan(const TaskPlan& plan,bool start,const Backend& backend) noexcept try {
 Result result;Snapshot before;result.error=backend.inspect(before);if(FAILED(result.error))return result;
 if(before.exists){result.error=backend.stop();if(FAILED(result.error))return result;}
 // The production backend closes over the new plan, while rollback uses prior XML.
 result.error=backend.put(plan.xml);
 if(SUCCEEDED(result.error)){result.registered=true;if(start){result.error=backend.start();result.running=SUCCEEDED(result.error);}}
 if(FAILED(result.error)){if(before.exists){result.rollback_error=backend.put(before.xml);if(SUCCEEDED(result.rollback_error)&&before.running)result.rollback_error=backend.start();}else if(result.registered)result.rollback_error=backend.remove();result.registered=before.exists;result.running=before.running&&SUCCEEDED(result.rollback_error);}
 return result;
} catch(...){return {E_UNEXPECTED};}
Result Configure(const std::wstring& root,bool start) noexcept try {
 Com com;if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE)return {com.hr};TaskPlan plan;HRESULT hr=Plan(root,true,true,plan);if(FAILED(hr))return {hr};Scheduler scheduler;hr=scheduler.Open(plan);if(FAILED(hr))return {hr};auto api=scheduler.Api();return ApplyTaskPlan(plan,start,api);
} catch(...){return {E_OUTOFMEMORY};}
HRESULT Restore(const std::wstring& root,const Snapshot& before) noexcept {
 if(!before.exists)return Remove(root);
 Snapshot current;HRESULT hr=Inspect(root,current);if(FAILED(hr))return hr;if(current.exists&&!Same(current.root,root)&&!Same(current.root,before.root))return HRESULT_FROM_WIN32(ERROR_BUSY);
 const auto restored=Configure(before.root,before.running);return restored.error;
}
HRESULT Inspect(const std::wstring& root,Snapshot& snapshot) noexcept try {Com com;if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE)return com.hr;TaskPlan plan;HRESULT hr=Plan(root,false,false,plan);if(FAILED(hr))return hr;Scheduler scheduler;hr=scheduler.Open(plan);return FAILED(hr)?hr:scheduler.InspectTask(snapshot);}catch(...){return E_OUTOFMEMORY;}
HRESULT StartExisting(const std::wstring& root) noexcept try {Com com;if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE)return com.hr;TaskPlan plan;HRESULT hr=Plan(root,false,true,plan);if(FAILED(hr))return hr;Scheduler scheduler;hr=scheduler.Open(plan);if(FAILED(hr))return hr;Snapshot before;hr=scheduler.InspectTask(before);if(FAILED(hr))return hr;if(!before.exists)return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);if(!Same(before.root,root)||!Same(before.executable,plan.executable))return HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH);return scheduler.Start();}catch(...){return E_OUTOFMEMORY;}
HRESULT Remove(const std::wstring& root) noexcept try {Com com;if(FAILED(com.hr)&&com.hr!=RPC_E_CHANGED_MODE)return com.hr;TaskPlan plan;HRESULT hr=Plan(root,true,false,plan);if(FAILED(hr))return hr;Scheduler scheduler;hr=scheduler.Open(plan);if(FAILED(hr))return hr;Snapshot before;hr=scheduler.InspectTask(before);if(FAILED(hr))return hr;if(!before.exists||!Same(before.root,root))return S_OK;hr=scheduler.Stop();return FAILED(hr)?hr:scheduler.Delete();}catch(...){return E_OUTOFMEMORY;}
}
