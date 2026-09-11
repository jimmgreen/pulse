#ifdef PULSE_WITH_SELFTEST
#include "change_tracking_app_test.h"
#include "app_internal.h"
#include "change_time.h"
#include "../common/localization.h"
#include "../bench/change_tracking_menu_test.h"
#include <cstdio>

namespace pulse::app {
bool RunChangeTrackingAppTest() {
    bool passed = true;
    FILE* log = nullptr;
    fopen_s(&log, "bench_data/change-app-results.log", "w");
    auto check = [&](bool ok, const char* name) {
        std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", name);
        if (log) std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name);
        passed &= ok;
    };
    AppPrefs prefs;
    prefs.persist = false;
    check(!prefs.change_tracking_enabled && prefs.change_tracking_days == 7, "tracking defaults disabled / seven days");
    prefs.FromJson(L"{\"change_tracking_days\":100}");
    check(prefs.change_tracking_days == 7, "unsupported tracking window normalizes");
    check(!prefs.change_tracking_enabled, "existing preferences without a tracking choice stay disabled");
    prefs.FromJson(L"{\"change_tracking_enabled\":true}");
    check(prefs.change_tracking_enabled, "explicit user opt-in remains enabled");
    prefs.change_tracking_days = 3;
    AppPrefs roundtrip;
    roundtrip.persist = false;
    roundtrip.FromJson(prefs.ToJson());
    check(roundtrip.change_tracking_days == 3 && roundtrip.change_tracking_enabled, "tracking preferences round trip");
    const auto now = ChangeNow();
    check(ChangeSince(3, now) == now - 3 * 86400 && ChangeSince(7, now) == now - 7 * 86400,
        "rolling tracking cutoffs exact");
    check(ChangeSince(1, now) <= now && now - ChangeSince(1, now) < 26 * 3600,
        "today uses local midnight");
    auto state = std::make_unique<AppState>();
    state->isolatedTest = true;
    state->places.persist = false;
    state->appPrefs.persist = false;
    state->appPrefs.change_tracking_enabled = true;
    state->changes.enabled = true;
    state->window_tabs.NewTab(L"pulse:changes:C:\\fixture");
    state->pane = state->window_tabs.Active()->panes.front().get();
    auto& tab = *state->pane->ActiveTab();
    index::ChangeResponse result;
    result.state = index::ChangeState::Gap;
    index::ChangeRecord removed;
    removed.kind = index::ChangeKind::Deleted; removed.path = L"C:\\fixture\\gone.txt"; removed.time = now;
    index::ChangeRecord moved = removed;
    moved.kind = index::ChangeKind::MovedOut; moved.path = L"C:\\other\\new.txt"; moved.old_path = L"C:\\fixture\\old.txt";
    index::ChangeRecord live = removed;
    live.kind = index::ChangeKind::Modified; live.path = L"C:\\fixture\\live.txt";
    result.records = {removed, moved, live};
    auto pending = [&](uint32_t id) {
        tab.change_request = id;
        state->changes.details[id] = {&tab, tab.current_path, tab.view_generation, false};
    };
    pending(1);
    ++tab.view_generation;
    ApplyChangeDetails(*state, 1, result);
    check(!tab.snapshot, "stale view generation rejects detail result");
    pending(2);
    ApplyChangeDetails(*state, 2, result);
    check(tab.snapshot && tab.snapshot->size() == 3, "matching detail result populates pane");
    if (tab.snapshot && tab.snapshot->size() == 3) {
        tab.selected = {0, 1, 2};
        check(EntryFullPath(tab, 0).empty() && EntryFullPath(tab, 1).empty(), "historical records cannot be operation sources");
        check(SelectedFullPaths(tab) == std::vector<std::wstring>{live.path}, "mixed selection operations include live items only");
        check((*tab.snapshot)[1].full_path == moved.old_path && (*tab.snapshot)[1].name == L"old.txt",
            "moved-out entry displays original path");
    }
    pending(3);
    index::ChangeSummary fresh;
    fresh.path = L"C:\\fixture";
    fresh.state = index::ChangeState::Available;
    fresh.incomplete = false;
    const auto summary_key = fs::NormalizePath(L"c:\\fixture") + L"\n7";
    state->changes.summaries[summary_key] = fresh;
    ui::PaneViewModel pane;
    FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
    check(pane.title_change_badge.label.empty(), "new tracking period with no events has no history warning");
    fresh.state = index::ChangeState::Gap;
    fresh.incomplete = true;
    state->changes.summaries[summary_key] = fresh;
    FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
    check(pane.title_change_badge.label.empty(), "interruption without events stays silent");
    for (auto status : {index::ChangeState::Gap, index::ChangeState::Scanning, index::ChangeState::Offline,
                       index::ChangeState::Unavailable, index::ChangeState::NotCovered}) {
        fresh.state = status;
        state->changes.summaries[summary_key] = fresh;
        FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
        check(pane.title_change_badge.label.empty(), "background status never creates a time badge");
    }
    fresh.count = 1; fresh.counts[1] = 1; fresh.last_change = now; fresh.state = index::ChangeState::Gap;
    state->changes.summaries[summary_key] = fresh;
    FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
    check(!pane.title_change_badge.label.empty() && pane.title_change_badge.status < 2 &&
        pane.title_change_badge.tooltip.find(l10n::Get(l10n::StringId::ChangeGap)) == std::wstring::npos,
        "captured changes survive a gap without interruption wording");
    auto deleted_summary = fresh;
    deleted_summary.counts[1] = 0;
    deleted_summary.counts[static_cast<uint32_t>(index::ChangeKind::Deleted)] = 1;
    deleted_summary.has_deleted = true;
    state->changes.summaries[summary_key] = deleted_summary;
    FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
    check(pane.title_change_badge.has_deleted && !pane.title_change_badge.label.empty() &&
        pane.title_change_badge.tooltip.find(l10n::Get(l10n::StringId::ChangeDeleted)) != std::wstring::npos,
        "confirmed recycle deletion produces a time badge with minus and deletion detail");
    state->changes.summaries[summary_key] = fresh;
    state->changes.requested_paths = {fresh.path};
    state->changes.summary_request = 17;
    index::ChangeResponse summaries;
    index::ChangeSummary failed = fresh; failed.count = 0; failed.last_change = 0;
    for (auto status : {index::ChangeState::Unavailable, index::ChangeState::Scanning, index::ChangeState::Offline}) {
        failed.state = status; summaries.summaries = {failed};
        ApplyChangeSummaries(*state, 17, summaries);
        check(state->changes.summaries[summary_key].count == 1, "transient failure preserves last successful summary");
    }
    fresh.last_change = now - 8 * 86400;
    state->changes.summaries[summary_key] = fresh;
    FillChangePane(*state, tab, pane, D2D1::RectF(0, 0, 600, 400));
    check(pane.title_change_badge.label.empty(), "cached event expires even without a successful refresh");
    fresh.last_change = now; state->changes.summaries[summary_key] = fresh;
    failed.state = index::ChangeState::Available; summaries.summaries = {failed};
    ApplyChangeSummaries(*state, 16, summaries);
    check(state->changes.summaries[summary_key].count == 1, "stale summary generation cannot erase records");
    ApplyChangeSummaries(*state, 17, summaries);
    check(state->changes.summaries[summary_key].count == 0, "successful empty result clears old time marker");
    state->appPrefs.change_tracking_days = 3; state->changes.days = 3;
    fresh.last_change = now; summaries.summaries = {fresh};
    ApplyChangeSummaries(*state, 17, summaries);
    check(state->changes.summaries[summary_key].count == 0 &&
        state->changes.summaries[fs::NormalizePath(L"c:\\fixture") + L"\n3"].count == 1,
        "summary cache separates display time ranges");
    state->appPrefs.change_tracking_days = 7; state->changes.days = 7;
    const auto before_failure = tab.snapshot;
    pending(4); ApplyChangeDetails(*state, 4, {});
    check(tab.snapshot == before_failure && !tab.change_status_text.empty() &&
        tab.banner_message == l10n::Get(l10n::StringId::ChangeScope), "failed refresh preserves rows and explains stale details");
    index::ChangeResponse empty; empty.state = index::ChangeState::Gap;
    pending(5); ApplyChangeDetails(*state, 5, empty);
    check(tab.snapshot->empty() && tab.change_status_text.empty() &&
        tab.change_empty_text == l10n::Get(l10n::StringId::ChangeNoRecords), "successful empty details use recorded-changes wording");
    pending(6); ApplyChangeDetails(*state, 6, {});
    check(tab.snapshot->empty() && tab.change_empty_text == l10n::Get(l10n::StringId::ChangeReadFailed),
        "first read failure is not represented as no recorded changes");
    empty.state = index::ChangeState::NotCovered;
    pending(7); ApplyChangeDetails(*state, 7, empty);
    check(tab.change_empty_text == l10n::Get(l10n::StringId::ChangeNotCovered), "unsupported location explained only in details");
    pending(3);
    const auto old = tab.snapshot;
    state->changes.enabled = false;
    ApplyChangeDetails(*state, 3, {});
    check(tab.snapshot == old, "disabled tracking ignores in-flight detail response");
    auto polling = std::make_unique<AppState>();
    polling->places.persist = false;
    polling->appPrefs.persist = false;
    polling->appPrefs.change_tracking_enabled = true;
    polling->changes.visible_paths = {L"C:\\fixture"};
    TickChangeTracking(*polling);
    const auto slow_request = polling->changes.summary_request;
    polling->changes.last_query = GetTickCount64() - 3000;
    TickChangeTracking(*polling);
    check(polling->changes.summary_pending && polling->changes.summary_request == slow_request,
        "slow summary is not invalidated by periodic polling");
    polling->changes.visible_paths = {L"C:\\other"};
    TickChangeTracking(*polling);
    check(polling->changes.summary_request != slow_request,
        "directory switch supersedes pending summary");
    RunRecentChangesMenuChecks(check);
    if (log) std::fclose(log);
    return passed;
}
}
#endif
