#include "../shell_bridge/legacy_call_observer.h"
#include <cstdio>
using namespace pulse::shell_bridge;
unsigned checks=0, failures=0;
void Check(bool ok,const char* name){++checks;if(!ok)++failures;std::printf("[%s] %s\n",ok?"PASS":"FAIL",name);}
template<class Ofn> void Run(){
    for(unsigned mode=0;mode<4;++mode){
        Ofn input{};input.lStructSize=mode==2?sizeof(DWORD):sizeof(input);
        input.Flags=OFN_EXPLORER|OFN_ENABLEHOOK|OFN_ENABLETEMPLATE;
        input.lCustData=42;
        unsigned calls=0, observations=0;
        SetLastError(1234);
        const BOOL result=ObserveLegacyCall(mode==3?nullptr:&input,[&](Ofn* exact){
            ++calls;Check(exact==(mode==3?nullptr:&input),"original pointer is unchanged");
            Check(GetLastError()==1234,"observer cannot change original entry error");SetLastError(5678);return mode==0?TRUE:FALSE;
        },[&](const LegacyCallObservation& value){
            ++observations;Check(value.complete==(mode<2),"short and null inputs do not read fields");
            if(value.complete)Check(value.flags==input.Flags && value.custom_data,"flags and custom-data presence captured without data");
            SetLastError(999);if(mode==1)throw 1;
        });
        Check(GetLastError()==5678 && result==(mode==0?TRUE:FALSE),"original return and final error survive observer");
        Check(calls==1 && observations==1,"original invoked exactly once including observer exception");
    }
}
int main(){Run<OPENFILENAMEW>();Run<OPENFILENAMEA>();std::printf("Checks=%u failures=%u\n",checks,failures);return failures?1:0;}
