#if defined(PULSE_WITH_SELFTEST) || defined(PULSE_WITH_TEST_HOOKS)
#include "operation_intent_ui.h"
#include "app_navigation.h"
#include "app_hosted_edit.h"
#include "create_rename_intent.h"
#include "app_details_tree.h"
#include "app_commands.h"
#include "app_ops_ui.h"
#include "delete_preflight.h"
#include "arrival_select.h"
#include "../ui/ui_renderer.h"
#include <chrono>
#include "context_menu.h"
#include "../common/localization.h"
#include <fstream>
#include <cstdio>
#include <filesystem>

static bool RunDeletePreparationUiTest() {
    using namespace pulse;
    namespace filesystem = std::filesystem;
    filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/delete_preflight_ui_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name); fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    const auto root = filesystem::current_path() / L"bench_data" /
        (L"delete-preparation-ui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    const bool created = filesystem::create_directories(root);
    check(created && s.hwnd, "isolated fixture and owner created");
    const auto file = root / L"keep.txt";
    if (created && s.hwnd) {
        std::ofstream(file) << "must survive";
        SetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", nullptr);
        double create_total = 0, start_total = 0;
        for (int i = 0; i < 8; ++i) {
            auto begin = std::chrono::steady_clock::now();
            ui::FileOperationCallbacks callbacks;
            const bool made = s.delete_preflight_window.Create(s.hwnd, std::move(callbacks));
            if (made) {
                s.delete_preflight_window.SetPreparationOnly(true);
                s.delete_preflight_window.SetTheme(s.darkMode, s.accentColor);
                ops::OpStatus status;
                status.active = true;
                status.can_pause = false;
                status.type = ops::OpType::RecycleDelete;
                status.phase = ops::OpPhase::Scanning;
                status.enumeration_complete = true;
                status.total_items = 1;
                status.source_label = file.wstring();
                s.delete_preflight_window.Update(status);
                s.delete_preflight_window.Show(false);
            }
            const double create_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            create_total += create_ms;
            check(made, "baseline synchronous preparation window created");
            s.delete_preflight_window.Destroy();
            begin = std::chrono::steady_clock::now();
            DeletePaths(s, {file.wstring()}, false);
            const double start_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begin).count();
            start_total += start_ms;
            const auto request = s.delete_preflight;
            const auto until = GetTickCount64() + 5000;
            while (request && !request->done.load() && GetTickCount64() < until) Sleep(1);
            check(request && request->done.load() && !request->failed && !request->preparation_shown &&
                !s.delete_preflight_window.IsVisible(), "fast read-only preparation finishes without constructing a window");
            // Cancel before consumption: no task is submitted and no recycle occurs.
            if (request) request->cancel.store(true);
            PollDeletePreflight(s);
            check(!s.delete_preflight, "completed request can still be canceled before submission");
            fprintf(log, "sample=%d baseline_create_ms=%.3f async_start_ms=%.3f\n", i, create_ms, start_ms);
        }
        fprintf(log, "samples=8 baseline_create_total_ms=%.3f async_start_total_ms=%.3f\n", create_total, start_total);
        SetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", L"30000");
        DeletePaths(s, {file.wstring()}, false);
        const auto slow = s.delete_preflight;
        if (slow) {
            slow->started_at = GetTickCount64();
            PollDeletePreflight(s);
            check(!slow->preparation_shown, "pending request does not construct UI inside grace period");
            // Advance only the UI deadline; the controlled probe remains pending.
            slow->started_at = GetTickCount64() - 200;
            PollDeletePreflight(s);
            check(slow->preparation_shown && s.delete_preflight_window.IsVisible(), "slow request exposes existing cancellable preparation window");
            slow->cancel.store(true);
            PollDeletePreflight(s);
            const auto until = GetTickCount64() + 5000;
            while (!slow->done.load() && GetTickCount64() < until) Sleep(1);
            check(slow->done.load() && !s.delete_preflight && !s.delete_preflight_window.IsVisible(), "slow cancellation closes UI and retires probe");
        } else check(false, "slow request started");
        SetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_DELAY_MS", nullptr);
        check(filesystem::exists(file) && filesystem::file_size(file) == 12, "all tests preserve source and never recycle");
        CancelDeletePreflight(s);
        filesystem::remove_all(root);
    }
    if (s.hwnd) DestroyWindow(s.hwnd);
    s.hwnd = nullptr;
    state.reset();
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}

namespace pulse::ui {
struct FileOperationWindowTestAccess {
    static uint64_t Task(const FileOperationWindow& window) { return window.status_.task_id; }
    static void Close(FileOperationWindow& window) { SendMessageW(window.hwnd_, WM_CLOSE, 0, 0); }
};
}

static bool RunOperationHistoryTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/operation_history_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name); fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    SetWindowLongPtrW(s.hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&s));
    const auto root = std::filesystem::current_path() / L"bench_data" /
        (L"history-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
    std::filesystem::create_directories(root);
    s.ops.Start([] {});
    const auto create = [&](const wchar_t* name) {
        ops::OpRequest request; request.type = ops::OpType::CreateTextFile;
        request.sources = {(root / name).wstring()};
        const auto task = s.ops.Submit(std::move(request));
        const auto deadline = GetTickCount64() + 10000;
        while (!s.ops.FinalStatus(task) && GetTickCount64() < deadline) Sleep(5);
        return task;
    };
    const auto first = create(L"first.txt");
    const auto second = create(L"second.txt");
    s.ops.DrainCompletions();
    check(s.ops.FinalStatus(first).has_value() && s.ops.FinalStatus(second).has_value(),
        "history contains two real completed tasks");
    check(!s.ops.UndoTask(first) && s.ops.CanUndoTask(second) &&
        std::filesystem::exists(root / L"first.txt") && std::filesystem::exists(root / L"second.txt"),
        "stale task undo never undoes the newer task");
    s.menu = std::make_unique<ui::FluentMenu>();
    const bool graphics = s.compositor.Init(s.hwnd) && s.menu->Create(s.hwnd, &s.compositor, 1.0f);
    check(graphics && s.menu->SaveDebugSnapshot(L"bench_data/operation_history_menu.png",
        BuildOperationHistoryMenu(s, s.ops.RecentStatuses())), "real completed history menu snapshot");
    check(graphics && s.menu->SaveDebugSnapshot(L"bench_data/operation_history_empty.png",
        BuildOperationHistoryMenu(s, {})), "empty history menu snapshot");
    HandleOperationHistoryAction(s, first, 0);
    UpdateOperationWindow(s, false);
    check(s.operationWindow && s.operationHistoryTaskId == first &&
        ui::FileOperationWindowTestAccess::Task(*s.operationWindow) == first,
        "historical details remain on the selected task after current-status refresh");
    HandleOperationHistoryAction(s, 0, 3);
    check(s.hiddenOperationHistory.contains(first) && s.hiddenOperationHistory.contains(second) &&
        s.ops.FinalStatus(first).has_value() && s.ops.CanUndoTask(second) &&
        std::filesystem::exists(root / L"first.txt") && std::filesystem::exists(root / L"second.txt"),
        "clear completed hides display records without deleting files or undo state");
    const auto destination = root / L"target";
    std::filesystem::create_directories(destination);
    std::ofstream(destination / L"first.txt") << "existing";
    ops::OpRequest transfer;
    transfer.type = ops::OpType::Copy;
    transfer.sources = {(root / L"first.txt").wstring()};
    transfer.dest_dir = destination.wstring();
    const auto transfer_task = s.ops.Submit(std::move(transfer));
    const auto deadline = GetTickCount64() + 10000;
    while (!s.ops.PendingConflict() && !s.ops.FinalStatus(transfer_task) && GetTickCount64() < deadline) Sleep(5);
    const auto conflict = s.ops.PendingConflict();
    check(conflict.has_value(), "real copy waits at deterministic name conflict");
    if (conflict) {
        HandleOperationStatusAction(s, 2);
        check(s.ops.Status().pause_requested, "status pill pauses the live copy");
        HandleOperationStatusAction(s, 2);
        check(!s.ops.Status().pause_requested && s.ops.Status().active, "status pill resumes the same live copy");
        HandleOperationStatusAction(s, -1);
        check(s.operationWindow && s.operationWindow->IsVisible() &&
            ui::FileOperationWindowTestAccess::Task(*s.operationWindow) == transfer_task,
            "running pill body opens the current task details");
        ui::FileOperationWindowTestAccess::Close(*s.operationWindow);
        check(!s.operationWindow->IsVisible() && s.ops.Status().active,
            "closing details only minimizes and leaves the copy active");
        s.ops.ResolveConflict(conflict->token, ops::ConflictChoice::Replace, false);
        const auto finished_by = GetTickCount64() + 10000;
        while (!s.ops.FinalStatus(transfer_task) && GetTickCount64() < finished_by) Sleep(5);
        const auto final = s.ops.FinalStatus(transfer_task);
        check(final && final->phase == ops::OpPhase::Completed,
            "minimized copy completes after conflict resolution");
    }
    s.operationWindow.reset(); s.ops.Stop();
    s.menu.reset(); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    state.reset();
    std::error_code error; std::filesystem::remove_all(root, error);
    check(!error, "isolated history fixture cleaned");
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}

static bool RunArrivalResultsTest() {
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/arrival_results_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", name); fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.window_tabs.NewTab(L"C:\\arrival-results");
    BindCurrentLayout(s);
    auto* tab = ActiveTab(s);
    ArmArrivalSelection(s, s.pane, 0, 10000);
    s.arrivalSelect.expected_only = true;
    s.arrivalSelect.until = 0;
    ExpectArrival(s, tab->current_path + L"\\copy (2).txt");
    ExpectArrival(s, L"C:\\another-folder\\unrelated.txt");
    check(!ApplyArrivalSelection(s, *s.pane, *tab, true) &&
        s.arrivalSelect.expected.size() == 1, "missing snapshot retains only expected paths in the navigated folder");
    auto entries = std::make_shared<std::vector<fs::DirEntry>>();
    fs::DirEntry unrelated; unrelated.name = L"unrelated.txt";
    entries->push_back(unrelated);
    tab->SetSnapshot(entries);
    check(!ApplyArrivalSelection(s, *s.pane, *tab, true) && tab->SelectedCount() == 0 &&
        !s.arrivalSelect.expected.empty(), "partial listing does not select unrelated rows or discard pending output");
    auto arrived = std::make_shared<std::vector<fs::DirEntry>>(*entries);
    fs::DirEntry output; output.name = L"copy (2).txt";
    arrived->push_back(output);
    tab->SetSnapshot(arrived);
    check(ApplyArrivalSelection(s, *s.pane, *tab, true) && tab->SelectedCount() == 1 &&
        tab->EntryAt(static_cast<size_t>(tab->selected_index)).name == output.name,
        "asynchronous listing selects only the actual keep-both output");
    tab->SelectOnly(0);
    check(!ApplyArrivalSelection(s, *s.pane, *tab, true) && s.arrivalSelect.dir.empty(),
        "manual selection cancels pending result selection");
    check(s.compositor.Init(s.hwnd), "arrival graphics initialized");
    s.renderer.SetCompositor(&s.compositor);
    s.compositor.Resize(900, 600);
    s.renderer.SetScale(1); s.compositor.RecreateTextFormats(1);
    auto many = std::make_shared<std::vector<fs::DirEntry>>();
    for (int i = 0; i < 150; ++i) {
        fs::DirEntry entry; entry.name = L"result_" + std::to_wstring(1000 + i) + L".txt";
        entry.full_path = tab->current_path + L"\\" + entry.name;
        many->push_back(std::move(entry));
    }
    tab->SetSnapshot(many); tab->ClearSelection();
    ArmArrivalSelection(s, s.pane, 0, 10000);
    ExpectArrival(s, many->at(75).full_path);
    check(ApplyArrivalSelection(s, *s.pane, *tab, true), "replacement result already in baseline is selected");
    auto centered = [&] {
        ui::PaneViewModel vm; app::FillPaneViewModel(vm, *s.pane, &s.places);
        auto bounds = FocusedPaneRect(s);
        auto list = s.renderer.PaneListRect(vm, bounds);
        auto item = s.renderer.ItemRectInPane(vm, bounds, vm.ViewIndex(tab->selected_index));
        return std::abs(item.top + item.bottom - list.top - list.bottom) < 2.0f;
    };
    check(centered(), "arrival result is centered in the actual pane");
    tab->scroll_y += 40;
    const float manual_scroll = tab->scroll_y;
    check(!ApplyArrivalSelection(s, *s.pane, *tab, true) && tab->scroll_y == manual_scroll,
        "unchanged watcher refresh preserves manual scrolling");
    s.arrivalSelect = {};
    ArmArrivalSelection(s, s.pane, 0, 10000);
    ExpectArrival(s, many->at(75).full_path);
    check(ApplyArrivalSelection(s, *s.pane, *tab, true) && centered(),
        "new operation centers a result even when already selected");
    ExpectArrival(s, many->at(76).full_path);
    check(ApplyArrivalSelection(s, *s.pane, *tab, true) && tab->SelectedCount() == 2,
        "multiple outputs stay selected together");
    tab->loading = false;
    auto vm = BuildVm(s, false);
    bool drawn = true;
    for (int frame = 0; frame < 24; ++frame) {
        s.compositor.Dc()->BeginDraw();
        s.renderer.Render(vm, D2D1::RectF(0,0,900,600), ui::MakeTheme(true, ui::HexColor(0x6474C7)));
        drawn = SUCCEEDED(s.compositor.Dc()->EndDraw()) && drawn;
        Sleep(16);
    }
    check(drawn &&
        s.compositor.SaveSnapshot(L"bench_data/arrival_center/selected.png"), "centered result screenshot saved");
    s.renderer.SetCompositor(nullptr); s.compositor.Shutdown();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    state.reset();
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}

bool RunOperationIntentUiTest() {
    if (GetEnvironmentVariableW(L"PULSE_TEST_OPERATION_HISTORY", nullptr, 0)) return RunOperationHistoryTest();
    if (GetEnvironmentVariableW(L"PULSE_TEST_ARRIVAL_RESULTS", nullptr, 0)) return RunArrivalResultsTest();
    if (GetEnvironmentVariableW(L"PULSE_TEST_DELETE_PREFLIGHT_UI", nullptr, 0)) return RunDeletePreparationUiTest();
    using namespace pulse;
    std::filesystem::create_directories(L"bench_data");
    FILE* log = nullptr;
    _wfopen_s(&log, L"bench_data/m12023_operation_intent_ui_test.log", L"w");
    if (!log) return false;
    int failures = 0;
    auto check = [&](bool ok, const char* label) {
        fprintf(log, "[%s] %s\n", ok ? "PASS" : "FAIL", label); fflush(log); failures += !ok;
    };
    auto state = std::make_unique<AppState>();
    auto& s = *state;
    s.isolatedTest = true; s.shot.active = true;
    s.appPrefs.persist = s.searchHistory.persist = s.ctxMenuPrefs.persist = s.places.persist = false;
    s.hwnd = CreateWindowExW(0, L"STATIC", L"", WS_POPUP, 0, 0, 900, 600,
        nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    s.window_tabs.NewTab(L"C:\\operation-intent-origin");
    auto* origin = s.window_tabs.Active()->panes.front()->ActiveTab();
    s.window_tabs.NewTab(L"C:\\operation-intent-other");
    BindCurrentLayout(s);
    auto* other = ActiveTab(s);
    app::QueueCreateRenameIntent(origin->create_rename_intents, 71, origin->current_path, origin->view_generation);
    app::QueueCreateRenameIntent(origin->create_rename_intents, 72, origin->current_path, origin->view_generation);
    app::QueueCreateRenameIntent(other->create_rename_intents, 73, other->current_path, other->view_generation);
    ops::CompletedOperation success;
    success.type = ops::OpType::CreateTextFile; success.task_id = 71;
    success.sources = success.destinations = {origin->current_path + L"\\same.txt"};
    ApplyOperationIntentCompletion(s, success);
    auto pending = app::PendingCreateRenameIntent(origin->create_rename_intents, origin->current_path, origin->view_generation);
    const bool exact_identity = pending && pending->task_id == 71 && pending->ready &&
        pending->view_generation == origin->view_generation &&
        fs::NormalizePath(pending->parent) == fs::NormalizePath(origin->current_path) &&
        fs::NormalizePath(pending->actual_path) == fs::NormalizePath(success.destinations.front());
    if (!exact_identity) fprintf(log, "[INFO] task=%llu parent=%ls actual=%ls expected_parent=%ls expected_actual=%ls\n",
        static_cast<unsigned long long>(pending ? pending->task_id : 0),
        pending ? pending->parent.c_str() : L"<none>", pending ? pending->actual_path.c_str() : L"<none>",
        origin->current_path.c_str(), success.destinations.front().c_str());
    check(exact_identity,
        "completion arms exact created path on its unfocused owner");
    check(origin->pending_generation != 0 && other->pending_generation == 0,
        "completion queues only origin refresh even when another tab is focused");
    check(origin->SelectedCount() == 0 && other->SelectedCount() == 0 && s.renameIndex == -1,
        "completion alone cannot select a same-name item or open another tab editor");
    FinishOperationIntents(s, ops::FinishedTask{71, {}});
    check(app::PendingCreateRenameIntent(origin->create_rename_intents, origin->current_path, origin->view_generation).has_value(),
        "success remains armed when the matching terminal is processed later");
    FinishOperationIntents(s, ops::FinishedTask{72, {}});
    check(origin->create_rename_intents.size() == 1 && other->create_rename_intents.size() == 1,
        "failed terminal removes only its own unresolved request");
    success.task_id = 73; success.destinations = {origin->current_path + L"\\same.txt"};
    ApplyOperationIntentCompletion(s, success);
    check(other->create_rename_intents.empty() && other->pending_generation == 0,
        "mismatched completion parent never refreshes or arms another directory");
    s.window_tabs.CloseTab(0);
    BindCurrentLayout(s);
    success.task_id = 71;
    ApplyOperationIntentCompletion(s, success);
    FinishOperationIntents(s, ops::FinishedTask{71, {}});
    check(ActiveTab(s) == other && other->create_rename_intents.empty() && other->pending_generation == 0,
        "late success and terminal after origin close cannot affect surviving tab");

    auto listing = [&](std::initializer_list<const wchar_t*> names) {
        auto snapshot = std::make_shared<std::vector<fs::DirEntry>>();
        for (const auto* name : names) {
            fs::DirEntry entry;
            entry.name = name;
            entry.full_path = other->current_path + L"\\" + name;
            entry.attrs = FILE_ATTRIBUTE_NORMAL;
            snapshot->push_back(std::move(entry));
        }
        return snapshot;
    };
    auto deliver = [&](const fs::SnapshotPtr& snapshot) {
        app::WorkResult result{};
        result.path = other->current_path;
        result.generation = other->pending_generation;
        result.snapshot = snapshot;
        check(result.generation != 0, "editor test delivers a requested snapshot generation");
        ApplyWorkerResult(s, result);
    };
    auto edit_text = [&] {
        wchar_t value[256]{};
        if (s.hwndRenameEdit) GetWindowTextW(s.hwndRenameEdit, value, ARRAYSIZE(value));
        return std::wstring(value);
    };
    auto editing = [&](std::wstring_view name) {
        return s.renameIndex >= 0 && static_cast<size_t>(s.renameIndex) < other->EntryCount() &&
            other->EntryAt(s.renameIndex).name == name;
    };
    other->SetSnapshot(listing({L"A.txt"}));
    other->SelectOnly(0);
    ShowRenameOverlay(s);
    check(IsWindow(s.hwndRenameEdit) && s.renameIndex == 0, "production rename creates a real EDIT for A");
    if (IsWindow(s.hwndRenameEdit) && s.renameIndex == 0) {
        SetWindowTextW(s.hwndRenameEdit, L"A unfinished draft.txt");
        app::QueueCreateRenameIntent(other->create_rename_intents, 81, other->current_path, other->view_generation);
        success.task_id = 81;
        success.sources = success.destinations = {other->current_path + L"\\B.txt"};
        ApplyOperationIntentCompletion(s, success);
        const auto changed = listing({L"B.txt", L"A.txt"});
        deliver(changed);
        check(editing(L"A.txt") &&
            edit_text() == L"A unfinished draft.txt", "B completion snapshot preserves A editor target and unfinished text");
        check(other->create_rename_intents.size() == 1 && other->create_rename_intents.front().task_id == 81,
            "snapshot while A edits does not consume B create intent");
        HideRenameOverlay(s, false);
        check(s.renameIndex == -1 && other->pending_generation != 0 && other->create_rename_intents.size() == 1,
            "ending A queues refresh without opening B synchronously");
        deliver(changed);
        check(editing(L"B.txt") && edit_text() == L"B.txt" &&
            other->create_rename_intents.empty(), "fresh valid snapshot opens B with its own text and consumes only B");

        app::QueueCreateRenameIntent(other->create_rename_intents, 82, other->current_path, other->view_generation);
        app::QueueCreateRenameIntent(other->create_rename_intents, 83, other->current_path, other->view_generation);
        success.task_id = 82; success.sources = success.destinations = {other->current_path + L"\\missing.txt"};
        ApplyOperationIntentCompletion(s, success);
        success.task_id = 83; success.sources = success.destinations = {other->current_path + L"\\C.txt"};
        ApplyOperationIntentCompletion(s, success);
        HideRenameOverlay(s, false);
        deliver(listing({L"A.txt", L"B.txt", L"C.txt"}));
        check(editing(L"C.txt") && edit_text() == L"C.txt",
            "production snapshot skips missing ready item and opens later existing C");
        check(other->create_rename_intents.size() == 1 && other->create_rename_intents.front().task_id == 82,
            "opening C keeps missing request identity separate");
    }
    other->create_rename_intents.clear();
    HideRenameOverlay(s, false);
    {
        const auto fixture = std::filesystem::absolute(L"bench_data") /
            (L"group-tree-ui-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
        const bool owned = !std::filesystem::exists(fixture) && std::filesystem::create_directory(fixture);
        check(owned, "tree grouping creates an exclusive real filesystem fixture");
        if (owned) {
            const auto nested_path = fixture / L"nested";
            const auto stem = l10n::Get(l10n::StringId::NewFolder);
            const auto grouped_name = stem + L" (2)";
            std::filesystem::create_directories(nested_path / stem);
            std::filesystem::create_directories(fixture / grouped_name);
            std::ofstream(nested_path / L"a.txt") << "first";
            std::ofstream(nested_path / L"b.txt") << "second";
            std::ofstream(nested_path / stem / L"keep.txt") << "existing child";
            std::ofstream(fixture / grouped_name / L"keep.txt") << "existing upper";
            other->pending_generation = 0;
            other->current_path = fs::NormalizePath(fixture.wstring());
            ++other->view_generation;
            other->view_mode = ui::ViewMode::Details;
            auto base = std::make_shared<std::vector<fs::DirEntry>>();
            fs::EnumerateDirectory(other->current_path, *base);
            other->SetSnapshot(base);
            other->tree = std::make_shared<app::DetailsTree>();
            other->tree->base = base;
            other->tree->combined = base;
            const auto nested = fs::NormalizePath(nested_path.wstring());
            const auto target = nested + L"\\" + grouped_name;
            auto key = nested;
            CharLowerBuffW(key.data(), static_cast<DWORD>(key.size()));
            auto& folder = other->tree->folders[key];
            folder.path = nested; folder.expanded = true; folder.state = app::DetailsTreeFolder::State::Ready;
            auto original_children = std::make_shared<std::vector<fs::DirEntry>>();
            fs::EnumerateDirectory(nested, *original_children);
            folder.listing = original_children;
            SyncDetailsTrees(s);
            other->ClearSelection();
            for (size_t i = base->size(); i < other->EntryCount(); ++i) {
                const auto entry = other->EntryAt(i);
                if (entry.name == L"a.txt" || entry.name == L"b.txt") other->ToggleSelect(static_cast<int>(i));
            }
            check(other->SelectedCount() == 2, "real expanded tree selects its two child files");
            s.ops.Start([] {});
            DispatchMenuCommand(s, app::CmdGroupIntoFolder);
            const auto task = other->create_rename_intents.empty() ? 0 : other->create_rename_intents.back().task_id;
            check(task != 0 && s.groupRenameFolder == target, "production grouping command plans the child parent and conflict-free name");
            bool finished = false;
            const auto deadline = GetTickCount64() + 15000;
            while (task && !finished && GetTickCount64() < deadline) {
                auto completions = s.ops.DrainCompletions();
                auto terminals = s.ops.DrainFinishedTasks();
                // A terminal can arrive between the two drains; collect its
                // preceding completion before resolving its intent.
                auto trailing = s.ops.DrainCompletions();
                completions.insert(completions.end(), trailing.begin(), trailing.end());
                for (const auto& completion : completions) ApplyOperationIntentCompletion(s, completion);
                for (const auto& terminal : terminals) {
                    FinishOperationIntents(s, terminal);
                    finished = finished || terminal.task_id == task;
                }
                if (!finished) Sleep(5);
            }
            check(finished && std::filesystem::exists(nested_path / grouped_name / L"a.txt") &&
                std::filesystem::exists(nested_path / grouped_name / L"b.txt") &&
                !std::filesystem::exists(nested_path / L"a.txt") && !std::filesystem::exists(nested_path / L"b.txt") &&
                std::filesystem::exists(nested_path / stem / L"keep.txt") &&
                std::filesystem::exists(fixture / grouped_name / L"keep.txt") && !std::filesystem::exists(fixture / stem),
                "real OpsManager moves only into the child target and preserves both conflicting folders");
            check(folder.state == app::DetailsTreeFolder::State::Idle && other->pending_generation == 0,
                "nested completion refreshes its expanded parent without navigating the displayed root");
            auto children = std::make_shared<std::vector<fs::DirEntry>>();
            fs::EnumerateDirectory(nested, *children);
            folder.generation = 1001; folder.state = app::DetailsTreeFolder::State::Loading;
            app::WorkResult result{};
            result.path = nested; result.generation = 1001; result.snapshot = children;
            NoteDetailsTreeResult(s, result);
            SyncDetailsTrees(s);
            TryRenameDetailsTreeCreation(s);
            const auto actual = s.renameIndex >= 0 ? other->EntryAt(static_cast<size_t>(s.renameIndex)).full_path : std::wstring{};
            check(s.renameIndex >= static_cast<int>(base->size()) && actual == target &&
                edit_text() == grouped_name && other->create_rename_intents.empty(),
                "real EDIT opens the grouped child, never the same-name row at the tree root");
            HideRenameOverlay(s, false);
            other->create_rename_intents.clear();
            s.ops.Stop();
            s.watches.Stop();
            std::error_code cleanup_error;
            std::filesystem::remove_all(fixture, cleanup_error);
            check(!cleanup_error && !std::filesystem::exists(fixture), "real grouping fixture cleanup");
        }
    }
    s.worker.Stop();
    DestroyWindow(s.hwnd); s.hwnd = nullptr;
    s.hwndRenameEdit = nullptr;
    if (s.editFont) { DeleteObject(s.editFont); s.editFont = nullptr; }
    if (s.editBrush) { DeleteObject(s.editBrush); s.editBrush = nullptr; }
    fprintf(log, "[INFO] destroying isolated test state\n"); fflush(log);
    state.reset();
    fprintf(log, "[PASS] isolated test state destroyed\n"); fflush(log);
    fprintf(log, "failures=%d\n", failures); fclose(log);
    return failures == 0;
}
#endif
