#include "../app/win_e_fast_entry.h"
#include <cstdio>
int main() {
 int fails=0;
 auto check=[&](bool ok,const char* s){printf("[%s] %s\n",ok?"PASS":"FAIL",s);fails+=!ok;};
 const wchar_t* direct[]={L"pulse.exe",L"--win-e"};
 const wchar_t* shell[]={L"pulse.exe",L"--shell-open",L"pulse:win-e"};
 const wchar_t* unrelated[]={L"pulse.exe",L"--shell-open",L"C:\\"};
 const wchar_t* extra[]={L"pulse.exe",L"--win-e",L"--test-instance"};
 int finds=0,forwards=0;
 auto run=[&](int n,const wchar_t*const* args,bool exists,bool accepted){
#ifdef PULSE_FAST_BASELINE
return std::optional<int>{};
#else
return pulse::app::TryWinEFastEntry(n,args,[&]{++finds;return exists;},[&]{++forwards;return accepted;});
#endif
};
 check(run(2,direct,true,true)==0 && forwards==1,"warm direct request forwards once before runtime startup");
 check(run(3,shell,true,true)==0 && forwards==2,"registered shell Win+E uses same fast path");
 check(!run(2,direct,false,true) && forwards==2,"no endpoint retains complete cold startup");
 check(run(2,direct,true,false)==1 && forwards==3,"uncertain delivery is terminal and cannot double-launch");
 int old=finds;
 check(!run(3,unrelated,true,true) && finds==old,"ordinary folder launch keeps existing routing");
 check(!run(3,extra,true,true) && finds==old,"test and extra arguments cannot bypass startup");
 check(!pulse::app::IsWinEFastEntry(0,nullptr),"missing argv is rejected");
 return fails?1:0;
}
