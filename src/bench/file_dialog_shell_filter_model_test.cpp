#include "../ui/picker_shell_item_filter.h"
#include "../ipc/picker_remote_protocol.h"
#include <cstdio>
#include <string>
using namespace pulse::ipc::picker::remote;
int main(){
 unsigned checks=0,failures=0;const auto check=[&](bool ok,const char* label){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);};
 std::vector<std::wstring> rows{L"allow",L"block",L"allow2"};const auto original=rows;bool current=true;
 auto path=[](const auto& row)->const std::wstring&{return row;};
 auto include=[](const std::wstring& row)->HRESULT{return row==L"block"?S_FALSE:S_OK;};
 check(pulse::ui::FilterShellItems(rows,path,include,[&]{return current;})==S_OK&&rows==std::vector<std::wstring>{L"allow",L"allow2"},"caller verdict removes file/folder rows only after complete batch");
 rows=original;unsigned calls=0;
 check(pulse::ui::FilterShellItems(rows,path,[&](const auto&)->HRESULT{if(++calls==2)current=false;return S_OK;},[&]{return current;})==HRESULT_FROM_WIN32(ERROR_RETRY)&&rows==original,"callback navigation/cancel invalidates generation without publishing partial rows");
 rows=original;current=true;
 check(pulse::ui::FilterShellItems(rows,path,[](const auto&)->HRESULT{return E_ACCESSDENIED;},[&]{return current;})==S_OK&&rows==original,"native oracle includes FAILED IncludeItem HRESULT instead of failing Show");
 rows.clear();check(pulse::ui::FilterShellItems(rows,path,include,[]{return true;})==S_OK&&rows.empty(),"empty enumeration does not fabricate callback items");
 check(!pulse::ui::ShellFilterCandidateNeedsCallback(pulse::ui::ShellFilterCandidateRole::NavigationShortcut),"sidebar navigation shortcuts never trigger caller filter or Shell probing");
 check(pulse::ui::ShellFilterCandidateNeedsCallback(pulse::ui::ShellFilterCandidateRole::EnumeratedView),"worker-enumerated current-view candidates still require caller filtering");
 Message config;config.request_id[0]=1;config.sequence=1;config.kind=Kind::ValidateConfig;config.config.initial_folder=L"C:\\Fixture";config.config.open_options=PULSE_PICKER_UI_V2_OPEN_SHELL_ITEM_FILTER;
 std::vector<uint8_t> bytes;Message decoded;
 check(Encode(config,bytes)&&Decode(bytes,decoded)&&decoded==config,"active listing filter flag roundtrips ValidateConfig");config.kind=Kind::Show;
 check(Encode(config,bytes)&&Decode(bytes,decoded)&&decoded==config,"active listing filter flag roundtrips Show");
 check(!ConfigSupportedByPeer(config.config.open_options,0,0),"old peer rejects active filter before sending Validate or Show");
 check(ConfigSupportedByPeer(config.config.open_options,0,PULSE_PICKER_UI_V2_FEATURE_SHELL_ITEM_FILTER),"new peer explicitly advertises caller-side IncludeItem");
 check(!ConfigSupportedByPeer(config.config.open_options,0,0x80000000u),"unknown feature never borrows filter capability");
 config.config.open_options|=0x100u;check(!Encode(config,bytes)&&!ConfigSupportedByPeer(config.config.open_options,0,0xffffffffu),"unknown config option remains rejected");
 Message event;event.request_id[0]=1;event.sequence=2;event.kind=Kind::Event;event.event.kind=PULSE_PICKER_UI_V2_EVENT_INCLUDE_ITEM;event.event.show_id=1;event.event.generation=2;event.event.window=3;event.event.proposed_folder=L"C:\\Fixture\\blocked.txt";
 check(Encode(event,bytes)&&Decode(bytes,decoded)&&decoded==event,"IncludeItem uses authenticated existing event/reply RPC with path only");
 OwnedEvent abi(event.event);Event copied;check(FromAbi(abi.Get(),copied)&&copied==event.event,"IncludeItem event ABI conversion preserves path and identity");
 event.event.proposed_folder.clear();check(!Encode(event,bytes),"empty filter target rejected by codec");event.event.proposed_folder=L"C:\\Fixture\\blocked.txt";event.event.control_id=1;check(!Encode(event,bytes),"filter event cannot smuggle unrelated control state");event.event.control_id=0;event.event.kind=16;check(!Encode(event,bytes),"unknown event remains rejected");
 std::printf("SHELL_FILTER_MODEL checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
