#include "../app/file_press_selection.h"
#include <cstdio>

// No persistence is exercised by this in-memory fixture.
namespace pulse::app { std::wstring GetPulseDataDir() { return {}; } }

int main() {
    using namespace pulse;
    int failures = 0;
    auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        failures += !okay;
    };
    app::Tab tab;
    tab.current_path = L"C:\\file-press-fixture";
    auto entries = std::make_shared<std::vector<fs::DirEntry>>(2500);
    for (size_t i = 0; i < entries->size(); ++i) {
        (*entries)[i].name = std::to_wstring(i) + L".bin";
        (*entries)[i].full_path = tab.current_path + L"\\" + (*entries)[i].name;
    }
    tab.SetSnapshot(entries);
    tab.SelectIndices({2, 7});
    app::ListSelectionGesture gesture;
    const void* pane = &failures;
    auto arm = [&](int row) {
        gesture.Cancel();
        gesture.context = {pane, &tab, tab.snapshot.get(), tab.TreeEntries(), tab.view_generation};
        gesture.revision = tab.selection_revision;
        gesture.revision_tracked = true;
        gesture.DeferReplace(row);
        app::CaptureFilePressIdentity(gesture, tab, row);
        gesture.ArmFile();
    };
    arm(2);
    const auto pressed = gesture.file_key;
    std::weak_ptr<const void> old_snapshot = tab.snapshot;
    entries.reset();
    bool stable = true;
    for (int refresh = 0; refresh < 100; ++refresh) {
        app::CapturePendingListingSelection(tab);
        const auto pending = app::TakeListingSelection(tab, tab.current_path);
        auto next = std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot);
        // Alternate stable-index size notifications and actual reorderings.
        (*next)[0].size += 4096;
        if (refresh % 2) std::rotate(next->begin(), next->begin() + 1, next->end());
        tab.SetSnapshot(next);
        if (refresh == 0) check(!old_snapshot.expired(), "press retains old snapshot before rebase");
        app::RestoreListingSelection(tab, pending);
        stable = stable && app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::Valid &&
            tab.SelectedCount() == 2 && gesture.PendingFile() &&
            app::ListingSelectionKey(tab.EntryAt(static_cast<size_t>(gesture.DeferredIndex()))) == pressed;
    }
    check(old_snapshot.expired(), "successful rebase releases original snapshot");
    check(stable && gesture.file_rebases == 100,
        "100 size refreshes and reorderings retain pressed identity, multi-selection, deferred index");
    tab.SelectOnly(gesture.DeferredIndex());
    check(app::ListingSelectionKey(tab.EntryAt(static_cast<size_t>(tab.selected_index))) == pressed,
        "deferred release selects original pressed file");
    arm(tab.selected_index);
    auto deleted = std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot);
    std::erase_if(*deleted, [&](const auto& e) { return app::ListingSelectionKey(e) == pressed; });
    tab.SetSnapshot(deleted);
    check(app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::SourceMissing,
        "source deletion cancels without dragging replacement row");
    tab.SelectOnly(0);
    arm(0);
    tab.SelectOnly(1);
    check(app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::SelectionChanged,
        "selection change without snapshot replacement cancels");
    arm(1);
    auto changed = std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot);
    tab.SetSnapshot(changed);
    tab.SelectOnly(2);
    check(app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::SelectionChanged,
        "refresh cannot replace selected payload identity");
    arm(2);
    ++tab.view_generation;
    check(app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::ContextChanged,
        "navigation generation cancels");
    arm(2);
    tab.current_path = L"C:\\other";
    check(app::MatchFilePress(gesture, pane, &tab) == app::FilePressMatch::ContextChanged,
        "directory change cancels even without snapshot publication");
    arm(2);
    app::Tab other;
    check(app::MatchFilePress(gesture, pane, &other) == app::FilePressMatch::ContextChanged &&
        app::MatchFilePress(gesture, &other, &tab) == app::FilePressMatch::ContextChanged,
        "tab and pane switches cancel");
    changed.reset();
    deleted.reset();
    arm(2);
    old_snapshot = tab.snapshot;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot));
    check(!old_snapshot.expired(), "active gesture retains latest captured snapshot");
    gesture.EndFile();
    check(old_snapshot.expired(), "EndFile releases retained snapshot");
    arm(2);
    old_snapshot = tab.snapshot;
    tab.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>(*tab.snapshot));
    gesture.Cancel();
    check(old_snapshot.expired(), "Cancel releases retained snapshot");
    return failures ? 1 : 0;
}
