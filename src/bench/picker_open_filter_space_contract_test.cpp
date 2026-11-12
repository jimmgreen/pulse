#include "../shell_bridge/file_dialog_contract.h"
#include "../common/picker_open_filter.h"
#include <cstdio>
using namespace pulse::shell_bridge;
namespace wire=pulse::ipc::picker;
namespace {
int failures=0;
void Check(bool ok,const char* text){std::printf("[%s] %s\n",ok?"PASS":"FAIL",text);if(!ok)++failures;}
std::vector<SetterCall> Calls(DialogFactory factory,std::u16string pattern){
    SetterCall options;options.setting=Setting::Options;options.native_result=S_OK;options.value=DefaultDialogOptions(factory)|FOS_FORCEFILESYSTEM|FOS_DONTADDTORECENT;
    SetterCall folder;folder.setting=Setting::Folder;folder.native_result=S_OK;folder.text=u"C:\\fixture";folder.filesystem_folder=true;
    SetterCall types;types.setting=Setting::FileTypes;types.native_result=S_OK;types.filters={{u"Drawing",std::move(pattern)}};
    return {options,folder,types};
}
ContractDecision Prepare(DialogFactory factory,const std::vector<SetterCall>& calls){wire::RequestId id{};id[0]=1;return PrepareFileDialogContract(factory,calls,id,factory==DialogFactory::Open?AccessPolicy::LocalOpenV1:AccessPolicy::LocalSaveV1);}
}
int main(){
    for(auto pattern:{u"*.dwg; *.dxf",u"*.dwg;  *.dxf"}){
        const auto calls=Calls(DialogFactory::Open,pattern);const auto decision=Prepare(DialogFactory::Open,calls);
        const auto* request=decision.request?std::get_if<wire::Request>(&decision.request->body):nullptr;
        Check(decision.disposition==Disposition::Prepared&&request&&request->filters[0].pattern==u"*.dwg;*.dxf","Open canonicalizes only post-semicolon ASCII spaces");
        Check(calls.back().filters[0].pattern==pattern,"original successful native setter pattern remains unchanged");
        Check(request&&pulse::picker_filter::OpenPattern(std::u16string_view(request->filters[0].pattern))&&pulse::picker_filter::FirstLiteralExtension(std::u16string_view(request->filters[0].pattern))==u"dwg","existing host parser and extension inference accept canonical request");
    }
    auto calls=Calls(DialogFactory::Open,u"*.dwg; *.dxf");calls.back().filters={{u"YJK",u"*.dwy"},{u"DWG",u"*.dwg"},{u"DXF",u"*.dxf"},{u"Drawing",u"*.dwg; *.dxf"},{u"All drawings",u"*.dwy;*.dwg;*.dxf"}};
    SetterCall index;index.setting=Setting::FileTypeIndex;index.native_result=S_OK;index.value=4;calls.push_back(index);
    const auto decision=Prepare(DialogFactory::Open,calls);const auto* request=decision.request?std::get_if<wire::Request>(&decision.request->body):nullptr;
    Check(request&&request->filters.size()==5&&request->filter_index==3&&request->filters[3].pattern==u"*.dwg;*.dxf"&&request->filters[4].pattern==u"*.dwy;*.dwg;*.dxf","exact observed five-filter YJK list preserves order and selection");
    for(auto pattern:{u"*.dwg ;*.dxf",u"*.dwg ; *.dxf",u" *.dwg;*.dxf",u"*.dwg;*.dxf ",u"*.d wg; *.dxf",u"*.dwg; \t*.dxf",u"*.dwg; ",u"*.dwg; ;*.dxf",u" ; ",u"*.dwg; C:\\*.dxf",u"*.dwg; ../*.dxf",u"*.dwg; report*.dxf",u"*.dwg; *.d?f"}){
        const auto invalid=Prepare(DialogFactory::Open,Calls(DialogFactory::Open,pattern));Check(invalid.disposition==Disposition::NativeOnly&&invalid.reason==NativeReason::UnsupportedFilter,"unproven spaces, empty-space segments, paths and unsupported globs remain native");
    }
    auto save=Prepare(DialogFactory::Save,Calls(DialogFactory::Save,u"*.dwg; *.dxf"));Check(save.disposition==Disposition::NativeOnly&&save.reason==NativeReason::UnsupportedFilter,"Save filter contract remains unchanged");
    auto failed=Calls(DialogFactory::Open,u"*.dwg; *.dxf");failed.back().native_result=E_INVALIDARG;Check(Prepare(DialogFactory::Open,failed).reason==NativeReason::SetterFailed,"failed native setter is never normalized into acceptance");
    Check(!pulse::picker_filter::OpenPattern(std::u16string_view(u"*.dwg; *.dxf")),"shared parser has not been globally loosened");
    std::printf("open-filter-space failures=%d\n",failures);return failures?1:0;
}
