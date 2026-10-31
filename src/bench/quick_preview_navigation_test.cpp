#include "../app/quick_preview_navigation.h"
#include <cstdio>

int main() {
    using pulse::app::QuickPreviewNavigationTarget;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", label);
        failures += !ok;
    };
    check(QuickPreviewNavigationTarget(0, 3, -1) == -1 &&
          QuickPreviewNavigationTarget(2, 3, 1) == -1,
          "first/last boundary produces no preview update target");
    check(QuickPreviewNavigationTarget(0, 1, -1) == -1 &&
          QuickPreviewNavigationTarget(0, 1, 1) == -1,
          "single-item preview never restarts on navigation");
    check(QuickPreviewNavigationTarget(0, 3, 1) == 1 &&
          QuickPreviewNavigationTarget(1, 3, -1) == 0 &&
          QuickPreviewNavigationTarget(1, 3, 1) == 2,
          "normal navigation keeps its neighboring view index");
    check(QuickPreviewNavigationTarget(-1, 0, 1) == -1 &&
          QuickPreviewNavigationTarget(0, 0, -1) == -1,
          "empty listing has no navigation target");
    check(QuickPreviewNavigationTarget(-1, 3, -1) == 0 &&
          QuickPreviewNavigationTarget(-1, 3, 1) == 1 &&
          QuickPreviewNavigationTarget(-1, 1, 1) == 0,
          "missing selection preserves existing first-row fallback");
    check(QuickPreviewNavigationTarget(0, 3, 0) == 1,
          "zero direction retains forward-navigation behavior");
    return failures ? 1 : 0;
}
