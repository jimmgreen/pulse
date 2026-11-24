// small_files_tree_scenario.inc - node_modules-shaped copy and delete while the
// user walks into the growing tree and back out with the real GoUp command.
// Enabled by PULSE_TEST_SMALL_FILES_TREE in the isolated small-files probe;
// fixture: <root>/source/tree/node_modules (bench_data/.../gen_tree.exe).
// Reports per phase (copy / after_copy control / delete): navigation latency
// and posted-message latency from the probe's existing observers.
#include "../app/tree_transfer_destination.h"
namespace tree_probe {
struct Window { size_t posted = 0, gaps = 0, timer = 0; };
// One-shot 25 ms WM_TIMER, the mechanism the clipboard paste poll waits on
// (kFileClipboardPasteTimer): time from arming to delivery.
constexpr UINT_PTR kPasteLikeTimer = 0x53464C53;
inline ULONGLONG timer_armed = 0;
inline std::vector<ULONGLONG>* timer_out = nullptr;
inline void CALLBACK PasteLikeTimer(HWND hwnd, UINT, UINT_PTR id, DWORD) {
    KillTimer(hwnd, id);
    if (timer_armed && timer_out) timer_out->push_back(GetTickCount64() - timer_armed);
    timer_armed = 0;
}
// A still-pending timer keeps running (no re-arm), so each sample is a true
// arming-to-delivery time; one pending at phase end is recorded as a lower bound.
inline void ArmPasteLikeTimer(HWND hwnd) {
    if (timer_armed) return;
    timer_armed = GetTickCount64();
    SetTimer(hwnd, kPasteLikeTimer, 25, PasteLikeTimer);
}
inline std::string Percentiles(std::vector<ULONGLONG> v) {
    if (v.empty()) return "n=0";
    std::sort(v.begin(), v.end());
    return "n=" + std::to_string(v.size()) + " p50=" + std::to_string(v[v.size() / 2]) +
           " p95=" + std::to_string(v[(v.size() - 1) * 95 / 100]) + " max=" + std::to_string(v.back());
}
} // namespace tree_probe

template<class Check, class Navigate, class Terminal>
void RunTreeScenario(AppState& state, const std::filesystem::path& root, const std::filesystem::path& output,
                     std::ofstream& report, LiveHeartbeat& samples, Check check, Navigate navigate,
                     Terminal terminal) {
    namespace disk = std::filesystem;
    const auto source = root / L"source" / L"tree" / L"node_modules";
    const auto dest = output / L"destination";
    const auto copied = dest / L"node_modules";
    const auto deep = copied / L"pkg00008" / L"lib";
    if (!check(disk::is_directory(source), "tree fixture present")) return;
    std::vector<ULONGLONG> down, up, timer25;
    tree_probe::timer_out = &timer25;
    tree_probe::Window window;
    const char* phase = "";
    const auto begin_phase = [&](const char* name) {
        phase = name; down.clear(); up.clear(); timer25.clear();
        window = {samples.posted_latency.size(), samples.gaps.size(), 0};
    };
    const auto end_phase = [&] {
        if (tree_probe::timer_armed) {
            timer25.push_back(GetTickCount64() - tree_probe::timer_armed);
            KillTimer(state.hwnd, tree_probe::kPasteLikeTimer);
            tree_probe::timer_armed = 0;
        }
        std::vector<ULONGLONG> posted(samples.posted_latency.begin() + window.posted, samples.posted_latency.end());
        std::vector<ULONGLONG> gaps(samples.gaps.begin() + window.gaps, samples.gaps.end());
        report << "TREE_PHASE phase=" << phase << " down[" << tree_probe::Percentiles(down) << "] up["
               << tree_probe::Percentiles(up) << "] posted[" << tree_probe::Percentiles(posted)
               << "] heartbeat[" << tree_probe::Percentiles(gaps) << "] timer25[" << tree_probe::Percentiles(timer25) << "]\n";
        report.flush();
    };
    const auto timed_down = [&](const disk::path& path) {
        const auto t = GetTickCount64();
        const bool ok = navigate(path);
        down.push_back(GetTickCount64() - t);
        report << "NAV phase=" << phase << " kind=down ms=" << down.back() << " ok=" << ok << '\n';
        return ok;
    };
    // GoUp is the toolbar/Alt+Up/Backspace command the user pressed.
    const auto timed_up = [&](const disk::path& expected) {
        const auto t = GetTickCount64();
        GoUp(state);
        const bool ok = PumpUntil([&] {
            const auto* tab = ActiveTab(state);
            return tab && tab->current_path == fs::NormalizePath(expected.wstring()) && !tab->loading && tab->snapshot;
        }, 15000);
        up.push_back(GetTickCount64() - t);
        report << "NAV phase=" << phase << " kind=up ms=" << up.back() << " ok=" << ok << '\n';
        return ok;
    };
    const auto walk = [&] {
        tree_probe::ArmPasteLikeTimer(state.hwnd);
        bool ok = timed_down(copied);
        PumpUntil([] { return false; }, 300);
        if (disk::is_directory(deep)) {
            ok = timed_down(deep) && ok;
            PumpUntil([] { return false; }, 300);
            ok = timed_up(deep.parent_path()) && ok;
            ok = timed_up(copied) && ok;
        }
        ok = timed_up(dest) && ok;
        PumpUntil([] { return false; }, 300);
        return ok;
    };

    check(navigate(dest), "tree: destination loaded");
    begin_phase("copy");
    ops::OpRequest copy;
    copy.type = ops::OpType::Copy;
    copy.sources = {source.wstring()};
    copy.dest_dir = dest.wstring();
    const auto started = GetTickCount64();
    const auto copying = state.ops.Submit(copy);
    check(copying && PumpUntil([&] {
        const auto status = state.ops.QueryTask(copying);
        return status && status->active && status->completed_items >= 200;
    }, 30000), "tree: copy active");
    // A second paste while the copy runs goes through the real paste submit
    // (SubmitTreeTransfer); a folder copy waits behind the bulk copy and the
    // user must be told so instead of seeing nothing (duplicate pastes).
    const auto second_source = source / L"pkg00001";
    const auto second_copy = dest / L"pkg00001";
    uint64_t second = 0;
    if (const auto* tab = ActiveTab(state); check(tab && tab->current_path == fs::NormalizePath(dest.wstring()),
                                                   "tree: second paste issued from the destination folder")) {
        const auto before = state.ops.QueuedTransferCount();
        const auto submitted_at = GetTickCount64();
        check(SubmitTreeTransfer(state, {second_source.wstring()}, TreeTransferDestinationsForTab(*tab), false),
              "tree: second paste submitted while the copy runs");
        const bool queued = state.ops.QueuedTransferCount() > before;
        if (queued && !state.queuedTransferNotices.empty()) second = state.queuedTransferNotices.back().task;
        const bool toast = PumpUntil([&] { return state.notification_toast.IsVisible(); }, 2000);
        const auto status = second ? state.ops.QueryTask(second) : std::nullopt;
        report << "QUEUE second_task=" << second << " queued=" << queued << " still_queued="
               << (status && status->phase == ops::OpPhase::Queued) << " toast=" << toast
               << " toast_ms=" << (toast ? GetTickCount64() - submitted_at : 0) << '\n';
        check(second && status && status->phase == ops::OpPhase::Queued, "tree: second paste waits behind the copy");
        check(toast, "tree: queued second paste shows a notice within 2 s");
    }
    int walks = 0;
    bool walks_ok = true;
    while (!terminal(copying) && walks < 12) { walks_ok = walk() && walks_ok; ++walks; }
    report << "TREE copy_walks=" << walks << '\n';
    check(walks >= 2, "tree: at least two down/up walks overlapped the copy");
    check(walks_ok, "tree: every navigation during copy completed");
    check(PumpUntil([&] { return terminal(copying); }, 300000), "tree: copy reaches terminal status");
    const auto copy_ms = GetTickCount64() - started;
    end_phase();
    const auto status = state.ops.FinalStatus(copying);
    check(status && status->phase == ops::OpPhase::Completed, "tree: copy succeeded");
    if (second) {
        check(PumpUntil([&] {
            const auto s2 = state.ops.QueryTask(second);
            return s2 && !s2->active && (s2->phase == ops::OpPhase::Completed || s2->phase == ops::OpPhase::Failed);
        }, 60000) && disk::is_directory(second_copy), "tree: queued second paste ran after the copy");
    }
    std::atomic<bool> counted{false};
    size_t src_files = 0, dst_files = 0;
    std::jthread counter([&] {
        std::error_code ec;
        for (auto it = disk::recursive_directory_iterator(source, ec); !ec && it != disk::recursive_directory_iterator(); it.increment(ec))
            src_files += it->is_regular_file();
        for (auto it = disk::recursive_directory_iterator(copied, ec); !ec && it != disk::recursive_directory_iterator(); it.increment(ec))
            dst_files += it->is_regular_file();
        counted = true;
    });
    PumpUntil([&] { return counted.load(); }, 120000);
    counter.join();
    report << "TREE copy_ms=" << copy_ms << " src_files=" << src_files << " dst_files=" << dst_files << '\n';
    check(src_files > 0 && src_files == dst_files, "tree: every file copied");

    begin_phase("after_copy");
    for (int i = 0; i < 3; ++i) walks_ok = walk() && walks_ok;
    end_phase();

    begin_phase("delete");
    ops::OpRequest remove;
    remove.type = ops::OpType::RealDelete;
    remove.sources = {copied.wstring()};
    if (disk::exists(second_copy)) remove.sources.push_back(second_copy.wstring());
    const auto delete_started = GetTickCount64();
    const auto deleting = state.ops.Submit(remove);
    check(deleting && PumpUntil([&] {
        const auto s = state.ops.QueryTask(deleting);
        return (s && s->active) || terminal(deleting);
    }, 30000), "tree: delete started");
    int delete_walks = 0;
    while (!terminal(deleting) && delete_walks < 12) {
        tree_probe::ArmPasteLikeTimer(state.hwnd);
        timed_down(dest);  // the tree's parent, as in the user's report
        PumpUntil([] { return false; }, 300);
        timed_up(output);
        PumpUntil([] { return false; }, 300);
        ++delete_walks;
    }
    check(PumpUntil([&] { return terminal(deleting); }, 300000), "tree: delete reaches terminal status");
    report << "TREE delete_ms=" << GetTickCount64() - delete_started << " delete_walks=" << delete_walks << '\n';
    end_phase();
    check(!disk::exists(copied) && !disk::exists(second_copy), "tree: delete removed only the copied fixture");
    tree_probe::timer_out = nullptr;
}
