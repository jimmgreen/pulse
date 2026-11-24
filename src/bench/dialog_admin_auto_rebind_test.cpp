#include "../dialog_launcher/admin_auto_rebind.h"
#include <cstdio>
using namespace pulse::dialog_launcher::admin_auto;
namespace cfg = pulse::shell_bridge::runtime;
int main() {
    unsigned checks=0, failures=0;
    auto check=[&](bool ok,const char* name){++checks;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);if(!ok)++failures;};
    cfg::PulseRuntimeAutoDiagnosticV1 good{};
    check(!CanAutoRebind(good),"default diagnostic never grants rebind");
    good.initialization=cfg::kAutoReadyForRebind;
    check(CanAutoRebind(good),"explicit fully restored diagnostic grants rebind");
    for(unsigned bit=0;bit<7;++bit){auto bad=good;switch(bit){case 0:bad.size=0;break;case 1:bad.version=2;break;case 2:bad.initialization=S_OK;break;case 3:bad.initialization=E_PENDING;break;case 4:bad.api_mask=1;break;case 5:bad.patched_slots=1;break;case 6:bad.protection_restored=0;break;}check(!CanAutoRebind(bad),"invalid or incomplete diagnostic rejects");}
    auto old=good;old.initialization=HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);check(!CanAutoRebind(old),"old generic aborted status never grants rebind");
    check(CanReuseAutoRuntime(true,false,10,20,100),"exact trusted runtime exports permit probe");
    check(!CanReuseAutoRuntime(false,false,10,20,100),"different bundle or runtime path rejects");
    check(!CanReuseAutoRuntime(true,true,10,20,100),"multiple runtime modules reject");
    check(!CanReuseAutoRuntime(true,false,0,20,100),"missing attach rejects");
    check(!CanReuseAutoRuntime(true,false,10,0,100),"missing diagnose rejects");
    check(!CanReuseAutoRuntime(true,false,100,20,100),"out of bounds attach rejects");
    check(!CanReuseAutoRuntime(true,false,10,100,100),"out of bounds diagnose rejects");
    check(kMaxAutoRebinds==8&&kAutoRebindCooldownMs==5000,"retry count and cooldown bounded");
    std::printf("checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
