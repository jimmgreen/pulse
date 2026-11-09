#include "../ui/fluent_menu.h"
namespace pulse::ui {
struct FluentMenuTestPeer {
    static HWND Window(FluentMenu& menu) { return menu.hwnd_; }
};
struct TitleLayoutTestAccess {
    static bool ContentIdle(MainRenderer& r) {
        for (const auto& n : r.nav_enter_) if (n.start || n.pending) return false;
        for (const auto& m : r.view_morph_) if (m.Running()) return false;
        return !r.pane_zoom_motion_.start;
    }
    static auto Geometry(MainRenderer& renderer,const WindowViewModel& vm,float width) { return renderer.ComputeTabStrip(vm,width); }
};
}
namespace pulse::ui {
bool SidebarSelectionMovingForTest(const MainRenderer*);
D2D1_RECT_F SidebarHoverPositionForTest(const MainRenderer*);
}
int RunConcurrentMotionProbe(pulse::AppState& s) {
    using namespace pulse;
    s.scale=1;s.renderer.SetScale(1);s.compositor.RecreateTextFormats(1);s.compositor.Resize(1440,900);
    auto* tab=ActiveTab(s);tab->current_path=L"C:\\concurrent-motion";tab->loading=false;tab->view_mode=ui::ViewMode::Details;
    auto rows=std::make_shared<std::vector<fs::DirEntry>>();
    for(int i=0;i<1000;++i){fs::DirEntry e;e.name=L"Quarterly financial report - review document "+std::to_wstring(i)+L".txt";e.full_path=tab->current_path+L"\\"+e.name;e.size=123456+i;e.attrs=FILE_ATTRIBUTE_NORMAL;rows->push_back(e);}
    tab->SetSnapshot(rows);auto vm=BuildVm(s,false);
    vm.status.task_active=true;vm.status.task_progress=-1;vm.status.task_text=L"Copying project documents";
    ui::HoverPreviewView preview;preview.visible=true;preview.folder=true;preview.name=L"Preview loading";preview.path=L"C:\\concurrent-motion\\preview";preview.pointer={850,400};preview.anchor={250,350,1100,384};preview.shown_ms=GetTickCount64();preview.listing=std::make_shared<ui::HoverFolderInfo>();
    bool ok=true;
    auto* fmt=s.compositor.SmallFormat();auto* factory=s.compositor.DwriteFactory();
    auto* cached=ui::CachedEllipsisLayout(factory,fmt,L"Repeated label",180,24);
    const auto builds=ui::EllipsisLayouts().builds;
    for(int i=0;i<100;++i)ok=ui::CachedEllipsisLayout(factory,fmt,L"Repeated label",180,24)==cached&&ok;
    ok=ui::EllipsisLayouts().builds==builds&&ok;
    auto* narrow=ui::CachedEllipsisLayout(factory,fmt,L"Repeated label",40,24);
    ok=narrow&&narrow!=cached&&ok;
    const auto alignment=fmt->GetTextAlignment();fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    auto* right=ui::CachedEllipsisLayout(factory,fmt,L"Repeated label",180,24);
    ok=right&&right!=cached&&right->GetTextAlignment()==DWRITE_TEXT_ALIGNMENT_TRAILING&&ok;fmt->SetTextAlignment(alignment);
    ui::typography::InvalidateMeasurements();const auto before_generation=ui::EllipsisLayouts().builds;
    ok=ui::CachedEllipsisLayout(factory,fmt,L"Repeated label",180,24)&&ui::EllipsisLayouts().builds==before_generation+1&&ok;
    for(int i=0;i<600;++i)ui::CachedEllipsisLayout(factory,fmt,L"Bounded "+std::to_wstring(i),100,24);
    ok=ui::EllipsisLayouts().entries.size()<=512&&ok;
    std::cout<<(ok?"[PASS] ":"[FAIL] ")<<"layout cache reuses unchanged text, separates width/alignment, invalidates typography, and stays bounded"<<std::endl;
    for(int scenario=0;scenario<4;++scenario) {
    vm.status.task_active=scenario!=1;preview.visible=scenario!=0;
    std::vector<double> total,list,other;
    for(int frame=0;frame<240;++frame) {
        vm.pane.hover_index=(frame/2)%16;for(auto& slot:vm.pane_slots)slot.pane.hover_index=(frame/2)%16;vm.hover_pane_index=0;
        if(scenario==3&&frame%24==0) {vm.pane.path=L"C:\\motion-"+std::to_wstring(frame);for(auto& slot:vm.pane_slots)slot.pane.path=vm.pane.path;}
        const auto start=std::chrono::steady_clock::now();s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm,{0,0,1440,900},ui::MakeTheme(true,ui::HexColor(0x6474C7)));
        s.renderer.DrawHoverPreview(preview,{0,0,1440,900},ui::MakeTheme(true,ui::HexColor(0x6474C7)),false);
        ok=SUCCEEDED(s.compositor.Dc()->EndDraw())&&ok;
        if(frame>=40){total.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count());list.push_back(s.renderer.LastFramePerf().list_us/1000.0);other.push_back(s.renderer.LastFramePerf().render_us/1000.0);}
        Sleep(4);
    }
    for(auto* values:{&total,&list,&other})std::sort(values->begin(),values->end());
    std::cout<<"CONCURRENT scenario="<<scenario<<" total_median="<<total[100]<<" total_p95="<<total[190]<<" total_max="<<total.back()<<" list_median="<<list[100]<<" render_median="<<other[100]<<std::endl;
    }
    ok=s.compositor.SaveSnapshot(L"../bench_data/concurrent_motion/concurrent.png")&&ok;
    std::cout<<"CONCURRENT_RESULT failures="<<!ok<<std::endl;return ok?0:1;
}
int RunSidebarSwitchProbe(pulse::AppState& s) {
    using namespace pulse;
    s.scale=1;s.renderer.SetScale(1);s.compositor.RecreateTextFormats(1);s.compositor.Resize(1100,720);
    auto vm=BuildVm(s,false);vm.pane_slots.clear();vm.tabs.clear();vm.sidebar.clear();
    ui::SidebarGroup group;group.header=L"Sidebar motion";
    for(int i=0;i<5;++i){ui::SidebarItem item;item.label=L"Folder "+std::to_wstring(i);item.path=L"C:\\sidebar-"+std::to_wstring(i);item.icon_glyph=L"\xE8B7";group.items.push_back(item);}
    vm.sidebar.push_back(group);vm.pane.path=group.items[0].path;vm.pane.loading=false;
    ui::TabView tab;tab.active=true;tab.id=1;tab.title=L"A";vm.tabs.push_back(tab);
    const auto bounds=D2D1::RectF(0,0,1100,720);
    bool drawn=true;std::vector<double> times;
    auto frame=[&]{auto begin=std::chrono::steady_clock::now();s.compositor.Dc()->BeginDraw();s.renderer.Render(vm,bounds,ui::MakeTheme(true,ui::HexColor(0x6474C7)));drawn=SUCCEEDED(s.compositor.Dc()->EndDraw())&&drawn;times.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());};
    frame();vm.hover_region=ui::HitTestResult::SidebarItem;vm.hover_control_index=2;
    for(int i=0;i<12;++i){frame();Sleep(16);}
    bool no_selection_glide=true,hover_preserved=true;
    for(int i=0;i<40;++i){
        const auto before=ui::SidebarHoverPositionForTest(&s.renderer);
        vm.tabs[0].id=2+i;vm.pane.path=group.items[i%2].path;frame();
        if(i>0) no_selection_glide &= ui::SidebarSelectionMovingForTest(&s.renderer);
        const auto after=ui::SidebarHoverPositionForTest(&s.renderer);
        hover_preserved &= std::fabs(before.top-after.top)<.5f;
        Sleep(8);
    }
    vm.hover_control_index=3;frame();const auto start=ui::SidebarHoverPositionForTest(&s.renderer);
    Sleep(32);frame();const auto mid=ui::SidebarHoverPositionForTest(&s.renderer);
    for(int i=0;i<30;++i){Sleep(16);frame();}
    const auto end=ui::SidebarHoverPositionForTest(&s.renderer);
    const bool glides=start.top<mid.top&&mid.top<end.top;
    const auto output=std::filesystem::absolute(L"../bench_data/sidebar_switch/hover.png");
    const bool screenshot=s.compositor.SaveSnapshot(output.c_str());
    const bool ok=drawn&&no_selection_glide&&hover_preserved&&glides&&screenshot;
    std::cout<<(no_selection_glide?"[PASS] ":"[FAIL] ")<<"tab switches retain an active continuously retargeted selection transition"<<std::endl;
    std::cout<<(hover_preserved?"[PASS] ":"[FAIL] ")<<"stationary hover survives 40 tab switches"<<std::endl;
    std::cout<<(glides?"[PASS] ":"[FAIL] ")<<"hover continues through intermediate positions after switching"<<std::endl;
    std::sort(times.begin(),times.end());std::cout<<"SIDEBAR_RENDER_MS median="<<times[times.size()/2]<<" p95="<<times[times.size()*95/100]<<std::endl;
    std::cout<<"SIDEBAR_SWITCH_RESULT failures="<<!ok<<std::endl;return ok?0:1;
}
int RunTitleLayoutProbe(pulse::AppState& s) {
    if(GetEnvironmentVariableW(L"PULSE_TEST_CONCURRENT_MOTION",nullptr,0))return RunConcurrentMotionProbe(s);
    if(GetEnvironmentVariableW(L"PULSE_TEST_SIDEBAR_SWITCH",nullptr,0))return RunSidebarSwitchProbe(s);
    using namespace pulse;
    const auto output=std::filesystem::absolute(L"../bench_data/title_layout_exact").lexically_normal();
    std::filesystem::create_directories(output);
    int failed=0;auto check=[&](bool ok,const char* label){std::cout<<(ok?"[PASS] ":"[FAIL] ")<<label<<std::endl;failed+=!ok;};
    s.appPrefs.persist=false;s.scale=1;s.renderer.SetScale(1);s.compositor.RecreateTextFormats(1);s.compositor.Resize(1440,720);
    auto vm=BuildVm(s,false);vm.tab_layout=0;vm.tabs.clear();
    const std::wstring titles[]={L"源代码",L"Downloads",L"照片",L"发布",L"设计稿 2026",L"Quarterly financial reports",L"备份",L"音乐",L"Desktop",L"Project requirements"};
    auto setTabs=[&](int count,int active){vm.tabs.clear();vm.active_tab=active;for(int i=0;i<count;++i){ui::TabView tab;tab.title=titles[i%10];tab.active=i==active;tab.id=100+i;vm.tabs.push_back(tab);}};
    auto frame=[&](int width,float scale,const wchar_t* file){
        s.scale=scale;s.renderer.SetScale(scale);s.compositor.RecreateTextFormats(scale);s.compositor.Resize(UINT(width*scale),UINT(620*scale));
        const auto bounds=D2D1::RectF(0,0,float(width)*scale,620*scale);vm.dark=true;
        auto current=BuildVm(s,false);vm.pane=current.pane;vm.pane_slots=current.pane_slots;vm.splitters=current.splitters;
        bool ok=true;for(int n=0;n<8;++n){s.compositor.Dc()->BeginDraw();s.renderer.Render(vm,bounds,ui::MakeTheme(true,ui::HexColor(0x6474C7)));ok=SUCCEEDED(s.compositor.Dc()->EndDraw())&&ok;Sleep(16);}
        if(file)check(ok&&s.compositor.SaveSnapshot((output/file).c_str()),"application title screenshot saved");
        auto g=ui::TitleLayoutTestAccess::Geometry(s.renderer,vm,bounds.right);
        const auto chrome=ui::MakeTitleChrome(bounds.right,scale,s.renderer.TitleBarHeight());
        check(g.new_x+32*scale<=chrome.windows_left-56*scale+.5f,"new-tab button preserves 56 DIP window drag reserve");
        const auto toggle=s.renderer.SidebarToggleRect(bounds.right);
        check(toggle.right<=g.x0+.5f,"sidebar toggle stays clear of tabs after brand borrowing");
        if(!g.hidden_indices.empty()) {
            const auto r=g.overflow_button;
            const auto hit=s.renderer.HitTest(vm,bounds,(r.left+r.right)*.5f,(r.top+r.bottom)*.5f);
            check(hit.region==ui::HitTestResult::TabScroll&&hit.index==0,"plus-N button has its own menu hit target");
            check(r.left>=g.x0&&r.right<=g.new_x,"overflow button remains before new-tab and drag region");
        }
        int shown=0;float previous=-1;
        for(size_t i=0;i<vm.tabs.size();++i){const auto r=g.ItemRect(vm,i);if(r.right<=r.left)continue;++shown;
            check(r.left>=previous-.5f&&r.right<=g.new_x+.5f,"visible tabs do not overlap or enter caption controls");previous=r.right;
            const auto hit=s.renderer.HitTest(vm,bounds,(r.left+r.right)*.5f,(r.top+r.bottom)*.5f);
            check(hit.region==ui::HitTestResult::Tab&&hit.index==int(i),"drawn tab center maps to its original identity");
            if(i<g.icon_only.size()&&g.icon_only[i]) {
                const auto edge=s.renderer.HitTest(vm,bounds,r.right-2*scale,(r.top+r.bottom)*.5f);
                check(edge.region!=ui::HitTestResult::TabClose,"icon-only tabs never expose an invisible close target");
            }
        }
        check(shown>0&&g.Width(vm,size_t(vm.active_tab))>0,"active tab stays visible at every width and DPI");
        return g;
    };
    setTabs(2,0);auto natural=frame(1440,1,L"title-natural.png");
    check(natural.Width(vm,0)<240&&natural.Width(vm,1)>natural.Width(vm,0),"natural tab widths follow measured labels instead of equal stretching");
    check(natural.x0<180,"wide window tabs sit next to compact brand instead of sidebar edge");
    vm.tabs[0].title=L"Quarterly financial reports and detailed project requirements";
    auto longTitle=frame(1440,1,L"title-full-text.png");
    const float expectedWidth=60+ui::MeasureTextWidth(s.compositor.DwriteFactory(),s.compositor.TabFormat(),vm.tabs[0].title);
    check(longTitle.Width(vm,0)>=expectedWidth-.5f,"uncrowded long title stays fully visible beyond old 240 DIP cap");
    setTabs(7,2);frame(1180,1,L"title-borrowed.png");
    setTabs(8,3);auto icons=frame(960,1,L"title-icons.png");
    check(std::any_of(icons.icon_only.begin(),icons.icon_only.end(),[](bool icon){return icon;}),"crowded inactive tabs collapse to icons");
    setTabs(14,7);frame(820,1,L"title-overflow.png");
    for(float scale:{1.0f,1.5f,2.0f})for(int width:{320,360,560,680,960,1440}) {
        setTabs(14,7);frame(width,scale,scale==1&&width==320?L"title-320.png":scale==1&&width==360?L"title-360.png":scale==2&&width==960?L"title-dpi200.png":nullptr);
    }
    setTabs(8,3);vm.window_identity=true;vm.window_identity_name=L"项目资料 / Project";vm.window_identity_rgb=0x4CC2FF;
    vm.tab_groups={{1,L"开发",0x6B80D2,false,0}};vm.tabs[0].group=0;vm.tabs[1].group=0;vm.tabs[0].pinned=true;
    frame(1180,1,L"title-group-identity.png");
    check(vm.tabs[0].pinned&&vm.tabs[0].group==0,"responsive layout leaves pin and group model unchanged");
    vm.window_identity=false;vm.tab_groups.clear();
    s.scale=1;s.renderer.SetScale(1);s.compositor.RecreateTextFormats(1);s.compositor.Resize(820,620);
    for(int i=1;i<14;++i){auto& added=s.window_tabs.NewTab(std::filesystem::temp_directory_path().wstring());added.title=titles[i%10];}
    s.window_tabs.active=7;s.pane=s.window_tabs.Active()->FocusedPane();
    auto actual=BuildVm(s,false);
    const auto hidden=s.renderer.HiddenTitleTabs(actual,820);
    const auto menu=BuildTitleTabOverflowItems(s);
    check(!hidden.empty()&&menu.size()==hidden.size(),"overflow menu includes every hidden tab exactly once");
    if(!menu.empty()) {
        const auto expected=s.window_tabs.items[size_t(menu.front().command-1)].get();
        SetWindowLongPtrW(s.hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&s));
        SetTimer(s.hwnd,0x7474,80,[](HWND owner,UINT,UINT_PTR timer,DWORD){
            auto* state=reinterpret_cast<AppState*>(GetWindowLongPtrW(owner,GWLP_USERDATA));
            if(!state||!state->menu)return;
            const auto popup=ui::FluentMenuTestPeer::Window(*state->menu);if(!popup||!IsWindowVisible(popup))return;
            KillTimer(owner,timer);PostMessageW(popup,WM_KEYDOWN,VK_HOME,0);PostMessageW(popup,WM_KEYDOWN,VK_RETURN,0);
        });
        ShowTitleTabOverflow(s,POINT{100,60});KillTimer(s.hwnd,0x7474);
        check(s.window_tabs.Active()==expected,"real overflow menu keyboard selection activates the chosen hidden tab");
    }
    std::vector<double> frame_ms;
    bool switched_cleanly = true;
    for (int i=0; i<80; ++i) {
        s.window_tabs.active=i%2; BindCurrentLayout(s);
        auto* tab=ActiveTab(s);tab->loading=false;
        tab->current_path=i%2 ? L"C:\\tab-B" : L"C:\\tab-A";
        auto rows=std::make_shared<std::vector<fs::DirEntry>>();
        for(int j=0;j<1000;++j){fs::DirEntry e;e.name=L"file-"+std::to_wstring(j)+L".txt";e.full_path=tab->current_path+L"\\"+e.name;rows->push_back(std::move(e));}
        tab->SetSnapshot(rows);
        auto current=BuildVm(s,false);
        const auto begin=std::chrono::steady_clock::now();
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(current,D2D1::RectF(0,0,820,620),ui::MakeTheme(true,ui::HexColor(0x6474C7)));
        const bool drawn=SUCCEEDED(s.compositor.Dc()->EndDraw());
        frame_ms.push_back(std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-begin).count());
        switched_cleanly = switched_cleanly && drawn && !s.paneLayoutMotion.start && ui::TitleLayoutTestAccess::ContentIdle(s.renderer);
    }
    check(switched_cleanly,"80 rapid tab switches retire previous content animations without morph or layout overlap");
    std::sort(frame_ms.begin(),frame_ms.end());
    std::cout<<"TAB_SWITCH_RENDER_MS median="<<frame_ms[40]<<" p95="<<frame_ms[76]<<" max="<<frame_ms.back()<<std::endl;
    std::cout<<"TITLE_LAYOUT_RESULT failures="<<failed<<std::endl;return failed?1:0;
}
