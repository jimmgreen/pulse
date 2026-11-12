#include "../dialog_launcher/admin_auto_attach.h"
#include "../common/dialog_auto_binding.h"
#include <cstdio>
using namespace pulse::dialog_launcher::admin_auto;
int main(){int failures=0,checks=0;auto check=[&](bool okay,const char*label){++checks;printf("[%s] %s\n",okay?"PASS":"FAIL",label);if(!okay)++failures;};
 Eligibility good{true,true,true,true,true,false};check(CheckEligibility(good)==0,"same elevated unrestricted session architecture accepted");
 for(unsigned bit=0;bit<6;++bit){auto test=good;switch(bit){case 0:test.same_user_session=false;break;case 1:test.elevated=false;break;case 2:test.unrestricted=false;break;case 3:test.same_architecture=false;break;case 4:test.unprotected_process=false;break;case 5:test.excluded=true;break;}check(CheckEligibility(test)!=0,"each identity/protection/exclusion boundary rejects");}
 check(StatusBasename(7,IMAGE_FILE_MACHINE_AMD64)==L"auto-helper-7-34404.bin","status locator numeric bounded");
 HelperStatusV1 status{};check(status.size==56&&status.magic==kStatusMagic&&!status.ready&&status.error==ERROR_IO_PENDING,"status defaults never claim readiness");
 GUID nonce{1,2,3,{4,5,6,7,8,9,10,11}};pulse::dialog_auto_binding::BindingV1 bound{};const DWORD captured=pulse::dialog_auto_binding::CaptureCurrentWithHelper(GetCurrentProcess(),1,nonce,bound);
 HANDLE token=nullptr;TOKEN_ELEVATION elevation{};DWORD size=0;if(OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token)){GetTokenInformation(token,TokenElevation,&elevation,sizeof(elevation),&size);CloseHandle(token);}printf("[DIAG] ordinary capture=%lu\n",captured);if(!elevation.TokenIsElevated)check(captured==ERROR_ELEVATION_REQUIRED||captured==ERROR_ACCESS_DENIED,"ordinary token cannot publish attach binding");
 printf("checks=%d failures=%d\n",checks,failures);return failures?1:0;}
