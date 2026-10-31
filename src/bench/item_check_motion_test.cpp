#include "../ui/item_check_motion.h"
#include <cstdio>
int main() {
    using pulse::ui::ItemCheckMotion;
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (!ok) ++failures;
    };
    ItemCheckMotion first, second;
    first.Begin(L"folder", 1);
    second.Begin(L"folder", 1);
    check(!first.Update(L"file", false, 1, 100, true).active, "Initial state is still");
    second.Update(L"file", false, 1, 100, true);
    check(first.Update(L"file", true, 2, 110, true).active, "Selection animates");
    check(!second.Update(L"file", false, 2, 110, true).active, "Other pane is isolated");
    check(!first.Update(L"file", false, 3, 120, true).active, "Deselect cancels");
    check(first.Update(L"file", true, 4, 130, true).progress == 0, "Reselect restarts");
    check(first.Update(L"file", true, 5, 200, true).progress == 0.5f, "Half duration");
    check(!first.Update(L"file", true, 6, 210, false).active, "Disable settles");
    check(!first.Update(L"file", true, 7, 220, true).active, "Reenable does not replay");
    first.Update(L"file", false, 8, 230, true);
    first.Update(L"file", true, 9, 240, true);
    check(!first.Update(L"file", true, 10, 380, true).active, "Completion stops frames");
    first.Begin(L"other", 11);
    check(!first.Update(L"file", true, 11, 390, true).active, "Navigation resets state");
    first.Begin(L"other", 20);
    check(!first.Update(L"file", false, 20, 400, true).active, "Expired rows initialize quietly");
    return failures ? 1 : 0;
}
