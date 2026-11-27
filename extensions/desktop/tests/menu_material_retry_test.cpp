#include "ui/fluent_menu.h"
#include <cstdio>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static bool Init(FluentMenu& menu) { return menu.EnsureWindow(); }
    static HWND Window(const FluentMenu& menu) { return menu.hwnd_; }
    static bool Pending(const FluentMenu& menu) { return menu.material_retry_pending_; }
    static unsigned Budget(const FluentMenu& menu) { return menu.material_retries_left_; }
    static void Hide(FluentMenu& menu) { menu.HideRootWindow(); }
    static void Recreate(FluentMenu& menu) {
        DestroyWindow(menu.hwnd_);
        menu.hwnd_ = nullptr;
        menu.EnsureWindow();
    }
};
}

int main() {
    using pulse::ui::FluentMenu;
    using Peer = pulse::ui::FluentMenuTestPeer;
    int failed = 0;
    auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failed += !ok;
    };
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
        FluentMenu menu;
        check(Peer::Init(menu) && !IsWindowVisible(Peer::Window(menu)), "create hidden real menu window");
        menu.RequestMaterialRefresh();
        const unsigned left = Peer::Budget(menu);
        for (int i = 0; i < 1000; ++i) menu.RequestMaterialRefresh();
        check(Peer::Pending(menu) && left == 15 && Peer::Budget(menu) == left,
              "repeated material requests coalesce into one timer");
        MSG timer{};
        auto drain = [&] {
            const auto end = GetTickCount64() + 1000;
            while (GetTickCount64() < end) {
                if (PeekMessageW(&timer, Peer::Window(menu), WM_TIMER, WM_TIMER, PM_REMOVE)) {
                    DispatchMessageW(&timer);
                    return true;
                }
                Sleep(1);
            }
            return false;
        };
        check(drain() && !Peer::Pending(menu), "closed menu timer drains without rendering or rescheduling");
        for (int i = 0; i < 15; ++i) {
            menu.RequestMaterialRefresh();
            if (!drain()) { check(false, "bounded timer fires"); break; }
        }
        menu.RequestMaterialRefresh();
        check(!Peer::Pending(menu) && Peer::Budget(menu) == 0, "material retry budget stops after sixteen attempts");
        menu.SetMaterialPainter({});
        menu.RequestMaterialRefresh();
        check(Peer::Pending(menu), "new painter resets retry budget");
        Peer::Hide(menu);
        check(!Peer::Pending(menu) && Peer::Budget(menu) == 16,
              "closing menu cancels timer and restores next-open budget");
        menu.RequestMaterialRefresh();
        Peer::Recreate(menu);
        check(!Peer::Pending(menu), "window destruction clears stale pending timer state");
        menu.RequestMaterialRefresh();
        check(drain() && !Peer::Pending(menu), "recreated menu can schedule and drain material retry");
    }
    CoUninitialize();
    std::printf("MENU_MATERIAL_RETRY failed=%d\n", failed);
    return failed ? 1 : 0;
}
