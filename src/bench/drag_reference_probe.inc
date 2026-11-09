#include "../app/app_input.h"
#include "../ui/drag_drop.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

namespace pulse::ui {
bool SaveDragStackReference(const std::vector<std::wstring>&, bool, DWORD, const std::wstring&, const wchar_t*, float);
}

int RunDragReferenceProbe(pulse::AppState& s) {
    using namespace pulse;
    namespace filesystem = std::filesystem;
    const auto output = (filesystem::current_path() / L"bench_data/drag_undo_review").lexically_normal();
    filesystem::create_directories(output);
    FILE* log = nullptr; _wfopen_s(&log, (output / L"drag-reference.log").c_str(), L"w");
    if (!log) return 2;
    int failed = 0;
    auto check = [&](bool ok, const char* label) { fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failed += !ok; };
    const auto root = output / (L"fixture-" + std::to_wstring(GetCurrentProcessId()));
    filesystem::create_directories(root / L"发布" / L"下一层");
    std::ofstream(root / L"说明.txt") << "drag fixture";
    auto* tab = ActiveTab(s);
    auto listing = [&](const filesystem::path& folder) {
        tab->current_path = folder.wstring(); tab->loading = false;
        auto entries = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto& item : filesystem::directory_iterator(folder)) {
            fs::DirEntry entry; entry.name = item.path().filename().wstring(); entry.full_path = item.path().wstring();
            entry.is_dir = item.is_directory(); entry.attrs = entry.is_dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            entries->push_back(entry);
        }
        tab->SetSnapshot(entries);
    };
    s.scale = 1.0f; s.renderer.SetScale(1.0f); s.compositor.RecreateTextFormats(1.0f); s.compositor.Resize(1100,720);
    const auto rect = D2D1::RectF(0,0,1100,720);
    auto frame = [&](const wchar_t* name, int spring_age = 400) {
        auto theme = ui::MakeTheme(s.darkMode, ui::HexColor(0x6474C7));
        bool ok = true;
        for (int frame_index=0; frame_index<28; ++frame_index) {
            if (name && s.springRow != -1) s.springStart=GetTickCount64()-spring_age;
            auto vm = BuildVm(s);
            s.compositor.Dc()->BeginDraw(); s.renderer.Render(vm,rect,theme);
            ok = SUCCEEDED(s.compositor.Dc()->EndDraw()) && ok;
            Sleep(16);
        }
        if (name) check(ok && s.compositor.SaveSnapshot((output/name).c_str()), "application renderer screenshot saved");
    };
    auto find = [&](ui::HitTestResult::Region region, int index = -1) {
        auto vm = BuildVm(s);
        int left=1100, right=-1, top=720, bottom=-1;
        for (int y=35;y<680;y+=3) for(int x=8;x<1090;x+=4) {
            auto h=s.renderer.HitTest(vm,rect,float(x),float(y));
            if(h.region==region && (index<0 || h.index==index)) {
                left=std::min(left,x);right=std::max(right,x);top=std::min(top,y);bottom=std::max(bottom,y);
            }
        }
        return right<0 ? POINT{-1,-1} : POINT{(left+right)/2,(top+bottom)/2};
    };
    const std::vector<std::wstring> source{(root/L"说明.txt").wstring()};
    constexpr DWORD allowed = DROPEFFECT_COPY|DROPEFFECT_MOVE;
    for (auto mode : {ui::ViewMode::Details, ui::ViewMode::MediumIcons, ui::ViewMode::LargeIcons}) {
        ClearDropFeedback(s); s.springEntered=false; listing(root); tab->view_mode=mode; frame(nullptr);
        int folder=-1; for(size_t i=0;i<tab->EntryCount();++i) if(tab->EntryAt(i).is_dir) folder=int(i);
        auto point=find(ui::HitTestResult::Row,folder);
        check(point.x>=0,"folder target exists in this view");
        auto effect=ResolveDropTarget(s,source,point,0,allowed,0);
        check(effect==DROPEFFECT_MOVE && s.dropDestDir==(root/L"发布").wstring(),"same-volume row drag moves into child");
        check(s.dropBadge == L"移动到「发布」\tCtrl 复制", "HTML move label and separate small hint match exactly");
        check(ResolveDropTarget(s,source,point,MK_CONTROL,allowed,0)==DROPEFFECT_COPY,"Ctrl changes row drop to copy");
        check(s.dropBadge == L"复制到「发布」", "HTML same-volume copy has no extra hint");
        const std::vector<std::wstring> cross_source{L"Z:\\pulse-drag-reference\\source.txt"};
        check(ResolveDropTarget(s,cross_source,point,0,allowed,0)==DROPEFFECT_COPY &&
            s.dropBadge == L"复制到「发布」\t跨磁盘默认复制 · Shift 移动", "HTML cross-volume copy hint matches exactly");
        check(ResolveDropTarget(s,source,point,MK_SHIFT,allowed,0)==DROPEFFECT_MOVE,"Shift changes row drop to move");
        s.springStart=GetTickCount64()-400;
        frame(mode==ui::ViewMode::Details ? L"drag-details.png" : mode==ui::ViewMode::MediumIcons ? L"drag-medium.png" : L"drag-large.png");
        if (mode == ui::ViewMode::LargeIcons) {
            frame(L"drag-enter-25.png",200);
            frame(L"drag-enter-75.png",600);
        }
        s.springStart=GetTickCount64()-801;
        ResolveDropTarget(s,source,point,0,allowed,0);
        check(tab->current_path==fs::NormalizePath((root/L"发布").wstring()),"800ms spring enters child");
        listing(root/L"发布"); frame(nullptr); point=find(ui::HitTestResult::Row,0);
        ResolveDropTarget(s,source,point,0,allowed,0);
        if (mode == ui::ViewMode::LargeIcons) frame(L"drag-enter-open.png",200);
        s.springStart=GetTickCount64()-801;
        ResolveDropTarget(s,source,point,0,allowed,0);
        check(tab->current_path==fs::NormalizePath((root/L"发布" / L"下一层").wstring()),"second spring enter remains enabled");
    }
    listing(root/L"发布"); tab->view_mode=ui::ViewMode::Details; s.dragFilesOver=true; frame(nullptr);
    auto parent=find(ui::HitTestResult::DropParent);
    check(parent.x>=0,"parent overlay has its own hit target");
    check(ResolveDropTarget(s,{(root/L"发布" / L"下一层").wstring()},parent,0,allowed,0)==DROPEFFECT_MOVE,"parent overlay accepts move to parent");
    s.springStart=GetTickCount64()-400; frame(L"drag-parent.png");
    s.springStart=GetTickCount64()-801; ResolveDropTarget(s,source,parent,0,allowed,0);
    check(tab->current_path==fs::NormalizePath(root.wstring()),"parent overlay springs upward");
    listing(root/L"发布"); frame(nullptr); auto up=find(ui::HitTestResult::NavUp);
    check(up.x>=0 && ResolveDropTarget(s,{(root/L"发布" / L"下一层").wstring()},up,0,allowed,0)==DROPEFFECT_MOVE,"Up button accepts file drop");
    listing(root); frame(nullptr); int folder=-1;for(size_t i=0;i<tab->EntryCount();++i)if(tab->EntryAt(i).is_dir)folder=int(i);
    auto own=find(ui::HitTestResult::Row,folder);
    check(ResolveDropTarget(s,{(root/L"发布").wstring()},own,0,allowed,0)==DROPEFFECT_NONE && !s.dropBadge.empty(),"self-drop rejects with explanation");
    check(s.dropBadge == L"不能放进它自己里面", "HTML self-drop explanation matches exactly");
    int file_row = -1;
    for (size_t i=0;i<tab->EntryCount();++i) if(!tab->EntryAt(i).is_dir) file_row=int(i);
    auto file_point=find(ui::HitTestResult::Row,file_row);
    check(ResolveDropTarget(s,source,file_point,0,allowed,0)==DROPEFFECT_NONE && !s.dropBadge.empty(), "same-folder drop is a labelled no-op");
    check(ResolveDropTarget(s,source,POINT{-50,-50},0,allowed,0)==DROPEFFECT_NONE && s.dropBadge == L"松手取消",
        "HTML no-target label matches exactly");
    listing(root); frame(nullptr); own=find(ui::HitTestResult::Row,folder);
    s.ops.Start([] {});
    check(DropExecute(s,source,own,MK_CONTROL,allowed,0)==DROPEFFECT_COPY, "actual child drop queues copy");
    auto deadline=GetTickCount64()+15000;
    while(s.ops.HasPendingOperations() && GetTickCount64()<deadline) Sleep(5);
    check(filesystem::exists(root/L"发布" / L"说明.txt") && filesystem::exists(root/L"说明.txt"), "actual child copy preserves source and creates target");
    listing(root/L"发布"); s.dragFilesOver=true; frame(nullptr); parent=find(ui::HitTestResult::DropParent);
    check(DropExecute(s,{(root/L"发布" / L"下一层").wstring()},parent,0,allowed,0)==DROPEFFECT_MOVE,"actual parent drop queues move");
    deadline=GetTickCount64()+15000;
    while(s.ops.HasPendingOperations() && GetTickCount64()<deadline) Sleep(5);
    check(filesystem::is_directory(root/L"下一层") && !filesystem::exists(root/L"发布" / L"下一层"), "actual parent move changes filesystem exactly once");
    s.ops.Stop();
    const std::vector<std::wstring> ghost_paths{(root/L"说明.txt").wstring(), (root/L"发布").wstring(), (root/L"下一层").wstring()};
    for (bool dark : {false, true}) for (DWORD effect : {DWORD(DROPEFFECT_MOVE), DWORD(DROPEFFECT_COPY), DWORD(DROPEFFECT_NONE)}) {
        wchar_t formatted[128]{};
        swprintf_s(formatted, l10n::Get(effect == DROPEFFECT_MOVE ? l10n::StringId::DragHintMoveFormat : l10n::StringId::DragHintCopyFormat).c_str(), L"发布");
        const std::wstring label = effect == DROPEFFECT_NONE ? l10n::Get(l10n::StringId::DragDropIntoSelf) :
            std::wstring(formatted) + (effect == DROPEFFECT_MOVE ? L"\t" + l10n::Get(l10n::StringId::DragHintCtrlCopy) : L"");
        const auto image = output / (std::wstring(L"drag-ghost-") + (dark ? L"dark-" : L"light-") + std::to_wstring(effect) + L".png");
        check(ui::SaveDragStackReference(ghost_paths,dark,effect,label,image.c_str(),dark ? 1.5f : 1.0f),
            "actual drag-stack renderer saves separate action pill at 100/150 percent DPI");
    }
    check(ui::SaveDragStackReference(ghost_paths, false, DROPEFFECT_NONE, L"", (output/L"drag-ghost-cancel.png").c_str(), 1),
        "HTML no-target cancel pill is rendered");
    check(ui::SaveDragStackReference(ghost_paths, false, DROPEFFECT_COPY, L"复制到「发布」\t跨磁盘默认复制 · Shift 移动",
        (output/L"drag-ghost-cross-volume.png").c_str(), 1), "HTML cross-volume small hint is rendered");
    ui::ListDropSource cancel;
    check(cancel.QueryContinueDrag(TRUE,MK_LBUTTON)==DRAGDROP_S_CANCEL,"Esc cancels source drag");
    check(filesystem::exists(root/L"说明.txt"),"probe never changes source file");
    ClearDropFeedback(s);
    fprintf(log,"failures=%d\n",failed); fclose(log);
    DestroyWindow(s.hwnd); s.hwnd=nullptr;
    return failed ? 1 : 0;
}
