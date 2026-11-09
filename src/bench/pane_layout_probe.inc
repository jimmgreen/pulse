#include "../app/pane_layout_controls.h"
#include "../ui/pane_layout_strip.h"

int RunPaneLayoutProbe(pulse::AppState& s) {
    using namespace pulse;
    namespace fsys = std::filesystem;
    int failed = 0;
    const auto check = [&](bool ok,const char* text) { std::cout<<(ok?"[PASS] ":"[FAIL] ")<<text<<std::endl; failed += !ok; };
    const auto output = fsys::absolute(L"../bench_data/pane_layout_exact").lexically_normal();
    const auto fixture = output / (L"fixture-"+std::to_wstring(GetCurrentProcessId()));
    fsys::create_directories(fixture);
    auto* layout = s.window_tabs.Active();
    layout->panes.clear();
    const wchar_t* names[] = {L"源代码",L"照片",L"发布",L"下载"};
    for (int i=0;i<4;++i) {
        const auto folder = fixture/names[i]; fsys::create_directories(folder);
        auto pane = std::make_unique<app::Pane>(); auto& tab = pane->view;
        tab.current_path=folder.wstring(); tab.view_mode=ui::ViewMode::Details; tab.loading=false;
        auto rows=std::make_shared<std::vector<fs::DirEntry>>();
        for(int j=0;j<7;++j) { fs::DirEntry e; e.name=L"示例-"+std::to_wstring(j)+L".txt"; e.full_path=(folder/e.name).wstring(); e.attrs=FILE_ATTRIBUTE_NORMAL; e.size=1024*(j+1); rows->push_back(e); }
        tab.SetSnapshot(rows); tab.SelectOnly(i); layout->panes.push_back(std::move(pane));
    }
    std::vector<app::Pane*> panes; for(auto& p:layout->panes)panes.push_back(p.get());
    layout->root=app::MakePresetTree(app::LayoutPreset::FourGrid,panes); layout->layout=app::LayoutPreset::FourGrid;
    s.pane=panes[0]; panes[0]->focused=true; s.targetPane=panes[2]; panes[2]->target=true;
    s.appPrefs.status_hints_learned=true;
    s.scale=1; s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1); s.compositor.Resize(1180,760);
    auto bounds=[&] {return D2D1::RectF(0,0,float(s.compositor.Width()),float(s.compositor.Height()));};
    auto frame=[&](const wchar_t* name,bool dark=false) {
        s.darkMode=dark; bool ok=true;
        for(int i=0;i<24;++i) {auto vm=BuildVm(s,false);s.compositor.Dc()->BeginDraw();s.renderer.Render(vm,bounds(),ui::MakeTheme(dark,ui::HexColor(0x6474C7)));ok=SUCCEEDED(s.compositor.Dc()->EndDraw())&&ok;
            if(name&&std::wstring_view(name)==L"zoom-light.png"&&i<18&&i%3==0) {
                const auto shot=output/(L"zoom-frame-"+std::to_wstring(i)+L".png");
                check(s.compositor.SaveSnapshot(shot.c_str()),"temporary expand animation frame captured");
            }
            Sleep(16);}
        if(name)check(ok&&s.compositor.SaveSnapshot((output/name).c_str()),"application compositor screenshot saved");
    };
    auto find=[&](ui::HitTestResult::Region region,int index=-1) {
        const auto vm=BuildVm(s,false);const auto r=bounds();
        for(int y=int(r.bottom)-2;y>=55;y-=2)for(int x=int(r.right)-2;x>=4;x-=2){auto h=s.renderer.HitTest(vm,r,float(x),float(y));if(h.region==region&&(index<0||h.index==index))return POINT{x,y};}
        return POINT{-1,-1};
    };
    auto click=[&](ui::HitTestResult::Region region,int index=-1) {
        auto point=find(region,index); check(point.x>=0,"control has a real application hit target");
        if(point.x<0)return false;
        HandleLButtonDown(&s,s.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(point.x,point.y));
        HandleLButtonUp(&s,s.hwnd,WM_LBUTTONUP,0,MAKELPARAM(point.x,point.y)); return true;
    };
    for(float scale:{1.0f,1.5f,2.0f}) {
        const auto strip=D2D1::RectF(0,0,200*scale,22*scale);
        for(int i=0;i<7;++i) {
            const auto button=ui::PaneLayoutButtonBounds(strip,i,scale);
            check(std::fabs(button.right-button.left-26*scale)<.01f&&std::fabs(button.bottom-button.top-22*scale)<.01f,
                "HTML layout buttons measure 26 by 22 DIP");
            if(i&&i!=5)check(std::fabs(button.left-ui::PaneLayoutButtonBounds(strip,i-1,scale).right-scale)<.01f,
                "adjacent layout buttons keep HTML 1 DIP gap");
        }
    }
    frame(L"four-light.png");
    layout->root=app::MakePresetTree(app::LayoutPreset::TwoVertical,panes);layout->layout=app::LayoutPreset::TwoVertical;
    frame(nullptr);
    layout->root=app::MakePresetTree(app::LayoutPreset::TwoHorizontal,panes);layout->layout=app::LayoutPreset::TwoHorizontal;
    auto changed_layout=BuildVm(s,false);
    check(s.paneLayoutMotion.start!=0,"same-count layout orientation change starts HTML geometry transition");
    const auto transition_start=s.paneLayoutMotion.start;
    BuildVm(s,false);
    check(s.paneLayoutMotion.start==transition_start,"unchanged target does not restart the geometry transition");
    frame(L"orientation-stacked.png");
    layout->root=app::MakePresetTree(app::LayoutPreset::FourGrid,panes);layout->layout=app::LayoutPreset::FourGrid;
    frame(nullptr);
    auto original_root=layout->root.get();
    ui::HitTestResult preview;preview.region=ui::HitTestResult::PaneLayoutPreset;preview.index=3;
    HoverPaneLayoutControls(s,preview);Sleep(130);frame(L"layout-preview.png");
    check(BuildVm(s,false).layout_controls.preview_preset==3&&layout->root.get()==original_root,"preset hover previews without committing tree");
    DismissPaneLayoutControls(s);
    check(SetPaneZoom(s,panes[2]),"temporary expand accepts third pane"); frame(L"zoom-light.png");
    auto vm=BuildVm(s,false);
    check(layout->root.get()==original_root&&layout->VisiblePanes()==panes,"temporary expand preserves layout tree and background panes");
    check(vm.pane_slots.size()==1&&PaneAtSlot(s,0)==panes[2]&&s.pane==panes[2],"expanded slot input maps to actual third pane");
    check(s.targetPane==panes[2]&&panes[2]->view.SelectedIndices()==std::vector<int>{2},"target and selection survive expand");
    check(vm.layout_controls.zoom_index==2&&vm.layout_controls.pane_names.size()==4,"mini-map keeps all four original pane identities");
    click(ui::HitTestResult::PaneLayoutCell,1); frame(L"zoom-switched-dark.png",true);
    check(ZoomedPane(*layout)==panes[1]&&layout->root.get()==original_root,"mini-map click switches expanded pane without changing layout");
    auto cell=find(ui::HitTestResult::PaneLayoutCell,2);
    check(cell.x>=0,"hidden target pane has mini-map drop surface");
    const auto source=fixture/names[1]/L"copy-source.txt"; std::ofstream(source)<<"mini-map copy";
    const auto source_move=fixture/names[1]/L"move-source.txt"; std::ofstream(source_move)<<"mini-map move";
    constexpr DWORD allowed=DROPEFFECT_COPY|DROPEFFECT_MOVE;
    if(cell.x>=0) {
        check(ResolveDropTarget(s,{source.wstring()},cell,0,allowed,0)==DROPEFFECT_COPY&&s.dropDestDir==panes[2]->view.current_path,"mini-map defaults to copy on same volume");
        check(ResolveDropTarget(s,{source.wstring()},cell,MK_SHIFT,allowed,0)==DROPEFFECT_MOVE,"mini-map Shift requests move");
        Sleep(320); ResolveDropTarget(s,{source.wstring()},cell,0,allowed,0);frame(L"peek-drop-dark.png",true);
        s.ops.Start([]{});
        check(DropExecute(s,{source.wstring()},cell,0,allowed,0)==DROPEFFECT_COPY,"mini-map drop queues real copy");
        auto until=GetTickCount64()+15000;while(s.ops.HasPendingOperations()&&GetTickCount64()<until)Sleep(5);
        check(fsys::exists(source)&&fsys::exists(fixture/names[2]/source.filename()),"hidden pane copy preserves source and creates destination");
        cell=find(ui::HitTestResult::PaneLayoutCell,2);
        check(DropExecute(s,{source_move.wstring()},cell,MK_SHIFT,allowed,0)==DROPEFFECT_MOVE,"mini-map Shift drop queues real move");
        until=GetTickCount64()+15000;while(s.ops.HasPendingOperations()&&GetTickCount64()<until)Sleep(5);
        check(!fsys::exists(source_move)&&fsys::exists(fixture/names[2]/source_move.filename()),"hidden pane move changes filesystem once");s.ops.Stop();
        check(ZoomedPane(*layout)==panes[1],"drop keeps current pane expanded");
    }
    ClearDropFeedback(s);
    click(ui::HitTestResult::PaneLayoutRestore);frame(L"restored-light.png");
    check(!ZoomedPane(*layout)&&layout->root.get()==original_root,"restore returns original four-pane arrangement");
    for(int preset=0;preset<5;++preset){ApplyLayoutPreset(s,static_cast<app::LayoutPreset>(preset));frame(nullptr);vm=BuildVm(s,false);check(vm.pane_slots.size()==app::LayoutPresetCount(static_cast<app::LayoutPreset>(preset)),"preset produces expected visible pane count");}
    ApplyLayoutPreset(s,app::LayoutPreset::Three);frame(L"three-light.png");
    auto cells=ui::PaneLayoutCells(3,{0,0,100,100},0);
    check(cells.size()==3&&cells[0].bottom==100&&cells[1].left==50&&cells[1].bottom==50,"three-pane diagram matches HTML left-full right-stacked geometry");
    SetPaneZoom(s,s.pane);ApplyLayoutPreset(s,app::LayoutPreset::TwoHorizontal);frame(nullptr);
    check(!ZoomedPane(*layout),"choosing preset leaves temporary expansion");
    SetPaneZoom(s,s.pane);HandleKeyDown(&s,s.hwnd,WM_KEYDOWN,VK_ESCAPE,0);
    check(!ZoomedPane(*layout),"Escape restores temporary expansion");
    layout->compare=true;SetPaneZoom(s,s.pane);RestorePaneZoom(s);
    check(layout->compare,"temporary expansion preserves two-pane comparison state");
    ApplyLayoutPreset(s,app::LayoutPreset::Single);check(!TogglePaneZoom(s),"true single pane cannot temporarily expand");
    ApplyLayoutPreset(s,app::LayoutPreset::FourGrid);frame(nullptr);
    for(float scale:{1.0f,1.5f,2.0f})for(int width:{360,560,820,1180}) {
        s.scale=scale;s.renderer.SetScale(scale);s.compositor.RecreateTextFormats(scale);s.compositor.Resize(UINT(width*scale),UINT(720*scale));frame(nullptr);
        vm=BuildVm(s,false);auto strip=ui::PaneLayoutStripBounds(vm,bounds(),scale);
        check(strip.left>=0&&strip.right<=bounds().right,"layout strip stays inside window across width and DPI");
        if(scale==1.0f&&width==360) {
            SetPaneZoom(s,s.pane);frame(L"zoom-narrow.png",true);RestorePaneZoom(s);frame(nullptr);
            ui::HitTestResult collapsed;collapsed.region=ui::HitTestResult::PaneLayoutTray;
            HoverPaneLayoutControls(s,collapsed);Sleep(150);frame(L"collapsed-open.png",true);
            check(BuildVm(s,false).layout_controls.tray_open,"collapsed layout strip expands after hover delay");
            DismissPaneLayoutControls(s);
        }
        if(scale==1.0f&&(width==360||width==820)){auto file=L"width-"+std::to_wstring(width)+L".png";frame(file.c_str(),true);}
    }
    std::cout<<"PANE_LAYOUT_RESULT failures="<<failed<<std::endl;
    return failed?1:0;
}
