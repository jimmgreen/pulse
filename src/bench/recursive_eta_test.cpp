#include "../ops/recursive_eta_estimator.h"
#include <cstdio>
#include <chrono>
#include <string_view>
#ifdef _WIN32
#define PULSE_NATIVE_DELETE_TEST
#include "../ops/native_delete_stream.h"
#include <filesystem>
#include <fstream>
#endif

namespace {
using namespace pulse::ops;
int failures = 0, checks = 0;
void Check(bool pass, const char* label) {
    ++checks; if (!pass) ++failures;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
}
void Hints() {
    RecentCopyWorkCache hints;
    const uint64_t epoch = hints.BeginMutation();
    Check(hints.Remember(L"C:\\owned\\folder", {0, 16000, 65}, 1000, epoch), "a completed copy can record an advisory count receipt");
    const auto receipt = hints.Take(L"\\\\?\\c:\\OWNED\\folder\\", 1500);
    Check(receipt && receipt->total.files == 16000 && receipt->total.directories == 65,
        "copy receipts match normalized root paths without filesystem queries");
    Check(!hints.Take(L"C:\\owned\\folder", 1600), "a receipt is consumed rather than reused after deletion");
    hints.Remember(L"C:\\owned\\folder", {0, 10, 2}, 2000, epoch);
    Check(!hints.Take(L"C:\\owned\\folder", 2000 + RecentCopyWorkCache::kLifetimeMs + 1), "stale receipts expire");
    hints.Remember(L"C:\\owned\\folder", {0, 10, 2}, 3000, epoch);
    hints.Remember(L"C:\\owned\\folder2", {0, 20, 3}, 3000, epoch);
    hints.Take(L"C:\\owned\\folder\\child", 3100);
    Check(!hints.Take(L"C:\\owned\\folder", 3100), "touching a descendant invalidates its parent receipt");
    Check(hints.Take(L"C:\\owned\\folder2", 3100).has_value(), "path boundaries do not invalidate a merely similar name");
    const auto next = hints.BeginMutation();
    Check(!hints.Remember(L"C:\\owned\\late", {0, 10, 1}, 4000, epoch), "a late copy cannot publish across another mutation epoch");
    Check(hints.Remember(L"C:\\owned\\fresh", {0, 10, 1}, 4000, next), "the current copy epoch can record counts");
    hints.BeginMutation(true);
    Check(hints.Take(L"C:\\owned\\fresh", 4100).has_value(), "a delete starts a mutation while preserving consumable hints");
    const auto final_epoch = hints.BeginMutation();
    hints.Remember(L"C:\\owned\\a", {0, 1, 1}, 5000, final_epoch);
    hints.Remember(L"C:\\owned\\b", {0, 2, 1}, 5001, final_epoch);
    hints.Take(L"C:\\owned\\a", 5100);
    hints.Remember(L"C:\\owned\\b", {0, 22, 1}, 5200, final_epoch);
    const auto updated = hints.Take(L"C:\\owned\\b", 5300);
    Check(updated && updated->total.files == 22 && !hints.Take(L"C:\\owned\\b", 5300),
        "updating a receipt after a hole never creates stale duplicate roots");
    for (size_t i = 0; i < RecentCopyWorkCache::kCapacity + 5; ++i)
        hints.Remember(L"C:\\owned\\cache-" + std::to_wstring(i), {0, 20, 1}, 10000 + i, final_epoch);
    Check(!hints.Take(L"C:\\owned\\cache-0", 11000), "receipt memory is bounded and old entries are evicted");
    Check(hints.Take(L"C:\\owned\\cache-68", 11000).has_value(), "the newest bounded-cache entry remains usable");
}
void PriorPrediction() {
    RecursiveEtaEstimator estimator;
    estimator.Reset(1000, RecentCopyWork{{0, 16000, 65}, 900});
    OperationEta eta;
    for (uint64_t tick = 1250; tick <= 4000; tick += 250) {
        TransferWorkProgress work{{0, tick - 1000, (tick - 1000) / 256}, {}, false};
        eta = estimator.Observe(tick, work, nullptr, 71);
    }
    Check(eta.stage == EtaStage::Range && eta.source == EtaSource::RecentCopy,
        "a recent copy supplies an explicitly predicted range before delete enumeration completes");
    Check(eta.lower_seconds <= 13 && eta.upper_seconds >= 13 && eta.upper_seconds <= 30,
        "the receipt interval covers stable remaining work without an unbounded band");
    Check(eta.task_id == 71 && eta.processed_items > 3000, "advisory UI state binds the task and actual processed children");
    RecursiveEtaEstimator flat_prior; flat_prior.Reset(1000, RecentCopyWork{{0, 16000, 1}, 900});
    RecursiveWorkStats flat_population; flat_population.Root();
    for (uint64_t tick = 1250; tick <= 4000; tick += 250) {
        flat_population.Files(0, 250);
        eta = flat_prior.Observe(tick, {{0, tick - 1000, 0}, {}, false}, &flat_population, 73);
    }
    Check(eta.stage == EtaStage::Range && eta.source == EtaSource::RecentCopy,
        "a copied flat directory gets a range without completed sibling samples or directory-rate history");
    RecursiveWorkStats deep; deep.Root(); deep.Child(RecursiveWorkStats::kDepths - 1);
    eta = estimator.Observe(4250, {{0, 3250, 12}, {}, false}, &deep, 71);
    Check(eta.stage == EtaStage::Range && eta.source == EtaSource::RecentCopy,
        "a sampling depth limit does not discard a still-plausible independent copy prior");
    RecursiveEtaEstimator known; known.Reset(1000);
    for (uint64_t tick = 1250; tick <= 4000; tick += 250)
        eta = known.Observe(tick, {{0, tick - 1000, 0}, {0, 16000, 0}, true}, nullptr, 72);
    Check(eta.stage == EtaStage::Range && eta.source == EtaSource::KnownWork,
        "knowing the total does not falsely label unfinished work as finishing");
    TransferWorkProgress changed{{0, 17000, 65}, {}, false};
    eta = estimator.Observe(18000, changed, nullptr, 71);
    Check(eta.stage != EtaStage::Range && eta.lower_seconds == 0,
        "exhausting an unverified prior cannot leave a fake one-second countdown");
    changed.total = changed.completed; changed.total_known = true;
    eta = estimator.Observe(18250, changed, nullptr, 71);
    Check(eta.stage == EtaStage::Finishing, "only backend-known completed work enters finishing presentation");
}
void Streaming() {
    RecursiveWorkStats stats;
    stats.Root();
    for (unsigned i = 0; i < 64; ++i) stats.Child(0);
    stats.Listed(0, 0, 64, true);
    RecursiveEtaEstimator estimator; estimator.Reset(0);
    TransferWorkProgress work;
    size_t useful = 0, covered = 0;
    for (uint64_t group = 1; group <= 56; ++group) {
        stats.Files(1, 256);
        stats.Listed(1, 256, 0, true);
        work.completed = {0, group * 256, group};
        const auto eta = estimator.Observe(group * 256, work, &stats, 91);
        if (group < 5) continue;
        if (eta.stage == EtaStage::Range) {
            ++useful;
            const double truth = static_cast<double>((64 - group) * 256) / 1000;
            if (eta.lower_seconds <= truth + 1 && eta.upper_seconds >= truth) ++covered;
        }
    }
    std::printf("STREAM_ETA samples=52 numeric_ranges=%zu covered=%zu\n", useful, covered);
    Check(useful >= 45, "uniform sibling directories provide useful interval coverage without a prescan");
    Check(covered * 10 >= useful * 9, "stable synthetic deletion intervals cover their actual remaining work");
    Check(stats.KnownDirectories() == 65 && stats.levels[1].listed == 56, "frontier statistics stay separate from deletion completion counts");
    stats.Files(1, 10000);
    Check(stats.Surprised(), "an unexpectedly large active subtree invalidates the small-sibling assumption");
    work.completed.files += 10000;
    const auto surprise = estimator.Observe(20000, work, &stats, 91);
    Check(surprise.stage != EtaStage::Range, "new large-directory evidence withdraws a misleading narrow estimate");
}
void PartialFrontier() {
    RecursiveWorkStats stats; stats.Root();
    for (unsigned i = 0; i < 4; ++i) stats.Child(0);
    stats.Listed(0, 0, 4, true);
    for (unsigned i = 0; i < 3; ++i) { stats.Files(1, 10); stats.Listed(1, 10, 0, true); }
    stats.Files(1, 10); // discovered work is queued, not yet deleted
    TransferWork low, high;
    Check(RecursiveEtaEstimator::ForecastWork(stats, {0, 30, 3}, low, high) && low.files == 10 && high.files == 12,
        "already discovered pending files remain in the forecast after open-parent subtraction");
    Check(RecursiveEtaEstimator::ForecastWork(stats, {0, 35, 3}, low, high) && low.files <= 5 && high.files >= 5 && high.files <= 7,
        "processing part of an open directory neither drops nor double counts its remaining files");
}
void DiscoveryPrecision() {
    RecursiveWorkStats stats; stats.Root();
    for (unsigned i = 0; i < 64; ++i) stats.Child(0);
    stats.Listed(0, 0, 64, true);
    RecursiveEtaEstimator sampled, prior_only;
    const RecentCopyWork stale{{0, 120000, 65}, 0};
    sampled.Reset(0, stale); prior_only.Reset(0, stale);
    OperationEta eta, prior;
    size_t ranges = 0, covered = 0, sample_sources = 0;
    uint64_t width = 0, prior_width = 0;
    TransferWorkProgress work;
    for (uint64_t group = 1; group <= 56; ++group) {
        stats.Files(1, 1000); stats.Listed(1, 1000, 0, true);
        work.completed = {0, group * 1000, group};
        eta = sampled.Observe(group * 1000, work, &stats, 101);
        prior = prior_only.Observe(group * 1000, work, nullptr, 102);
        if (group < 8) continue;
        if (eta.source == EtaSource::DirectorySamples) ++sample_sources;
        if (eta.stage == EtaStage::Range) {
            ++ranges;
            if (eta.lower_seconds <= 65 - group && eta.upper_seconds >= 64 - group) ++covered;
            width += eta.upper_seconds - eta.lower_seconds;
        }
        if (prior.stage == EtaStage::Range) prior_width += prior.upper_seconds - prior.lower_seconds;
    }
    std::printf("DISCOVERY_PRECISION samples=49 ranges=%zu covered=%zu sample_sources=%zu width_sum=%llu prior_width_sum=%llu final=%llu-%llu prior_final=%llu-%llu\n",
        ranges, covered, sample_sources, static_cast<unsigned long long>(width), static_cast<unsigned long long>(prior_width),
        static_cast<unsigned long long>(eta.lower_seconds), static_cast<unsigned long long>(eta.upper_seconds),
        static_cast<unsigned long long>(prior.lower_seconds), static_cast<unsigned long long>(prior.upper_seconds));
    Check(sample_sources == 49, "current directory samples supersede a plausible but oversized copy receipt");
    Check(ranges == 49 && covered == ranges, "sample-priority ranges cover the stable replay's actual remaining duration");
    Check(width < prior_width, "observed population removes stale-prior interval inflation without narrowing sample margins");
    eta = sampled.Observe(60000, work, &stats, 101);
    Check(eta.stage != EtaStage::Range && eta.lower_seconds == 0 && eta.upper_seconds == 0,
        "a paused deletion withdraws its numeric interval instead of counting down");
    sampled.Reset(60000, stale);
    for (uint64_t group = 57; group <= 60; ++group) {
        stats.Files(1, 1000); stats.Listed(1, 1000, 0, true);
        work.completed = {0, group * 1000, group};
        eta = sampled.Observe(60000 + (group - 56) * 1000, work, &stats, 101);
    }
    Check(eta.stage != EtaStage::Finishing && !work.total_known,
        "resuming sample-based prediction never promotes inferred work into completion authority");

    RecursiveWorkStats closed; closed.Root(); closed.Files(0, 16000); closed.Listed(0, 16000, 0, true);
    TransferWork lower, upper;
    Check(RecursiveEtaEstimator::ForecastWork(closed, {0, 3000, 0}, lower, upper) &&
        lower.files == 13000 && upper.files == 13000 && lower.directories == 1 && upper.directories == 1,
        "a fully listed frontier retains precisely the discovered but unfinished work");
    RecursiveEtaEstimator listed; listed.Reset(0, stale);
    for (uint64_t tick = 250; tick <= 3000; tick += 250)
        eta = listed.Observe(tick, {{0, tick, 0}, {}, false}, &closed, 103);
    Check(eta.source == EtaSource::DirectorySamples && eta.stage == EtaStage::Range &&
        eta.lower_seconds <= 13 && eta.upper_seconds >= 13 && eta.upper_seconds < 30,
        "closed-listing evidence replaces an obsolete receipt without declaring finishing");
    closed.Child(0);
    Check(!RecursiveEtaEstimator::ForecastWork(closed, {0, 3000, 0}, lower, upper),
        "an unlisted discovered descendant prevents closed-frontier inference");
    closed.Listed(1, 0, 0, false);
    Check(!RecursiveEtaEstimator::ForecastWork(closed, {0, 3000, 0}, lower, upper),
        "incomplete enumeration never becomes closed-frontier evidence");
    RecursiveWorkStats empty;
    Check(!RecursiveEtaEstimator::ForecastWork(empty, {}, lower, upper),
        "the absence of a discovered root is not proof of an empty completed traversal");

    RecursiveWorkStats mixed; mixed.Root();
    for (unsigned i = 0; i < 64; ++i) mixed.Child(0);
    mixed.Listed(0, 0, 64, true);
    for (unsigned i = 0; i < 32; ++i) {
        const uint64_t files = i % 2 ? 1900 : 100;
        mixed.Files(1, files); mixed.Listed(1, files, 0, true);
    }
    Check(RecursiveEtaEstimator::ForecastWork(mixed, {0, 32000, 32}, lower, upper) &&
        lower.files <= 3200 && upper.files >= 60800,
        "heterogeneous siblings retain population uncertainty for a heavy or light remaining tail");
    mixed.Files(1, 200000);
    Check(mixed.Surprised() && !RecursiveEtaEstimator::ForecastWork(mixed, {0, 32000, 32}, lower, upper),
        "a newly oversized open directory still invalidates sampled population prediction");
}
void UnknownAndBounds() {
    RecursiveWorkStats flat; flat.Root();
    RecursiveEtaEstimator estimator; estimator.Reset(0);
    OperationEta eta;
    TransferWorkProgress work;
    for (uint64_t tick = 250; tick <= 5000; tick += 250) {
        flat.Files(0, 250);
        work.completed.files = tick;
        eta = estimator.Observe(tick, work, &flat, 100);
    }
    Check(eta.stage == EtaStage::Unknown && eta.processed_items == 5000,
        "an unbounded flat directory becomes honest progress, not permanent learning text");
    Check(eta.lower_seconds == 0 && eta.upper_seconds == 0,
        "missing directory-population evidence does not fabricate a total-task ETA");
    RecursiveWorkStats deep; deep.Root(); deep.Child(RecursiveWorkStats::kDepths - 1);
    Check(!deep.reliable, "depth overflow is explicit rather than an extrapolated infinite subtree");
    RecursiveWorkStats huge; huge.Root(); huge.levels[0].seen_files = UINT64_MAX; huge.Files(0, 1);
    Check(!huge.reliable, "counter overflow disables population prediction safely");
    TransferEtaEstimator cost; cost.Reset(0);
    for (uint64_t tick = 250; tick <= 4000; tick += 250) cost.ObserveWork(tick, {0, tick / 10, 0});
    Check(cost.seconds() == 0 && cost.ProjectedSeconds({0, 200, 0}) >= 1,
        "conditional cost projection does not claim authoritative task completion or totals");
    Check(sizeof(RecursiveEtaEstimator) < 4096 && sizeof(RecursiveWorkStats) < 2048,
        "stream estimator and depth summaries have constant small memory bounds");
}
void Benchmark() {
    for (bool cached : {true, false}) {
        RecursiveWorkStats stats;
        stats.levels[0].known = stats.levels[0].listed = stats.levels[0].samples = 1;
        stats.levels[0].mean_children = 100000;
        stats.levels[0].seen_children = stats.levels[0].closed_children = 100000;
        stats.levels[1].known = 100000;
        stats.levels[1].mean_files = 256;
        RecursiveEtaEstimator estimator;
        estimator.Reset(0, cached ? std::optional<RecentCopyWork>{RecentCopyWork{{0, 100000000, 100000}, 0}} : std::nullopt);
        uint64_t checksum = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (uint64_t i = 1; i <= 20000; ++i) {
            const uint64_t files = 25600 + i * 20, directories = files / 256;
            stats.levels[1].listed = stats.levels[1].samples = directories;
            stats.levels[1].closed_files = directories * 256;
            stats.levels[1].seen_files = files;
            const auto eta = estimator.Observe(i * 100, {{0, files, directories}, {}, false}, cached ? nullptr : &stats, 1);
            checksum ^= eta.lower_seconds + eta.upper_seconds;
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
        std::printf("RECURSIVE_ETA_BENCH mode=%s observations=20000 elapsed_us=%lld estimator_bytes=%zu discovery_bytes=%zu checksum=%llu\n",
            cached ? "copy-prior" : "frontier", static_cast<long long>(elapsed), sizeof(estimator), sizeof(stats),
            static_cast<unsigned long long>(checksum));
    }
}
#ifdef _WIN32
void SlowTestMutation() { Sleep(8); }
void NativeDiscovery(bool parallel, unsigned workers, bool controlled_slow = false) {
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)))) { Check(false, "locate owned discovery fixture parent"); return; }
    const auto parent = std::filesystem::path(exe).parent_path() / L"bench_data";
    std::filesystem::create_directories(parent);
    const auto root = parent / (L"recursive-stats-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + (parallel ? L"-parallel-" : L"-serial-") + std::to_wstring(workers));
    if (!CreateDirectoryW(root.c_str(), nullptr)) { Check(false, "exclusively create owned discovery fixture"); return; }
    for (unsigned group = 0; group < 8; ++group) {
        const auto dir = root / std::to_wstring(group);
        std::filesystem::create_directory(dir);
        for (unsigned file = 0; file < 32; ++file) { std::ofstream output(dir / std::to_wstring(file)); output << "owned discovery fixture"; }
    }
    size_t count = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (entry.is_regular_file()) ++count;
    if (count != 256) { Check(false, "owned discovery setup verified before delete"); return; }
    RecursiveWorkStats last;
    RecursiveEtaEstimator live_forecast;
    live_forecast.Reset(GetTickCount64());
    uint64_t numeric_ranges = 0;
    uint64_t reports = 0, previous_files = 0;
    bool monotonic = true;
    std::atomic<bool> cancel{false};
    auto report = [&](const std::wstring&, float, const TransferWorkProgress* work = nullptr) {
        if (!work || !work->discovery) return;
        last = *work->discovery;
        monotonic = monotonic && work->completed.files >= previous_files;
        previous_files = work->completed.files;
        if (controlled_slow) {
            const auto eta = live_forecast.Observe(GetTickCount64(), *work, work->discovery, 991);
            if (eta.stage == EtaStage::Range && eta.source == EtaSource::DirectorySamples) ++numeric_ranges;
        }
        ++reports;
    };
    struct ResetMutationHook {
        ~ResetMutationHook() { native_delete::after_mutation = nullptr; }
    } reset_hook;
    if (controlled_slow) native_delete::after_mutation = SlowTestMutation;
    const auto outcome = parallel ? native_delete::DeleteNativeParallel(root.wstring(), cancel, report, workers)
                                  : native_delete::DeleteNative(root.wstring(), cancel, report, workers);
    Check(outcome.done && !std::filesystem::exists(root), "discovery instrumentation preserves actual native deletion outcome");
    Check(reports > 0 && monotonic && last.reliable && last.KnownDirectories() == 9 && last.SeenFiles() == 256,
        "native discovery snapshots preserve monotonic actual counts without another enumeration");
    Check(last.levels[0].samples == 1 && last.levels[0].mean_children == 8 &&
        last.levels[1].samples == 8 && last.levels[1].mean_files == 32,
        "native directory samples describe completed listings at the correct depths");
    uint64_t pending = 0;
    for (const auto& level : last.levels) pending += level.known - level.listed;
    Check(pending == 0 && previous_files == 256, "native frontier accounting is empty only after the real traversal ends");
    if (controlled_slow) {
        std::printf("CONTROLLED_NATIVE_STREAM test_only_delay_ms=8 numeric_ranges=%llu\n", static_cast<unsigned long long>(numeric_ranges));
        Check(numeric_ranges > 0, "controlled slow native traversal emits uncached intervals before actual completion");
    }
}
#endif
}
int main(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--bench") { Benchmark(); return 0; }
    Hints(); PriorPrediction(); Streaming(); PartialFrontier(); DiscoveryPrecision(); UnknownAndBounds();
#ifdef _WIN32
    NativeDiscovery(false, 1); NativeDiscovery(false, 4); NativeDiscovery(true, 4); NativeDiscovery(true, 2, true);
#endif
    std::printf("RECURSIVE_ETA_RESULT checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
