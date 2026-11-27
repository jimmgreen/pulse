#include <cstdio>
#define PULSE_ANIMATION_SEARCH_TRACE_STAGE(stage) do { \
    static unsigned trace_count = 0; \
    if (GetEnvironmentVariableW(L"PULSE_ANIMATION_TRACE", nullptr, 0) != 0 && trace_count++ < 32) { \
        std::fprintf(stderr, "RENDER stage=%s tick=%llu\n", stage, GetTickCount64()); std::fflush(stderr); \
    } \
} while (0)
#define PULSE_ANIMATION_SEARCH_TRACE_STATUS(status) do { \
    static unsigned trace_status_count = 0; \
    if (GetEnvironmentVariableW(L"PULSE_ANIMATION_TRACE", nullptr, 0) != 0 && trace_status_count++ < 32) { \
        std::fprintf(stderr, "PRESENT status=0x%08lx tick=%llu\n", static_cast<unsigned long>(status), GetTickCount64()); std::fflush(stderr); \
    } \
} while (0)
#include ANIMATION_SEARCH_SOURCE
#undef PULSE_ANIMATION_SEARCH_TRACE_STATUS
#undef PULSE_ANIMATION_SEARCH_TRACE_STAGE
#include <cstdio>
#ifndef ANIMATION_MRU_HEADER
#define ANIMATION_MRU_HEADER "../ui/tab_mru_view.h"
#define ANIMATION_WINDOW_SWITCHER_HEADER "../ui/window_switcher_view.h"
#endif
#define private public
#include ANIMATION_MRU_HEADER
#include ANIMATION_WINDOW_SWITCHER_HEADER
#undef private

namespace pulse {
struct GlobalSearchWindowTestPeer {
    using State = GlobalSearchWindow::Impl;
    static inline unsigned hidden_ticks = 0, visible_ticks = 0, paint_messages = 0;
    static LRESULT CALLBACK Observe(HWND hwnd, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
        if (message == WM_TIMER && wp == kAnimationTimer) { if (!IsWindowVisible(hwnd)) ++hidden_ticks; else ++visible_ticks; }
        if (message == WM_PAINT) ++paint_messages;
        if (message == WM_NCDESTROY) RemoveWindowSubclass(hwnd, Observe, id);
        return DefSubclassProc(hwnd, message, wp, lp);
    }
    static void Pump(DWORD duration) {
        const auto until = GetTickCount64() + duration;
        while (GetTickCount64() < until) {
            MSG msg{};
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                static unsigned trace_messages = 0;
                const bool trace = GetEnvironmentVariableW(L"PULSE_ANIMATION_TRACE", nullptr, 0) != 0 && trace_messages++ < 256;
                if (trace) { std::fprintf(stderr, "DISPATCH begin hwnd=%p message=%u wp=%llu tick=%llu\n", static_cast<void*>(msg.hwnd), msg.message, static_cast<unsigned long long>(msg.wParam), GetTickCount64()); std::fflush(stderr); }
                TranslateMessage(&msg); DispatchMessageW(&msg);
                if (trace) { std::fprintf(stderr, "DISPATCH end message=%u tick=%llu\n", msg.message, GetTickCount64()); std::fflush(stderr); }
            }
            Sleep(1);
        }
    }
    static bool CapturePresentedFrame(pulse::ui::Compositor& compositor, const std::wstring& path) {
        auto* source = compositor.HeldFrame();
        if (!source) return false;
        const auto size = source->GetPixelSize();
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> readback;
        const auto properties = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_CPU_READ | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            source->GetPixelFormat());
        HRESULT result = compositor.Dc()->CreateBitmap(size, nullptr, 0, &properties, &readback);
        if (SUCCEEDED(result)) result = readback->CopyFromBitmap(nullptr, source, nullptr);
        D2D1_MAPPED_RECT mapped{};
        if (FAILED(result) || FAILED(readback->Map(D2D1_MAP_OPTIONS_READ, &mapped))) return false;
        Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
        Microsoft::WRL::ComPtr<IWICBitmap> bitmap;
        Microsoft::WRL::ComPtr<IWICStream> stream;
        Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
        Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
        result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (SUCCEEDED(result)) result = factory->CreateBitmapFromMemory(size.width, size.height, GUID_WICPixelFormat32bppPBGRA,
            mapped.pitch, mapped.pitch * size.height, mapped.bits, &bitmap);
        if (SUCCEEDED(result)) result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) result = stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE);
        if (SUCCEEDED(result)) result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (SUCCEEDED(result)) result = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
        if (SUCCEEDED(result)) result = frame->WriteSource(bitmap.Get(), nullptr);
        if (SUCCEEDED(result)) result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();
        size_t colored = 0;
        for (UINT y = 0; y < size.height; ++y) for (UINT x = 0; x < size.width; ++x) {
            const auto* pixel = mapped.bits + static_cast<size_t>(y) * mapped.pitch + x * 4;
            if (pixel[0] + pixel[1] + pixel[2] > 30) ++colored;
        }
        std::printf("captured_nonblack_pixels=%zu\n", colored);
        readback->Unmap();
        return SUCCEEDED(result) && colored > 1000;
    }
    static int Run(const wchar_t* output, bool visual_only = false) {
        // Real production window proc/render/lifecycle; no query clients or catalogue are started.
        State state;
        state.animations = ui::motion::SystemAnimationsEnabled();
        state.plate_motion.SetAnimationsEnabled(state.animations); state.scope_motion.SetAnimationsEnabled(state.animations);
        std::printf("search_system_animations=%d high_contrast=%d\n",state.animations,ui::IsHighContrast());
        state.context.show_apps = false;
        state.scale = 1;
        state.current_folder = L"C:\\PulseAnimationFixture";
        WNDCLASSW cls{}; cls.lpfnWndProc = State::WindowProc; cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"PulseAnimationSearch"; RegisterClassW(&cls);
        if (!CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName, L"Isolated search",
            WS_POPUP | WS_CLIPCHILDREN, 80, 80, 960, 620, nullptr, nullptr, cls.hInstance, &state)) return 2;
        state.edit = ui::CreateChildEdit(state.hwnd);
        if (!state.edit || !state.EnsureTarget()) return 2;
        SetWindowSubclass(state.hwnd, Observe, 1, 0);
        for (int i = 0; i < 12; ++i) {
            const auto name = L"Result " + std::to_wstring(i + 1) + L".txt";
            state.rows.push_back({name, L"C:\\PulseAnimationFixture\\" + name, L"", false});
        }
        state.total = state.rows.size(); state.query = L"Result"; state.raw_query = state.query;
        state.GroupRows(); state.Layout();
        ShowWindow(state.hwnd, SW_SHOW);
        std::printf("search_visible_when_open=%d\n", IsWindowVisible(state.hwnd));
        if (!IsWindowVisible(state.hwnd) || IsIconic(state.hwnd)) return 2;
        state.appear_start = ui::motion::NowMs();
        state.compositor.CaptureNextFrame();
        state.Render();
        const bool snapshot = CapturePresentedFrame(state.compositor, output);
        state.compositor.ReleaseHeldFrame();
        if (visual_only) {
            state.compositor.CaptureNextFrame();
            SendMessageW(state.hwnd, WM_KEYDOWN, VK_DOWN, 0); state.Render();
            const bool selected_snapshot = CapturePresentedFrame(state.compositor, std::wstring(output) + L"-selected-first.png");
            state.compositor.ReleaseHeldFrame();
            const bool selection_input = state.selected == 1;
            const bool selected_motion = state.plate_motion.Active(ui::motion::NowMs());
            state.grid_view = true; state.marked_paths.push_back(state.rows[0].path);
            state.Layout(); state.compositor.CaptureNextFrame(); state.Render();
            const bool grid_snapshot = CapturePresentedFrame(state.compositor, std::wstring(output) + L"-grid-marked.png");
            state.compositor.ReleaseHeldFrame();
            const bool marked = state.IsGrid() && state.IsMarked(state.rows[0].path);
            state.Hide();
            const bool ok = ui::IsHighContrast() && snapshot && selected_snapshot && grid_snapshot &&
                selection_input && !selected_motion && marked && !IsWindowVisible(state.hwnd);
            std::printf("[%s] hc_visual_only initial=%d selected_first=%d grid_marked=%d real_down_selected=%d selected_motion=%d hidden=%d; not_a_performance_or_full_lifecycle_test\n",
                ok ? "PASS" : "FAIL", snapshot, selected_snapshot, grid_snapshot, selection_input, selected_motion, !IsWindowVisible(state.hwnd));
            return ok ? 0 : 1;
        }
        const bool opening_motion = state.plate_motion.Active(ui::motion::NowMs()) || state.scope_motion.Active(ui::motion::NowMs());
        const auto opening_frame = state.frame; visible_ticks = paint_messages = 0;
        Pump(650);
        const auto opening_frames = state.frame - opening_frame;
        const auto opening_timers = visible_ticks, opening_paints = paint_messages;
        const auto idle_first = state.frame; Pump(180); const auto opening_idle = state.frame - idle_first;
        if (ui::IsHighContrast()) state.compositor.CaptureNextFrame();
        SendMessageW(state.hwnd, WM_KEYDOWN, VK_DOWN, 0); state.Render();
        const bool selection_input = state.selected == 1;
        bool hc_snapshots = true;
        if (ui::IsHighContrast()) {
            hc_snapshots = CapturePresentedFrame(state.compositor, std::wstring(output) + L"-selected-first.png");
            state.compositor.ReleaseHeldFrame();
        }
        const bool selected_motion = state.plate_motion.Active(ui::motion::NowMs()) || state.scope_motion.Active(ui::motion::NowMs());
        Pump(450);
        const auto idle_selected = state.frame; Pump(180); const auto selection_idle = state.frame - idle_selected;
        std::printf("[MOTION] search initial_active=%d selected_active=%d opening_timers=%u opening_paints=%u opening_idle=%llu selection_idle=%llu real_down_selected=%d\n",opening_motion,selected_motion,opening_timers,opening_paints,opening_idle,selection_idle,selection_input);
        state.busy = true; state.Render();
        state.Hide();
        hidden_ticks = 0;
        Pump(350);
        const auto hidden = hidden_ticks;
        ShowWindow(state.hwnd, SW_SHOW);
        state.busy = false; state.Render();
        const auto steady_frame = state.frame;
        Pump(350);
        const auto steady_frames = state.frame - steady_frame;
        if (ui::IsHighContrast()) {
            state.grid_view = true; state.marked_paths.push_back(state.rows[0].path);
            state.Layout(); state.compositor.CaptureNextFrame(); state.Render();
            const bool grid_snapshot = CapturePresentedFrame(state.compositor, std::wstring(output) + L"-grid-marked.png");
            state.compositor.ReleaseHeldFrame();
            hc_snapshots = hc_snapshots && grid_snapshot && state.IsGrid() && state.IsMarked(state.rows[0].path);
            std::printf("[%s] hc_current_grid_marked_snapshot=%d selected=%d marked=%d\n", hc_snapshots ? "PASS" : "FAIL", grid_snapshot, state.selected, state.IsMarked(state.rows[0].path));
        }
        state.Hide();
        const bool ok = hc_snapshots && snapshot && !IsWindowVisible(state.hwnd) && state.rows.size() == 12 && state.selected >= 0 &&
            selection_input && opening_idle == 0 && selection_idle == 0 &&
            (state.animations || (!opening_motion && !selected_motion && opening_timers == 0));
        std::printf("[%s] native_search snapshot=%d opening_frames=%llu hidden_timer_ticks=%u reopened_steady_frames=%llu rows=%zu\n",
            ok ? "PASS" : "FAIL", snapshot, opening_frames, hidden, steady_frames, state.rows.size());
        return ok ? 0 : 1;
    }
};
}

namespace {
struct SwitcherProbe {
    pulse::ui::Compositor compositor;
    pulse::ui::TabMruView mru;
    pulse::ui::WindowSwitcherView switcher;
    bool cards = false, dark = false, rendered = true;
    unsigned frames = 0;
    void Draw() {
        auto* dc = compositor.Dc();
        const auto theme = pulse::ui::IsHighContrast() ? pulse::ui::MakeHighContrastTheme() : pulse::ui::MakeTheme(dark, D2D1::ColorF(0x2588EE));
        dc->BeginDraw(); dc->Clear(theme.bg);
        if (cards) switcher.Draw(dc, compositor.DwriteFactory(), compositor.TextFormat(), theme);
        else mru.Draw(dc, compositor.DwriteFactory(), compositor.TextFormat(), theme);
        rendered = SUCCEEDED(dc->EndDraw()) && SUCCEEDED(compositor.Present()) && rendered;
        ++frames;
    }
    static LRESULT CALLBACK Proc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
        auto* probe = reinterpret_cast<SwitcherProbe*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_PAINT && probe) {
            PAINTSTRUCT paint{}; BeginPaint(hwnd, &paint); probe->Draw(); EndPaint(hwnd, &paint); return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    bool Capture(const std::wstring& path) {
        compositor.CaptureNextFrame(); Draw();
        const bool ok = pulse::GlobalSearchWindowTestPeer::CapturePresentedFrame(compositor, path);
        compositor.ReleaseHeldFrame(); return ok;
    }
};
int RunSwitchers(const std::wstring& prefix) {
    WNDCLASSW cls{}; cls.lpfnWndProc = SwitcherProbe::Proc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"PulseAnimationSwitcherProbe"; RegisterClassW(&cls);
    bool all = true;
    const bool animations = pulse::ui::motion::SystemAnimationsEnabled();
    std::printf("switcher_system_animations=%d high_contrast=%d\n",animations,pulse::ui::IsHighContrast());
    for (bool cards : {false, true}) {
        SwitcherProbe probe; probe.cards = cards; probe.dark = cards;
        HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, cls.lpszClassName,
            L"Isolated switcher", WS_POPUP, 50, 50, 1000, 680, nullptr, nullptr, cls.hInstance, nullptr);
        if (!hwnd || !probe.compositor.Init(hwnd)) return 2;
        probe.compositor.RecreateTextFormats(1.0f);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&probe));
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        const bool visible = IsWindowVisible(hwnd) && !IsIconic(hwnd);
        const auto client = D2D1::RectF(0, 0, 1000, 680);
        pulse::ui::TabMruData tabs; tabs.title = L"Recent tabs"; tabs.count = L"3 tabs";
        tabs.current = L"Current"; tabs.selected = 0;
        tabs.rows = {{L"Documents",L"C:\\Fixture"},{L"Pictures",L"C:\\Fixture"},{L"Downloads",L"C:\\Fixture"}};
        tabs.hints = {{{true,L"Ctrl+Tab"},{false,L"Switch"}}};
        pulse::ui::WindowSwitcherData windows; windows.selected = 0; windows.hint = L"Ctrl + `  Switch window";
        for (int i=0; i<3; ++i) {
            pulse::ui::WindowSwitcherCard card; card.name = L"Window " + std::to_wstring(i+1);
            card.folder = L"C:\\Fixture\\Documents"; card.tab_count=L"3 tabs";
            card.tabs={L"Documents",L"Pictures",L"Downloads"};card.active_tab=0; windows.cards.push_back(card);
        }
        auto show = [&] { if(cards) probe.switcher.Show(windows,client,1.0f,hwnd); else probe.mru.Show(tabs,client,1.0f,hwnd); };
        auto hit = [&](float x,float y) {return cards?probe.switcher.HitCard(x,y):probe.mru.HitRow(x,y);};
        auto open = [&] {return cards?probe.switcher.IsOpen():probe.mru.IsOpen();};
        auto hide = [&] {if(cards)probe.switcher.Hide();else probe.mru.Hide();};
        show();
        const std::wstring tag = cards?L"-windows":L"-mru";
        const bool first = probe.Capture(prefix+tag+L"-first.png");
        const bool motion_gate = cards ? probe.switcher.animations_ : probe.mru.animations_;
        auto spring_active = [&] {const auto now=pulse::ui::motion::NowMs();return cards ? probe.switcher.ring_.Active(now) || probe.switcher.plate_.Active(now) : probe.mru.ring_.Active(now);};
        const bool first_spring_active=spring_active();
        const auto opening = probe.frames;
        pulse::GlobalSearchWindowTestPeer::Pump(600);
        const auto opening_frames = probe.frames-opening;
        const auto opening_idle_start=probe.frames;pulse::GlobalSearchWindowTestPeer::Pump(180);
        const auto opening_idle=probe.frames-opening_idle_start;
        POINT target{-1,-1};
        for(int y=0; y<680 && target.x<0; y+=3) for(int x=0;x<1000;x+=3) if(hit(static_cast<float>(x),static_cast<float>(y))==2){target={x,y};break;}
        const bool hit_ok = target.x>=0 && hit(-1,-1)==-1;
        const bool hover_changed=cards?probe.switcher.SetHover(2):probe.mru.SetHover(2);
        const bool hover_stable=!(cards?probe.switcher.SetHover(2):probe.mru.SetHover(2));
        tabs.selected=windows.selected=2;show();
        const bool selected_first=probe.Capture(prefix+tag+L"-selected-first.png");
        const bool selected_spring_active=spring_active();
        const auto moving=probe.frames;
        pulse::GlobalSearchWindowTestPeer::Pump(600);
        const auto selection_frames=probe.frames-moving;
        const bool selected=probe.Capture(prefix+tag+L"-selected.png");
        const auto idle_start=probe.frames;pulse::GlobalSearchWindowTestPeer::Pump(200);
        const auto idle_frames=probe.frames-idle_start;
        hide();
        const bool immediate_no_hit=hit(static_cast<float>(target.x),static_cast<float>(target.y))==-1;
        const bool close_motion=animations ? open() : !open();
        pulse::GlobalSearchWindowTestPeer::Pump(350);
        const bool closed=!open();
        show();probe.Draw();hide();pulse::GlobalSearchWindowTestPeer::Pump(350);
        const bool rapid_closed=!open();
        const auto hidden_start=probe.frames;pulse::GlobalSearchWindowTestPeer::Pump(150);
        const auto hidden_frames=probe.frames-hidden_start;
        const bool ok=visible && first && selected_first && selected && hit_ok && hover_changed && hover_stable && immediate_no_hit &&
            close_motion && closed && rapid_closed && probe.rendered && opening_idle==0 && (cards || !pulse::ui::IsHighContrast() || !selected_spring_active) && (animations ? (opening_frames>0 && selection_frames>0) : (!motion_gate && !first_spring_active && !selected_spring_active)) && idle_frames==0 && hidden_frames==0;
        std::printf("[MOTION] switcher gate=%d first_spring_active=%d selected_spring_active=%d opening_idle=%u\n",motion_gate,first_spring_active,selected_spring_active,opening_idle);
        std::printf("[%s] native_switcher kind=%s visible=%d first=%d selected=%d hit=%d hover=%d stable=%d open_frames=%u selection_frames=%u idle_frames=%u close_motion=%d immediate_no_hit=%d closed=%d rapid_closed=%d hidden_frames=%u rendered=%d\n",
            ok?"PASS":"FAIL",cards?"windows":"mru",visible,first,selected,hit_ok,hover_changed,hover_stable,opening_frames,selection_frames,idle_frames,close_motion,immediate_no_hit,closed,rapid_closed,hidden_frames,probe.rendered);
        all=ok&&all;SetWindowLongPtrW(hwnd,GWLP_USERDATA,0);probe.compositor.Shutdown();DestroyWindow(hwnd);
    }
    return all?0:1;
}
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    std::printf("native_probe_start\n");
    const bool switchers = argc == 3 && std::wstring_view(argv[1]) == L"--switchers";
    const bool visual_only = argc == 3 && std::wstring_view(argv[1]) == L"--hc-visual";
    if (argc != 2 && !switchers && !visual_only) return 2;
    const auto name = L"PulseAnimationSearch_" + std::to_wstring(GetCurrentProcessId());
    HDESK original = GetThreadDesktop(GetCurrentThreadId());
    HDESK isolated = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!isolated || !SetThreadDesktop(isolated)) return 3;
    // Consume the hidden console launch's first-ShowWindow override on a fixture, not the tested popup.
    HWND bootstrap = CreateWindowExW(0, L"STATIC", L"Startup fixture", WS_POPUP, 0, 0, 1, 1,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    ShowWindow(bootstrap, SW_SHOW);
    DestroyWindow(bootstrap);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    pulse::ui::RefreshHighContrast();
    const int result = switchers ? RunSwitchers(argv[2]) : pulse::GlobalSearchWindowTestPeer::Run(visual_only ? argv[2] : argv[1], visual_only);
    CoUninitialize(); SetThreadDesktop(original); CloseDesktop(isolated);
    return result;
}
