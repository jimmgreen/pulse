#include "../app/app_internal.h"
#include "../app/app_commands.h"
#include "../common/localization.h"
#include "../ui/ui_renderer.h"
#include <filesystem>
#include <cstdio>
#include <algorithm>
#include <vector>

void Render(pulse::AppState&);
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

namespace {
using namespace pulse;
unsigned failures = 0;
struct Stats { unsigned frames = 0, deadlines = 0; std::vector<double> ms; };
Stats* observed = nullptr;
void Check(bool ok, const char* text) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
    std::fflush(stdout);
    if (!ok) ++failures;
}
uint64_t Cpu() {
    FILETIME created{}, exited{}, kernel{}, user{};
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);
    return (uint64_t{kernel.dwHighDateTime} << 32 | kernel.dwLowDateTime) +
           (uint64_t{user.dwHighDateTime} << 32 | user.dwLowDateTime);
}
LRESULT CALLBACK Host(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* state = GetAppState(hwnd);
    if (!state) return DefWindowProcW(hwnd, msg, wp, lp);
    if (msg == WM_PAINT) {
        PAINTSTRUCT paint{};
        BeginPaint(hwnd, &paint);
        Render(*state);
        EndPaint(hwnd, &paint);
        if (observed) { ++observed->frames; observed->ms.push_back(state->lastFrameMs); }
        return 0;
    }
    if (msg == WM_FRAME_PUMP || msg == WM_TIMER) {
        if (observed && msg == WM_TIMER && wp == 0x54504C44) ++observed->deadlines;
        return WndProc(hwnd, msg, wp, lp);
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
void Pump(unsigned ms) {
    const auto end = GetTickCount64() + ms;
    do {
        MSG msg{};
        // A continuously armed frame pump can keep this queue nonempty.
        // Bound the sampling interval even while animation messages arrive.
        while (GetTickCount64() < end && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg); DispatchMessageW(&msg);
        }
        Sleep(1);
    } while (GetTickCount64() < end);
}
Stats Sample(const char* label, unsigned duration) {
    Stats stats;
    const auto cpu = Cpu(), begin = GetTickCount64();
    observed = &stats;
    Pump(duration);
    observed = nullptr;
    const auto elapsed = GetTickCount64() - begin;
    std::sort(stats.ms.begin(), stats.ms.end());
    const double p95 = stats.ms.empty() ? 0 : stats.ms[(stats.ms.size() - 1) * 95 / 100];
    std::printf("METRIC %s elapsed_ms=%llu frames=%u cpu_ms=%.3f frame_p95_ms=%.3f deadlines=%u\n",
        label, static_cast<unsigned long long>(elapsed), stats.frames,
        static_cast<double>(Cpu() - cpu) / 10000.0, p95, stats.deadlines);
    std::fflush(stdout);
    return stats;
}
void Run(const std::filesystem::path& out, float scale, bool dark) {
    const std::wstring tag = std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark" : L"-light");
    std::printf("CASE scale=%.1f dark=%d\n", scale, dark ? 1 : 0);
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true;
    s.appPrefs.persist = s.searchHistory.persist = s.places.persist = false;
    s.appPrefs.show_hints = false;
    s.scale = scale; s.darkMode = dark;
    s.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP,
        L"PulseAnimationCoreProbe", L"Pulse animation core", WS_POPUP,
        100, 100, 1200, 760, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!s.hwnd || !s.compositor.Init(s.hwnd)) { Check(false, "native compositor init"); return; }
    s.compositor.RecreateTextFormats(scale);
    s.renderer.SetCompositor(&s.compositor); s.renderer.SetScale(scale);
    s.window_tabs.EnsureDefault(); s.pane = s.window_tabs.Active()->FocusedPane();
    auto* tab = ActiveTab(s);
    const auto fixture = out / L"fixture";
    std::filesystem::create_directories(fixture);
    tab->NavigateTo(fixture.wstring()); tab->loading = false;
    auto rows = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 240; ++i) {
        fs::DirEntry entry; entry.name = L"动画性能测试文件-" + std::to_wstring(i) + L".txt";
        entry.size = 1024 + i; entry.attrs = FILE_ATTRIBUTE_NORMAL; rows->push_back(std::move(entry));
    }
    tab->snapshot = rows; tab->snapshot_path = tab->current_path;
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    SetThemeMode(s, dark ? 2 : 1);
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    // Hidden process launch must not suppress the isolated desktop's test window.
    ShowWindow(s.hwnd, SW_SHOWNOACTIVATE);
    Check(IsWindowVisible(s.hwnd) && !IsIconic(s.hwnd), "isolated native window is visible for sampling");
    Check(s.framePump.Start(s.hwnd, WM_FRAME_PUMP), "real production frame pump starts");
    Render(s); Pump(1200);
    ui::GroupWheelData group_data;
    for (int i = 0; i < 5; ++i) {
        ui::GroupWheelOption option; option.value = i; option.name = L"分组 " + std::to_wstring(i);
        group_data.options.push_back(std::move(option));
    }
    group_data.applied = 2;
    auto& wheel = s.renderer.GroupWheelPicker();
    wheel.Open(std::move(group_data), D2D1::RectF(200, 60, 300, 90),
               D2D1::RectF(0, 0, 1200, 760), scale, true);
    Render(s);
    Check(wheel.SelectedValue() == 2, "group picker starts on its actual selected value");
    Sample("group-open", 700);
    wheel.Step(1); Render(s);
    auto roll = Sample("group-user-roll", 1000);
    Check(roll.frames > 0 && wheel.SelectedValue() == 3, "user-driven group rolling and snapping remain");
    wheel.Close(); Render(s); Pump(250);
    Check(!wheel.Visible(), "group picker closes after its short fade");
    ToggleTheme(s);
    Check(s.compositor.HeldFrame() != nullptr, "theme captures a real previous frame");
    Check(s.darkMode != dark, "production theme command updates theme and accent state");
    Render(s);
    Check(s.themeReveal.start != 0, "theme transition is retained on its first frame");
    Sample("theme-first-250ms", 250);
    Check(s.themeReveal.start == 0, "theme keeps a short transition rather than a long reveal");
    Sample("theme-tail", 450);
    Check(s.themeReveal.start == 0, "theme transition releases its captured frame");
    const auto task = s.ops.CreateExternalTask(L"动画性能测试", L"fixture", fixture.wstring(), fixture.wstring());
    ops::OpStatus status;
    status.active = true; status.type = ops::OpType::Copy; status.phase = ops::OpPhase::Running;
    status.summary = L"正在复制测试文件"; status.percent = 50; status.destination_dir = fixture.wstring();
    Check(task && s.ops.UpdateExternalTask(task, status), "production task model accepts active fixture");
    Render(s); Pump(250);
    status.active = false; status.phase = ops::OpPhase::Completed; status.percent = 100;
    status.summary = L"已完成复制 1 个文件"; status.completed_items = status.total_items = 1;
    Check(s.ops.FinishExternalTask(task, ops::ExternalOutcome::Completed, status), "production completion edge");
    Render(s);
    const auto finished = GetTickCount64();
    const auto initial = s.renderer.TaskPillRect();
    Check(initial.right > initial.left && !BuildVm(s, false).status.task_text.empty(), "completed pill is visible in production renderer");
    auto pop = Sample("completion-pop", 550);
    if (ui::motion::SystemAnimationsEnabled()) Check(pop.frames > 0, "short completion animation is retained");
    Pump(250);
    for (int i = 0; i < 3; ++i) {
        auto quiet = Sample("static-hold", 1400);
        Check(quiet.frames <= 2, "static completion hold stops animation redraws");
    }
    ui::Compositor::SetFrameCopyEnabled(true); Render(s);
    Check(s.compositor.SaveSnapshot((out / (tag + L"-hold.png")).c_str()), "save actual completed surface");
    ui::Compositor::SetFrameCopyEnabled(false);
    // Wait for the real 12-second action deadline, not an accelerated mock clock.
    if (GetTickCount64() - finished < 11750) Pump(static_cast<unsigned>(11750 - (GetTickCount64() - finished)));
    auto expiry = Sample("completion-deadline", 1100);
    const auto compact = s.renderer.TaskPillRect();
    Check(compact.right - compact.left < initial.right - initial.left, "deadline compacts completed summary without input");
    ui::Compositor::SetFrameCopyEnabled(true); Render(s);
    Check(s.compositor.SaveSnapshot((out / (tag + L"-compact.png")).c_str()), "save actual compact surface");
    ui::Compositor::SetFrameCopyEnabled(false);
    auto after = Sample("after-deadline", 700);
    Check(after.frames <= 2, "after deadline remains idle");
    std::printf("DEADLINE observed=%u hold_width=%.2f compact_width=%.2f\n",
        expiry.deadlines, initial.right - initial.left, compact.right - compact.left);
    ShowWindow(s.hwnd, SW_HIDE);
    auto hidden = Sample("hidden", 300);
    Check(hidden.frames == 0, "hidden window performs no paints");
    s.framePump.Stop(); s.compositor.Shutdown();
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, 0); DestroyWindow(s.hwnd); s.hwnd = nullptr;
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto out = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(out / L"profile");
    SetEnvironmentVariableW(L"LOCALAPPDATA", (out / L"profile").c_str());
    SetEnvironmentVariableW(L"PULSE_TEST_DATA_DIR", (out / L"profile").c_str());
    const auto name = L"PulseAnimationCore-" + std::to_wstring(GetCurrentProcessId());
    HDESK desktop = CreateDesktopW(name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 3;
    OleInitialize(nullptr); l10n::Initialize(GetModuleHandleW(nullptr), L"zh-CN");
    WNDCLASSW wc{}; wc.lpfnWndProc = Host; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"PulseAnimationCoreProbe"; RegisterClassW(&wc);
    std::puts("INPUT actual Win32 production renderer/frame pump; isolated desktop; fixture status only; no user file operations");
    Run(out, 1.0f, false); Run(out, 1.5f, true);
    std::printf("RESULT failures=%u\n", failures);
    OleUninitialize(); CloseDesktop(desktop);
    return failures ? 1 : 0;
}
