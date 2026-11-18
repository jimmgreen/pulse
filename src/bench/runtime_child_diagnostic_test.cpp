#include "../shell_bridge/runtime_child_process.h"
#include "../shell_bridge/runtime_child_observer.h"
#include "../shell_bridge/runtime_child_session.h"
#include "../shell_bridge/runtime_child_injection.h"
#include <iostream>
#include <array>
#include <string_view>
namespace model = pulse::shell_bridge::runtime;
namespace {
int checks = 0, failures = 0, native_calls = 0, prepare_calls = 0, injection_calls = 0;
bool prepare_success = false;
DWORD prepared_flags = 0, bind_error = ERROR_SUCCESS, inject_error = ERROR_SUCCESS;
int bind_calls = 0, resume_calls = 0;
wchar_t prepared_environment[] = L"CHILD_MODEL=private\0";
bool allowed = false, nested_gate = false, nested_log = false, native_success = true;
DWORD native_error = 0x1234, observed_incoming = 0;
constexpr DWORD incoming = 0x789a;
std::array<model::ChildCallDiagnostic, model::kChildDiagnosticCapacity> delivered{};
size_t delivered_count = 0;
LPCWSTR app = L"owned-model-image", cwd = L"owned-model-directory";
wchar_t command[] = L"private-model-argument";
SECURITY_ATTRIBUTES process_security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE}, thread_security{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
wchar_t environment[] = L"MODEL=fixture\0";
STARTUPINFOW startup{sizeof(STARTUPINFOW)};
PROCESS_INFORMATION information{};
DWORD flags = 0;
bool exact = true;
void Check(bool ok, const char* text) { ++checks; if(!ok)++failures; std::cout<<(ok?"[PASS] ":"[FAIL] ")<<text<<'\n'; }
BOOL WINAPI Native(LPCWSTR a, LPWSTR c, LPSECURITY_ATTRIBUTES p, LPSECURITY_ATTRIBUTES t,
    BOOL inherit, DWORD f, LPVOID e, LPCWSTR d, LPSTARTUPINFOW s, LPPROCESS_INFORMATION out) {
    ++native_calls; observed_incoming = GetLastError();
    exact &= a==app && c==command && p==&process_security && t==&thread_security && inherit==TRUE && f==flags && e==environment && d==cwd && s==&startup && out==&information;
    if(native_success && out) out->dwProcessId=4242;
    SetLastError(native_error); return native_success?TRUE:FALSE;
}
BOOL Invoke() {
    SetLastError(incoming);
    return model::HookCreateProcessW(app,command,&process_security,&thread_security,TRUE,flags,environment,cwd,&startup,&information);
}
bool Enabled() noexcept {
    if(nested_gate) { nested_gate=false; Invoke(); }
    if(!allowed) model::SetChildGateDiagnostic(model::ChildGateReason::MarkerPresent,ERROR_CANCELLED);
    SetLastError(0xdeaf); return allowed;
}
void Log(const char*,DWORD,DWORD,model::ChildCallEntry) noexcept { SetLastError(0xbeef); }
void Diagnostic(const model::ChildCallDiagnostic& event) noexcept {
    if(delivered_count<delivered.size()) delivered[delivered_count++]=event;
    if(nested_log) { nested_log=false; Invoke(); }
    SetLastError(0xdead);
}
bool Has(model::ChildCallBranch branch);
#if defined(PULSE_DIALOG_UI_PROOF)
std::array<unsigned,4> observer_calls{};
bool observer_exact=true, observer_incoming=true, nest_observers=false, nest_w=false;
char narrow_command[]="opaque-model-command";
LPCSTR narrow_app="opaque-model-app", narrow_cwd="opaque-model-cwd";
STARTUPINFOA narrow_startup{sizeof(STARTUPINFOA)};
HANDLE opaque_token=reinterpret_cast<HANDLE>(uintptr_t{0x3333});
// Deliberately unreadable addresses: observers must pass credentials through,
// never copy, format, compare their contents or authenticate them.
LPCWSTR opaque_user=reinterpret_cast<LPCWSTR>(uintptr_t{1});
LPCWSTR opaque_domain=reinterpret_cast<LPCWSTR>(uintptr_t{2});
LPCWSTR opaque_password=reinterpret_cast<LPCWSTR>(uintptr_t{3});
constexpr DWORD logon_flags=0xa123, creation_flags=0xb456;
LPPROCESS_INFORMATION expected_output=&information;
void StartObserved(unsigned index) {
    ++observer_calls[index];observer_incoming &= GetLastError()==incoming;
}
BOOL FinishObserved(LPPROCESS_INFORMATION output) {
    if(native_success && output)output->dwProcessId=8181;
    SetLastError(native_error);return native_success?TRUE:FALSE;
}
BOOL WINAPI NativeA(LPCSTR a,LPSTR c,LPSECURITY_ATTRIBUTES p,LPSECURITY_ATTRIBUTES t,BOOL inherit,
    DWORD f,LPVOID e,LPCSTR d,LPSTARTUPINFOA st,LPPROCESS_INFORMATION out) {
    StartObserved(0);
    observer_exact &= a==narrow_app&&c==narrow_command&&p==&process_security&&t==&thread_security&&inherit==TRUE&&f==creation_flags&&e==environment&&d==narrow_cwd&&st==&narrow_startup&&out==expected_output;
    if(nest_observers) {
        nest_observers=false;SetLastError(incoming);
        model::ObserveCreateProcessAsUserW(opaque_token,app,command,&process_security,&thread_security,TRUE,creation_flags,environment,cwd,&startup,out);
    }
    if(nest_w) {nest_w=false;Invoke();}
    return FinishObserved(out);
}
BOOL WINAPI NativeAsUser(HANDLE token,LPCWSTR a,LPWSTR c,LPSECURITY_ATTRIBUTES p,LPSECURITY_ATTRIBUTES t,BOOL inherit,
    DWORD f,LPVOID e,LPCWSTR d,LPSTARTUPINFOW st,LPPROCESS_INFORMATION out) {
    StartObserved(1);
    observer_exact &= token==opaque_token&&a==app&&c==command&&p==&process_security&&t==&thread_security&&inherit==TRUE&&f==creation_flags&&e==environment&&d==cwd&&st==&startup&&out==expected_output;
    return FinishObserved(out);
}
BOOL WINAPI NativeWithToken(HANDLE token,DWORD lf,LPCWSTR a,LPWSTR c,DWORD f,LPVOID e,LPCWSTR d,LPSTARTUPINFOW st,LPPROCESS_INFORMATION out) {
    StartObserved(2);
    observer_exact &= token==opaque_token&&lf==logon_flags&&a==app&&c==command&&f==creation_flags&&e==environment&&d==cwd&&st==&startup&&out==expected_output;
    return FinishObserved(out);
}
BOOL WINAPI NativeWithLogon(LPCWSTR user,LPCWSTR domain,LPCWSTR password,DWORD lf,LPCWSTR a,LPWSTR c,
    DWORD f,LPVOID e,LPCWSTR d,LPSTARTUPINFOW st,LPPROCESS_INFORMATION out) {
    StartObserved(3);
    observer_exact &= user==opaque_user&&domain==opaque_domain&&password==opaque_password&&lf==logon_flags&&a==app&&c==command&&f==creation_flags&&e==environment&&d==cwd&&st==&startup&&out==expected_output;
    return FinishObserved(out);
}
BOOL InvokeObserved(unsigned index) {
    SetLastError(incoming);
    switch(index) {
    case 0:return model::ObserveCreateProcessA(narrow_app,narrow_command,&process_security,&thread_security,TRUE,creation_flags,environment,narrow_cwd,&narrow_startup,expected_output);
    case 1:return model::ObserveCreateProcessAsUserW(opaque_token,app,command,&process_security,&thread_security,TRUE,creation_flags,environment,cwd,&startup,expected_output);
    case 2:return model::ObserveCreateProcessWithTokenW(opaque_token,logon_flags,app,command,creation_flags,environment,cwd,&startup,expected_output);
    default:return model::ObserveCreateProcessWithLogonW(opaque_user,opaque_domain,opaque_password,logon_flags,app,command,creation_flags,environment,cwd,&startup,expected_output);
    }
}
#if defined(PULSE_RUNTIME_CHILD_ROUTING_TEST)
HANDLE replacement_token=nullptr, model_child_process=nullptr;
DWORD as_user_flags=CREATE_UNICODE_ENVIRONMENT, expected_flags=0;
LPVOID expected_environment=nullptr;
bool as_user_exact=true, as_user_nested=false;
unsigned as_user_calls=0, nested_w_calls=0;
DWORD WINAPI ResumeModel(HANDLE thread) { ++resume_calls;as_user_exact &= thread==reinterpret_cast<HANDLE>(uintptr_t{0x4567});SetLastError(0xbabe);return 1; }
BOOL WINAPI NestedW(LPCWSTR a,LPWSTR c,LPSECURITY_ATTRIBUTES p,LPSECURITY_ATTRIBUTES t,BOOL inherit,
    DWORD f,LPVOID e,LPCWSTR d,LPSTARTUPINFOW st,LPPROCESS_INFORMATION out) {
    ++nested_w_calls;
    as_user_exact &= a==app&&c==command&&p==&process_security&&t==&thread_security&&inherit==TRUE&&f==expected_flags&&e==expected_environment&&d==cwd&&st==&startup&&out==&information;
    out->dwProcessId=9191;SetLastError(native_error);return TRUE;
}
BOOL WINAPI RoutedAsUser(HANDLE token,LPCWSTR a,LPWSTR c,LPSECURITY_ATTRIBUTES p,LPSECURITY_ATTRIBUTES t,BOOL inherit,
    DWORD f,LPVOID e,LPCWSTR d,LPSTARTUPINFOW st,LPPROCESS_INFORMATION out) {
    ++as_user_calls;
    as_user_exact &= GetLastError()==incoming&&token==replacement_token&&a==app&&c==command&&p==&process_security&&t==&thread_security&&inherit==TRUE&&f==expected_flags&&e==expected_environment&&d==cwd&&st==&startup&&out==&information;
    if(as_user_nested) model::HookCreateProcessW(a,c,p,t,inherit,f,e,d,st,out);
    if(native_success){out->dwProcessId=9191;out->hProcess=model_child_process?model_child_process:GetCurrentProcess();out->hThread=reinterpret_cast<HANDLE>(uintptr_t{0x4567});}
    SetLastError(native_error);return native_success?TRUE:FALSE;
}
BOOL InvokeAsUser(bool routed) {
    expected_flags=as_user_flags|(routed?CREATE_SUSPENDED:0);
    expected_environment=routed?static_cast<LPVOID>(prepared_environment):static_cast<LPVOID>(environment);
    SetLastError(incoming);
    return model::ObserveCreateProcessAsUserW(replacement_token,app,command,&process_security,&thread_security,TRUE,
        as_user_flags,environment,cwd,&startup,&information);
}
bool EarlyResolver(model::EarlyChildRouting& result) noexcept {
    try {
        result.control=L"model-control";result.dll="model.dll";
        result.enabled=Enabled;result.log=Log;result.diagnostics=Diagnostic;
        return true;
    } catch (...) { return false; }
}
int AsUserTests(bool early=false) {
    HANDLE own=nullptr;
    Check(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&own)!=FALSE,"query only own process token for AsUser policy");
    if(!own)return 1;
    replacement_token=own;allowed=true;prepare_success=true;native_success=true;
    model::original_observed_as_user=RoutedAsUser;model::original_create_process=NestedW;model::test_resume_child_thread=ResumeModel;
    if(early) model::SetEarlyChildRoutingResolver(EarlyResolver);
    else Check(model::ConfigureChildRouting(L"model-control","model.dll",Enabled,Log,Diagnostic),"configure shared AsUser test transaction");
    model::PublishChildRoutingDiagnostics();
    BOOL result=InvokeAsUser(true);DWORD error=GetLastError();
    Check(result&&error==native_error&&as_user_calls==1&&prepare_calls==1&&bind_calls==1&&injection_calls==1&&resume_calls==1,"eligible AsUser shares one prepare create bind inject and own resume");
    Check(as_user_exact,"AsUser preserves optional token security inheritance cwd pointers and native error");
    auto snapshot=model::ReadChildRoutingDiagnostics();
    Check(snapshot.calls==0&&snapshot.events[0].entry==model::ChildCallEntry::CreateProcessAsUserW&&snapshot.events[0].branch==model::ChildCallBranch::Routed&&snapshot.events[0].child_pid==9191,"AsUser transaction is attributed to actual entry and returned PID");
    Check(snapshot.events[0].early_resolved==early,"early transaction resolves locally without Configure or worker publication");
    int before_bind=bind_calls,before_inject=injection_calls,before_resume=resume_calls;unsigned before_create=as_user_calls;
    native_success=false;result=InvokeAsUser(true);error=GetLastError();
    Check(!result&&error==native_error&&as_user_calls==before_create+1&&bind_calls==before_bind&&injection_calls==before_inject&&resume_calls==before_resume,"failed original create returns once without bind injection or resume");
    native_success=true;as_user_flags|=CREATE_SUSPENDED;before_resume=resume_calls;before_create=as_user_calls;
    result=InvokeAsUser(true);error=GetLastError();
    Check(result&&error==native_error&&as_user_calls==before_create+1&&resume_calls==before_resume,"caller CREATE_SUSPENDED remains caller-owned");
    as_user_flags&=~CREATE_SUSPENDED;as_user_nested=true;before_resume=resume_calls;int before_prepare=prepare_calls;before_inject=injection_calls;
    result=InvokeAsUser(true);error=GetLastError();as_user_nested=false;
    Check(result&&error==native_error&&nested_w_calls==1&&prepare_calls==before_prepare+1&&injection_calls==before_inject+1&&resume_calls==before_resume+1&&Has(model::ChildCallBranch::Reentry),"nested W forwards once without duplicate preparation injection or resume");
    Check(as_user_exact,"nested native W receives exactly AsUser forwarded flags and environment");
    replacement_token=nullptr;before_create=as_user_calls;before_inject=injection_calls;
    result=InvokeAsUser(true);error=GetLastError();
    Check(result&&error==native_error&&as_user_calls==before_create+1&&injection_calls==before_inject+1&&as_user_exact,"NULL optional token remains NULL in original call");
    replacement_token=reinterpret_cast<HANDLE>(uintptr_t{0x3333});before_create=as_user_calls;before_prepare=prepare_calls;before_resume=resume_calls;
    result=InvokeAsUser(false);error=GetLastError();snapshot=model::ReadChildRoutingDiagnostics();
    const auto& rejected=snapshot.events[snapshot.count-1];
    Check(result&&error==native_error&&as_user_calls==before_create+1&&prepare_calls==before_prepare&&resume_calls==before_resume,"ineligible token calls original unchanged and never alters suspension");
    Check(rejected.branch==model::ChildCallBranch::TokenContract&&rejected.token_reason==static_cast<DWORD>(model::AsUserTokenReason::Query)&&rejected.gate_error==ERROR_INVALID_HANDLE,"token rejection records exact reason and Win32 error without token contents");
    HANDLE restricted=nullptr;
    Check(CreateRestrictedToken(own,DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,0,nullptr,&restricted)!=FALSE,"make only an owned reduced-permission token fixture");
    if(restricted){replacement_token=restricted;before_prepare=prepare_calls;result=InvokeAsUser(false);error=GetLastError();snapshot=model::ReadChildRoutingDiagnostics();
        const auto& denied=snapshot.events[snapshot.count-1];
        Check(result&&error==native_error&&prepare_calls==before_prepare&&denied.branch==model::ChildCallBranch::TokenContract&&denied.gate_error==ERROR_ACCESS_DENIED,"reduced permissions remain native rather than borrowing caller authority");CloseHandle(restricted);}
    replacement_token=own;
    HANDLE wrong_type=CreateEventW(nullptr,TRUE,FALSE,nullptr), queried=nullptr;
    Check(wrong_type!=nullptr,"create owned non-process handle for post-create identity failure");
    if(wrong_type){
        const BOOL queried_ok=OpenProcessToken(wrong_type,TOKEN_QUERY,&queried);const DWORD query_error=GetLastError();
        Check(!queried_ok,"baseline rejects non-process token query");if(queried_ok)CloseHandle(queried);
        model_child_process=wrong_type;before_inject=injection_calls;before_bind=bind_calls;before_resume=resume_calls;
        result=InvokeAsUser(true);error=GetLastError();snapshot=model::ReadChildRoutingDiagnostics();const auto& post=snapshot.events[snapshot.count-1];
        Check(result&&error==native_error&&injection_calls==before_inject&&bind_calls==before_bind&&resume_calls==before_resume+1&&post.gate_error==query_error&&post.token_reason==static_cast<DWORD>(model::AsUserTokenReason::Query),"post-create token failure prevents bind and injection but resumes only own suspension");
        model_child_process=nullptr;CloseHandle(wrong_type);
    }
    replacement_token=own;allowed=false;before_prepare=prepare_calls;result=InvokeAsUser(false);error=GetLastError();
    Check(result&&error==native_error&&prepare_calls==before_prepare&&as_user_exact,"disabled gate clobbering LastError cannot change native API result");
    allowed=true;bind_error=ERROR_ACCESS_DENIED;before_inject=injection_calls;before_resume=resume_calls;
    result=InvokeAsUser(true);error=GetLastError();
    Check(result&&error==native_error&&injection_calls==before_inject&&resume_calls==before_resume+1,"bind rejection resumes only own added suspension without terminating created app");
    bind_error=ERROR_SUCCESS;inject_error=ERROR_NOT_SUPPORTED;before_resume=resume_calls;
    result=InvokeAsUser(true);error=GetLastError();
    Check(result&&error==native_error&&resume_calls==before_resume+1,"injection rejection preserves original successful creation and own resume");
    if(early) {
        model::SetEarlyChildRoutingResolver(nullptr);
        before_create=as_user_calls;before_prepare=prepare_calls;before_inject=injection_calls;
        result=InvokeAsUser(false);error=GetLastError();
        Check(result&&error==native_error&&as_user_calls==before_create+1&&prepare_calls==before_prepare&&
            injection_calls==before_inject&&as_user_exact,"unavailable early resolver remains one unchanged native call");
    }
    Check(CloseHandle(own)!=FALSE,"close own token fixture");
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
#endif
int ObserverTests() {
    model::original_observed_create_a=NativeA;model::original_observed_as_user=NativeAsUser;
    model::original_observed_with_token=NativeWithToken;model::original_observed_with_logon=NativeWithLogon;
    for(unsigned i=0;i<4;++i) {
        native_success=true;expected_output=&information;auto calls=observer_calls[i];
        BOOL result=InvokeObserved(i);DWORD error=GetLastError();
        Check(result&&error==native_error&&observer_calls[i]==calls+1,"observed success calls corresponding original exactly once and preserves LastError");
        auto snapshot=model::ReadChildRoutingDiagnostics();const auto& success=snapshot.events[snapshot.count-1];
        Check(success.entry==static_cast<model::ChildCallEntry>(i+1)&&success.result&&success.child_pid==8181&&success.native_error==native_error,"observed success preserves entry and returned PID in bounded buffer");
        native_success=false;expected_output=reinterpret_cast<LPPROCESS_INFORMATION>(uintptr_t{1});calls=observer_calls[i];
        result=InvokeObserved(i);error=GetLastError();
        Check(!result&&error==native_error&&observer_calls[i]==calls+1,"observed failure calls original exactly once without reading undefined output");
        snapshot=model::ReadChildRoutingDiagnostics();const auto& failed=snapshot.events[snapshot.count-1];
        Check(!failed.result&&failed.child_pid==0&&failed.entered_tick<=failed.returned_tick,"failed observer has no invented PID and retains call timing");
    }
    Check(observer_exact&&observer_incoming,"all argument pointers including opaque token and credentials forwarded unchanged");
    auto snapshot=model::ReadChildRoutingDiagnostics();
    Check(snapshot.calls==0&&snapshot.total_calls==8&&snapshot.count==8&&delivered_count==0,"pre-ready observers buffer independently without increasing W counters or logging");
    native_success=true;expected_output=&information;nest_observers=true;
    const auto a=observer_calls[0],u=observer_calls[1];InvokeObserved(0);
    Check(observer_calls[0]==a+1&&observer_calls[1]==u+1,"nested public API observations each call their own original once");
    nest_w=true;const int w=native_calls;InvokeObserved(0);
    snapshot=model::ReadChildRoutingDiagnostics();
    Check(native_calls==w+1&&snapshot.calls==1&&Has(model::ChildCallBranch::NotConfigured)&&!Has(model::ChildCallBranch::Reentry),"observer does not mark W reentrant or suppress its existing decision path");
    Check(exact&&observer_exact&&observer_incoming,"nested observations retain caller arguments and error state");
    Check(model::ConfigureChildRouting(L"model-control","model.dll",Enabled,Log,Diagnostic),"configure only diagnostic flush for test");
    model::PublishChildRoutingDiagnostics();
    Check(delivered_count==snapshot.count,"ready flush includes all earlier nested entry observations");
    model::PublishChildRoutingDiagnostics();Check(delivered_count==snapshot.count,"observer records are not delivered twice");
    for(const auto& event: snapshot.events) { if(!event.sequence)continue; Check(event.entry==model::ChildCallEntry::CreateProcessW || event.entry==model::ChildCallEntry::CreateProcessAsUserW || event.branch==model::ChildCallBranch::ObserveOnly,"remaining public observers never claim child routing or injection"); }
    Check(prepare_calls==0&&injection_calls==0,"observer mode performs no authentication child creation preparation or injection");
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
#endif
bool Has(model::ChildCallBranch branch) {
    const auto snapshot=model::ReadChildRoutingDiagnostics();
    return snapshot.branches[static_cast<size_t>(branch)]!=0;
}
}
// Link seams isolate observation/forwarding from filesystem, process creation and
// image mutation. No fake runtime is installed into any external process.
namespace pulse::shell_bridge::runtime {
struct ChildSessionPreparation::State {};
ChildSessionPreparation::ChildSessionPreparation() noexcept = default;
ChildSessionPreparation::~ChildSessionPreparation() = default;
ChildSessionPreparation::ChildSessionPreparation(ChildSessionPreparation&&) noexcept = default;
ChildSessionPreparation& ChildSessionPreparation::operator=(ChildSessionPreparation&&) noexcept = default;
LPVOID ChildSessionPreparation::Environment() noexcept { return prepared_environment; }
DWORD ChildSessionPreparation::EnvironmentFlags() const noexcept { return prepared_flags; }
DWORD ChildSessionPreparation::Bind(HANDLE,DWORD,ChildSessionDiagnostic&) noexcept { ++bind_calls; return bind_error; }
DWORD PrepareChildSession(const std::wstring&,LPCVOID,DWORD call_flags,ChildSessionPreparation&,ChildSessionDiagnostic& diagnostic) noexcept {
    ++prepare_calls; prepared_flags=call_flags; diagnostic.stage=ChildSessionStage::Environment;
    diagnostic.error=prepare_success?ERROR_SUCCESS:ERROR_BAD_ENVIRONMENT; return diagnostic.error;
}
ChildInjectionResult InjectChildRuntime(HANDLE,const char*,bool) noexcept { ++injection_calls; return {inject_error, inject_error==ERROR_SUCCESS}; }
}
int wmain(int argc,wchar_t** argv) {
    std::cout<<std::unitbuf;
    model::original_create_process=Native;
#if defined(PULSE_DIALOG_UI_PROOF)
    if(argc==2&&std::wstring_view(argv[1])==L"--observers")return ObserverTests();
#if defined(PULSE_RUNTIME_CHILD_ROUTING_TEST)
    if(argc==2&&std::wstring_view(argv[1])==L"--as-user")return AsUserTests();
    if(argc==2&&std::wstring_view(argv[1])==L"--early-as-user")return AsUserTests(true);
#endif
#endif
    const bool missing=argc==2&&std::wstring_view(argv[1])==L"--missing-enabled";
    if(argc!=1&&!missing)return 2;
    model::original_create_process=Native;
    const int first=native_calls; BOOL result=Invoke(); DWORD error=GetLastError();
    Check(result&&error==native_error&&native_calls==first+1,"preconfiguration success calls native once and preserves LastError");
    Check(exact&&observed_incoming==incoming,"all original pointers flags and incoming error reach native unchanged");
    native_success=false; result=Invoke(); error=GetLastError();
    Check(!result&&error==native_error&&native_calls==first+2,"preconfiguration failure preserves native error and single call");
    auto before=model::ReadChildRoutingDiagnostics();
    Check(before.count==2&&before.events[0].child_pid==4242&&before.events[0].result&&before.events[1].child_pid==0&&!before.events[1].result,"buffer retains returned PID only on success before configuration");
    Check(before.events[0].entered_tick<=before.events[0].returned_tick&&before.events[0].sequence==1&&before.events[1].sequence==2,"buffer preserves per-call times and sequence");
    Check(delivered_count==0,"preconfiguration does not log");
    Check(model::ConfigureChildRouting(L"model-control","model.dll",missing?nullptr:Enabled,Log,Diagnostic),"publish immutable test routing");
    Check(delivered_count==0,"configuration alone does not drain before readiness");
    model::PublishChildRoutingDiagnostics();
    Check(delivered_count==2&&delivered[0].child_pid==4242&&delivered[1].native_error==native_error,"ready flush retains original pre-ready PID and native error");
    model::PublishChildRoutingDiagnostics(); Check(delivered_count==2,"repeated ready flush does not duplicate events");
    const int saved=native_calls; result=Invoke();error=GetLastError();
    Check(!result&&error==native_error&&native_calls==saved+1,"configured bypass failure remains a single native call");
    Check(Has(missing?model::ChildCallBranch::MissingEnabled:model::ChildCallBranch::Disabled),"bypass reason is distinguished");
    if(!missing) {
        auto snapshot=model::ReadChildRoutingDiagnostics();const auto& event=snapshot.events[snapshot.count-1];
        Check(event.gate==model::ChildGateReason::MarkerPresent&&event.gate_error==ERROR_CANCELLED,"disabled decision retains exact gate reason");
        nested_gate=true;const int count=native_calls;Invoke();error=GetLastError();
        Check(native_calls==count+2&&error==native_error&&Has(model::ChildCallBranch::Reentry),"enabled callback reentry is observed without recursive policy calls");
        allowed=true;startup.cb=0;const int startup_count=native_calls;Invoke();error=GetLastError();
        Check(native_calls==startup_count+1&&error==native_error&&Has(model::ChildCallBranch::StartupContract),"invalid startup forwards unchanged once after diagnostic logging");
        startup.cb=sizeof(startup);const int prepare_count=native_calls;Invoke();error=GetLastError();
        snapshot=model::ReadChildRoutingDiagnostics();const auto& failed=snapshot.events[snapshot.count-1];
        Check(native_calls==prepare_count+1&&error==native_error&&prepare_calls==1&&failed.branch==model::ChildCallBranch::PrepareFailed,"prepare failure forwards original call once");
        Check(failed.prepare_stage==static_cast<DWORD>(model::ChildSessionStage::Environment)&&failed.gate_error==ERROR_BAD_ENVIRONMENT,"prepare failure stage and original diagnostic error retained");
        allowed=false;native_success=true;nested_log=true;const int log_count=native_calls;result=Invoke();error=GetLastError();
        Check(result&&native_calls==log_count+2&&error==native_error,"diagnostic callback reentry is bounded and preserves outer native result");
        Check(exact&&observed_incoming==incoming,"diagnostic callbacks never alter forwarded original parameters");
    }
    native_success=false;
    for(size_t i=0;i<model::kChildDiagnosticCapacity+5;++i)Invoke();
    const auto final=model::ReadChildRoutingDiagnostics();
    Check(final.count==model::kChildDiagnosticCapacity&&final.dropped==final.calls-model::kChildDiagnosticCapacity,"fixed-capacity buffer reports dropped records without overwriting earlier PIDs");
    Check(final.events[0].child_pid==4242&&delivered_count==model::kChildDiagnosticCapacity,"earliest evidence retained and each buffered event delivered at most once");
    Check(prepare_calls==(missing?0:1)&&injection_calls==0,"diagnostic-only seams never inject or create a real process");
    std::cout<<checks<<" checks, "<<failures<<" failures\n";return failures?1:0;
}
