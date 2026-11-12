#include "../shell_bridge/file_dialog_contract.h"
#include <cstdio>
using namespace pulse::shell_bridge;
int main(){
    unsigned checks=0,failures=0;const auto check=[&](bool ok,const char* text){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);};
    pulse::ipc::picker::RequestId id{};id[0]=1;
    SetterCall options;options.setting=Setting::Options;options.native_result=S_OK;options.value=DefaultDialogOptions(DialogFactory::Open)|FOS_FORCEFILESYSTEM|FOS_DONTADDTORECENT;
    SetterCall folder;folder.setting=Setting::Folder;folder.native_result=S_OK;folder.filesystem_folder=true;folder.text=u"C:\\Fixture";
    SetterCall filter;filter.setting=Setting::ShellFilter;filter.native_result=S_OK;
    std::vector<SetterCall> calls{options,folder,filter};
    const auto prepare=[&]{return PrepareFileDialogContract(DialogFactory::Open,calls,id,AccessPolicy::LocalOpenV1);};
    check(prepare().disposition==Disposition::Prepared,"successful null filter is an inactive setting, not a permanent native gate");
    calls.back().value=1;check(prepare().reason==NativeReason::ShellFilter,"effective active filter requires an explicitly negotiated caller callback contract");
    calls.push_back(filter);check(prepare().disposition==Disposition::Prepared,"clearing an earlier successful filter removes only that filter requirement");
    SetterCall unknown;unknown.setting=Setting::Unknown;unknown.native_result=S_OK;calls.push_back(unknown);check(prepare().disposition==Disposition::NativeOnly,"filter clear preserves an independent unsupported setting");
    calls={options,folder,filter};calls.back().native_result=E_INVALIDARG;check(prepare().reason==NativeReason::SetterFailed&&prepare().observed_result==E_INVALIDARG,"native failed setter HRESULT remains observable and gated");
    calls={options,folder,filter};calls.back().value=1;PreparationContext context;context.shell_item_filter_supported=true;
    auto supported=PrepareFileDialogContract(DialogFactory::Open,calls,id,AccessPolicy::LocalOpenV1,context);
    check(supported.disposition==Disposition::Prepared&&supported.shell_item_filter,"negotiated active filesystem filter becomes an explicit caller callback obligation");
    check(!BridgeRequestFor(supported),"basic picker pipe without callback RPC never drops active filter");
    calls.back().value=2;check(PrepareFileDialogContract(DialogFactory::Open,calls,id,AccessPolicy::LocalOpenV1,context).disposition==Disposition::InvalidArgument,"invalid filter presence field is not coerced to true");
    calls.back().value=1;calls.front().value|=FOS_SUPPORTSTREAMABLEITEMS;context.native_options_supported=true;
    check(PrepareFileDialogContract(DialogFactory::Open,calls,id,AccessPolicy::LocalOpenV1,context).reason==NativeReason::ShellFilter,"unsupported Shell stream enumeration stays native before display");
    std::printf("SHELL_FILTER_CONTRACT checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
