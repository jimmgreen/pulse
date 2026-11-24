#include "../ui/fluent_menu.h"
#include <cstdio>
#include <windowsx.h>

namespace pulse::ui {
struct FluentMenuTestPeer {
    static void Prepare(FluentMenu& menu, RECT anchor, int count) {
        std::vector<FluentMenuItem> items;
        for (int i = 0; i < count; ++i) {
            FluentMenuItem item;
            item.text = L"Menu command " + std::to_wstring(i);
            item.command = 501 + i;
            items.push_back(std::move(item));
        }
        menu.SetDropdownRect(anchor);
        menu.model_.SetItems(std::move(items));
        menu.model_.Layout(menu.compositor_->DwriteFactory(), menu.scale_, 200.0f);
        menu.LayoutWindow({anchor.left, anchor.bottom});
        menu.open_ = true;
        menu.animating_out_ = false;
        menu.result_ = 0;
    }
    static RECT Card(FluentMenu& menu) {
        return {menu.base_x_ + menu.kShadowMargin, menu.base_y_ + menu.kShadowMargin,
            menu.base_x_ + menu.kShadowMargin + menu.model_.WidthPx(),
            menu.base_y_ + menu.kShadowMargin + static_cast<LONG>(menu.BodyHeightPx())};
    }
    static int ReleaseAt(FluentMenu& menu, POINT screen) {
        SendMessageW(menu.hwnd_, WM_LBUTTONUP, 0,
            MAKELPARAM(screen.x - menu.base_x_, screen.y - menu.base_y_));
        return menu.result_;
    }
    static int ClickFirst(FluentMenu& menu) {
        const LPARAM point = MAKELPARAM(menu.kShadowMargin + 40,
            menu.kShadowMargin + static_cast<int>(menu.model_.RowHeightPx() / 2));
        SendMessageW(menu.hwnd_, WM_LBUTTONDOWN, MK_LBUTTON, point);
        SendMessageW(menu.hwnd_, WM_LBUTTONUP, 0, point);
        return menu.result_;
    }
    static bool Scrollable(FluentMenu& menu) { return menu.MaxScrollPx() > 0; }
};
}

int wmain() {
    using namespace pulse::ui;
    int failures = 0;
    const auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        failures += !okay;
    };
    const HRESULT ole = OleInitialize(nullptr);
    HWND owner = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, L"STATIC", L"",
        WS_POPUP, -30000, -30000, 800, 600, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    {
        Compositor compositor;
        check(owner && compositor.Init(owner), "hidden menu compositor initialized");
        if (!compositor.DwriteFactory()) return 2;
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
        const RECT work = monitor.rcWork;
        for (const float scale : {1.0f, 1.5f}) {
            compositor.RecreateTextFormats(scale);
            FluentMenu menu;
            check(menu.Create(owner, &compositor, scale), "real hidden menu created");
            for (const int position : {1, 2, 3}) {
                const LONG top = work.top + (work.bottom - work.top) * position / 4;
                const RECT anchor{work.left + 100, top, work.left + 140, top + static_cast<LONG>(32 * scale)};
                FluentMenuTestPeer::Prepare(menu, anchor, 80);
                RECT overlap{};
                const RECT card = FluentMenuTestPeer::Card(menu);
                check(!IntersectRect(&overlap, &anchor, &card),
                    "tall control menu never covers its opening button");
                check(FluentMenuTestPeer::Scrollable(menu), "oversized menu retains commands through scrolling");
                check(FluentMenuTestPeer::ReleaseAt(menu, {(anchor.left + anchor.right) / 2,
                    (anchor.top + anchor.bottom) / 2}) == 0,
                    "opening-button release cannot invoke a menu command");
                FluentMenuTestPeer::Prepare(menu, anchor, 80);
                check(FluentMenuTestPeer::ClickFirst(menu) == 501,
                    "intentional menu press and release invokes once");
                menu.Dismiss();
            }
        }
    }
    DestroyWindow(owner);
    if (SUCCEEDED(ole)) OleUninitialize();
    return failures ? 1 : 0;
}
