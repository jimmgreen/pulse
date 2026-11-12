#pragma once
#include "../ui/ui_renderer.h"
#include <cmath>
#include <cstdio>

namespace pulse::ui {
// Windowless geometry/hit regression; callable from the private picker test hook.
inline int TestPickerEmbeddedLayout() {
    int failures = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", text);
        if (!ok) ++failures;
    };
    const auto close_enough = [](float a, float b) { return std::abs(a - b) < 0.1f; };
    MainRenderer renderer;
    WindowViewModel vm{};
    for (float scale : {1.0f, 1.5f, 2.0f}) {
        renderer.SetScale(scale);
        renderer.SetEmbedded(40, 100);
        renderer.SetEmbeddedBand(36);
        renderer.SetEmbeddedPickerRecent(0);
        renderer.SetEmbeddedPickerLayout(false);
        check(close_enough(renderer.ToolbarHeightDip(), 80), "legacy embedded keeps 44 DIP navigation");
        const auto legacy = renderer.ToolbarLayoutAt(1000 * scale, 0);
        const auto legacy_sidebar = renderer.SidebarRect(1000 * scale, 640 * scale);
        check(legacy.navigation[0].left >= legacy_sidebar.right && close_enough(legacy_sidebar.top, 44 * scale),
              "legacy embedded sidebar and navigation stay unchanged");
        renderer.SetEmbeddedPickerLayout(true);
        for (float width : {420.0f, 640.0f, 1000.0f}) {
            const auto bounds = D2D1::RectF(0, 0, width * scale, 640 * scale);
            const auto nav = renderer.ToolbarLayoutAt(bounds.right, 0);
            const auto sidebar = renderer.SidebarRect(bounds.right, bounds.bottom);
            check(close_enough(renderer.ToolbarHeightDip(), 82) && close_enough(sidebar.top, 122 * scale),
                  "picker hierarchy is title40 nav46 strip36 body122");
            check(nav.navigation[0].left < sidebar.right && close_enough(nav.address.top, 46 * scale) &&
                  close_enough(nav.address.bottom - nav.address.top, 34 * scale), "navigation crosses sidebar with centered 34 DIP field");
            const auto back = nav.navigation[0];
            check(renderer.HitTest(vm, bounds, (back.left + back.right) / 2, (back.top + back.bottom) / 2).region == HitTestResult::NavBack,
                  "full width navigation hit matches painted back button");
            check(renderer.HitTest(vm, bounds, 20 * scale, 100 * scale).region == HitTestResult::None,
                  "host jump strip cannot hit hidden toolbar commands or sidebar");
        }
        renderer.SetEmbeddedPickerRecent(162);
        const auto bounds = D2D1::RectF(0, 0, 1000 * scale, 640 * scale);
        const auto pane = renderer.ContentRect(bounds.right, bounds.bottom);
        const auto recent = renderer.EmbeddedPickerRecentRect(pane);
        const auto list = renderer.PaneListRect(vm.pane, pane);
        check(close_enough(renderer.SidebarRect(bounds.right, bounds.bottom).top, 122 * scale) &&
              close_enough(pane.top, 126 * scale), "recent cards do not move sidebar or pane toolbar");
        check(close_enough(recent.top, 166 * scale) && close_enough(recent.bottom, 328 * scale) &&
              close_enough(list.top, 360 * scale), "pane toolbar then recent cards then column header and rows");
        check(renderer.HitTest(vm, bounds, pane.left + 40 * scale, 220 * scale).region == HitTestResult::None,
              "recent card band cannot hit underlying rows");
        check(renderer.HitTest(vm, bounds, pane.left + 40 * scale, 340 * scale).region == HitTestResult::ColumnHeader,
              "column header hit follows recent band offset");
        renderer.SetEmbeddedPickerRecent(0);
        check(close_enough(renderer.PaneListRect(vm.pane, pane).top, 198 * scale),
              "removing recent cards restores original list offset");
    }
    MainRenderer ordinary;
    const float original = ordinary.ToolbarHeightDip();
    ordinary.SetEmbeddedPickerLayout(true);
    ordinary.SetEmbeddedPickerRecent(162);
    check(!ordinary.EmbeddedPickerLayout() && ordinary.ToolbarHeightDip() == original && ordinary.EmbeddedPickerRecentHeight() == 0,
          "picker switch does not change ordinary main window");
    return failures;
}
}

