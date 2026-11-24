#include "app_navigation.h"
#include "app_input.h"
#include "app_ops_ui.h"
#include "operation_intent_ui.h"
#include "app_commands.h"
#include "app_hosted_edit.h"
#include "pane_column_input.h"
#include "vertical_tabs.h"
#include "entry_group.h"
#include "content_results_ui.h"
#include "content_navigation.h"
#include "tag_color_commands.h"
#include "../ui/ui_renderer.h"
#include <chrono>
#include <condition_variable>
#include <cstdio>

using namespace pulse;
namespace {
struct ProbeTaskResult {
    uint64_t task_id = 0;
    ops::OpType type = ops::OpType::Copy;
    bool success = false;
    std::vector<std::wstring> sources, moved_sources;
};
void ApplyOperationTaskResults(AppState& s, const std::vector<ProbeTaskResult>& results) {
    for (const auto& result : results) {
        if (result.success) {
            ops::CompletedOperation completed;
            completed.task_id = result.task_id; completed.type = result.type;
            completed.destinations = result.sources;
            ApplyOperationIntentCompletion(s, completed);
        }
        ops::FinishedTask finished;
        finished.task_id = result.task_id; finished.moved_sources = result.moved_sources;
        FinishOperationIntents(s, finished);
    }
}
std::vector<ProbeTaskResult> DrainProbeTasks(AppState& s) {
    static std::map<uint64_t, ProbeTaskResult> pending;
    for (const auto& completed : s.ops.DrainCompletions()) {
        if (completed.refresh_only) continue;
        auto& result = pending[completed.task_id];
        result.task_id = completed.task_id; result.type = completed.type; result.success = true;
        result.sources = completed.destinations;
    }
    std::vector<ProbeTaskResult> results;
    for (const auto& finished : s.ops.DrainFinishedTasks()) {
        auto result = pending[finished.task_id];
        result.task_id = finished.task_id; result.moved_sources = finished.moved_sources;
        pending.erase(finished.task_id); results.push_back(std::move(result));
    }
    return results;
}
}


// Called only by the isolated selection probe; no operation is dispatched.
int RunNavigationAuditProbe(AppState& s, FILE* log) {
    int failures = 0;
    const auto check = [&](bool ok, const char* name) {
        std::fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name);
        std::fflush(log);
        failures += !ok;
    };
    {
        app::WorkerPool worker;
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<app::WorkResult> results;
        const auto first = worker.LoadPaths(L"pulse:recent", {}, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto second = worker.LoadPaths(L"pulse:recent", {L"pulse:skip"}, ui::SortColumn::Name, ui::SortDirection::Asc);
        const auto cancelled = worker.LoadPaths(L"pulse:recent", {}, ui::SortColumn::Name, ui::SortDirection::Asc);
        worker.CancelGeneration(cancelled);
        worker.Start([&](app::WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            ready.notify_all();
        });
        {
            std::unique_lock lock(mutex);
            ready.wait_for(lock, std::chrono::seconds(5), [&] { return results.size() == 2; });
        }
        worker.Stop();
        const auto received = [&](uint64_t generation) {
            return std::any_of(results.begin(), results.end(), [&](const auto& result) {
                return result.generation == generation && result.snapshot && !result.cancelled;
            });
        };
        check(received(first) && received(second) && !received(cancelled),
              "overlapping virtual requests complete independently; cancelling one preserves other consumers");
    }
    auto* tab = ActiveTab(s);
    if (!tab) return failures + 1;
    const auto fixture_root = tab->current_path;
    const auto original_snapshot = tab->snapshot;
    for (int scenario = 0; scenario < 5; ++scenario) {
        auto old_rows = std::make_shared<std::vector<fs::DirEntry>>(3);
        for (int i = 0; i < 3; ++i) {
            (*old_rows)[i].name = std::wstring(1, static_cast<wchar_t>(L'a' + i));
            (*old_rows)[i].attrs = FILE_ATTRIBUTE_NORMAL;
        }
        tab->SetSnapshot(old_rows);
        tab->SelectOnly(0);
        CaptureListingSelection(*tab);
        if (scenario == 1 || scenario == 4) tab->SelectOnly(2);
        if (scenario == 2) tab->ToggleSelect(2);
        if (scenario == 3) tab->ClearSelection();
        auto next_rows = std::make_shared<std::vector<fs::DirEntry>>(*old_rows);
        std::reverse(next_rows->begin(), next_rows->end());
        if (scenario == 4) next_rows->erase(next_rows->begin());
        app::WorkResult result{};
        result.path = fixture_root;
        result.generation = 1000000 + scenario;
        result.snapshot = next_rows;
        tab->pending_generation = result.generation;
        ApplyWorkerResult(s, result);
        std::vector<std::wstring> selected;
        for (int i : tab->SelectedIndices()) selected.push_back(tab->EntryAt(i).name);
        std::sort(selected.begin(), selected.end());
        const std::vector<std::wstring> expected = scenario == 0 ? std::vector<std::wstring>{L"a"}
            : scenario == 1 ? std::vector<std::wstring>{L"c"}
            : scenario == 2 ? std::vector<std::wstring>{L"a", L"c"} : std::vector<std::wstring>{};
        check(selected == expected, "ApplyWorkerResult preserves latest selection/clear/missing identity across reordered refresh");
    }
    auto filtered_rows = std::make_shared<std::vector<fs::DirEntry>>(3);
    const wchar_t* filtered_names[] = {L"a.keep", L"b.secret", L"c.keep"};
    for (int i = 0; i < 3; ++i) {
        (*filtered_rows)[i].name = filtered_names[i];
        (*filtered_rows)[i].attrs = FILE_ATTRIBUTE_NORMAL;
    }
    tab->SetSnapshot(filtered_rows);
    tab->filter_text = L"*.keep";
    tab->view_mode = ui::ViewMode::Details;
    for (bool reverse : {false, true}) {
        tab->SelectOnly(reverse ? 2 : 0);
        HandleListRowClick(s, reverse ? 0 : 2, false, true);
        check(tab->SelectedIndices() == std::vector<int>({0, 2}),
              "Shift mouse entry selects visible range in both directions");
    }
    // Synchronize visibility preferences before establishing the keyboard anchor.
    // BuildVm legitimately clears a selection when these preferences change.
    tab->SetShowHiddenFiles(s.appPrefs.show_hidden_files);
    tab->SetShowProtectedOsFiles(s.appPrefs.show_protected_os_files);
    BYTE original_keys[256]{};
    GetKeyboardState(original_keys);
    BYTE shift_keys[256]{};
    shift_keys[VK_SHIFT] = 0x80;
    SetKeyboardState(shift_keys);
    for (UINT key : {VK_DOWN, VK_UP, VK_NEXT, VK_PRIOR, VK_HOME, VK_END}) {
        tab->SelectOnly(key == VK_UP || key == VK_PRIOR || key == VK_HOME ? 2 : 0);
        const int focus_before_vm = tab->selected_index;
        const int anchor_before_vm = tab->selection_anchor;
        const auto before_vm = BuildVm(s);
        std::fprintf(log, "[INFO] pre key=%u focus=%d/%d anchor=%d/%d rows=%zu source0=%d source1=%d\n",
            key, focus_before_vm, tab->selected_index, anchor_before_vm, tab->selection_anchor,
            before_vm.pane.EntryCount(), before_vm.pane.SourceIndex(0), before_vm.pane.SourceIndex(1));
        const auto before_keys = GetKeyState(VK_SHIFT);
        HandleKeyDown(&s, s.hwnd, WM_KEYDOWN, key, 0);
        if (tab->SelectedIndices() != std::vector<int>({0, 2})) {
            const auto vm = BuildVm(s);
            std::fprintf(log, "[INFO] key=%u shift_before=%d shift_after=%d focus=%d anchor=%d count=%d rows=%zu\n",
                key, before_keys, GetKeyState(VK_SHIFT), tab->selected_index,
                tab->selection_anchor, tab->SelectedCount(), vm.pane.EntryCount());
        }
        check(tab->SelectedIndices() == std::vector<int>({0, 2}),
              "Shift arrow/PageUp/PageDown/Home/End entry excludes filtered files");
    }
    SetKeyboardState(original_keys);
    tab->filter_text.clear();
    tab->SetSnapshot(original_snapshot);
    tab->ClearSelection();
    for (const auto* path : {L"pulse:recycle", L"pulse:saved-search:probe", L"pulse:search:probe", L"C:\\fixture"}) {
        app::Pane pane;
        pane.view.current_path = path;
        pane.view.SetSnapshot(std::make_shared<std::vector<fs::DirEntry>>());
        ui::PaneViewModel view;
        app::FillPaneViewModel(view, pane);
        const D2D1_RECT_F bounds{0, 0, 1400, 600};
        const auto old_details = pane.view.details_column_dividers;
        const auto old_search = pane.view.search_column_dividers;
        app::ResizePaneColumn(pane.view, view, s.renderer, bounds, 0, 450);
        const bool resized = view.is_search ? pane.view.search_column_dividers != old_search
                                           : pane.view.details_column_dividers != old_details;
        check(resized && (view.is_search ? pane.view.details_column_dividers == old_details
                                        : pane.view.search_column_dividers == old_search),
              "column drag writes the width storage chosen by rendered view");
        app::AutoFitPaneColumn(pane.view, view, s.renderer, bounds, 0);
        check(view.is_search ? pane.view.search_column_dividers == old_search
                            : pane.view.details_column_dividers == old_details,
              "column auto-fit resets the same visible layout storage");
    }
    // Exercise the production vertical drop entry with an isolated tab model.
    {
        auto probe = std::make_unique<AppState>();
        auto& tabs = probe->window_tabs;
        tabs.NewTab(fixture_root).pinned = true;
        tabs.NewTab(fixture_root);
        tabs.NewTab(fixture_root);
        auto* active = tabs.Active();
        auto* pinned = tabs.items.front().get();
        CommitVerticalTabDrag(*probe, L"pulse:tab:2", 0);
        check(tabs.items.front().get() == pinned && tabs.Active() == active,
              "vertical normal-tab drag cannot cross pinned prefix or change active identity");
        CommitVerticalTabDrag(*probe, L"pulse:tab:0", 3);
        check(tabs.items.front().get() == pinned, "vertical pinned-tab drag cannot cross into normal tabs");
        tabs.MoveTab(1, 2);
        check(tabs.items.front().get() == pinned, "within-region tab moves preserve pinned prefix");
    }
    for (int empty = 0; empty < 3; ++empty) {
        app::Workspace workspace;
        workspace.root = fixture_root;
        workspace.layout = static_cast<int>(app::LayoutPreset::Three);
        workspace.pane_paths = {fixture_root, fixture_root, fixture_root};
        workspace.pane_paths[empty].clear();
        workspace.pane_views = {ui::ViewMode::Details, ui::ViewMode::Content, ui::ViewMode::Details};
        s.places.workspaces.push_back(workspace);
        OpenWorkspace(s, static_cast<int>(s.places.workspaces.size() - 1));
        const auto visible = LiveLayout(s).VisiblePanes();
        bool restored = visible.size() == workspace.pane_paths.size();
        for (size_t i = 0; restored && i < visible.size(); ++i)
            restored = visible[i]->view.current_path == workspace.pane_paths[i] &&
                       visible[i]->view.view_mode == workspace.pane_views[i];
        check(restored, "OpenWorkspace restores explicit This PC and view mode in visible first/middle/last pane");
    }
    // Stop consumers before queueing a controlled shared request. Departing
    // one pane must not cancel the generation another pane still awaits.
    s.worker.Stop();
    ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical);
    auto panes = LiveLayout(s).VisiblePanes();
    if (panes.size() == 2) {
        std::mutex mutex;
        std::condition_variable ready;
        std::vector<app::WorkResult> results;
        const auto generation = s.worker.Refresh(fixture_root, ui::SortColumn::Name, ui::SortDirection::Asc);
        for (auto* pane : panes) {
            pane->view.current_path = fixture_root;
            pane->view.pending_generation = generation;
            pane->view.applied_generation = 0;
        }
        StartLoadingPath(s, panes[0]->view, L"", PathLoadReason::RestoreSession);
        s.worker.Start([&](app::WorkResult result) {
            std::lock_guard lock(mutex);
            results.push_back(std::move(result));
            ready.notify_all();
        });
        {
            std::unique_lock lock(mutex);
            ready.wait_for(lock, std::chrono::seconds(5), [&] {
                return std::any_of(results.begin(), results.end(), [&](const auto& r) { return r.generation == generation; });
            });
        }
        s.worker.Stop();
        bool received = false;
        for (auto& result : results) if (result.generation == generation) {
            received = result.snapshot && !result.cancelled;
            ApplyWorkerResult(s, result);
        }
        check(received && panes[1]->view.pending_generation == 0 && !panes[1]->view.loading,
              "one pane navigates away while the other consumes their shared directory generation");
    } else check(false, "shared-generation test requires two visible panes");
    // The worker remains stopped: only inspect admission into the discovery
    // set, never read ADS on the synthetic fixture paths.
    auto* discovery_tab = ActiveTab(s);
    if (discovery_tab) {
        auto rows = std::make_shared<std::vector<fs::DirEntry>>(300);
        for (int i = 0; i < 300; ++i) {
            wchar_t name[48]{};
            swprintf_s(name, L"%c%03d.%s", i < 100 ? L'a' : L'r', i, i % 2 ? L"skip" : L"keep");
            (*rows)[i].name = name;
            (*rows)[i].full_path = fixture_root + L"\\" + name;
            (*rows)[i].attrs = FILE_ATTRIBUTE_NORMAL;
        }
        discovery_tab->current_path = fixture_root;
        discovery_tab->loading = false;
        discovery_tab->SetSnapshot(rows);
        for (int scenario = 0; scenario < 4; ++scenario) {
            discovery_tab->view_mode = scenario < 2 ? ui::ViewMode::List
                : scenario == 2 ? ui::ViewMode::MediumIcons : ui::ViewMode::Details;
            discovery_tab->scroll_x = scenario == 0 ? 660 * s.scale : scenario == 1 ? 880 * s.scale : 0;
            discovery_tab->scroll_y = 0;
            discovery_tab->filter_text = scenario == 3 ? L"*.keep" : L"";
            discovery_tab->group_by = scenario == 3 ? static_cast<int>(app::GroupBy::Name) : 0;
            discovery_tab->collapsed_groups.clear();
            if (scenario == 3) discovery_tab->collapsed_groups[fixture_root].insert(
                app::GroupKey((*rows)[0], app::GroupBy::Name, {}));
            ++discovery_tab->group_collapse_rev;
            s.tagAdsDiscoveryQueued.clear();
            s.tagAdsDiscoveryChecked.clear();
            ui::PaneViewModel view;
            app::FillPaneViewModel(view, *s.pane, &s.places);
            const auto bounds = FocusedPaneRect(s);
            const auto [first, last] = s.renderer.VisibleRangeInPane(view, bounds);
            QueueVisibleTagDiscovery(s);
            std::unordered_set<std::wstring> expected;
            for (int row = first; row >= 0 && row <= last; ++row) {
                const int source = view.SourceIndex(row);
                if (source < 0 || (scenario == 3 && source < 100)) continue;
                expected.insert(TagDiscoveryKey((*rows)[source].full_path));
            }
            check(!expected.empty() && s.tagAdsDiscoveryQueued == expected &&
                  (scenario >= 2 || first > 0),
                  "ADS admission follows List horizontal scrolling, icon columns, or filtered expanded groups");
            if (scenario == 2) check(last - first + 1 > 2,
                "icon-grid discovery spans multiple visual columns");
            if (scenario == 3) check(!s.tagAdsDiscoveryQueued.contains(TagDiscoveryKey((*rows)[0].full_path)) &&
                !s.tagAdsDiscoveryQueued.contains(TagDiscoveryKey((*rows)[101].full_path)),
                "collapsed and filter-excluded source rows never enter ADS discovery");
        }
    } else check(false, "ADS layout test requires an active pane");
    if (auto* content_tab = ActiveTab(s)) {
        auto store = std::make_shared<index::ContentResultStore>(nullptr, 0);
        std::vector<index::ContentHit> hits;
        for (const auto* name : {L"a.keep", L"b.skip", L"c.keep"}) {
            index::ContentHit hit; hit.name = name; hit.path = fixture_root + L"\\" + name;
            hits.push_back(std::move(hit));
        }
        check(store->Append(hits), "content selection fixture appends isolated rows");
        content_tab->current_path = app::MakeSearchPath(L"content:probe");
        content_tab->content_results = store;
        content_tab->filter_text.clear(); content_tab->content_filter.clear();
        content_tab->ClearSelection();
        const auto settle = [&] {
            const auto until = GetTickCount64() + 5000;
            do {
                RefreshContentResults(s);
                CompleteContentSelection(s);
                if (!store->Filtering() && !store->Sorting() && !s.contentSelectionAction) return true;
                Sleep(5);
            } while (GetTickCount64() < until);
            return false;
        };
        RefreshContentResults(s);
        for (int scenario = 0; scenario < 4; ++scenario) {
            content_tab->filter_text = L"*.keep";
            RefreshContentResults(s);
            check(settle(), "initial content filter reaches stable view");
            std::mutex gate_mutex;
            std::condition_variable gate_ready;
            bool entered = false, released = false;
            store->Match({}, [&](auto) {
                std::unique_lock lock(gate_mutex);
                entered = true; gate_ready.notify_all();
                gate_ready.wait_for(lock, std::chrono::seconds(5), [&] { return released; });
            });
            {
                std::unique_lock lock(gate_mutex);
                gate_ready.wait_for(lock, std::chrono::seconds(5), [&] { return entered; });
            }
            check(entered, "content worker blocked before clear-filter commit");
            SelectContentPattern(s, scenario == 1 ? L"*.missing" : L"*.keep");
            if (scenario == 2) content_tab->ClearSelection();
            if (scenario == 3) SelectContentPattern(s, L"*.skip");
            {
                std::lock_guard lock(gate_mutex); released = true; gate_ready.notify_all();
            }
            check(settle() && content_tab->SelectedCount() == (scenario == 0 ? 2 : scenario == 3 ? 1 : 0),
                  "pattern waits for clear-filter commit; zero matches, changed selection and replacement stay safe");
        }
        SelectContentPattern(s, L"*.keep");
        check(settle() && content_tab->SelectedCount() == 2, "content pattern without original filter selects matches");
        content_tab->SelectOnly(0);
        content_tab->RememberContentSelection();
        const auto query = content_tab->current_path;
        app::RememberContentNavigation(*content_tab);
        auto other_store = std::make_shared<index::ContentResultStore>(nullptr, 0);
        other_store->Append({hits[1]});
        content_tab->content_results = other_store;
        content_tab->content_order_revision = store->OrderRevision() + 17;
        content_tab->content_revision = other_store->Revision();
        content_tab->content_focus_path = hits[1].path;
        content_tab->current_path = query;
        check(app::RestoreContentNavigation(*content_tab) && content_tab->content_order_revision == store->OrderRevision(),
              "restoring A after a different store restores A order rather than leftover B order");
        bool invoked = false;
        std::wstring resolved;
        const bool deferred = DeferContentSelection(s, [&](AppState& state) {
            invoked = true;
            resolved = ActiveTab(state)->EntryAt(0).full_path;
        });
        check(deferred && settle() && invoked && resolved == hits[0].path,
              "restored content selection resolves the exact original path through production defer callback");
        content_tab->content_results.reset();
        content_tab->content_navigation.clear();
        s.contentSelectionAction.reset();
    }
    for (size_t custom_colors : {3u, 4u, 5u}) {
        const size_t palette_size = 7 + custom_colors;
        bool valid = !app::IsTagColorCommand(app::kCustomTagColorCommand, palette_size) &&
                     !app::IsTagColorCommand(app::kCreateTagCommand, palette_size);
        for (size_t i = 0; i < palette_size; ++i) {
            const int command = app::kTagColorBaseCommand + static_cast<int>(i);
            valid &= app::IsTagColorCommand(command, palette_size) && command != app::kCustomTagColorCommand;
        }
        check(valid, "3/4/5 custom colors keep picker action and every swatch in independent command ranges");
    }
    {
        app::StagingTray tray;
        const auto first = fixture_root + L"\\a.keep";
        const auto second = fixture_root + L"\\b.secret";
        tray.Collect({first, second}, true);
        tray.MarkPendingMove(8001, {first, second}, 0);
        tray.Collect({first}, true);
        check(tray.batches().size() == 2 && tray.batches()[0].items.size() == 2 &&
              tray.batches()[0].items[0].inflight_task == 8001 && tray.batches()[1].items[0].inflight_task == 0,
              "tray retains queued move cards and does not claim later identical collection");
        tray.MarkPendingMove(8002, {first}, 0);
        check(tray.batches()[0].items[0].inflight_task == 8001,
              "second release cannot steal an in-flight tray item");
        tray.CompleteMove(8001, {first});
        check(tray.batches().size() == 2 && tray.batches()[0].items.size() == 1 &&
              tray.batches()[0].items[0].path == fs::NormalizePath(second) &&
              tray.batches()[0].items[0].inflight_task == 0 && tray.batches()[1].items.size() == 1,
              "partial move consumes confirmed original item only and releases failed item for retry");
        tray.MarkPendingMove(8003, {second}, 0);
        tray.CompleteMove(8003, {});
        check(tray.batches()[0].items[0].inflight_task == 0,
              "cancelled or skipped move retains original card for retry");
        tray.CompleteMove(8001, {first, second});
        check(tray.batches().size() == 2, "late duplicate terminal cannot consume newer or retriable tray cards");
        tray.MarkPendingMove(8004, {second}, 0);
        tray.CompleteMove(8004, {second});
        check(tray.batches().size() == 1 && tray.batches()[0].items[0].path == fs::NormalizePath(first),
              "successful retry removes only matching batch and preserves newer staged duplicate");
    }
    {
        ApplyLayoutPreset(s, app::LayoutPreset::TwoVertical);
        auto views = LiveLayout(s).VisiblePanes();
        if (views.size() != 2) check(false, "create-intent test requires two panes");
        else {
            auto* saved_pane = s.pane;
            auto& origin = views[0]->view;
            auto& other = views[1]->view;
            const auto parent = fs::NormalizePath(fixture_root);
            const auto elsewhere = parent + L"\\other";
            const auto created = parent + L"\\a.keep";
            origin.current_path = parent; other.current_path = elsewhere;
            origin.filter_text.clear(); other.filter_text.clear();
            origin.content_results.reset(); other.content_results.reset();
            origin.create_rename_intents.clear();
            app::QueueCreateRenameIntent(origin.create_rename_intents, 9001, parent, origin.view_generation);
            app::QueueCreateRenameIntent(origin.create_rename_intents, 9002, parent, origin.view_generation);
            s.pane = views[1];
            HideRenameOverlay(s, false);
            ProbeTaskResult success; success.task_id = 9001; success.type = ops::OpType::CreateTextFile;
            success.success = true; success.sources = {created};
            ProbeTaskResult failed; failed.task_id = 9002; failed.type = ops::OpType::CreateTextFile;
            failed.sources = {parent + L"\\failed.txt"};
            ApplyOperationTaskResults(s, {success, failed});
            check(origin.create_rename_intents.size() == 1 && origin.create_rename_intents[0].ready &&
                  other.create_rename_intents.empty(),
                  "rapid create success/failure preserves only exact successful task in originating pane");
            const auto apply = [&](app::Tab& view, const std::wstring& path, uint64_t generation) {
                auto rows = std::make_shared<std::vector<fs::DirEntry>>(2);
                (*rows)[0].name = L"b.secret"; (*rows)[1].name = L"a.keep";
                for (auto& entry : *rows) entry.attrs = FILE_ATTRIBUTE_NORMAL;
                view.pending_generation = generation; view.applied_generation = 0;
                app::WorkResult result{}; result.path = path; result.generation = generation; result.snapshot = rows;
                ApplyWorkerResult(s, result);
            };
            apply(other, elsewhere, 9101);
            check(s.renameIndex < 0 && !origin.create_rename_intents.empty(),
                  "same basename snapshot in focused other folder cannot consume create intent or open editor");
            apply(origin, parent, 9102);
            check(origin.selected_index >= 0 && origin.EntryAt(origin.selected_index).name == L"a.keep" &&
                  origin.create_rename_intents.empty() && s.renameIndex < 0,
                  "background origin selects exact successful creation without opening editor in other pane");
            origin.create_rename_intents.clear();
            app::QueueCreateRenameIntent(origin.create_rename_intents, 9003, parent, origin.view_generation);
            StartLoadingPath(s, origin, elsewhere, PathLoadReason::RestoreSession);
            success.task_id = 9003;
            ApplyOperationTaskResults(s, {success});
            check(origin.create_rename_intents.empty() && s.renameIndex < 0,
                  "navigation cancels original create-editor intent before late completion");
            s.pane = saved_pane;
        }
    }
    {
        // Real entry points and worker completion; every mutation stays under
        // this probe's explicit fixture root, with preferences non-persistent.
        const auto sandbox = fixture_root + L"\\task-lifecycle-" +
            std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64());
        const auto destination = sandbox + L"\\destination";
        const bool directories = CreateDirectoryW(sandbox.c_str(), nullptr) &&
            CreateDirectoryW(destination.c_str(), nullptr);
        auto views = LiveLayout(s).VisiblePanes();
        check(directories && views.size() == 2 && !s.appPrefs.persist,
              "real command lifecycle fixture is isolated and preferences cannot persist");
        if (directories && views.size() == 2 && !s.appPrefs.persist) {
            auto* saved_pane = s.pane;
            auto& origin = views[0]->view;
            auto& other = views[1]->view;
            origin.current_path = sandbox; origin.net_readonly = false;
            origin.create_rename_intents.clear(); origin.content_results.reset();
            other.current_path = destination; other.net_readonly = false;
            other.create_rename_intents.clear(); other.content_results.reset();
            s.pane = views[0];
            CreateNewItem(s, true);
            const auto intent = origin.create_rename_intents.empty()
                ? app::CreateRenameIntent{} : origin.create_rename_intents.back();
            check(intent.task_id != 0 && intent.parent == fs::NormalizePath(sandbox),
                  "real CreateNewItem binds returned operation task to origin and full requested path");
            s.pane = views[1];
            const auto wait_terminal = [&](uint64_t id) {
                std::optional<ProbeTaskResult> matched;
                const auto deadline = GetTickCount64() + 10000;
                while (!matched && GetTickCount64() < deadline) {
                    auto results = DrainProbeTasks(s);
                    for (const auto& result : results) if (result.task_id == id) matched = result;
                    ApplyOperationTaskResults(s, results);
                    if (!matched) Sleep(5);
                }
                return matched;
            };
            const auto created = intent.task_id ? wait_terminal(intent.task_id) : std::nullopt;
            check(created && created->success && GetFileAttributesW(created->sources.front().c_str()) != INVALID_FILE_ATTRIBUTES &&
                  origin.create_rename_intents.size() == 1 && origin.create_rename_intents.front().ready &&
                  other.create_rename_intents.empty() && s.renameIndex < 0,
                  "real Shell create completion marks only background origin intent and never opens other pane editor");
            if (created && created->success) {
                auto rows = std::make_shared<std::vector<fs::DirEntry>>(1);
                (*rows)[0].name = created->sources.front().substr(created->sources.front().find_last_of(L"\\/") + 1);
                (*rows)[0].is_dir = true; (*rows)[0].attrs = FILE_ATTRIBUTE_DIRECTORY;
                app::WorkResult result{}; result.path = sandbox; result.generation = 9201; result.snapshot = rows;
                origin.pending_generation = result.generation; origin.applied_generation = 0;
                ApplyWorkerResult(s, result);
                check(origin.selected_index == 0 && origin.create_rename_intents.empty() && s.renameIndex < 0,
                      "real created item is selected by origin refresh after focus changes");
            }
            const auto source = sandbox + L"\\move-me.txt";
            HANDLE file = CreateFileW(source.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
            const bool wrote = file != INVALID_HANDLE_VALUE;
            if (wrote) CloseHandle(file);
            s.tray.Clear();
            if (wrote) s.tray.Collect({source}, true);
            ReleaseTrayBatch(s, 0);
            const uint64_t move_task = s.tray.batches().empty() ? 0 : s.tray.batches()[0].items[0].inflight_task;
            check(wrote && move_task != 0 && s.tray.batches().size() == 1,
                  "real ReleaseTrayBatch retains move card after enqueue and associates returned task");
            const auto moved = move_task ? wait_terminal(move_task) : std::nullopt;
            check(moved && moved->success && s.tray.batches().empty() &&
                  GetFileAttributesW((destination + L"\\move-me.txt").c_str()) != INVALID_FILE_ATTRIBUTES &&
                  GetFileAttributesW(source.c_str()) == INVALID_FILE_ATTRIBUTES,
                  "real move terminal consumes originating tray card only after successful filesystem move");
            s.pane = saved_pane;
        }
    }
    return failures;
}
