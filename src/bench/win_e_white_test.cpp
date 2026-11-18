#include "../app/app_internal.h"
#include "../app/open_routing.h"
#include "../app/window_factory.h"
#include "../app/tray_reveal.h"
#include <filesystem>
#include <cstdio>
#include <fstream>
#include "../ui/ui_renderer.h"
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int);

static bool running=false; static int failures=0;
static void Pump(unsigned ms) {const auto end=GetTickCount64()+ms;while(GetTickCount64()<end){MSG m{};while(GetTickCount64()<end&&PeekMessageW(&m,nullptr,0,0,PM_REMOVE)){TranslateMessage(&m);DispatchMessageW(&m);}Sleep(1);}}
static void CALLBACK Probe(HWND,UINT,UINT_PTR timer,DWORD){
 if(running)return;
 HWND first=nullptr;EnumThreadWindows(GetCurrentThreadId(),[](HWND h,LPARAM p)->BOOL{wchar_t c[128]{};GetClassNameW(h,c,128);if(wcscmp(c,pulse::app::SingleInstanceCoordinator::WindowClassName())==0){*reinterpret_cast<HWND*>(p)=h;return FALSE;}return TRUE;},reinterpret_cast<LPARAM>(&first));
 auto* s=first?reinterpret_cast<pulse::AppState*>(GetWindowLongPtrW(first,GWLP_USERDATA)):nullptr;
 if(!s||!s->startupComplete)return;running=true;KillTimer(nullptr,timer);
 s->appPrefs.open_win_e=0; s->appPrefs.new_window_shows=1;
 std::vector<HWND> opened;
 for(int i=0;i<12;++i){auto result=pulse::OpenExternalFolder(*s,L"pulse:win-e");auto* t=result.target;
  if(!t||t==s){printf("FAIL create %d\n",i);++failures;continue;}
  opened.push_back(t->hwnd);Pump(200);
  auto path=std::filesystem::absolute(L"bench_data/win_e_white_current")/(L"window-"+std::to_wstring(i)+L".png");
  bool saved=t->compositor.SaveSnapshot(path.c_str());printf("WINDOW %d saved=%u hwnd=%p recovery=%u first=%u\n",i,saved,t->hwnd,t->compositor.NeedsRecovery(),t->first_present_reported);fflush(stdout);failures+=!saved || !t->first_present_reported;
 }
 const auto folder=std::filesystem::absolute(L"bench_data/win_e_white_current/data/drag-fixture");
 std::filesystem::create_directories(folder);
 for(int i=0;i<200;++i)std::ofstream(folder/(std::to_wstring(i)+L".txt"))<<"fixture";
 pulse::StartFreshAt(*s,folder.wstring());ShowWindow(first,SW_SHOWNORMAL);Pump(1500);
 auto* tab=pulse::ActiveTab(*s); auto vm=pulse::BuildVm(*s,false);
 if(tab&&!vm.pane_slots.empty()) {
  const auto& slot=vm.pane_slots[0];const auto rect=s->renderer.PaneListRect(slot.pane,slot.rect);
  POINT bottom{LONG((rect.left+rect.right)/2),LONG(rect.bottom-2)};
  POINT top{bottom.x,LONG(rect.top+2)};
  POINT center{bottom.x,LONG((rect.top+rect.bottom)/2)};
  tab->scroll_y=0;bool moved=pulse::TickDropEdgeScroll(*s,bottom);float first_offset=tab->scroll_y;
  for(int i=0;i<5;++i)pulse::TickDropEdgeScroll(*s,bottom);
  bool continued=tab->scroll_y>first_offset;float previous=tab->scroll_y;
  bool idle=!pulse::TickDropEdgeScroll(*s,center)&&tab->scroll_y==previous;
  bool up=pulse::TickDropEdgeScroll(*s,top)&&tab->scroll_y<previous;
  for(int i=0;i<50;++i)pulse::TickDropEdgeScroll(*s,top);
  bool clamp=tab->scroll_y==0&&!pulse::TickDropEdgeScroll(*s,top);
  printf("DRAG moved=%u continued=%u center_idle=%u up=%u top_clamp=%u\n",moved,continued,idle,up,clamp);
  failures+=!(moved&&continued&&idle&&up&&clamp);
 } else {++failures;printf("FAIL drag fixture not loaded\n");}
 for(auto h:opened)DestroyWindow(h);DestroyWindow(first);PostQuitMessage(0);
}
int wmain(){setvbuf(stdout,nullptr,_IONBF,0);SetTimer(nullptr,0,500,Probe);wchar_t args[]=L"--test-instance";int code=wWinMain(GetModuleHandleW(nullptr),nullptr,args,SW_SHOWNORMAL);printf("RESULT failures=%d app=%d\n",failures,code);return failures?1:code;}





