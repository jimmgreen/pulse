#include "../picker_inprocess/caller_activation_scope.h"
#include <cstdio>
#include <stdexcept>
using namespace pulse::picker_inprocess;
namespace {
unsigned checks=0, failures=0;
void Check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);}
HANDLE Current(){HANDLE value=nullptr;GetCurrentActCtx(&value);if(value)ReleaseActCtx(value);return value;}
DWORD EarlyPop(ULONG_PTR cookie){
    __try { DeactivateActCtx(0,cookie); return 0; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return GetExceptionCode(); }
}
}
int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    ACTCTXW spec{};spec.cbSize=sizeof(spec);spec.lpSource=argv[1];
    HANDLE ui=CreateActCtxW(&spec);
    Check(ui!=INVALID_HANDLE_VALUE,"create isolated UI context");if(ui==INVALID_HANDLE_VALUE)return 1;
    const HANDLE original=Current();
    CallerActivation caller;Check(caller.Capture(),"capture caller context including null process default");
    ULONG_PTR outer=0,nested=0;
    Check(ActivateActCtx(ui,&outer)!=FALSE,"enter outer UI frame");
    Check(ActivateActCtx(ui,&nested)!=FALSE,"simulate nested window-dispatch frame");
    Check(EarlyPop(outer)==0xC015000Ful,"old callback pop reproduces captured crash status");
    for(unsigned mode=0;mode<3;++mode){
        try {
            CallerActivationFrame frame(caller);
            Check(frame.Entered() && Current()==original,"callback observes captured caller context");
            if(mode==1)throw std::runtime_error("callback failure");
            if(mode==2){CallerActivationFrame reentrant(caller);Check(reentrant.Entered() && Current()==original,"nested callback preserves caller context");}
        }catch(const std::runtime_error&){}
        Check(Current()==ui,"callback return or exception restores nested UI frame");
    }
    Check(DeactivateActCtx(0,nested)!=FALSE,"nested dispatch frame remains independently poppable");
    Check(DeactivateActCtx(0,outer)!=FALSE,"outer Show frame remains independently poppable");
    Check(Current()==original,"outer unwind preserves caller context");
    ReleaseActCtx(ui);
    std::printf("Checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
