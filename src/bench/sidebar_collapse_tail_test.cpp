#ifndef PULSE_SIDEBAR_PROGRESS_HEADER
#define PULSE_SIDEBAR_PROGRESS_HEADER "../ui/sidebar_collapse_motion.h"
#endif
#include PULSE_SIDEBAR_PROGRESS_HEADER
#include <cstdio>
#include <cmath>

namespace {
unsigned checks = 0, failures = 0;
void Check(bool ok, const char* name) {
    ++checks;
    if (!ok) ++failures;
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
}
void CurveChecks() {
    using namespace pulse::ui;
    const auto duration = motion::SpringSettleMs(motion::spring::Smooth);
    float worst = 0.0f;
    bool monotonic = true, early_preserved = true, continuous_reverse = true;
    for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) {
        for (const float width : {160.0f, 224.0f, 600.0f, 4000.0f}) {
            for (const float from : {0.0f, 0.37f, 0.83f, 1.0f}) {
                for (const float target : {0.0f, 1.0f}) {
                    float previous = from;
                    for (unsigned ms = 0; ms <= duration + 40; ++ms) {
                        const float value = from + (target - from) * SidebarCollapseProgress(static_cast<float>(ms));
                        monotonic &= value >= 0.0f && value <= 1.0f &&
                            (target >= from ? value + 0.000001f >= previous : value - 0.000001f <= previous);
                        if (ms == 0) continuous_reverse &= value == from;
                        if (ms == duration) worst = (std::max)(worst, std::abs(value - previous) * (width - 48.0f) * scale);
                        previous = value;
                    }
                }
            }
        }
    }
    for (unsigned ms = 0; ms <= duration - 80; ++ms)
        early_preserved &= std::abs(SidebarCollapseProgress(static_cast<float>(ms)) -
            motion::SpringProgress(motion::spring::Smooth, static_cast<float>(ms))) < 0.000001f;
    Check(monotonic, "collapse and expansion remain bounded and monotonic across widths and DPI");
    Check(continuous_reverse, "rapid reversal begins at current position without a positional jump");
    Check(early_preserved, "initial spring motion is unchanged before the final 80 ms");
    Check(SidebarCollapseProgress(0) == 0 && SidebarCollapseProgress(static_cast<float>(duration)) == 1 &&
        SidebarCollapseProgress(1000) == 1, "exact start and stable endpoint");
    Check(worst < 0.05f, "cutoff has less than 0.05 physical pixel residual even at maximum width and 200 percent DPI");
    std::printf("CURVE duration_ms=%u worst_cutoff_px=%.6f widths=160,224,600,4000 dpi=100,125,150,200\n", duration, worst);
}
}

#ifdef PULSE_SIDEBAR_NATIVE
#include "../ui/ui_renderer.h"
#include "../common/localization.h"
#include <filesystem>
namespace pulse::ui {
struct UiModuleAuditTest {
    static void Sample(MainRenderer& renderer, bool collapsed, float from, unsigned ms) {
        renderer.sidebar_collapsed_ = renderer.user_sidebar_collapsed_ = collapsed;
        renderer.collapse_from_ = from;
        renderer.collapse_value_ = from;
        renderer.collapse_start_ = motion::NowMs() - ms;
        renderer.collapse_anim_ = true;
    }
    static float Value(const MainRenderer& r) { return r.collapse_value_; }
    static unsigned Elapsed(const MainRenderer& r) { return static_cast<unsigned>(r.motion_now_ - r.collapse_start_); }
    static bool Active(const MainRenderer& r) { return r.collapse_anim_; }
};
}
int wmain(int argc, wchar_t** argv) {
    using namespace pulse::ui;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc != 2) return 2;
    const auto out = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(out);
    const auto desktop_name = L"PulseSidebarTail-" + std::to_wstring(GetCurrentProcessId());
    const auto previous = GetThreadDesktop(GetCurrentThreadId());
    const auto desktop = CreateDesktopW(desktop_name.c_str(), nullptr, nullptr, 0, GENERIC_ALL, nullptr);
    if (!desktop || !SetThreadDesktop(desktop)) return 3;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    pulse::l10n::Initialize(GetModuleHandleW(nullptr), L"en-US");
    CurveChecks();
    const HWND window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOREDIRECTIONBITMAP, L"STATIC",
        L"Pulse sidebar tail probe", WS_POPUP, 0, 0, 2600, 1500, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    Check(window != nullptr, "create isolated native window without activating user desktop");
    if (window) {
        Compositor comp;
        const bool initialized = comp.Init(window);
        Check(initialized, "initialize real D2D and DirectComposition renderer");
        if (initialized) {
            comp.SetSyncInterval(0);
            Compositor::SetFrameCopyEnabled(true);
            MainRenderer renderer;
            renderer.SetCompositor(&comp);
            WindowViewModel vm;
            vm.tray_deck.hidden = true;
            SidebarGroup group;
            group.header = L"Quick access";
            for (const auto* name : {L"Desktop", L"Downloads", L"Documents"}) {
                SidebarItem item; item.label = name; item.icon_glyph = L"\xE8B7";
                group.items.push_back(item);
            }
            vm.sidebar.push_back(group);
            bool production_match = true, settled = true, disabled = true, reversed = true;
            for (const float scale : {1.0f, 1.25f, 1.5f, 2.0f}) for (const bool dark : {false, true}) {
                const int width = static_cast<int>(1300 * scale), height = static_cast<int>(750 * scale);
                SetWindowPos(window, nullptr, 0, 0, width, height, SWP_NOZORDER | SWP_NOACTIVATE);
                comp.Resize(width, height); comp.RecreateTextFormats(scale);
                renderer.SetScale(scale); renderer.InvalidateTypography();
                const auto theme = MakeTheme(dark, D2D1::ColorF(0x0078D4));
                vm.dark = dark;
                const auto draw = [&] {
                    comp.Dc()->BeginDraw();
                    renderer.Render(vm, D2D1::RectF(0, 0, static_cast<float>(width), static_cast<float>(height)), theme);
                    const auto hr = comp.Dc()->EndDraw();
                    return SUCCEEDED(hr) && SUCCEEDED(comp.Present());
                };
                for (const bool collapse : {true, false}) {
                    for (const unsigned ms : {0u, 180u, 214u, 260u, 284u, 293u, 294u, 340u, 500u}) {
                        UiModuleAuditTest::Sample(renderer, collapse, collapse ? 0.0f : 1.0f, ms);
                        if (!draw()) { Check(false, "production frame renders and presents"); continue; }
                        const float actual = UiModuleAuditTest::Value(renderer);
                        const float expected = SidebarCollapseProgress(static_cast<float>(UiModuleAuditTest::Elapsed(renderer)));
                        production_match &= std::abs(actual - (collapse ? expected : 1.0f - expected)) < 0.000001f;
                        if (ms >= 294) settled &= !UiModuleAuditTest::Active(renderer) && actual == (collapse ? 1.0f : 0.0f);
                        if (collapse && (ms == 293 || ms == 294 || ms == 500)) {
                            const auto name = std::to_wstring(static_cast<int>(scale * 100)) + (dark ? L"-dark-" : L"-light-") + std::to_wstring(ms) + L".png";
                            Check(comp.SaveSnapshot((out / name).c_str()), "capture actual native sidebar tail frame");
                        }
                    }
                }
                UiModuleAuditTest::Sample(renderer, true, 0.0f, 150); draw();
                const float turn = UiModuleAuditTest::Value(renderer);
                const bool animate = motion::SystemAnimationsEnabled();
                renderer.SetSidebarCollapsed(false, true);
                reversed &= UiModuleAuditTest::Value(renderer) == (animate ? turn : 0.0f);
                draw();
                reversed &= UiModuleAuditTest::Value(renderer) <= turn && UiModuleAuditTest::Value(renderer) >= 0.0f;
                renderer.SetSidebarCollapsed(false, false); renderer.SetSidebarCollapsed(true, false); draw();
                disabled &= !UiModuleAuditTest::Active(renderer) && UiModuleAuditTest::Value(renderer) == 1.0f;
            }
            Check(production_match, "real production Render uses the tested finite-tail curve at all sampled timestamps");
            Check(settled, "collapse and expansion stop exactly at their endpoint without later width drift");
            Check(reversed, "production renderer handles reversal from a partially collapsed frame");
            Check(disabled, "animation-disabled collapse reaches its endpoint immediately");
            Compositor::SetFrameCopyEnabled(false);
        }
        DestroyWindow(window);
    }
    CoUninitialize(); SetThreadDesktop(previous); CloseDesktop(desktop);
    std::printf("SIDEBAR_NATIVE checks=%u failures=%u dpi=100,125,150,200 themes=light,dark desktop=private input_sent=0\n", checks, failures);
    return failures ? 1 : 0;
}
#else
int main() {
    CurveChecks();
    std::printf("SIDEBAR_CURVE checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
#endif
