#include "../picker_inprocess/caller_callback_invocation.h"
#include <cstdio>
#include <vector>
using namespace pulse::picker_inprocess;
namespace {
unsigned checks=0,failures=0;
void Check(bool ok,const char* label){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);}
struct Operations {
    CallerInvocationStage failed=CallerInvocationStage::None;
    std::vector<CallerInvocationStage> trace;
    HRESULT Step(CallerInvocationStage stage){trace.push_back(stage);return stage==failed?E_ACCESSDENIED:S_OK;}
};
}
int main(){
    for(const HRESULT verdict:{S_OK,S_FALSE,HRESULT(2),E_ACCESSDENIED}){
        Operations operations;unsigned calls=0;
        const auto outcome=InvokeCallerCallback(operations,[&]{++calls;return verdict;});
        Check(outcome.infrastructure_result==S_OK&&outcome.caller_returned&&outcome.caller_result==verdict&&calls==1,"user HRESULT is not confused with invocation infrastructure");
        const bool included=outcome.infrastructure_result==S_OK&&outcome.caller_result!=S_FALSE;
        Check(included==(verdict!=S_FALSE),"native oracle exact S_FALSE hides; S_OK HR2 and E_ACCESSDENIED include");
        Check(operations.trace==std::vector<CallerInvocationStage>{CallerInvocationStage::Push,CallerInvocationStage::EnterDpi,CallerInvocationStage::EnterLanguage,CallerInvocationStage::RestoreDpi,CallerInvocationStage::RestoreLanguage,CallerInvocationStage::Pop},"successful callback restores UI context and pops exactly once");
    }
    for(const auto fault:{CallerInvocationStage::Push,CallerInvocationStage::EnterDpi,CallerInvocationStage::EnterLanguage,CallerInvocationStage::RestoreDpi,CallerInvocationStage::RestoreLanguage,CallerInvocationStage::Pop}){
        Operations operations;operations.failed=fault;unsigned calls=0;
        const auto outcome=InvokeCallerCallback(operations,[&]{++calls;return S_FALSE;});
        const bool ended=FAILED(outcome.infrastructure_result);
        const bool included=!ended&&outcome.caller_result!=S_FALSE;
        std::printf("FAULT stage=%d caller_calls=%u caller_hr=%08lX infra_hr=%08lX ended=%u included=%u\n",static_cast<int>(fault),calls,static_cast<unsigned long>(outcome.caller_result),static_cast<unsigned long>(outcome.infrastructure_result),ended,included);
        Check(ended&&!included&&outcome.failed_stage==fault,"internal push DPI language restore or pop failure terminates instead of fail-open");
        Check(calls==(fault==CallerInvocationStage::RestoreDpi||fault==CallerInvocationStage::RestoreLanguage||fault==CallerInvocationStage::Pop?1u:0u),"failed entry never invokes caller; failed cleanup preserves observed caller invocation count");
        if(fault!=CallerInvocationStage::Push)Check(operations.trace.back()==CallerInvocationStage::Pop,"every pushed activation frame has one checked pop attempt");
    }
    Operations operations;
    const auto exception=InvokeCallerCallback(operations,[]()->HRESULT{throw 1;});
    Check(exception.infrastructure_result==E_FAIL&&exception.failed_stage==CallerInvocationStage::Callback&&!exception.caller_returned,"throwing callback is not a returned native HRESULT");
    std::printf("CALLER_INVOCATION checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
