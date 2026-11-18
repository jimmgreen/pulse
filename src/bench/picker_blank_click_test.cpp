#include "../ui/picker_blank_click.h"
#include <cstdio>
#include <initializer_list>
int main() {
    using namespace pulse::ui;
    int failures = 0;
    auto check = [&](bool okay, const char* name) { printf("[%s] %s\n", okay ? "PASS" : "FAIL", name); failures += !okay; };
    pulse::app::BlankPaneClickRelease release;
    release.owns_capture = release.blank_list_hit = true;
    release.drag_width = release.drag_height = 4;
    {
        PickerBlankClick click;
        click.Begin(true, false, 1, 20, 20);
        check(!click.Release(release, 1, 20, 20), "first empty click never navigates");
        click.Begin(true, true, 1, 20, 20);
        check(click.Release(release, 1, 20, 20), "qualified blank double release navigates once");
        check(!click.Release(release, 1, 20, 20), "duplicate release never navigates");
    }
    for (int reason = 0; reason < 6; ++reason) {
        PickerBlankClick click;
        click.Begin(reason != 0, false, 1, 20, 20); click.Release(release, 1, 20, 20);
        click.Begin(true, true, 1, 20, 20);
        auto rejected = release;
        if (reason == 1) rejected.modified = true;
        if (reason == 2) rejected.marquee_active = true;
        if (reason == 3) rejected.blank_list_hit = false;
        if (reason == 4) click.Move(40, 20, 4, 4);
        check(!click.Release(rejected, reason == 5 ? 2 : 1, 20, 20),
            "file-first, modifiers, marquee, row release, drag, navigation cannot trigger back");
    }
    check(PickerBlankNavigation(0, true, true) == PickerBlankAction::None, "disabled preference is inert");
    check(PickerBlankNavigation(1, true, true) == PickerBlankAction::Back, "back uses available history");
    check(PickerBlankNavigation(1, false, true) == PickerBlankAction::Up, "back without history falls back to parent");
    check(PickerBlankNavigation(2, true, true) == PickerBlankAction::Up, "up ignores history");
    check(PickerBlankNavigation(2, false, false) == PickerBlankAction::None &&
        PickerBlankNavigation(1, false, false) == PickerBlankAction::None, "root without history stays put");
    return failures ? 1 : 0;
}
