#include "../dialog_launcher/admin_auto_attach.h"
#include "../dialog_launcher/admin_auto_rebind.h"
#include "../dialog_launcher/admin_session_prune.h"
#include "../dialog_launcher/admin_version_retry.h"
#include "../common/dialog_auto_binding.h"
#include "../shell_bridge/runtime_config.h"
#include <objbase.h>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
using namespace pulse::dialog_launcher::admin_auto;
namespace {
namespace fs=std::filesystem;
namespace cfg=pulse::shell_bridge::runtime;
FILETIME DaysAgo(int days){FILETIME now{};GetSystemTimeAsFileTime(&now);ULARGE_INTEGER t{};t.LowPart=now.dwLowDateTime;t.HighPart=now.dwHighDateTime;t.QuadPart-=static_cast<ULONGLONG>(days)*24*3600*10000000ull;return {t.LowPart,t.HighPart};}
void Backdate(const fs::path& file,int days){HANDLE h=CreateFileW(file.c_str(),FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(h==INVALID_HANDLE_VALUE)return;const FILETIME t=DaysAgo(days);SetFileTime(h,nullptr,nullptr,&t);CloseHandle(h);}
void Put(const fs::path& file,const void* data,size_t bytes){HANDLE h=CreateFileW(file.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);DWORD w=0;if(h!=INVALID_HANDLE_VALUE){WriteFile(h,data,static_cast<DWORD>(bytes),&w,nullptr);CloseHandle(h);}}
std::wstring Guid(){GUID g{};CoCreateGuid(&g);wchar_t text[40]{};StringFromGUID2(g,text,40);return text;}
// A session directory as the helper publishes it: session.bin plus runtime logs.
fs::path MakeSession(const fs::path& root,DWORD pid,FILETIME created,int age_days,const std::wstring& nonce_override={}){
 const auto name=Guid();const auto dir=root/name;fs::create_directories(dir);
 auto session=std::make_unique<cfg::Session>();session->target_pid=pid;session->target_created=created;
 wcscpy_s(session->nonce,(nonce_override.empty()?name:nonce_override).c_str());
 Put(dir/cfg::kSessionFile,session.get(),sizeof(*session));const char log[]="decision\n";Put(dir/L"runtime-decisions.tsv",log,sizeof(log)-1);Put(dir/L"auto-binding.bin",log,4);
 if(age_days)for(const auto& entry:fs::directory_iterator(dir))if(entry.is_regular_file())Backdate(entry.path(),age_days);
 return dir;}
bool ExitedChild(DWORD& pid,FILETIME& created){
 wchar_t command[]=L"cmd.exe /d /c exit 0";STARTUPINFOW si{};si.cb=sizeof(si);PROCESS_INFORMATION pi{};
 if(!CreateProcessW(nullptr,command,nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi))return false;
 WaitForSingleObject(pi.hProcess,10000);FILETIME e{},k{},u{};const bool ok=GetProcessTimes(pi.hProcess,&created,&e,&k,&u)!=0;pid=pi.dwProcessId;
 CloseHandle(pi.hThread);CloseHandle(pi.hProcess);return ok;}
}
int main(){int failures=0,checks=0;auto check=[&](bool okay,const char*label){++checks;printf("[%s] %s\n",okay?"PASS":"FAIL",label);if(!okay)++failures;};
 Eligibility good{true,true,true,true,true,false};check(CheckEligibility(good)==0,"same elevated unrestricted session architecture accepted");
 for(unsigned bit=0;bit<6;++bit){auto test=good;switch(bit){case 0:test.same_user_session=false;break;case 1:test.elevated=false;break;case 2:test.unrestricted=false;break;case 3:test.same_architecture=false;break;case 4:test.unprotected_process=false;break;case 5:test.excluded=true;break;}check(CheckEligibility(test)!=0,"each identity/protection/exclusion boundary rejects");}
 check(StatusBasename(7,IMAGE_FILE_MACHINE_AMD64)==L"auto-helper-7-34404.bin","status locator numeric bounded");
 HelperStatusV1 status{};check(status.size==56&&status.magic==kStatusMagic&&!status.ready&&status.error==ERROR_IO_PENDING,"status defaults never claim readiness");
 {
  pulse::shell_bridge::runtime::PulseRuntimeAutoDiagnosticV1 diag{};
  diag.initialization=S_OK;diag.api_mask=1;diag.patched_slots=4;
  check(!CanAutoRebind(diag)&&!ShouldContinueProbing(diag),"active S_OK runtime stops periodic rebind probing");
  diag.initialization=E_PENDING;diag.api_mask=0;diag.patched_slots=0;
  check(!CanAutoRebind(diag)&&ShouldContinueProbing(diag),"pending runtime keeps bounded probing alive");
  diag.initialization=pulse::shell_bridge::runtime::kAutoReadyForRebind;diag.protection_restored=1;
  check(CanAutoRebind(diag),"restored runtime authorizes rebind");
 }
 GUID nonce{1,2,3,{4,5,6,7,8,9,10,11}};pulse::dialog_auto_binding::BindingV1 bound{};const DWORD captured=pulse::dialog_auto_binding::CaptureCurrentWithHelper(GetCurrentProcess(),1,nonce,bound);
 HANDLE token=nullptr;TOKEN_ELEVATION elevation{};DWORD size=0;if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size);CloseHandle(token);}printf("[DIAG] ordinary capture=%lu\n",captured);if(!elevation.TokenIsElevated)check(captured==ERROR_ELEVATION_REQUIRED||captured==ERROR_ACCESS_DENIED,"ordinary token cannot publish attach binding");
 {
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);const fs::path root=fs::path(temp)/(L"pulse-admin-prune-"+std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;fs::remove_all(root,ec);fs::create_directories(root);
  FILETIME self_created{},e{},k{},u{};GetProcessTimes(GetCurrentProcess(),&self_created,&e,&k,&u);
  DWORD dead_pid=0;FILETIME dead_created{};check(ExitedChild(dead_pid,dead_created),"exited child process available for session fixtures");
  const auto exited_old=MakeSession(root,dead_pid,dead_created,3);
  const auto exited_recent=MakeSession(root,dead_pid,dead_created,0);
  const auto live_old=MakeSession(root,GetCurrentProcessId(),self_created,3);
  FILETIME reused=self_created;reused.dwLowDateTime^=1;const auto reused_pid=MakeSession(root,GetCurrentProcessId(),reused,3);
  const auto mismatch=MakeSession(root,dead_pid,dead_created,3,Guid());
  const auto nested=MakeSession(root,dead_pid,dead_created,3);fs::create_directories(nested/L"sub");
  const auto foreign=root/L"keep-me";fs::create_directories(foreign);
  const auto before_boot=MakeSession(root,4,DaysAgo(400),3); // PID 4 is System: never the bound target
  const auto report=PruneExitedSessions(root.wstring(),kSessionPruneIdleMs);
  check(!fs::exists(exited_old),"exited target idle past grace period is removed");
  check(!fs::exists(reused_pid),"reused PID with a different creation time counts as exited");
  check(fs::exists(exited_recent/cfg::kSessionFile),"recently written exited session keeps its logs");
  check(fs::exists(live_old/cfg::kSessionFile),"running target keeps its session");
  check(fs::exists(mismatch/cfg::kSessionFile),"session naming another directory is retained");
  check(fs::exists(nested/L"sub")&&fs::exists(nested/cfg::kSessionFile),"directory with unexpected subdirectory is retained");
  check(fs::exists(foreign),"non-session directory is untouched");
  check(!fs::exists(before_boot),"target created before this boot is removed even when its PID is inaccessible");
  printf("[DIAG] removed=%u recent=%u live=%u retained=%u limited=%d\n",report.removed,report.recent,report.live,report.retained,report.limited);
  check(report.removed==3&&report.recent==1&&report.live==1&&report.retained==2&&!report.limited,"prune report counts each outcome");
  const auto second=PruneExitedSessions(root.wstring(),0);
  check(!fs::exists(exited_recent)&&second.removed==1&&fs::exists(live_old),"zero grace removes the remaining exited session only");
  fs::remove_all(root,ec);
 }
 {
  wchar_t temp[MAX_PATH]{};GetTempPathW(MAX_PATH,temp);const fs::path root=fs::path(temp)/(L"pulse-version-retry-"+std::to_wstring(GetCurrentProcessId()));
  std::error_code ec;fs::remove_all(root,ec);
  const std::string cur(64,'1'),x86(64,'2'),prev(64,'3'),app(64,'4'),app_prev(64,'5'),retired(64,'6');
  const auto wide=[](const std::string& s){return std::wstring(s.begin(),s.end());};
  const auto text=[&](const fs::path& file,const std::string& body){Put(file,body.data(),body.size());};
  for(const auto& d:{cur,x86,prev})fs::create_directories(root/L"DialogBridge"/wide(d));
  for(const auto& d:{app,app_prev})fs::create_directories(root/L"AppRuntime"/wide(d));
  fs::create_directories(root/L"DialogBridge"/L"not-a-version");
  check(!HasRetiredVersions(root.wstring()),"no current pointer: nothing is reported retired");
  text(root/L"dialog-current.json","{\"schema\":1,\"directory\":\"DialogBridge/"+cur+"\",\"manifest_sha256\":\""+cur+"\"}");
  text(root/L"dialog-x86-current.json","{\"schema\":1,\"directory\":\"DialogBridge/"+x86+"\",\"manifest_sha256\":\""+x86+"\"}");
  text(root/L".pulse-dialog-previous",prev);
  text(root/L"app-current.json","{\"schema\":1,\"directory\":\"AppRuntime/"+app+"\",\"manifest_sha256\":\""+app+"\"}");
  text(root/L".pulse-app-previous",app_prev);
  check(!HasRetiredVersions(root.wstring()),"current, companion and previous versions plus unknown names are not retired");
  fs::create_directories(root/L"DialogBridge"/wide(retired));
  check(HasRetiredVersions(root.wstring()),"unnamed dialog version is retired");
  fs::remove(root/L"DialogBridge"/wide(retired));fs::create_directories(root/L"AppRuntime"/wide(retired));
  check(HasRetiredVersions(root.wstring()),"unnamed application runtime version is retired");
  fs::remove(root/L"AppRuntime"/wide(retired));fs::create_directories(root/L"DialogBridge"/wide(std::string(64,'A')));
  check(!HasRetiredVersions(root.wstring()),"uppercase digest-shaped name is not a setup version");
  fs::remove_all(root,ec);
  ULONGLONG interval=0;
  check(NextRetry(kCleanupRetained,interval)==RetryNext::Again&&interval==kVersionRetryMinMs,"retained versions retry after the minimum interval");
  check(NextRetry(kCleanupRetained,interval)==RetryNext::Again&&interval==2*kVersionRetryMinMs,"repeated retention backs off");
  for(int i=0;i<10;++i)NextRetry(kCleanupRetained,interval);
  check(interval==kVersionRetryMaxMs,"backoff is capped");
  check(NextRetry(kCleanupBusy,interval)==RetryNext::Again&&interval==kVersionRetryMinMs,"busy install lock retries soon");
  check(NextRetry(kCleanupOk,interval)==RetryNext::Stop,"complete cleanup stops retrying");
  check(NextRetry(2,interval)==RetryNext::Stop&&NextRetry(7,interval)==RetryNext::Stop,"non-elevated or invalid install stops retrying");
 }
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
