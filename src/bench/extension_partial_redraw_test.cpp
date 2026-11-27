#include "../ui/extension_tree.h"
#include "../ui/ui_compositor.h"
#include "../common/localization.h"
#include <windowsx.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

namespace {
using pulse::ui::ExtensionTreeView;
int failures=0;
bool foreground_probe_enabled=false;
bool foreground_owned_seen=false;
HWND last_foreground=nullptr;
void ObserveForeground(const char* phase) {
    if(!foreground_probe_enabled)return;
    const HWND window=GetForegroundWindow();DWORD process=0;
    const DWORD thread=GetWindowThreadProcessId(window,&process);
    foreground_owned_seen=foreground_owned_seen||process==GetCurrentProcessId();
    if(window!=last_foreground||std::string(phase)!="pump") {
        wchar_t name[128]{};GetClassNameW(window,name,128);
        std::printf("[FOREGROUND] phase=%s hwnd=%p process=%lu thread=%lu class=%ls probe_process=%lu owned=%d\n",phase,
            static_cast<void*>(window),process,thread,name,GetCurrentProcessId(),process==GetCurrentProcessId());
        last_foreground=window;
    }
}
void CALLBACK ForegroundEvent(HWINEVENTHOOK,DWORD,HWND,LONG,LONG,DWORD,DWORD) { ObserveForeground("event"); }

void Check(bool condition,const char* label) {
    std::printf("[%s] %s\n",condition?"PASS":"FAIL",label);std::fflush(stdout);failures+=!condition;
}
void Pump(DWORD ms) {
    const auto until=GetTickCount64()+ms;
    do {
        ObserveForeground("pump");
        MSG message{};
        if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)) {
            if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}
        } else Sleep(1);
    } while(GetTickCount64()<until);
}
uint64_t Cpu() {
    FILETIME created{},ended{},kernel{},user{};GetProcessTimes(GetCurrentProcess(),&created,&ended,&kernel,&user);
    const auto value=[](FILETIME t){return(static_cast<uint64_t>(t.dwHighDateTime)<<32)|t.dwLowDateTime;};
    return value(kernel)+value(user);
}
bool Save(const std::filesystem::path& path,const std::vector<uint32_t>& pixels,UINT width,UINT height) {
    if(pixels.size()!=static_cast<size_t>(width)*height)return false;
    BITMAPFILEHEADER f{};f.bfType=0x4d42;f.bfOffBits=sizeof(f)+sizeof(BITMAPINFOHEADER);f.bfSize=f.bfOffBits+static_cast<DWORD>(pixels.size()*4);
    BITMAPINFOHEADER h{};h.biSize=sizeof(h);h.biWidth=static_cast<LONG>(width);h.biHeight=-static_cast<LONG>(height);h.biPlanes=1;h.biBitCount=32;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<const char*>(&f),sizeof(f));out.write(reinterpret_cast<const char*>(&h),sizeof(h));
    out.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*4));return out.good();
}
template<class F> void FullFallback(ExtensionTreeView& view,const char* reason,F action) {
    const auto before=view.PaintStats();action();UpdateWindow(view.Hwnd());const auto after=view.PaintStats();
    std::printf("[FALLBACK] reason=%s frames=%llu partial=%llu drawn_items=%llu\n",reason,
        static_cast<unsigned long long>(after.frames-before.frames),static_cast<unsigned long long>(after.partial_frames-before.partial_frames),
        static_cast<unsigned long long>(after.drawn_items-before.drawn_items));
    Check(after.frames>before.frames&&after.partial_frames==before.partial_frames,"state/layout change draws a full fallback frame");
}
void Pixels(ExtensionTreeView& view,const std::filesystem::path& out,const char* phase,bool partial) {
    // Establish a full-frame reference outside the measured performance interval.
    InvalidateRect(view.Hwnd(),nullptr,FALSE);UpdateWindow(view.Hwnd());
    std::vector<uint32_t> reference,changed;UINT width=0,height=0,cw=0,ch=0;
    const auto read_before=view.PaintStats();
    Check(view.CopyPresentedPixels(reference,width,height)&&!reference.empty(),"read-only snapshot contains actual submitted full frame");
    Check(view.PaintStats().frames==read_before.frames,"snapshot read does not trigger another Render or Present");
    const auto bounds=view.NodeBounds("progress");Check(bounds.has_value(),"visible progress layout bounds available for exact mask");
    if(reference.empty()||!bounds)return;
    const int left=static_cast<int>(std::floor(bounds->left))-2,top=static_cast<int>(std::floor(bounds->top))-2;
    const int right=static_cast<int>(std::ceil(bounds->right))+2,bottom=static_cast<int>(std::ceil(bounds->bottom))+2;
    const auto before=view.PaintStats();auto after=before;uint64_t observed=before.frames;
    const auto until=GetTickCount64()+500;size_t outside=0,inside=0;unsigned observed_frames=0;
    do {
        Pump(5);after=view.PaintStats();if(after.frames==observed)continue;
        observed=after.frames;++observed_frames;
        Check(view.CopyPresentedPixels(changed,cw,ch)&&cw==width&&ch==height,"read-only snapshot returns following submitted timer frame");
        Check(view.PaintStats().frames==after.frames,"following snapshot read leaves successful Render/Present count unchanged");
        if(changed.size()!=reference.size())return;
        inside=0;size_t this_outside=0;
        for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){
            const auto i=static_cast<size_t>(y)*width+x;if(changed[i]==reference[i])continue;
            if(static_cast<int>(x)>=left&&static_cast<int>(x)<right&&static_cast<int>(y)>=top&&static_cast<int>(y)<bottom)++inside;else ++this_outside;
        }
        outside=std::max(outside,this_outside); // Never discard an earlier outside-mask mismatch.
        if(outside || (inside && (!partial || after.partial_frames>before.partial_frames)))break;
    } while(GetTickCount64()<until);
    Check(after.frames>before.frames,"real production progress timer submits next frame");
    if(partial)Check(after.partial_frames>before.partial_frames,"timer frame actually uses partial Present path");
    else Check(after.partial_frames==before.partial_frames,"same-exe env=0 timer frame remains full");
    std::printf("[PIXEL_WAIT] phase=%s observed_frames=%u\n",phase,observed_frames);
    std::printf("[PIXELS] phase=%s width=%u height=%u mask=%d,%d,%d,%d outside_changed=%zu inside_changed=%zu\n",phase,width,height,left,top,right,bottom,outside,inside);
    Check(outside==0,"pixels outside animated progress match complete-frame reference exactly");
    Check(inside>0,"progress area actually changes between submitted frames");
    Check(Save(out/(std::string(phase)+"-full.bmp"),reference,width,height),"full reference BMP saved");
    Check(Save(out/(std::string(phase)+"-timer.bmp"),changed,width,height),"actual following timer BMP saved");
}
}

int main(int argc,char** argv) {
    using namespace pulse;
    if(argc<3){std::fprintf(stderr,"usage: extension_partial_redraw_test output_directory local_partial_0_or_1 [--current-desktop]\n");return 2;}
    const bool partial=std::string(argv[2])=="1";
    const bool current_desktop=argc>3&&std::string(argv[3])=="--current-desktop";
    foreground_probe_enabled=current_desktop;
    const HWND foreground_before=GetForegroundWindow();
    ObserveForeground("before");
    HWINEVENTHOOK foreground_hook=current_desktop?SetWinEventHook(EVENT_SYSTEM_FOREGROUND,EVENT_SYSTEM_FOREGROUND,nullptr,ForegroundEvent,0,0,WINEVENT_OUTOFCONTEXT):nullptr;
    SetEnvironmentVariableW(L"PULSE_PARTIAL_REDRAW",L"1");SetEnvironmentVariableW(L"PULSE_EXTENSION_PARTIAL_REDRAW",partial?L"1":L"0");
    const auto out=std::filesystem::absolute(argv[1]);std::filesystem::create_directories(out);
    const auto desktop_name=L"PulseExtensionPartial-"+std::to_wstring(GetCurrentProcessId());
    HDESK desktop=nullptr;
    if(!current_desktop){
        desktop=CreateDesktopW(desktop_name.c_str(),nullptr,nullptr,0,GENERIC_ALL,nullptr);
        if(!desktop||!SetThreadDesktop(desktop)){std::printf("[FAIL] isolated desktop error=%lu\n",GetLastError());return 2;}
    }
    std::printf("[DESKTOP] current_desktop=%d foreground_before=%p no_desktop_switch=1\n",current_desktop?1:0,static_cast<void*>(foreground_before));
    OleInitialize(nullptr);l10n::Initialize(GetModuleHandleW(nullptr),L"zh-CN");
    WNDCLASSW cls{};cls.lpfnWndProc=DefWindowProcW;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"PulseExtensionPartialProbe";RegisterClassW(&cls);
    HWND parent=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE,cls.lpszClassName,L"Extension partial redraw probe",WS_POPUP,80,80,900,900,nullptr,nullptr,cls.hInstance,nullptr);
    std::printf("[PROBE_WINDOW] hwnd=%p process=%lu class=PulseExtensionPartialProbe\n",static_cast<void*>(parent),GetCurrentProcessId());
    ShowWindow(parent,SW_SHOWNOACTIVATE);ShowWindow(parent,SW_SHOWNOACTIVATE);
    ObserveForeground("shown");
    Check(IsWindowVisible(parent)&&!IsIconic(parent),"real no-activate parent HWND is visible and non-iconic");
    if(current_desktop)Check(GetForegroundWindow()==foreground_before,"showing the explicitly authorized window preserves foreground HWND");
    extensions::UiTree tree;tree.root.id="root";
    for(int i=0;i<40;++i){
        extensions::UiNode n;n.id="static-"+std::to_string(i);n.type=i%4==3?extensions::UiNodeType::Button:extensions::UiNodeType::Text;
        n.text="Static readable extension control "+std::to_string(i);if(n.type==extensions::UiNodeType::Button)n.command="fixture-action";
        tree.root.children.push_back(n);
        if(i==2){extensions::UiNode p;p.id="progress";p.type=extensions::UiNodeType::Progress;p.progress_state="indeterminate";tree.root.children.push_back(p);}
    }
    ui::Compositor::SetFrameCopyEnabled(false);
    bool commands_enabled=true;
    ExtensionTreeView view;ui::ExtensionTreeCallbacks callbacks;callbacks.command_enabled=[&](const std::string&){return commands_enabled;};
    callbacks.on_command=[](const std::string&){};
    Check(view.Create(parent,tree,callbacks,false,D2D1::ColorF(0x527d70),1.0f),"production extension tree created with text/buttons/progress");
    view.SetBounds({0,0,900,900});UpdateWindow(view.Hwnd());
    const auto initial=view.PaintStats();Check(initial.frames>0&&initial.partial_frames==0,"initial paint is full fallback");
    Check(IsWindowVisible(view.Hwnd()),"production extension child HWND visible");Pump(250);
    const auto before=view.PaintStats();const auto cpu=Cpu();const auto start=GetTickCount64();Pump(2000);const auto elapsed=GetTickCount64()-start;
    const auto after=view.PaintStats();const auto cpu_delta=Cpu()-cpu;
    std::printf("[METRIC] local_partial=%d global_partial=1 frame_copy=0 wall_ms=%llu process_cpu_ms=%.3f frames=%llu partial_frames=%llu drawn_items=%llu\n",partial?1:0,
        static_cast<unsigned long long>(elapsed),static_cast<double>(cpu_delta)/10000.0,
        static_cast<unsigned long long>(after.frames-before.frames),static_cast<unsigned long long>(after.partial_frames-before.partial_frames),static_cast<unsigned long long>(after.drawn_items-before.drawn_items));
    Check(after.frames-before.frames>=10,"real 2-second timer interval produces live progress frames");
    Check(partial?after.partial_frames>before.partial_frames:after.partial_frames==before.partial_frames,"local switch selects observed partial/full render path");
    ui::Compositor::SetFrameCopyEnabled(true);
    Pixels(view,out,"initial",partial);
    FullFallback(view,"resize",[&]{view.SetBounds({0,0,840,840});});Pixels(view,out,"resize",partial);
    FullFallback(view,"theme",[&]{view.SetTheme(true,D2D1::ColorF(0x527d70),1.0f);});Pixels(view,out,"theme",partial);
    Check(IsWindowEnabled(view.NodeWindow("static-3")),"refresh fixture button initially enabled through real command callback");
    FullFallback(view,"refresh",[&]{commands_enabled=false;view.Refresh();});
    Check(!IsWindowEnabled(view.NodeWindow("static-3")),"Refresh applies the actual enabled-state change");Pixels(view,out,"refresh",partial);
    const auto old_bounds=view.NodeBounds("progress");
    SendMessageW(view.Hwnd(),WM_MOUSEWHEEL,MAKEWPARAM(0,static_cast<WORD>(-WHEEL_DELTA)),MAKELPARAM(500,500));
    const auto scroll_deadline=GetTickCount64()+500;bool moved=false;
    do {
        // Dispatch one message at a time: inspect geometry after ScrollFrameMessage
        // before dispatching its queued WM_PAINT, not before the async scroll ran.
        MSG message{};
        if(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){
            if(message.message!=WM_QUIT){TranslateMessage(&message);DispatchMessageW(&message);}
        }else Sleep(1);
        const auto now_bounds=view.NodeBounds("progress");
        moved=old_bounds&&now_bounds&&now_bounds->top!=old_bounds->top;
    }while(!moved&&GetTickCount64()<scroll_deadline);
    Check(moved,"real scroll frame pump changes laid-out progress position");
    FullFallback(view,"scroll",[]{});
    Pump(400); // Let preserved smooth scrolling settle before static-region comparison.
    // Scrolling may move the progress out of the viewport; bring it back through
    // the production reveal path before checking another animated pixel pair.
    view.RevealNode("progress");UpdateWindow(view.Hwnd());Pixels(view,out,"scroll",partial);
    ObserveForeground("before-close");
    view.Close();DestroyWindow(parent);ui::Compositor::SetFrameCopyEnabled(false);OleUninitialize();
    ObserveForeground("after-close");
    if(foreground_hook)UnhookWinEvent(foreground_hook);
    if(current_desktop)Check(!foreground_owned_seen,"no sampled or foreground-event window belongs to the probe process");
    const HWND foreground_after=GetForegroundWindow();
    std::printf("[DESKTOP] foreground_after=%p unchanged=%d\n",static_cast<void*>(foreground_after),foreground_after==foreground_before);
    if(current_desktop)Check(foreground_after==foreground_before,"closing the authorized probe preserves original foreground HWND");
    std::printf("[RESULT] failures=%d\n",failures);return failures?1:0;
}
