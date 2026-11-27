#include "../app/picker_saved_followup_policy.h"
#include "../app/listing_selection_restore.h"
#include <cstdio>
#include <memory>
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }
using namespace pulse;
namespace {
int failures = 0;
void Check(bool pass, const char* name) { std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", name); failures += !pass; }
fs::SnapshotPtr Snapshot(bool with_new = true) {
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry old; old.name = L"old.txt"; old.attrs = FILE_ATTRIBUTE_NORMAL;
    entries->push_back(old);
    if (with_new) { old.name = L"new.txt"; entries->push_back(old); }
    return entries;
}
void Init(app::Tab& tab) { tab.current_path = L"C:\\fixture"; tab.SetSnapshot(Snapshot()); tab.loading = false; tab.ClearSelection(); }
}
int main() {
    app::Tab tab;
    Init(tab);
    auto intent = app::ArmPickerSavedSelection(tab, false);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) == 1 && tab.IsSelected(1), "empty selection selects exactly saved file");
    Init(tab); tab.SelectOnly(0); intent = app::ArmPickerSavedSelection(tab, false);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0 && tab.IsSelected(0), "existing selection preserved");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, true);
    Check(!intent.armed, "rename or drag at submission blocks selection");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", true) < 0 && !intent.armed, "rename or drag after submission cancels intent permanently");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false); tab.SelectOnly(0);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0 && tab.IsSelected(0), "user selects while application writes");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false); tab.SelectOnly(0); tab.ClearSelection();
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0, "user selects then clears does not revive intent");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false); tab.ClearSelection();
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0, "explicit empty selection action cancels intent");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false); ++tab.view_generation;
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0, "navigate away and back invalidates generation");
    Init(tab); tab.loading = true; intent = app::ArmPickerSavedSelection(tab, false);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0 && intent.armed, "loading waits without selecting stale snapshot");
    tab.SetSnapshot(Snapshot()); tab.loading = false;
    app::ListingSelectionRestore restore;
    app::RestoreListingSelection(tab, restore);
    Check(tab.HasFocusHint() && app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) == 1,
        "production listing restore focus revision permits loaded save selection");
    Init(tab); tab.loading = true; intent = app::ArmPickerSavedSelection(tab, false);
    tab.SelectOnly(0); tab.ClearSelection(); tab.SetSnapshot(Snapshot()); tab.loading = false;
    app::RestoreListingSelection(tab, restore);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0,
        "production refresh does not hide intervening user actions");
    Init(tab); intent = app::ArmPickerSavedSelection(tab, false); tab.pending_selected_names = {L"old.txt"};
    Check(app::ApplyPickerSavedSelection(tab, intent, L"new.txt", false) < 0, "existing pending navigation selection wins");
    tab.pending_selected_names.clear(); Init(tab); intent = app::ArmPickerSavedSelection(tab, false);
    Check(app::ApplyPickerSavedSelection(tab, intent, L"missing.txt", false) < 0 && intent.armed,
        "file not yet in directory snapshot remains pending");
    app::WindowTabs tabs; tabs.NewTab(L"C:\\foreground"); tabs.NewTab(L"C:\\fixture"); tabs.active = 0;
    auto* background = tabs.items[1]->ActiveFolder(); Init(*background);
    intent = app::ArmPickerSavedSelection(*background, false);
    Check(app::ApplyPickerSavedSelection(*background, intent, L"new.txt", false) == 1 && tabs.active == 0,
        "background tab selection never activates its layout");
    std::printf("RESULT failures=%d\n", failures);
    return failures ? 1 : 0;
}
