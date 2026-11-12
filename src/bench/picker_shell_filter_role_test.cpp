#include "../ui/picker_shell_item_filter.h"
#include <cstdio>
#include <string>
#include <vector>
using namespace pulse::ui;
int main(){
 unsigned checks=0,failures=0;const auto check=[&](bool ok,const char* label){++checks;failures+=!ok;std::printf("[%s] %s\n",ok?"PASS":"FAIL",label);};
 const std::vector<std::wstring> local{L"C:\\fixture\\visible.txt",L"C:\\fixture\\blocked.txt"};
 for(const auto* bad:{L"C:\\fixture\\deleted-favorite",L"\\\\offline\\share\\folder"}){
  auto current_rows=local;std::vector<std::wstring> shortcuts{L"C:\\fixture",bad};const auto before=shortcuts;bool closed=false;unsigned binds=0,probe_io=0;
  const auto result=FilterShellItems(shortcuts,[](const auto& path)->const std::wstring&{return path;},[&](const std::wstring& path)->HRESULT{
   ++binds;if(path==bad){++probe_io;closed=true;return HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);}return S_OK;
  },[&]{return !closed;},ShellFilterCandidateRole::NavigationShortcut);
  std::printf("SHORTCUT result=%08lX closed=%u binds=%u probe_io=%u\n",static_cast<unsigned long>(result),closed,binds,probe_io);
  check(result==S_OK&&!closed,"unrelated stale or offline shortcut does not abort readable current view");
  check(binds==0&&probe_io==0,"navigation shortcuts never trigger caller-STA Shell rebind or probe IO");
  check(shortcuts==before&&current_rows==local,"conservative shortcut policy preserves navigation UI and current listing");
 }
 std::vector<std::wstring> rows{L"allow",L"block",L"hr2",L"denied"};unsigned callbacks=0;
 const auto filtered=FilterShellItems(rows,[](const auto& row)->const std::wstring&{return row;},[&](const std::wstring& row)->HRESULT{++callbacks;return row==L"block"?S_FALSE:row==L"hr2"?HRESULT(2):row==L"denied"?E_ACCESSDENIED:S_OK;},[]{return true;},ShellFilterCandidateRole::EnumeratedView);
 check(filtered==S_OK&&callbacks==4&&rows==std::vector<std::wstring>{L"allow",L"hr2",L"denied"},"actual enumerated view preserves S_FALSE HR2 and E_ACCESSDENIED native verdicts");
 check(ShellFilterCandidateNeedsCallback(ShellFilterCandidateRole::EnumeratedView),"enumerated current-view candidates still require caller filter");
 std::printf("SHELL_FILTER_ROLES checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
