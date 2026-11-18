#include "../app/list_selection_gesture.h"
#include <cstdio>

int main() {
    using pulse::app::ListSelectionGesture;
    int failures = 0;
    const auto check = [&](bool okay, const char* label) {
        std::printf("[%s] %s\n", okay ? "PASS" : "FAIL", label);
        failures += !okay;
    };
    ListSelectionGesture gesture;
    gesture.ActivateBand();
    check(!gesture.BandActive(), "idle cannot activate a band");
    gesture.ArmBand();
    gesture.row_index = 7;
    gesture.ActivateBand();
    check(gesture.BandActive() && !gesture.PendingBand() && !gesture.PendingFile(), "pending band activates exclusively");
    gesture.EndFile();
    check(gesture.BandActive(), "file end cannot terminate a band");
    gesture.ArmFile();
    gesture.ActivateBand();
    check(gesture.PendingFile() && !gesture.BandActive() && gesture.row_index == 7, "file arm is exclusive and does not erase prepared data");
    gesture.DeferToggle(7);
    gesture.EndFile();
    gesture.EndFile();
    check(!gesture.PendingFile() && !gesture.BandActive() && gesture.DeferredIndex() == -1,
        "file end is idempotent and cannot leave a deferred selection behind");
    gesture.ArmFile();
    gesture.DeferToggle(4);
    gesture.ResetBand();
    check(gesture.PendingFile() && gesture.DeferredIndex() == 4 && gesture.ToggleOnRelease(), "band reset preserves file phase and deferred click");
    gesture.ArmBand();
    gesture.DeferReplace(2);
    check(gesture.PendingBand() && !gesture.PendingFile() && !gesture.ToggleOnRelease(), "band arm excludes file and replace clears deferred toggle");
    gesture.base = {1, 2};
    gesture.before = {3, 4};
    gesture.hits = {1, 2};
    gesture.context = {&gesture, &gesture, &gesture, &gesture, 9};
    gesture.revision = 42;
    gesture.revision_tracked = gesture.auto_scroll = gesture.dirty = true;
    gesture.before_anchor = 3;
    std::unordered_set<int> selected;
    bool all = false;
    gesture.delta.Apply(selected, all, {}, false, false, 10, gesture.hits);
    gesture.Cancel();
    gesture.Cancel();
    check(!gesture.PendingBand() && !gesture.BandActive() && !gesture.PendingFile() &&
        gesture.base.empty() && gesture.before.empty() && gesture.hits.empty() &&
        gesture.context == pulse::app::MarqueeContext{} && gesture.revision == 0 &&
        !gesture.revision_tracked && !gesture.auto_scroll && !gesture.dirty &&
        gesture.before_anchor == -1 && gesture.DeferredIndex() == -1 && !gesture.ToggleOnRelease(),
        "cancel clears ownership, scratch, revision, deferred state and is idempotent");
    gesture.ArmBand();
    gesture.ActivateBand();
    gesture.hits = {6, 7};
    gesture.delta.Apply(selected, all, gesture.base, false, false, 10, gesture.hits);
    check(!all && selected == std::unordered_set<int>({6, 7}), "second partial band replaces first band rather than leaking its rows");
    gesture.hits = {7, 8};
    gesture.delta.Apply(selected, all, gesture.base, false, false, 10, gesture.hits);
    check(selected == std::unordered_set<int>({7, 8}), "second band continues with its own delta");
    gesture.hits.reserve(8192);
    gesture.base.reserve(8192);
    gesture.before.reserve(8192);
    gesture.ResetBand();
    check(gesture.hits.capacity() <= pulse::app::kMarqueeRetainedHitCapacity &&
        gesture.base.bucket_count() <= pulse::app::kMarqueeRetainedBucketCount &&
        gesture.before.bucket_count() <= pulse::app::kMarqueeRetainedBucketCount,
        "reset releases oversized scratch allocations");
    gesture.Cancel();
    gesture.ArmFile();
    gesture.context = {&gesture, &gesture, &selected, nullptr, 10};
    gesture.file_path = L"C:\\fixture";
    gesture.file_key = L"path:C:\\fixture\\a";
    gesture.file_selection = {L"a", L"b"};
    gesture.DeferReplace(0);
    bool refresh_stable = true;
    for (int refresh = 0; refresh < 100; ++refresh) {
        auto next = gesture.context;
        next.snapshot = refresh % 2 ? static_cast<const void*>(&selected) : &failures;
        refresh_stable = refresh_stable && gesture.RebaseFile(next, L"C:\\fixture", refresh % 2, {L"b", L"a"}) &&
            gesture.PendingFile() && gesture.DeferredIndex() == refresh % 2 && gesture.file_index == refresh % 2;
    }
    check(refresh_stable, "continuous refresh preserves selected identities and remaps deferred row");
    check(gesture.file_rebases == 100, "refresh count remains available for drag diagnostics");
    check(!gesture.RebaseFile(gesture.context, L"C:\\fixture", -1, {L"a", L"b"}),
        "deleted pressed source cancels rather than dragging a replacement row");
    check(!gesture.RebaseFile(gesture.context, L"C:\\fixture", 0, {L"a", L"c"}),
        "changed multi-selection cannot substitute a different drag payload");
    auto next = gesture.context;
    next.tab = &failures;
    check(!gesture.RebaseFile(next, L"C:\\fixture", 0, {L"a", L"b"}), "tab switch cancels press");
    next = gesture.context;
    ++next.generation;
    check(!gesture.RebaseFile(next, L"C:\\fixture", 0, {L"a", L"b"}), "navigation generation cancels press");
    check(!gesture.RebaseFile(gesture.context, L"C:\\other", 0, {L"a", L"b"}), "directory switch cancels press");
    gesture.Cancel();
    check(gesture.file_selection.empty() && gesture.file_key.empty() && gesture.file_rebases == 0,
        "cancel clears captured file identities");
    return failures ? 1 : 0;
}
