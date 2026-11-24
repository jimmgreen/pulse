#include "../app/app_commands.h"
#include "../app/app_hosted_edit.h"
#include "../app/app_navigation.h"
#include "../app/app_state.h"
#include "../app/app_runtime.h"
#include "../app/open_routing.h"
#include "../common/localization.h"
#include "../common/runtime_log.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <functional>
#include <atomic>
#include <thread>
#include <vector>

using namespace pulse;
#include "small_files_ui_stress.inc"
#include "small_files_task_stress.inc"
namespace {
template<class State>
bool ReviewFailureRecorded(const State& state, uint64_t task) {
    if constexpr (requires { state.operationFailureNotified.contains(task); })
        return state.operationFailureNotified.contains(task);
    return false;
}
struct LiveHeartbeat {
    std::vector<ULONGLONG> gaps;
    ULONGLONG previous = 0;
    std::vector<ULONGLONG> posted_latency;
    std::atomic<bool> enabled{true};
    std::atomic<LPARAM> epoch{1};
    UINT message = 0;
    uint64_t ops_messages = 0, kick_messages = 0, paint_messages = 0;
    ULONGLONG ops_ms = 0, kick_ms = 0, max_ops_ms = 0, max_kick_ms = 0;
};
thread_local LiveHeartbeat* heartbeat = nullptr;
thread_local std::ofstream* timing_report = nullptr;
thread_local AppState* pumping_state = nullptr;
struct ProbeLaunch { AppState* state; std::wstring root; };
std::unique_ptr<ProbeLaunch> pending_launch;
LRESULT CALLBACK ObservePostedLatency(HWND hwnd, UINT message, WPARAM wp, LPARAM lp,
                                     UINT_PTR, DWORD_PTR data) {
    auto& sample = *reinterpret_cast<LiveHeartbeat*>(data);
    if (message == sample.message) {
        if (sample.enabled && lp == sample.epoch.load() && wp <= GetTickCount64())
            sample.posted_latency.push_back(GetTickCount64() - wp);
        return 0;
    }
    const auto started = GetTickCount64();
    const auto result = DefSubclassProc(hwnd, message, wp, lp);
    const auto elapsed = GetTickCount64() - started;
    if (message == WM_OPS_NOTIFY) {
        ++sample.ops_messages; sample.ops_ms += elapsed;
        sample.max_ops_ms = (std::max)(sample.max_ops_ms, elapsed);
    } else if (message == WM_UI_KICK) {
        ++sample.kick_messages; sample.kick_ms += elapsed;
        sample.max_kick_ms = (std::max)(sample.max_kick_ms, elapsed);
    } else if (message == WM_PAINT) ++sample.paint_messages;
    return result;
}
void CALLBACK SampleHeartbeat(HWND, UINT, UINT_PTR, DWORD) {
    const auto now = GetTickCount64();
    if (heartbeat && heartbeat->enabled && heartbeat->previous) heartbeat->gaps.push_back(now - heartbeat->previous);
    if (heartbeat && heartbeat->enabled) heartbeat->previous = now;
}
bool PumpUntil(const std::function<bool()>& done, ULONGLONG timeout) {
    const auto end = GetTickCount64() + timeout;
    do {
        const auto budget = GetTickCount64() + 20;
        MSG message{};
        while (GetTickCount64() < budget) {
            const auto peek_started = GetTickCount64();
            const bool found = PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE) != FALSE;
            if (timing_report && GetTickCount64() - peek_started >= 50) {
                *timing_report << "SLOW_PEEK elapsed_ms=" << GetTickCount64() - peek_started << '\n';
                timing_report->flush();
            }
            if (!found) break;
            if (message.message == WM_QUIT) return false;
            const auto dispatch_started = GetTickCount64();
            if (pumping_state) NoteUserInput(*pumping_state, message);
            TranslateMessage(&message);
            DispatchMessageW(&message);
            const auto elapsed = GetTickCount64() - dispatch_started;
            if (timing_report && elapsed >= 50) {
                *timing_report << "SLOW_DISPATCH message=" << message.message << " wparam=" << message.wParam
                               << " elapsed_ms=" << elapsed << '\n';
                timing_report->flush();
            }
        }
        const auto query_started = GetTickCount64();
        const bool complete = done();
        if (timing_report && GetTickCount64() - query_started >= 50) {
            *timing_report << "SLOW_PREDICATE elapsed_ms=" << GetTickCount64() - query_started << '\n';
            timing_report->flush();
        }
        if (complete) return true;
        Sleep(2);
    } while (GetTickCount64() < end);
    return done();
}
}

#include "small_files_tree_scenario.inc"

int RunSmallFilesLiveProbe(AppState& state, const wchar_t* fixture_root) {
    namespace disk = std::filesystem;
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    const auto expected = disk::weakly_canonical(disk::path(executable).parent_path().parent_path() /
        L"bench_data/small-files-responsive-20261119");
    const auto root = disk::weakly_canonical(fixture_root);
    if (!state.isolatedTest || root != expected || !disk::is_directory(root / L"source" / L"many")) return 2;
    ShowWindow(state.hwnd, SW_SHOWNOACTIVATE);
    const auto output = root / (L"app-live-" + std::to_wstring(GetCurrentProcessId()));
    if (!disk::create_directory(output)) return 3;
    disk::create_directory(output / L"destination");
    std::ofstream report(output / L"VERIFICATION.txt");
    // Switch only this isolated process's diagnostics before timed work starts.
    diagnostics::runtime::Shutdown();
    diagnostics::runtime::Initialize(output.wstring(), "copy_probe");
    const auto log_health = diagnostics::runtime::GetHealth();
    report << "RUNTIME_LOG enabled=" << log_health.enabled
           << " initialization_error=" << log_health.initialization_error << '\n';
    timing_report = &report;
    pumping_state = &state;
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        report << (ok ? "[PASS] " : "[FAIL] ") << label << '\n';
        report.flush();
        failures += !ok;
        return ok;
    };
    LiveHeartbeat samples;
    heartbeat = &samples;
    constexpr UINT_PTR timer = 0x53464C50;
    samples.message = RegisterWindowMessageW(L"Pulse.SmallFilesLiveProbe.PostedLatency");
    check(samples.message && SetWindowSubclass(state.hwnd, ObservePostedLatency, timer,
        reinterpret_cast<DWORD_PTR>(&samples)), "posted-message latency observer installed in actual main window");
    std::jthread poster([&](std::stop_token stop) {
        while (!stop.stop_requested()) {
            if (samples.enabled) PostMessageW(state.hwnd, samples.message, GetTickCount64(), samples.epoch.load());
            Sleep(50);
        }
    });
    check(SetTimer(state.hwnd, timer, 20, SampleHeartbeat) != 0, "real main-window heartbeat installed");
    const bool stress_enabled = GetEnvironmentVariableW(L"PULSE_TEST_SMALL_FILES_STRESS", nullptr, 0) != 0;
    SmallFilesTaskStress stress;
    stress.root = output;
    if (stress_enabled) {
        std::atomic<bool> prepared{false};
        std::jthread prepare([&] { stress.Prepare(); prepared = true; });
        check(PumpUntil([&] { return prepared.load(); }, 180000), "background stress fixture preparation finished");
        prepare.join();
        check(stress.prepared, "stress fixture preparation succeeded");
        if (!stress.preparation_error.empty()) report << "STRESS_PREPARE_ERROR " << stress.preparation_error << '\n';
    }
    const auto navigate = [&](const disk::path& path) {
        HideRenameOverlay(state, false);
        NavigateTo(state, path.wstring());
        return PumpUntil([&] {
            const auto* tab = ActiveTab(state);
            return tab && tab->current_path == fs::NormalizePath(path.wstring()) && !tab->loading && tab->snapshot;
        }, 15000);
    };
    const auto final_status = [&](uint64_t id) -> std::optional<ops::OpStatus> {
        if (!stress_enabled) return state.ops.FinalStatus(id);
        const auto status = state.ops.QueryTask(id);
        if (status && !status->active && (status->phase == ops::OpPhase::Completed || status->phase == ops::OpPhase::Failed)) return status;
        return std::nullopt;
    };
    const auto terminal = [&](uint64_t id) { return final_status(id).has_value(); };
    const auto run_creation = [&](uint64_t heavy, const char* phase) {
        auto* tab = ActiveTab(state);
        if (!tab || tab->current_path != fs::NormalizePath((output / L"destination").wstring())) {
            check(false, "new-folder command stays inside the private fixture destination");
            return;
        }
        const auto started = GetTickCount64();
        CreateNewItem(state, true); // The toolbar/Ctrl+Shift+N production entry, including KeepBoth.
        const uint64_t light = tab && !tab->create_rename_intents.empty() ? tab->create_rename_intents.back().task_id : 0;
        const bool done = light && PumpUntil([&] { return terminal(light); }, 120000);
        const auto status = light ? state.ops.FinalStatus(light) : std::nullopt;
        const auto primary = final_status(heavy);
        const bool primary_running = !primary;
        const bool before_primary = status && (!primary || status->ended_tick < primary->ended_tick);
        report << "METRIC phase=" << phase << " create_ms=" << GetTickCount64() - started
               << " primary_still_running=" << primary_running << '\n';
        check(done && status && status->phase == ops::OpPhase::Completed, "application new-folder command completed");
        check(before_primary, "new-folder command finished before bulk operation (worker timestamps)");
        PumpUntil([&] { return false; }, 200);
        report << "NOTE timed run omits synchronous GPU readback/PNG capture\n";
    };

    if (GetEnvironmentVariableW(L"PULSE_TEST_SMALL_FILES_TREE", nullptr, 0) != 0) {
        RunTreeScenario(state, root, output, report, samples, check, navigate, terminal);
        KillTimer(state.hwnd, timer);
        poster.request_stop();
        poster.join();
        RemoveWindowSubclass(state.hwnd, ObservePostedLatency, timer);
        heartbeat = nullptr;
        timing_report = nullptr;
        pumping_state = nullptr;
        report << "UI_MESSAGES ops=" << samples.ops_messages << " ops_ms=" << samples.ops_ms
               << " max_ops_ms=" << samples.max_ops_ms << " kicks=" << samples.kick_messages
               << " kick_ms=" << samples.kick_ms << " max_kick_ms=" << samples.max_kick_ms
               << " paints=" << samples.paint_messages << '\n';
        report << "RESULT failures=" << failures << '\n';
        return failures ? 1 : 0;
    }
    check(navigate(output / L"destination"), "destination loaded through application navigation");
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {(root / L"source" / L"many").wstring()};
    copy.dest_dir = (output / L"destination").wstring();
    const auto copying = state.ops.Submit(copy);
    if (stress_enabled) check(copying && state.ops.RetainTask(copying), "primary copy retained across concurrent stress tasks");
    check(copying && PumpUntil([&] {
        const auto status = state.ops.QueryTask(copying);
        return status && status->active && status->completed_items >= 100;
    }, 30000), "bulk copy active on actual filesystem");
    if (stress_enabled && stress.prepared) {
        check(!state.notification_toast.IsVisible(), "REVIEW no failure toast before injected filesystem error");
        ops::OpRequest failed_create;
        failed_create.type = ops::OpType::CreateFolder;
        failed_create.collision_policy = ops::CollisionPolicy::KeepBoth;
        failed_create.sources = {fs::NormalizePath((output / std::wstring(255, L'n')).wstring())};
        const auto failed_id = state.ops.Submit(std::move(failed_create));
        check(failed_id && state.ops.RetainTask(failed_id), "REVIEW independent failure task retained");
        check(PumpUntil([&] {
            const auto failed = state.ops.QueryTask(failed_id);
            return failed && !failed->active && failed->phase == ops::OpPhase::Failed;
        }, 5000), "REVIEW actual KeepBoth filesystem failure completed");
        check(!terminal(copying), "REVIEW failure completed while primary copy remained active");
        check(PumpUntil([&] {
            return state.notification_toast.IsVisible() && ReviewFailureRecorded(state, failed_id);
        }, 1000), "REVIEW failed secondary task produced its own visible error toast");
        report << "REVIEW failure_task=" << failed_id << " toast_visible=" << state.notification_toast.IsVisible()
               << " notified=" << ReviewFailureRecorded(state, failed_id) << '\n';
        state.ops.ReleaseTask(failed_id);
        failures += stress.Submit(state, report);
        failures += RunSmallFilesUiStress(state, output, copying, report);
        check(navigate(output / L"destination"), "returned to private destination after UI stress");
    }
    check(navigate(output / L"destination" / L"many"), "growing directory opened while bulk copy is active");
    PumpUntil([&] { return false; }, 3000);
    check(!terminal(copying), "directory-notification stress overlapped bulk copy");
    check(navigate(output / L"destination"), "navigate to sibling create destination during copy");
    run_creation(copying, "copy");
    // Exercise real watching/rendering of a growing directory, not a mock list.
    check(navigate(output / L"destination" / L"many"), "growing copied directory remains navigable");
    check(PumpUntil([&] { return terminal(copying); }, 180000), "bulk copy reaches terminal status");
    const auto copy_status = final_status(copying);
    check(copy_status && copy_status->phase == ops::OpPhase::Completed, "bulk copy succeeded");
    if (stress_enabled && stress.prepared) failures += stress.CheckFinished(state, copying, report);
    size_t expected_files = 0, actual_files = 0, mismatches = 0;
    std::unordered_set<std::wstring> expected_names;
    std::atomic<bool> verification_done{false};
    std::string verification_error;
    bool stress_files_ok = false;
    std::jthread verifier([&](std::stop_token stop) {
        try {
            for (const auto& entry : disk::directory_iterator(root / L"source" / L"many")) {
                if (stop.stop_requested()) break;
                ++expected_files;
                expected_names.insert(entry.path().filename().wstring());
                std::ifstream a(entry.path(), std::ios::binary), b(output / L"destination" / L"many" / entry.path().filename(), std::ios::binary);
                const std::string left((std::istreambuf_iterator<char>(a)), {}), right((std::istreambuf_iterator<char>(b)), {});
                if (!b || left != right) ++mismatches;
            }
            for (const auto& entry : disk::directory_iterator(output / L"destination" / L"many")) { (void)entry; ++actual_files; }
            if (stress_enabled && stress.prepared) stress_files_ok = stress.VerifyFiles();
        } catch (const std::exception& error) { verification_error = error.what(); }
        verification_done = true;
    });
    check(PumpUntil([&] { return verification_done.load(); }, 180000), "background byte verification finished while UI continued pumping");
    verifier.request_stop();
    verifier.join();
    if (!verification_error.empty()) report << "VERIFY_ERROR " << verification_error << '\n';
    report << "COPY expected=" << expected_files << " actual=" << actual_files << " mismatches=" << mismatches << '\n';
    check(verification_error.empty() && expected_files == 30000 && actual_files == expected_files && !mismatches,
        "all 30000 copied files verified byte-for-byte");
    check(PumpUntil([&] {
        const auto* tab = ActiveTab(state);
        return tab && !tab->loading && tab->snapshot && tab->snapshot->size() == expected_files;
    }, 15000), "watched growing directory converged to all 30000 visible entries without manual refresh");
    const auto* settled_tab = ActiveTab(state);
    bool exact_snapshot = settled_tab && settled_tab->snapshot && settled_tab->snapshot->size() == expected_names.size();
    if (exact_snapshot) {
        for (const auto& entry : *settled_tab->snapshot)
            if (!expected_names.erase(entry.name)) { exact_snapshot = false; break; }
    }
    check(exact_snapshot && expected_names.empty(), "final visible snapshot matches every copied filename exactly once");
    std::vector<double> vm_samples;
    bool vm_complete = true;
    for (int i = 0; i < 12; ++i) {
        PumpUntil([] { return false; }, 25);
        const auto begin = std::chrono::steady_clock::now();
        const auto vm = BuildVm(state);
        vm_samples.push_back(std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - begin).count());
        vm_complete = vm_complete && vm.pane.EntryCount() == 30000;
    }
    std::sort(vm_samples.begin(), vm_samples.end());
    report << "STABLE_VM samples=" << vm_samples.size() << " entries=30000 min_ms=" << vm_samples.front()
           << " p50_ms=" << vm_samples[vm_samples.size() / 2] << " max_ms=" << vm_samples.back() << '\n';
    check(vm_complete, "repeated production view models retain all 30000 entries");
    if (stress_enabled) check(stress.prepared && stress_files_ok, "concurrent stress file payloads dependencies and cancelled deletion verified");
    check(navigate(output / L"destination"), "parent navigation before permanent deletion");
    ops::OpRequest remove;
    remove.type = ops::OpType::RealDelete;
    remove.sources = {(output / L"destination" / L"many").wstring()};
    const auto deleting = state.ops.Submit(remove);
    check(deleting && PumpUntil([&] {
        const auto status = state.ops.QueryTask(deleting);
        return status && status->active;
    }, 30000), "bulk delete active on private copied fixture only");
    run_creation(deleting, "delete");
    check(PumpUntil([&] { return terminal(deleting); }, 180000), "bulk delete reaches terminal status");
    const auto delete_status = state.ops.FinalStatus(deleting);
    check(delete_status && delete_status->phase == ops::OpPhase::Completed &&
        !disk::exists(output / L"destination" / L"many"), "bulk deletion removed only the copied fixture");
    if (stress_enabled && copying) state.ops.ReleaseTask(copying);
    KillTimer(state.hwnd, timer);
    poster.request_stop();
    poster.join();
    RemoveWindowSubclass(state.hwnd, ObservePostedLatency, timer);
    heartbeat = nullptr;
    timing_report = nullptr;
    pumping_state = nullptr;
    report << "UI_MESSAGES ops=" << samples.ops_messages << " ops_ms=" << samples.ops_ms
           << " max_ops_ms=" << samples.max_ops_ms << " kicks=" << samples.kick_messages
           << " kick_ms=" << samples.kick_ms << " max_kick_ms=" << samples.max_kick_ms
           << " paints=" << samples.paint_messages << '\n';
    std::sort(samples.gaps.begin(), samples.gaps.end());
    if (!samples.gaps.empty()) report << "HEARTBEAT samples=" << samples.gaps.size()
        << " p95_ms=" << samples.gaps[(samples.gaps.size() - 1) * 95 / 100]
        << " max_ms=" << samples.gaps.back() << '\n';
    check(!samples.gaps.empty(), "main-window timer serviced during real file operations");
    std::sort(samples.posted_latency.begin(), samples.posted_latency.end());
    if (!samples.posted_latency.empty()) report << "POSTED_LATENCY samples=" << samples.posted_latency.size()
        << " p95_ms=" << samples.posted_latency[(samples.posted_latency.size() - 1) * 95 / 100]
        << " max_ms=" << samples.posted_latency.back() << '\n';
    check(!samples.posted_latency.empty() && samples.posted_latency.back() < 1000,
        "no one-second posted-message processing stall throughout operations/navigation/background verification");
    report << "RESULT failures=" << failures << '\n';
    return failures ? 1 : 0;
}

namespace {
void CALLBACK RunScheduledSmallFilesProbe(HWND hwnd, UINT, UINT_PTR timer, DWORD) {
    KillTimer(hwnd, timer);
    auto launch = std::move(pending_launch);
    if (!launch) return;
    const int result = RunSmallFilesLiveProbe(*launch->state, launch->root.c_str());
    DestroyWindow(hwnd);
    PostQuitMessage(result);
}
}
bool ScheduleSmallFilesLiveProbe(AppState& state, const wchar_t* root) {
    pending_launch = std::make_unique<ProbeLaunch>(ProbeLaunch{&state, root});
    return SetTimer(state.hwnd, 0x53464C51, 100, RunScheduledSmallFilesProbe) != 0;
}
