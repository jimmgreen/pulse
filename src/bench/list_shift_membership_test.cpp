#include <windows.h>
#include <cstdio>
#include <cstring>
// Exercise motion deterministically without changing the user's settings.
inline BOOL TestAnimationSetting(UINT, UINT, PVOID value, UINT) {
    *static_cast<BOOL*>(value) = TRUE;
    return TRUE;
}
#define SystemParametersInfoW TestAnimationSetting
#include "../ui/ui_motion.h"
#undef SystemParametersInfoW

int main(int argc, char** argv) {
    const bool baseline = argc == 2 && std::strcmp(argv[1], "--baseline") == 0;
    using pulse::ui::motion::ListShiftMotion;
    bool ok = true;
    const auto check = [&](bool value, const char* label) {
        std::printf("[%s] %s\n", value ? "PASS" : "FAIL", label); ok &= value;
    };
    ListShiftMotion grid;
    grid.BeginFrame(1, 0, 0, 8, 1, 1000, true, 0, baseline);
    grid.Offset(4, {300, 0}); grid.EndFrame();
    grid.BeginFrame(1, 0, 0, 9, 2, 1016, true, 0, baseline);
    const auto inserted = grid.Offset(4, {0, 100});
    grid.Offset(9, {0, 0});
    check(inserted.x == 0 && inserted.y == 0, "grid insertion snaps wrapped item instead of crossing other cells");
    check(grid.Flash(9) == 1, "new grid file retains its arrival flash");
    grid.EndFrame();
    grid.BeginFrame(1, 0, 0, 8, 3, 1032, true, 0, baseline);
    const auto removed = grid.Offset(4, {300, 0});
    check(removed.x == 0 && removed.y == 0, "rapid removal does not restart a crossing grid animation");
    grid.EndFrame();
    grid.BeginFrame(1, 0, 0, 8, 4, 1048, true, 1, false);
    const auto sorted = grid.Offset(4, {0, 100});
    check(sorted.x == 300 && sorted.y == -100, "explicit grid sort retains FLIP motion");
    grid.EndFrame();
    ListShiftMotion details;
    details.BeginFrame(2, 0, 0, 8, 1, 1000, true);
    details.Offset(4, {0, 60}); details.EndFrame();
    details.BeginFrame(2, 0, 0, 9, 2, 1016, true);
    const auto row = details.Offset(4, {0, 80});
    check(row.x == 0 && row.y == -20, "details insertion retains vertical row motion");
    details.EndFrame();
    return ok ? 0 : 1;
}
