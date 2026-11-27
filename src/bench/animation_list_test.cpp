#include "../app/app_internal.h"
#include "../app/app_input.h"
#include "../app/app_messages.h"
#include "../app/app_runtime.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include <windowsx.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <algorithm>
#include <iomanip>

void Render(pulse::AppState&);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace pulse;
using Clock = std::chrono::steady_clock;
struct Measure { unsigned frames = 0; double render_ms = 0, last_ms = 0; Clock::time_point began, last_counted; };
Measure* measured = nullptr;
void Note(AppState& state, Clock::time_point before) {
    if (!measured || state.lastFrameTime == before || state.lastFrameTime == measured->last_counted) return;
    // WM_FRAME_PUMP can synchronously dispatch WM_PAINT; count its submitted frame once.
    measured->last_counted = state.lastFrameTime;
    ++measured->frames;
    measured->render_ms += state.lastFrameMs;
    measured->last_ms = std::chrono::duration<double, std::milli>(Clock::now() - measured->began).count();
}
LRESULT CALLBACK Owner(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);
    const auto before = state->lastFrameTime;
    LRESULT result = 0;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps{}; BeginPaint(hwnd, &ps); state->compositor.CaptureNextFrame(); Render(*state);
        state->compositor.CancelCaptureNextFrame(); EndPaint(hwnd, &ps); break;
    }
    case WM_MOUSEMOVE: result = HandleMouseMove(state, hwnd, msg, wp, lp); break;
    case WM_LBUTTONDOWN: result = HandleLButtonDown(state, hwnd, msg, wp, lp); break;
    case WM_LBUTTONUP: result = HandleLButtonUp(state, hwnd, msg, wp, lp); break;
    case WM_KEYDOWN: result = HandleKeyDown(state, hwnd, msg, wp, lp); break;
    case WM_MOUSEWHEEL: result = HandleMouseWheel(state, hwnd, msg, wp, lp); break;
    case WM_TIMER: result = WndProc(hwnd, msg, wp, lp); break;
    default:
        if (msg == WM_FRAME_PUMP) result = WndProc(hwnd, msg, wp, lp);
        else return DefWindowProcW(hwnd, msg, wp, lp);
    }
    Note(*state, before);
    return result;
}
void Pump(DWORD ms) {
    const auto end = GetTickCount64() + ms;
    do {
        MSG message{};
        if (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) continue;
            TranslateMessage(&message); DispatchMessageW(&message);
        } else Sleep(1);
    } while (GetTickCount64() < end);
}
uint64_t CpuUs() {
    FILETIME c{}, e{}, k{}, u{}; GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u);
    const auto n = [](FILETIME f) { return (static_cast<uint64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
    return (n(k) + n(u)) / 10;
}
void Paint(AppState& state) {
    const auto before = state.lastFrameTime; state.compositor.CaptureNextFrame();
    Render(state); state.compositor.CancelCaptureNextFrame(); Note(state, before);
}
std::vector<uint32_t> Pixels(AppState& state, D2D1_RECT_F rect) {
    using Microsoft::WRL::ComPtr;
    auto* dc = state.compositor.Dc();
    ComPtr<ID2D1Bitmap1> source = state.compositor.HeldFrame();
    if (!source) return {}; // Read the submitted frame, not the next empty backbuffer.
    const auto size = source->GetPixelSize();
    D2D1_BITMAP_PROPERTIES1 props{};
    props.pixelFormat = source->GetPixelFormat(); props.dpiX = props.dpiY = 96;
    props.bitmapOptions = D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    ComPtr<ID2D1Bitmap1> read;
    if (FAILED(dc->CreateBitmap(size, nullptr, 0, &props, &read)) || FAILED(read->CopyFromBitmap(nullptr, source.Get(), nullptr))) return {};
    D2D1_MAPPED_RECT map{}; if (FAILED(read->Map(D2D1_MAP_OPTIONS_READ, &map))) return {};
    const int left = std::clamp(static_cast<int>(rect.left), 0, static_cast<int>(size.width));
    const int right = std::clamp(static_cast<int>(rect.right), left, static_cast<int>(size.width));
    const int top = std::clamp(static_cast<int>(rect.top), 0, static_cast<int>(size.height));
    const int bottom = std::clamp(static_cast<int>(rect.bottom), top, static_cast<int>(size.height));
    std::vector<uint32_t> result;
    for (int y = top; y < bottom; ++y) {
        const auto* row = reinterpret_cast<const uint32_t*>(map.bits + y * map.pitch);
        result.insert(result.end(), row + left, row + right);
    }
    read->Unmap(); return result;
}
bool SavePixels(const std::filesystem::path& path, const std::vector<uint32_t>& pixels, int width, int height) {
    if (pixels.size()!=static_cast<size_t>(width)*height) return false;
    BITMAPFILEHEADER file{}; file.bfType=0x4d42; file.bfOffBits=sizeof(file)+sizeof(BITMAPINFOHEADER);
    file.bfSize=file.bfOffBits+static_cast<DWORD>(pixels.size()*4);
    BITMAPINFOHEADER info{}; info.biSize=sizeof(info); info.biWidth=width; info.biHeight=-height;
    info.biPlanes=1; info.biBitCount=32; info.biCompression=BI_RGB;
    std::ofstream stream(path,std::ios::binary);
    stream.write(reinterpret_cast<const char*>(&file),sizeof(file));
    stream.write(reinterpret_cast<const char*>(&info),sizeof(info));
    stream.write(reinterpret_cast<const char*>(pixels.data()),static_cast<std::streamsize>(pixels.size()*4));
    return stream.good();
}
size_t Ink(const std::vector<uint32_t>& pixels, bool dark) {
    return static_cast<size_t>(std::count_if(pixels.begin(), pixels.end(), [dark](uint32_t p) {
        const unsigned b=p&255, g=(p>>8)&255, r=(p>>16)&255;
        return dark ? r>160 && g>160 && b>160 : r<140 && g<140 && b<140;
    }));
}
}
int main(int argc, char** argv) {
    using namespace pulse;
    const auto name = L"PulseAnimationList-" + std::to_wstring(GetCurrentProcessId());
    const HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) { std::cout << "[FAIL] isolated desktop error=" << GetLastError() << '\n'; return 2; }
    OleInitialize(nullptr);
    l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    const auto output = std::filesystem::absolute(argc>1 ? argv[1] : "bench_data/animation-refine-20261124/list/run");
    std::filesystem::create_directories(output / "fixture-a");
    std::filesystem::create_directories(output / "fixture-b");
    int failed = 0;
    const auto check = [&](bool condition, const char* label) { std::cout << (condition?"[PASS] ":"[FAIL] ") << label << '\n'; failed += !condition; };
    BOOL animations=TRUE; SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION,0,&animations,0);
    std::cout << "[INFO] isolated desktop; real Win32/compositor/Render/BuildVm/input; animation_enabled=" << animations << '\n';
    check(animations != FALSE, "animation-enabled baseline is available without changing system preferences");
    WNDCLASSW wc{}; wc.lpfnWndProc=Owner; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"PulseAnimationListProbe";
    RegisterClassW(&wc);
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i=0;i<120;++i) {
        fs::DirEntry entry; entry.name=L"fixture-readable-"+std::to_wstring(1000+i)+L".txt";
        std::ofstream(output / "fixture-a" / entry.name) << "Pulse animation production fixture\n";
        std::ofstream(output / "fixture-b" / entry.name) << "Pulse animation production fixture\n";
        entries->push_back(std::move(entry));
    }
    for (bool dark : {false,true}) {
        auto owned = std::make_unique<AppState>(); auto& state=*owned;
        state.isolatedTest=true; state.places.persist=state.appPrefs.persist=state.searchHistory.persist=false;
        state.ctxMenuPrefs.persist=false; state.appPrefs.show_hints=false; state.darkMode=dark;
        state.scale=1; state.hwnd=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_NOREDIRECTIONBITMAP,wc.lpszClassName,
            L"Pulse animation production probe",WS_POPUP,100,100,1100,760,nullptr,nullptr,wc.hInstance,nullptr);
        check(state.hwnd && state.compositor.Init(state.hwnd), "real compositor initializes");
        if (!state.hwnd || !state.compositor.Dc()) return 2;
        state.renderer.SetCompositor(&state.compositor); state.renderer.SetIconNotifyWindow(state.hwnd);
        state.renderer.SetScale(1); state.compositor.RecreateTextFormats(1);
        state.window_tabs.EnsureDefault(); BindCurrentLayout(state); auto* tab=ActiveTab(state);
        tab->current_path=fs::NormalizePath((output/"fixture-a").wstring()); tab->SetSnapshot(entries); tab->loading=false;
        tab->view_mode=ui::ViewMode::Details; tab->SelectOnly(-1);
        SetWindowLongPtrW(state.hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(&state));
        ShowWindow(state.hwnd,SW_SHOWNOACTIVATE); // First ShowWindow may use STARTUPINFO
        ShowWindow(state.hwnd,SW_SHOWNOACTIVATE); // Explicit visible state on the isolated desktop.
        check(IsWindowVisible(state.hwnd) && !IsIconic(state.hwnd),"test HWND is genuinely visible on its isolated desktop");
        state.framePump.Start(state.hwnd,WM_FRAME_PUMP);
        Paint(state); Pump(1000);
        for (int repeat=0;repeat<3;++repeat) {
            tab->current_path=fs::NormalizePath((output/(repeat%2?"fixture-a":"fixture-b")).wstring());
            Measure m{}; m.began=Clock::now(); measured=&m; const auto cpu=CpuUs();
            Paint(state);
            const auto vm=BuildVm(state,false); const auto bounds=FocusedPaneRect(state);
            auto row=state.renderer.ItemRectInPane(vm.pane,bounds,0);
            row.left+=55; row.right=std::min(row.right,row.left+260); row.top+=3; row.bottom-=3;
            const auto initial=Pixels(state,row); const size_t initial_ink=Ink(initial,dark);
            const double first_ms=std::chrono::duration<double,std::milli>(state.lastFrameTime-m.began).count();
            Pump(900); measured=nullptr; const auto cpu_used=CpuUs()-cpu; Paint(state);
            const size_t final_ink=Ink(Pixels(state,row),dark);
            std::cout<<"[METRIC] navigation dark="<<dark<<" repeat="<<repeat<<" frames="<<m.frames<<" render_ms="<<m.render_ms
                <<" thread_cpu_us="<<cpu_used<<" last_frame_ms="<<m.last_ms<<" first_frame_ms="<<first_ms
                <<" first_name_ink="<<initial_ink<<" settled_name_ink="<<final_ink<<'\n';
            check(final_ink>20 && initial_ink>=final_ink*9/10,"ready navigation names readable on first painted frame");
            if(repeat==0) check(SavePixels(output/(dark?"nav-dark.bmp":"nav-light.bmp"),Pixels(state,D2D1::RectF(0,0,1100,760)),1100,760),"navigation screenshot saved");
        }
        // Loading has a real 150 ms deadline, but no visual change before it.
        tab->loading=true;
        Measure loading{}; loading.began=Clock::now(); measured=&loading;
        Paint(state); Pump(100); measured=nullptr;
        std::cout<<"[METRIC] loading_initial dark="<<dark<<" frames="<<loading.frames<<" render_ms="<<loading.render_ms<<'\n';
        check(loading.frames<=2,"initial loading delay waits without continuous paints");
        Pump(250);
        check(state.renderer.TickMotion(GetTickCount64()),"visible loading shimmer still animates after its deadline");
        tab->loading=false; Paint(state); Pump(700); Paint(state);
        auto landing_vm=BuildVm(state,false);
        auto landing_row=state.renderer.ItemRectInPane(landing_vm.pane,FocusedPaneRect(state),0);
        landing_row.left+=55; landing_row.right=std::min(landing_row.right,landing_row.left+260);
        landing_row.top+=3; landing_row.bottom-=3;
        const auto before_landing=Ink(Pixels(state,landing_row),dark);
        state.dragLanding.dir=tab->current_path; state.dragLanding.pane=state.pane;
        state.dragLanding.names={entries->front().name}; state.dragLanding.since=ui::motion::NowMs();
        state.dragLanding.reveal_at=state.dragLanding.since+240;
        Paint(state);
        const auto landed_ink=Ink(Pixels(state,landing_row),dark);
        std::cout<<"[METRIC] landing dark="<<dark<<" immediate_name_ink="<<landed_ink<<" ready_name_ink="<<before_landing<<'\n';
        check(before_landing>20 && landed_ink>=before_landing*9/10,"real dropped row remains visible while its flight is pending");
        state.dragLanding={}; Paint(state); Pump(700);
        tab->view_mode=ui::ViewMode::MediumIcons; tab->icon_dip=48; Paint(state); Pump(1300);
        tab->icon_dip=72; Paint(state); Pump(400);
        Measure hold{}; hold.began=Clock::now(); measured=&hold; const auto hold_cpu=CpuUs();
        Pump(350); measured=nullptr;
        std::cout<<"[METRIC] size_hold dark="<<dark<<" frames="<<hold.frames<<" render_ms="<<hold.render_ms
            <<" thread_cpu_us="<<CpuUs()-hold_cpu<<'\n';
        check(hold.frames<=2,"static icon-size label hold does not continuously repaint");
        Pump(700); tab->icon_dip=0;
        // Production target-layout switch, no artificial advancement of the motion clock.
        tab->view_mode=ui::ViewMode::MediumIcons; Paint(state); Pump(800);
        Measure morph{}; morph.began=Clock::now(); measured=&morph; const auto cpu=CpuUs();
        tab->view_mode=ui::ViewMode::Details; Paint(state); Pump(700); measured=nullptr;
        std::cout<<"[METRIC] morph dark="<<dark<<" frames="<<morph.frames<<" render_ms="<<morph.render_ms
            <<" thread_cpu_us="<<CpuUs()-cpu<<" last_frame_ms="<<morph.last_ms<<'\n';
        check(morph.frames>1,"real frame pump advances the retained view transition");
        check(morph.last_ms<350,"view transition settles within wall-clock budget on this machine");
        Paint(state); // Capture the settled reference outside the timed interval.
        const auto reference_vm=BuildVm(state,false);
        auto reference_name=state.renderer.ItemRectInPane(reference_vm.pane,FocusedPaneRect(state),2);
        reference_name.left+=55; reference_name.right=std::min(reference_name.right,reference_name.left+260);
        reference_name.top+=3; reference_name.bottom-=3;
        const auto reference_pixels=Pixels(state,reference_name);
        tab->view_mode=ui::ViewMode::MediumIcons; Paint(state); Pump(800);
        const auto click_started=Clock::now();
        tab->view_mode=ui::ViewMode::Details; Paint(state);
        // No preceding mouse-move/snap: the name and hit cell must already agree.
        const auto moving_vm=BuildVm(state,false);
        const auto moving_cell=state.renderer.ItemRectInPane(moving_vm.pane,FocusedPaneRect(state),2);
        auto moving_name=moving_cell; moving_name.left+=55; moving_name.right=std::min(moving_name.right,moving_name.left+260);
        moving_name.top+=3; moving_name.bottom-=3;
        const auto moving_pixels=Pixels(state,moving_name);
        check(Ink(moving_pixels,dark)>20,"transition first frame paints row A at its authoritative hit cell");
        size_t changed_name_pixels=0;
        if (moving_pixels.size()==reference_pixels.size()) {
            for(size_t i=0;i<moving_pixels.size();++i)
                changed_name_pixels+=moving_pixels[i]!=reference_pixels[i];
        } else changed_name_pixels=reference_pixels.size();
        std::cout<<"[METRIC] first_name_reference_diff="<<changed_name_pixels<<" pixels="<<reference_pixels.size()<<'\n';
        check(!reference_pixels.empty() && changed_name_pixels<=2,"first-frame A name pixels match its settled identity reference");
        const auto first_frame=Pixels(state,D2D1::RectF(0,0,1100,760));
        const int moving_x=static_cast<int>(moving_cell.left+90),moving_y=static_cast<int>((moving_cell.top+moving_cell.bottom)*.5f);
        const double click_ms=std::chrono::duration<double,std::milli>(Clock::now()-click_started).count();
        SendMessageW(state.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(moving_x,moving_y));
        SendMessageW(state.hwnd,WM_LBUTTONUP,0,MAKELPARAM(moving_x,moving_y));
        std::cout<<"[METRIC] direct_click_age_ms="<<click_ms<<'\n';
        check(click_ms<150,"direct click arrives before the retained transition deadline");
        check(tab->selected_index==2,"direct mid-transition click selects the already painted A without a preceding move");
        check(SavePixels(output/(dark?"morph-first-dark.bmp":"morph-first-light.bmp"),first_frame,1100,760),"actual transition first-frame pixels saved after click");
        Pump(100); Paint(state); // Capture the selected state, after the direct-click assertion above.
        check(SavePixels(output/(dark?"morph-click-dark.bmp":"morph-click-light.bmp"),Pixels(state,D2D1::RectF(0,0,1100,760)),1100,760),"direct-click result screenshot saved");
        SendMessageW(state.hwnd,WM_KEYDOWN,VK_CONTROL,0);
        auto vm=BuildVm(state,false); auto row=state.renderer.ItemRectInPane(vm.pane,FocusedPaneRect(state),0);
        row.left+=55; row.right=std::min(row.right,row.left+260); row.top+=3; row.bottom-=3;
        const auto after_input=Pixels(state,row); const auto input_ink=Ink(after_input,dark);
        Pump(700); Paint(state); const auto settled_ink=Ink(Pixels(state,row),dark);
        std::cout<<"[METRIC] morph_input dark="<<dark<<" immediate_name_ink="<<input_ink<<" settled_name_ink="<<settled_ink<<'\n';
        check(settled_ink>20 && input_ink>=settled_ink*9/10,"input immediately finishes geometry and paints readable target");
        vm=BuildVm(state,false); const auto cell=state.renderer.ItemRectInPane(vm.pane,FocusedPaneRect(state),2);
        const int x=static_cast<int>(cell.left+90), y=static_cast<int>((cell.top+cell.bottom)*.5f);
        SendMessageW(state.hwnd,WM_LBUTTONDOWN,MK_LBUTTON,MAKELPARAM(x,y));
        SendMessageW(state.hwnd,WM_LBUTTONUP,0,MAKELPARAM(x,y));
        check(tab->selected_index==2,"real mouse button handlers select the painted target row");
        state.scrollAnimating=false; tab->scroll_y=200;
        StartSmoothScroll(state,500,false); Sleep(5); UpdateSmoothScroll(state);
        const float before=tab->scroll_y;
        StartSmoothScroll(state,-50,false); Sleep(8); UpdateSmoothScroll(state);
        std::cout<<"[METRIC] reverse before="<<before<<" after="<<tab->scroll_y<<" target="<<state.scrollTargetY<<'\n';
        check(tab->scroll_y<before,"reverse wheel motion immediately follows opposite input");
        CancelScrollAnimation(state); Pump(300);
        auto drives=std::make_shared<std::vector<fs::DirEntry>>();
        fs::DirEntry drive; drive.name=L"Fixture volume"; drive.full_path=L"C:\\";
        drive.is_dir=true; drive.drive_type=DRIVE_FIXED;
        drive.drive_total=10ull*1024*1024*1024; drive.drive_free=4ull*1024*1024*1024;
        drives->push_back(drive);
        tab->current_path.clear(); tab->view_mode=ui::ViewMode::Tiles; tab->SetSnapshot(drives);
        tab->scroll_y=0; tab->SelectOnly(-1); Paint(state); Pump(1200);
        tab->current_path=fs::NormalizePath((output/"fixture-a").wstring()); tab->SetSnapshot(entries);
        Paint(state); Pump(800);
        tab->current_path.clear(); tab->SetSnapshot(drives);
        Measure capacity{}; capacity.began=Clock::now(); measured=&capacity; Paint(state);
        const auto capvm=BuildVm(state,false);
        const auto capcell=state.renderer.ItemRectInPane(capvm.pane,FocusedPaneRect(state),0);
        const auto first_capacity=Pixels(state,capcell);
        Pump(950); measured=nullptr; Paint(state); const auto last_capacity=Pixels(state,capcell);
        const auto colored=[](const auto& pixels) {
            return static_cast<size_t>(std::count_if(pixels.begin(),pixels.end(),[](uint32_t p) {
                const int b=p&255,g=(p>>8)&255,r=(p>>16)&255;
                return std::max({r,g,b})-std::min({r,g,b})>45;
            }));
        };
        const auto first_color=colored(first_capacity),last_color=colored(last_capacity);
        std::cout<<"[METRIC] capacity dark="<<dark<<" first_color_pixels="<<first_color<<" settled_color_pixels="<<last_color
            <<" frames="<<capacity.frames<<" render_ms="<<capacity.render_ms<<'\n';
        check(last_color>40 && first_color>=last_color*9/10,"drive capacity view is accurate on its first ready frame");
        check(SavePixels(output/(dark?"capacity-dark.bmp":"capacity-light.bmp"),Pixels(state,D2D1::RectF(0,0,1100,760)),1100,760),"capacity screenshot saved");
        state.framePump.Stop(); SetWindowLongPtrW(state.hwnd,GWLP_USERDATA,0); DestroyWindow(state.hwnd); state.hwnd=nullptr;
    }
    std::cout<<"[RESULT] failures="<<failed<<'\n';
    return failed?1:0;
}
