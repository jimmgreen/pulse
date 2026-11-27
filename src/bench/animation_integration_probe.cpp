#include "../app/animation_integration_hooks.h"
#include "../app/app_internal.h"
#include "../app/app_navigation.h"
#include "../app/app_runtime.h"
#include "../app/tab_detach.h"
#include "../app/window_factory.h"
#include "../app/global_search_controller.h"
#include "../ui/ui_renderer.h"
#include "../ui/drag_drop.h"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

void Render(pulse::AppState&);
namespace pulse::ui {
void SetAnimationIntegrationTabPlateObserver(void (*)(const MainRenderer*, uint64_t, uint64_t, float, D2D1_RECT_F, D2D1_RECT_F));
}
namespace pulse {
DWORD PrepareAnimationIntegrationDesktop(int argc, wchar_t** argv) {
    wchar_t desktop_name[128]{};
    const DWORD length = GetEnvironmentVariableW(L"PULSE_TEST_ISOLATED_DESKTOP", desktop_name, ARRAYSIZE(desktop_name));
    if (!length) return 0;
    if (length >= ARRAYSIZE(desktop_name)) return ERROR_INVALID_NAME;
    wchar_t enabled[4]{};
    if (argc != 2 || !argv || wcscmp(argv[1], L"--test-instance") != 0 ||
        GetEnvironmentVariableW(L"PULSE_TEST_ANIMATION_INTEGRATION", enabled, ARRAYSIZE(enabled)) != 1 || enabled[0] != L'1' ||
        GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", nullptr, 0) <= 1 ||
        GetEnvironmentVariableW(L"PULSE_TEST_WINDOW_MANAGEMENT", nullptr, 0) <= 1) return ERROR_INVALID_PARAMETER;
    const std::wstring name(desktop_name);
    const std::wstring prefix = L"PulseAnimationCloseout-";
    if (name.size() <= prefix.size() || name.compare(0, prefix.size(), prefix) != 0 ||
        !std::all_of(name.begin() + prefix.size(), name.end(), [](wchar_t ch) { return ch >= L'0' && ch <= L'9'; })) return ERROR_INVALID_NAME;
    // DLL initialization uses the inherited desktop; no application windows/OLE
    // exist yet. Keep this non-input desktop alive until this test process exits.
    static HDESK test_desktop = nullptr;
    test_desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!test_desktop) return GetLastError();
    if (!SetThreadDesktop(test_desktop)) {
        const DWORD error = GetLastError(); CloseDesktop(test_desktop); test_desktop = nullptr; return error;
    }
    return 0;
}
namespace {
FILE* report = nullptr;
int failures = 0;
POINT virtual_cursor{};
AppState* drop_window = nullptr;
bool cancel_move = false;
unsigned move_loops = 0;
const ui::MainRenderer* observed_renderer=nullptr;
D2D1_RECT_F plate_current{},plate_target{};
uint64_t plate_frame=0,plate_now=0,plate_samples=0;
void ObservePlate(const ui::MainRenderer* renderer,uint64_t frame,uint64_t now,float,D2D1_RECT_F current,D2D1_RECT_F target) {
    if(renderer!=observed_renderer)return;
    plate_frame=frame;plate_now=now;plate_current=current;plate_target=target;++plate_samples;
}
void PlateReport(const char* phase,UINT dpi) {
    std::fprintf(report,"[TAB_PLATE] phase=%s dpi=%u frame=%llu now=%llu samples=%llu current=%.3f,%.3f target=%.3f,%.3f\n",phase,dpi,
        static_cast<unsigned long long>(plate_frame),static_cast<unsigned long long>(plate_now),static_cast<unsigned long long>(plate_samples),
        plate_current.left,plate_current.right,plate_target.left,plate_target.right);
}
LRESULT CALLBACK ObserveMruRoute(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto& state=*reinterpret_cast<AppState*>(data);
    const bool before=state.tabMru.IsOpen();
    const auto result=DefSubclassProc(hwnd,msg,wp,lp);
    if(report&&(before!=state.tabMru.IsOpen()||msg==WM_KEYDOWN||msg==WM_KEYUP||msg==WM_KILLFOCUS||msg==WM_ACTIVATE||msg==WM_CANCELMODE||msg==WM_SIZE)) {
        std::fprintf(report,"[MRU_ROUTE] msg=0x%x wp=%llu open_before=%d open_after=%d ctrl=%d focus=%p main=%p\n",msg,
            static_cast<unsigned long long>(wp),before,state.tabMru.IsOpen(),(GetKeyState(VK_CONTROL)&0x8000)!=0,
            static_cast<void*>(GetFocus()),static_cast<void*>(hwnd));std::fflush(report);
    }
    return result;
}
void Check(bool ok, const char* what) {
    std::fprintf(report, "[%s] %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(report); failures += !ok;
}
void Pump(DWORD ms) {
    const auto until = GetTickCount64() + ms;
    do {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message != WM_QUIT) { TranslateMessage(&message); DispatchMessageW(&message); }
        } else Sleep(1);
    } while (GetTickCount64() < until);
}
template<class F> bool Until(F condition, DWORD ms=10000) {
    const auto until=GetTickCount64()+ms;
    while (!condition() && GetTickCount64()<until) Pump(10);
    return condition();
}
struct Keys {
    BYTE saved[256]{};
    Keys() { GetKeyboardState(saved); }
    ~Keys() { SetKeyboardState(saved); }
    void Set(bool ctrl=false, bool shift=false, bool button=false, bool alt=false) {
        BYTE keys[256]{};
        if (ctrl) keys[VK_CONTROL]=keys[VK_LCONTROL]=0x80;
        if (shift) keys[VK_SHIFT]=keys[VK_LSHIFT]=0x80;
        if (button) keys[VK_LBUTTON]=0x80;
        if (alt) keys[VK_MENU]=keys[VK_LMENU]=0x80;
        Check(SetKeyboardState(keys)!=FALSE,"test-thread keyboard state set without SendInput");
    }
};
POINT TabPoint(AppState& s, int index) {
    D2D1_RECT_F rect{};
    const auto vm=BuildVm(s,false);
    Check(s.renderer.TabItemRect(vm,static_cast<float>(s.compositor.Width()),index,&rect),"production tab rectangle available");
    return POINT{static_cast<LONG>(rect.left+20*s.scale),static_cast<LONG>((rect.top+rect.bottom)*.5f)};
}
void Mouse(AppState& s, UINT msg, POINT p, WPARAM keys=0) {
    SendMessageW(s.hwnd,msg,keys,MAKELPARAM(p.x,p.y));
}
void Capture(AppState& s, const std::filesystem::path& file) {
    s.compositor.SetFrameCopyEnabled(true);
    Render(s);
    Check(s.compositor.SaveSnapshot(file.c_str()),"actual submitted main-window frame saved");
}
bool MoveLoop(AppState& moving, HWND from) {
    ++move_loops;
    Check(from!=nullptr && moving.window_tabs.items.size()==1,"mouse detach created one-tab real production window");
    ShowWindow(moving.hwnd,SW_SHOWNOACTIVATE);
    if (drop_window) {
        auto p=TabPoint(*drop_window,0); ClientToScreen(drop_window->hwnd,&p); virtual_cursor=p;
        SetWindowPos(moving.hwnd,HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    }
    RECT rect{}; GetWindowRect(moving.hwnd,&rect);
    SendMessageW(moving.hwnd,WM_MOVING,WMSZ_LEFT,reinterpret_cast<LPARAM>(&rect));
    if (drop_window && !cancel_move)
        Check(drop_window->tabDropIndex>=0,"WM_MOVING production hit testing arms peer tab-strip drop");
    return cancel_move;
}
LRESULT CALLBACK ObserveDirectoryResult(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    const auto result=DefSubclassProc(hwnd,message,wp,lp);
    // This is the existing production SearchDirectory worker completion message.
    if(message==WM_APP+188)++*reinterpret_cast<unsigned*>(data);
    return result;
}
void SearchGridFixture(AppState& s,const std::filesystem::path& folder,const std::filesystem::path& out) {
    s.globalSearchWindow.Hide();Pump(50);
    Check(ShowGlobalSearchWithQuery(s,folder.filename().wstring()),"current controller reopens real known fixture-folder query");
    HWND search=FindWindowW(L"Pulse.GlobalSearch",nullptr);
    HWND edit=search?FindWindowExW(search,nullptr,L"EDIT",nullptr):nullptr;
    Check(search&&edit&&IsWindowVisible(search),"actual search and query editor visible for filesystem grid probe");
    if(!search||!edit)return;
    unsigned results=0;
    Check(SetWindowSubclass(search,ObserveDirectoryResult,0xa12,reinterpret_cast<DWORD_PTR>(&results))!=FALSE,"observe actual directory-worker completion without replacing results");
    Keys keys;keys.Set();
    SendMessageW(edit,WM_KEYDOWN,VK_UP,1);
    SendMessageW(edit,WM_KEYDOWN,VK_RIGHT,1);
    wchar_t query[128]{};GetWindowTextW(edit,query,ARRAYSIZE(query));
    Check(query[0]==0,"real right-arrow route enters the selected fixture directory and clears query");
    Check(Until([&]{return results>0;},5000),"production directory enumeration posts actual filesystem results");
    const auto before=results;
    SendMessageW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"readable-anchor"));
    Check(Until([&]{return results>before;},5000),"production directory query completes for real readable-anchor file");
    GetWindowTextW(edit,query,ARRAYSIZE(query));Check(wcscmp(query,L"readable-anchor")==0,"actual query retains matching filename term");
    UpdateWindow(search);
    // Derive hit positions from the current child/client geometry and the existing
    // DrawResultBar DIP layout, rather than setting Impl.grid_view/marked_paths.
    RECT client{},editor{};GetClientRect(search,&client);GetWindowRect(edit,&editor);
    POINT top_left{editor.left,editor.top},bottom_right{editor.right,editor.bottom};ScreenToClient(search,&top_left);ScreenToClient(search,&bottom_right);
    const float scale=s.scale,width=static_cast<float>(client.right)/scale;
    const float grow=std::max(0.0f,(static_cast<float>(top_left.y+bottom_right.y)/scale)-72.0f);
    const float header=72.0f+grow;
    const LONG view_x=static_cast<LONG>((width-49.0f)*scale),view_y=static_cast<LONG>((header+22.0f)*scale);
    SendMessageW(search,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(view_x,view_y));SendMessageW(search,WM_LBUTTONUP,0,MAKELPARAM(view_x,view_y));UpdateWindow(search);
    const float left=width>=700.0f?217.0f:0.0f;
    const LONG tile_x=static_cast<LONG>((left+45.0f)*scale),tile_y=static_cast<LONG>((header+43.0f+45.0f)*scale);
    keys.Set(true,false,true);
    SendMessageW(search,WM_LBUTTONDOWN,MK_CONTROL|MK_LBUTTON,MAKELPARAM(tile_x,tile_y));
    keys.Set(true);SendMessageW(search,WM_LBUTTONUP,MK_CONTROL,MAKELPARAM(tile_x,tile_y));keys.Set();UpdateWindow(search);
    std::fprintf(report,"[GRID_INPUT] thumbnails=%ld,%ld ctrl_tile=%ld,%ld worker_results=%u\n",view_x,view_y,tile_x,tile_y,results);
    Check(s.globalSearchWindow.SavePresentedSnapshot((out/L"current-search-grid-marked-first.png").wstring()),"first actual submitted marked-grid frame captured read-only");
    Pump(200);
    Check(s.globalSearchWindow.SavePresentedSnapshot((out/L"current-search-grid-marked-settled.png").wstring()),"settled marked-grid frame captured separately");
    // The real selection's stage callback provides a path oracle without a new
    // production inspection API. Only this test instance's fixture shelf changes.
    keys.Set(false,false,false,true);SendMessageW(edit,WM_SYSKEYDOWN,'A',1);keys.Set();
    const auto expected=(folder/L"readable-anchor.txt").wstring();bool staged=false;
    for(const auto& batch:s.core.tray.batches())for(const auto& item:batch.items)if(_wcsicmp(item.path.c_str(),expected.c_str())==0)staged=true;
    Check(staged,"real selected filesystem result stages the exact fixture path through production callback");
    std::fprintf(report,"[VISUAL_REQUIRED] marked-grid first/settled: actual readable-anchor match, selected tile and checkmark must be visible/readable\n");
    RemoveWindowSubclass(search,ObserveDirectoryResult,0xa12);
}
void DragOff(AppState& s, Keys& keys) {
    const auto start=TabPoint(s,static_cast<int>(s.window_tabs.active));
    keys.Set(false,false,true); Mouse(s,WM_LBUTTONDOWN,start,MK_LBUTTON);
    auto end=start; end.y+=static_cast<LONG>(100*s.scale);
    virtual_cursor=end; ClientToScreen(s.hwnd,&virtual_cursor);
    Mouse(s,WM_MOUSEMOVE,end,MK_LBUTTON); Pump(350);
    keys.Set();
}
}

int RunAnimationIntegrationProbe(AppState& s, const std::wstring& report_path) {
    if (!s.isolatedTest || !GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",nullptr,0)) return 2;
    if (_wfopen_s(&report,report_path.c_str(),L"wb")!=0 || !report) return 2;
    failures=0;move_loops=0;
    wchar_t desktop_name[128]{}, expected_desktop[128]{};DWORD desktop_bytes=0;
    const bool desktop_read=GetUserObjectInformationW(GetThreadDesktop(GetCurrentThreadId()),UOI_NAME,desktop_name,sizeof(desktop_name),&desktop_bytes)!=FALSE;
    GetEnvironmentVariableW(L"PULSE_TEST_ISOLATED_DESKTOP",expected_desktop,ARRAYSIZE(expected_desktop));
    std::fprintf(report,"[DESKTOP] actual=%ls expected=%ls switched_input_desktop=0\n",desktop_name,expected_desktop);
    Check(desktop_read&&expected_desktop[0]&&wcscmp(desktop_name,expected_desktop)==0,"actual production UI thread remains on explicit non-input fixture desktop");
    wchar_t data[32768]{}; GetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR",data,ARRAYSIZE(data));
    const std::filesystem::path out(data), a=out/L"folder-A", b=out/L"folder-B", c=out/L"folder-C", drop=out/L"drop-target";
    for (const auto& dir : {a,b,c,drop}) {
        std::filesystem::create_directories(dir);
        std::ofstream(dir/L"readable-anchor.txt")<<"integration fixture\n";
    }
    s.appPrefs.persist=false; s.places.persist=false; s.searchHistory.persist=false;
    s.appPrefs.vertical_tabs=false;
    ShowWindow(s.hwnd,SW_SHOWNOACTIVATE); ShowWindow(s.hwnd,SW_SHOWNOACTIVATE);
    SetWindowPos(s.hwnd,nullptr,100,100,1500,1000,SWP_NOZORDER|SWP_NOACTIVATE);
    Check(IsWindowVisible(s.hwnd)&&!IsIconic(s.hwnd),"actual --test-instance main window is visible on isolated desktop");
    using GetWindowDpi = UINT (WINAPI*)(HWND);
    const auto get_dpi=reinterpret_cast<GetWindowDpi>(GetProcAddress(GetModuleHandleW(L"user32.dll"),"GetDpiForWindow"));
    const UINT actual_dpi=get_dpi?get_dpi(s.hwnd):static_cast<UINT>(s.scale*96.0f);
    std::fprintf(report,"[CAPABILITY] actual_window_dpi=%u renderer_scale=%.3f\n",actual_dpi,s.scale);
    Check(std::abs(s.scale-static_cast<float>(actual_dpi)/96.0f)<.01f,"real window DPI matches production renderer scale");
    NavigateTo(s,a.wstring());
    Check(Until([&]{return ActiveTab(s)&&!ActiveTab(s)->loading&&ActiveTab(s)->EntryCount()>0;}),"actual fixture directory finishes loading");
    wchar_t phase[80]{};
    if (GetEnvironmentVariableW(L"PULSE_TEST_ANIMATION_PHASE",phase,ARRAYSIZE(phase))) {
        BOOL animations=TRUE;HIGHCONTRASTW hc{sizeof(hc)};
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
        SystemParametersInfoW(SPI_GETHIGHCONTRAST,sizeof(hc),&hc,0);
        std::fprintf(report,"[SYSTEM_PHASE] phase=%ls client_animation=%d high_contrast=%d actual_dpi=%u\n",phase,animations!=FALSE,(hc.dwFlags&HCF_HIGHCONTRASTON)!=0,actual_dpi);
        if(wcscmp(phase,L"animations-off")==0)Check(!animations,"system animation-off setting observed by current production process");
        if(wcscmp(phase,L"high-contrast")==0)Check((hc.dwFlags&HCF_HIGHCONTRASTON)!=0,"system high-contrast setting observed by current production process");
        Capture(s,out/L"system-main.png");
        ui::Compositor::SetFrameCopyEnabled(true);
        Check(ShowGlobalSearchWithQuery(s,L"2+2"),"current production search controller shows real popup");
        HWND search=nullptr;
        Check(Until([&]{search=FindWindowW(L"Pulse.GlobalSearch",nullptr);return search&&IsWindowVisible(search)&&s.globalSearchWindow.Visible();},5000),"current search HWND visible on isolated desktop");
        if(search) {
            DWORD process=0;GetWindowThreadProcessId(search,&process);
            Check(process==GetCurrentProcessId(),"search popup belongs to this current merged application instance");
            HWND edit=FindWindowExW(search,nullptr,L"EDIT",nullptr);
            Check(edit!=nullptr,"search has real query input editor");
            if(edit){SendMessageW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L"3+4"));wchar_t value[32]{};GetWindowTextW(edit,value,ARRAYSIZE(value));Check(wcscmp(value,L"3+4")==0,"real query editor accepts and retains input");}
            Pump(300);
            Check(s.globalSearchWindow.SavePresentedSnapshot((out/L"system-search.png").wstring()),"read-only capture saves actual submitted current search frame");
            if(edit) {
                SendMessageW(edit,WM_SETTEXT,0,reinterpret_cast<LPARAM>(L">"));
                wchar_t selected_query[8]{};GetWindowTextW(edit,selected_query,ARRAYSIZE(selected_query));
                Check(wcscmp(selected_query,L">")==0,"real query editor enters deterministic production command mode");
                SendMessageW(edit,WM_KEYDOWN,VK_DOWN,1);UpdateWindow(search);
                Check(IsWindowVisible(search)&&s.globalSearchWindow.Visible(),"real command results remain visible after selection key");
                Check(s.globalSearchWindow.SavePresentedSnapshot((out/L"current-search-selected.png").wstring()),"actual submitted command selection frame saved for contrast inspection");
                std::fprintf(report,"[VISUAL_REQUIRED] current-search-selected.png: first submitted command-selection frame must have visible readable high-contrast foreground\n");
                Pump(200);
                Check(s.globalSearchWindow.SavePresentedSnapshot((out/L"current-search-selected-settled.png").wstring()),"settled command selection frame retained separately from first-frame capture");
            }
            SearchGridFixture(s,a,out);
            s.globalSearchWindow.Hide();Pump(50);
            Check(!s.globalSearchWindow.Visible()&&!IsWindowVisible(search),"production search Hide completes under system setting");
        }
        std::fprintf(report,"[RESULT] integration_failures=%d\n",failures);std::fclose(report);report=nullptr;return failures?1:0;
    }
    const auto index_a=s.window_tabs.active; const auto* tab_a=s.window_tabs.Active();
    OpenTabAt(s,b.wstring()); const auto index_b=s.window_tabs.active; const auto* tab_b=s.window_tabs.Active();
    OpenTabAt(s,c.wstring()); const auto index_c=s.window_tabs.active;
    Pump(500);
    // Deliberately no render/pump between real activations: history is an input
    // fact, not a sampling side effect of how many frames happen to be drawn.
    SwitchTab(s,index_a);Check(s.window_tabs.Active()==tab_a,"rapid activation A reaches actual tab identity");
    SwitchTab(s,index_b);Check(s.window_tabs.Active()==tab_b,"rapid activation B reaches actual tab identity");
    SwitchTab(s,index_c);Check(s.window_tabs.active==index_c,"rapid activation C reaches actual tab index");
    std::fprintf(report,"[MRU_HISTORY] active=%p A=%p B=%p C=%p order=",static_cast<const void*>(s.window_tabs.Active()),
        static_cast<const void*>(tab_a),static_cast<const void*>(tab_b),static_cast<const void*>(s.window_tabs.items[index_c].get()));
    for(const auto* item:s.tabMru.order)std::fprintf(report,"%p,",static_cast<const void*>(item));
    std::fprintf(report,"\n");
    Pump(500);
    observed_renderer=&s.renderer;ui::SetAnimationIntegrationTabPlateObserver(ObservePlate);
    Keys keys;
    // WM_KEYDOWN is delivered to the focused production window in real use.
    // The isolated no-activate setup has no focused window until established.
    SetFocus(s.hwnd);Pump(50);
    Check(GetFocus()==s.hwnd,"production main window owns test-thread focus before keyboard shortcuts");
    Check(SetWindowSubclass(s.hwnd,ObserveMruRoute,0xa11,reinterpret_cast<DWORD_PTR>(&s))!=FALSE,"read-only production MRU message observer installed");
    std::fprintf(report,"[MRU_PRECONDITION] focus=%p main=%p ctrl=%d\n",static_cast<void*>(GetFocus()),static_cast<void*>(s.hwnd),(GetKeyState(VK_CONTROL)&0x8000)!=0);
    // The production WndProc, not TabMruView::Show, owns the shortcut and release.
    keys.Set(true); SendMessageW(s.hwnd,WM_KEYDOWN,VK_TAB,1); Pump(40);
    Check(s.tabMru.IsOpen(),"WM_KEYDOWN Ctrl+Tab opens actual MRU overlay");
    Check(s.tabMru.IsOpen()&&s.tabMru.shown[static_cast<size_t>(s.tabMru.selected)]==tab_b,"MRU begins at previously active tab B");
    Capture(s,out/L"mru-actual-dpi.png");
    keys.Set(); SendMessageW(s.hwnd,WM_KEYUP,VK_CONTROL,0); Pump(100);
    Check(!s.tabMru.IsOpen()&&s.window_tabs.Active()==tab_b,"WM_KEYUP Ctrl commits MRU selection through production route");
    keys.Set(true); SendMessageW(s.hwnd,WM_KEYDOWN,VK_TAB,1);
    SendMessageW(s.hwnd,WM_KEYDOWN,VK_ESCAPE,1);keys.Set();Pump(50);
    Check(!s.tabMru.IsOpen()&&s.window_tabs.Active()==tab_b,"Escape cancels MRU without changing active tab");
    keys.Set(true,true); SendMessageW(s.hwnd,WM_KEYDOWN,VK_TAB,1);
    Check(s.tabMru.IsOpen(),"Ctrl+Shift+Tab reverse route opens MRU");
    SendMessageW(s.hwnd,WM_KEYDOWN,VK_ESCAPE,1);keys.Set();Pump(200);
    RemoveWindowSubclass(s.hwnd,ObserveMruRoute,0xa11);
    // Native pointer-message reorder, preserving the identity of active content.
    auto from=TabPoint(s,static_cast<int>(index_a)); auto to=TabPoint(s,static_cast<int>(s.window_tabs.items.size()-1));
    to.x+=static_cast<LONG>(90*s.scale);
    keys.Set(false,false,true);Mouse(s,WM_LBUTTONDOWN,from,MK_LBUTTON);
    for(int step=1;step<=8;++step) { POINT p{from.x+(to.x-from.x)*step/8,from.y};Mouse(s,WM_MOUSEMOVE,p,MK_LBUTTON);Pump(15); }
    const bool dragging=s.tabDragging;
    keys.Set();Mouse(s,WM_LBUTTONUP,to);Pump(400);
    Check(dragging&&!s.tabDragging&&!s.tabDragPending,"mouse press/move/release completes production tab-reorder gesture");
    Check(s.window_tabs.items.front().get()!=tab_a&&s.window_tabs.Active()==tab_a,"tab reorder moves A and preserves its active content identity");
    Capture(s,out/L"tabs-reordered.png");
    WindowInit init;init.path=drop.wstring();init.rect=RECT{1700,100,3200,1100};
    const HWND peer_hwnd=OpenPulseWindow(s,init);auto* peer=s.core.windows.Lookup(peer_hwnd);
    Check(peer!=nullptr,"production window factory creates isolated peer");
    if(peer) {
        ShowWindow(peer_hwnd,SW_SHOWNOACTIVATE);ShowWindow(peer_hwnd,SW_SHOWNOACTIVATE);Pump(400);
        keys.Set(true);
        SendMessageW(s.hwnd,WM_KEYDOWN,VK_OEM_3,static_cast<LPARAM>((0x29u<<16)|1u));Pump(40);
        Check(s.switcher.IsOpen(),"Ctrl+physical-grave scan-code enters production window-MRU route");
        Capture(s,out/L"window-mru-actual-dpi.png");
        SendMessageW(s.hwnd,WM_KEYDOWN,VK_ESCAPE,1);keys.Set();
        Check(!s.switcher.IsOpen(),"window-MRU Escape route closes overlay without merging windows");
        const AnimationIntegrationDragInput input{[] {return virtual_cursor;},[] {return true;},MoveLoop};
        Check(SetAnimationIntegrationDragInput(s,&input),"isolated-only OS move-loop adapter activated");
        const auto windows_before=s.core.windows.Count(), source_tabs=s.window_tabs.items.size(), peer_tabs=peer->window_tabs.items.size();
        const auto* moved=s.window_tabs.Active();drop_window=peer;cancel_move=false;
        DragOff(s,keys);
        Check(move_loops==1,"mouse route reached production detached-window modal boundary once");
        Check(s.core.windows.Count()==windows_before&&s.window_tabs.items.size()+1==source_tabs&&peer->window_tabs.items.size()==peer_tabs+1,"drag-out and drop merges the live tab and closes temporary window");
        Check(peer->window_tabs.Active()==moved,"dropped tab preserves live layout identity");
        Capture(*peer,out/L"tab-dropped-peer.png");
        // Repeat with Escape: source tab identity and count must be restored.
        const auto cancel_count=s.window_tabs.items.size();const auto* cancelled=s.window_tabs.Active();
        drop_window=nullptr;cancel_move=true;DragOff(s,keys);
        Check(move_loops==2&&s.core.windows.Count()==windows_before&&s.window_tabs.items.size()==cancel_count&&
              std::any_of(s.window_tabs.items.begin(),s.window_tabs.items.end(),[&](const auto& p){return p.get()==cancelled;}),"cancelled tear-out returns original live tab and destroys temporary window");
        SetAnimationIntegrationDragInput(s,nullptr);drop_window=nullptr;cancel_move=false;
        // Real OLE data payload -> registered production DropTarget -> async ops -> filesystem.
        NavigateTo(*peer,drop.wstring());Check(Until([&]{return ActiveTab(*peer)&&!ActiveTab(*peer)->loading;}),"drop target loads through production navigation");
        const auto source_file=a/L"drag-copy.txt";std::ofstream(source_file)<<"verified actual drop bytes\n";
        auto* object=ui::FileDataObject::Create({source_file.wstring()},DROPEFFECT_COPY);
        const auto bounds=FocusedPaneRect(*peer);POINT point{static_cast<LONG>(bounds.left+200*peer->scale),static_cast<LONG>(bounds.bottom-100*peer->scale)};
        ClientToScreen(peer_hwnd,&point);POINTL location{point.x,point.y};DWORD effect=DROPEFFECT_COPY;
        Check(object&&peer->dropTarget,"production file IDataObject and main-window drop target exist");
        if(object&&peer->dropTarget) {
            const HRESULT entered=peer->dropTarget->DragEnter(object,MK_CONTROL,location,&effect);
            Check(SUCCEEDED(entered)&&effect==DROPEFFECT_COPY,"DragEnter routes to actual copy destination");
            effect=DROPEFFECT_COPY;peer->dropTarget->DragOver(MK_CONTROL,location,&effect);
            const HRESULT dropped=peer->dropTarget->Drop(object,MK_CONTROL,location,&effect);
            Check(SUCCEEDED(dropped)&&effect==DROPEFFECT_COPY,"OLE Drop dispatches real fixture copy operation");
            Check(Until([&]{return std::filesystem::exists(drop/L"drag-copy.txt");},15000),"drop operation creates destination file");
            std::ifstream copied(drop/L"drag-copy.txt");std::string line;std::getline(copied,line);
            Check(line=="verified actual drop bytes"&&std::filesystem::exists(source_file),"copy bytes verified and source remains intact");
            object->Release();Pump(500);Capture(*peer,out/L"file-drop-completed.png");
        } else if(object) object->Release();
    }
    // Real hardware DPI above; these messages are explicitly synthetic boundaries,
    // not a claim that this single-monitor machine performed a cross-monitor move.
    for (UINT dpi : {96u,192u,actual_dpi}) {
        RECT rect{100,100,1600,1100};
        SendMessageW(s.hwnd,WM_DPICHANGED,MAKELONG(dpi,dpi),reinterpret_cast<LPARAM>(&rect));
        Capture(s,out/(L"dpi-first-frame-"+std::to_wstring(dpi)+L".png"));PlateReport("dpi-first-frame",dpi);
        Check(std::fabs(plate_current.left-plate_target.left)<0.5f&&std::fabs(plate_current.right-plate_target.right)<0.5f,
            "DPI rebuild first submitted frame has active plate at current target geometry");
        Pump(250);
        Check(std::abs(s.scale-static_cast<float>(dpi)/96.0f)<.01f,"synthetic WM_DPICHANGED updates production scale");
        const auto target=(s.window_tabs.active+1)%s.window_tabs.items.size();
        const auto* expected=s.window_tabs.items[target].get();
        const auto p=TabPoint(s,static_cast<int>(target));
        Mouse(s,WM_LBUTTONDOWN,p);Mouse(s,WM_LBUTTONUP,p);Pump(100);
        Check(s.window_tabs.Active()==expected&&!s.tabDragPending&&!s.tabDragging,"other-tab hit and gesture cleanup remain valid at DPI boundary");
        std::fprintf(report,"[DPI_BOUNDARY] synthetic_dpi=%u actual_monitor_dpi=%u\n",dpi,actual_dpi);
        Capture(s,out/(L"synthetic-dpi-"+std::to_wstring(dpi)+L".png"));PlateReport("first-capture",dpi);
        const auto samples_before=plate_samples;
        Pump(1300); // Beyond SpringRectMotion's 1200ms hard wall-clock cutoff.
        Capture(s,out/(L"synthetic-dpi-"+std::to_wstring(dpi)+L"-settled.png"));PlateReport("past-deadline",dpi);
        Check(plate_samples>samples_before&&std::fabs(plate_current.left-plate_target.left)<0.5f&&std::fabs(plate_current.right-plate_target.right)<0.5f,
            "actual submitted active-tab plate reaches target after hard wall-clock deadline");
    }
    ui::SetAnimationIntegrationTabPlateObserver(nullptr);observed_renderer=nullptr;
    keys.Set();SetAnimationIntegrationDragInput(s,nullptr);
    if(peer_hwnd&&IsWindow(peer_hwnd))DestroyWindow(peer_hwnd);
    Pump(100);
    std::fprintf(report,"[RESULT] integration_failures=%d\n",failures);std::fclose(report);report=nullptr;
    return failures?1:0;
}
}
