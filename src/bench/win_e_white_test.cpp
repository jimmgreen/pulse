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
  // 滚动到最后一项之后: past the last row the blank room opens; leaving glides back.
  s->appPrefs.list_scroll_past_end=true;s->renderer.SetScrollPastEnd(true);
  pulse::ReturnDropScrolls(*s);while(pulse::TickDropScrollReturn(*s)&&s->dropScrollReturning)Sleep(2);
  tab->scroll_y=0;for(int i=0;i<4000&&pulse::TickDropEdgeScroll(*s,bottom);++i){}
  const float base=s->renderer.MaxScrollForPane(slot.pane,slot.rect),room=s->renderer.ScrollPastEndForPane(slot.pane,slot.rect);
  const float view=rect.bottom-rect.top;
  bool past=room>0.0f&&room<view&&fabsf(tab->scroll_y-(base+room))<0.5f;
  pulse::TickDropEdgeScroll(*s,center);bool kept=fabsf(tab->scroll_y-(base+room))<0.5f;
  pulse::ReturnDropScrolls(*s);for(int i=0;i<400&&s->dropScrollReturning;++i){pulse::TickDropScrollReturn(*s);Sleep(2);}
  bool back=tab->scroll_y==0.0f&&tab->drop_scroll_home<0.0f;
  s->renderer.SetScrollPastEnd(false);
  tab->scroll_y=0;for(int i=0;i<4000&&pulse::TickDropEdgeScroll(*s,bottom);++i){}
  bool off=fabsf(tab->scroll_y-base)<0.5f&&s->renderer.ScrollPastEndForPane(slot.pane,slot.rect)==0.0f;
  s->renderer.SetScrollPastEnd(true);
  printf("PAST_END past=%u kept=%u back=%u off=%u base=%.1f room=%.1f view=%.1f\n",past,kept,back,off,base,room,view);
  failures+=!(past&&kept&&back&&off);
  // 侧边栏底边: rows leave at the bottom edge cut, not whole. Scanning up from
  // the tray, the first row (or header) is at most a gap away at any scroll.
  {
   auto sv=pulse::BuildVm(*s,false);
   const float W=(float)s->compositor.Width(),H=(std::min)((float)s->compositor.Height(),460.0f*s->scale);
   const D2D1_RECT_F win=D2D1::RectF(0,0,W,H),sb=s->renderer.SidebarRect(W,H),tray=s->renderer.StagingTrayRect(sv,W,H);
   const float edge=tray.bottom>tray.top?tray.top:sb.bottom,most=s->renderer.SidebarMaxScroll(sv,W,H),x=(sb.left+sb.right)*0.5f;
   float worst=0.0f;int probes=0;
   for(float at=0.0f;most>0.0f&&at<=most;at+=2.0f*s->scale){
    sv.sidebar_scroll=at;++probes;float y=edge-1.0f;
    for(;y>sb.top;y-=1.0f){const auto h=s->renderer.HitTest(sv,win,x,y).region;
     if(h>=pulse::ui::HitTestResult::SidebarHeader&&h<=pulse::ui::HitTestResult::SidebarItemExpand)break;}
    worst=(std::max)(worst,edge-y);
   }
   printf("SIDEBAR probes=%d max_scroll=%.1f worst_blank=%.1f scale=%.2f\n",probes,most,worst,s->scale);
   failures+=probes>0&&worst>44.0f*s->scale;
  }
 } else {++failures;printf("FAIL drag fixture not loaded\n");}
 for(auto h:opened)DestroyWindow(h);DestroyWindow(first);PostQuitMessage(0);
}
int wmain(){setvbuf(stdout,nullptr,_IONBF,0);SetTimer(nullptr,0,500,Probe);wchar_t args[]=L"--test-instance";int code=wWinMain(GetModuleHandleW(nullptr),nullptr,args,SW_SHOWNORMAL);printf("RESULT failures=%d app=%d\n",failures,code);return failures?1:code;}





