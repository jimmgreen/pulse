#include "../ops/transfer_eta_estimator.h"
#include "../ops/large_copy_eta_estimator.h"
#ifdef _WIN32
#include "../ops/transfer_rate_estimator.h"
#include "../ops/native_delete_stream.h"
#include <filesystem>
#include <fstream>
#endif
#include <chrono>
#include <cstdio>
#include <string_view>
#include <vector>

namespace {
using pulse::ops::TransferEtaEstimator;
using pulse::ops::TransferWork;
using pulse::ops::TransferWorkProgress;
int failures = 0, checks = 0;
bool legacy = false;
void Check(bool pass, const char* label) {
    ++checks;
    if (!pass) ++failures;
    std::printf("[%s] %s\n", pass ? "PASS" : "FAIL", label);
}
struct Estimator {
    TransferEtaEstimator model;
#ifdef _WIN32
    pulse::ops::TransferRateEstimator old;
#endif
    void Reset(uint64_t tick = 0) {
        model.Reset(tick);
#ifdef _WIN32
        old.Reset(tick, 0);
#endif
    }
    void Observe(uint64_t tick, const TransferWorkProgress& progress) {
        model.Observe(tick, progress);
#ifdef _WIN32
        old.Observe(tick, progress.completed.bytes, progress.total.bytes);
#endif
    }
    uint64_t Seconds() const {
#ifdef _WIN32
        if (legacy) return old.eta_seconds();
#endif
        return model.seconds();
    }
};
struct Item { uint64_t bytes; double fixed, data; bool directory = false; };
struct ReplayResult { size_t samples = 0, estimates = 0; double error = 0, worst = 0; };
ReplayResult Replay(const char* label, const std::vector<Item>& items, double from, double until = 1e100) {
    TransferWorkProgress progress;
    progress.total_known = true;
    double duration = 0;
    for (const auto& item : items) {
        progress.total.bytes += item.bytes;
        if (item.directory) ++progress.total.directories; else ++progress.total.files;
        duration += item.fixed + item.data;
    }
    Estimator estimator; estimator.Reset();
    size_t index = 0;
    double started = 0;
    TransferWork settled;
    ReplayResult result;
    for (uint64_t tick = 250; static_cast<double>(tick) / 1000 < duration; tick += 250) {
        const double now = static_cast<double>(tick) / 1000;
        while (index < items.size() && now + 1e-9 >= started + items[index].fixed + items[index].data) {
            const auto& item = items[index++];
            settled.bytes += item.bytes;
            if (item.directory) ++settled.directories; else ++settled.files;
            started += item.fixed + item.data;
        }
        progress.completed = settled;
        if (index < items.size() && items[index].data > 0 && now > started + items[index].fixed) {
            const double fraction = (std::min)(1.0, (now - started - items[index].fixed) / items[index].data);
            progress.completed.bytes += static_cast<uint64_t>(static_cast<double>(items[index].bytes) * fraction);
        }
        estimator.Observe(tick, progress);
        if (now < from || now > until || duration - now < 1) continue;
        ++result.samples;
        if (const auto eta = estimator.Seconds()) {
            ++result.estimates;
            const double error = std::abs(static_cast<double>(eta) - (duration - now));
            result.error += error;
            result.worst = (std::max)(result.worst, error);
        }
    }
    std::printf("ETA_REPLAY case=%s mode=%s samples=%zu estimates=%zu mae_s=%.3f max_error_s=%.3f duration_s=%.3f\n",
        label, legacy ? "legacy" : "workload", result.samples, result.estimates,
        result.estimates ? result.error / static_cast<double>(result.estimates) : -1, result.worst, duration);
    return result;
}
void Replays() {
    const auto accurate = [](const ReplayResult& result, double maximum_mean) {
        return result.samples > 0 && result.estimates * 10 >= result.samples * 7 &&
            result.error / static_cast<double>((std::max)(size_t{1}, result.estimates)) <= maximum_mean;
    };
    std::vector<Item> items(240, {8ull << 20, 0.01, 0.25});
    Check(accurate(Replay("large-files", items, 4), 2.5), "large-file ETA preserves useful coverage and low mean error");
    items.assign(3000, {0, 0.01, 0});
    Check(accurate(Replay("empty-files", items, 3), 1.5), "zero-byte files have a real per-file ETA, not an absent byte estimate");
    items.assign(400, {0, 0.05, 0, true});
    Check(accurate(Replay("directories", items, 3), 1.5), "directory-only work is estimated without inventing transferred bytes");
    items.clear();
    for (unsigned group = 0; group < 64; ++group) {
        items.push_back({0, 0.0005, 0, true});
        for (unsigned file = 0; file < 256; ++file)
            items.push_back({file % 2 ? 512ull : 0ull, 0.001, file % 2 ? 512.0 / (32ull << 20) : 0.0});
    }
    items.insert(items.end(), 64, {0, 0.0005, 0, true});
    Check(accurate(Replay("directory-create-and-finalize", items, 3), 1.5),
        "interleaved directory preparation and deferred metadata retain useful ETA coverage");
    items.clear();
    for (unsigned i = 0; i < 40; ++i) {
        items.push_back({8ull << 20, 0.01, 0.25});
        for (unsigned j = 0; j < 40; ++j) items.push_back({1024, 0.01, 1024.0 / (32ull << 20)});
    }
    items.insert(items.end(), 1000, {0, 0.01, 0});
    Check(accurate(Replay("mixed-and-empty-tail", items, 4), 3.0), "mixed byte and item costs predict remaining metadata work");
    Check(accurate(Replay("after-last-byte", items, 27), 1.5), "finishing all bytes does not erase ETA while many files remain");
    items.assign(100, {8ull << 20, 0.01, 0.1});
    items.insert(items.end(), 100, {8ull << 20, 0.01, 0.5});
    const auto slowdown = Replay("sustained-slowdown", items, 20);
    Check(accurate(slowdown, 4.0) && slowdown.worst <= 8.0,
        "a sustained device slowdown is followed without retaining the old fast rate");
    items.assign(1000, {0, 0.01, 0});
    items.push_back({200ull << 20, 0.01, 20});
    Check(accurate(Replay("small-to-large", items, 14), 2.0), "the estimator learns a new byte-dominated phase after small-file work");
    items.assign(800, {0, 0.01, 0});
    items.insert(items.end(), 15, {0, 0.05, 0});
    items.insert(items.end(), 4000, {0, 0.01, 0});
    const auto transient = Replay("short-stall", items, 8, 16);
    Check(accurate(transient, 4.0) && transient.worst <= 10.0,
        "a subsecond stall does not become the forecast for all remaining files");
    items.assign(800, {0, 0.01, 0});
    for (unsigned burst = 0; burst < 4; ++burst) {
        items.insert(items.end(), 5, {0, 0.05, 0});
        items.insert(items.end(), 55, {0, 0.25 / 55, 0});
    }
    items.insert(items.end(), 4000, {0, 0.01, 0});
    const auto alternating = Replay("alternating-callback-batches", items, 8, 18);
    Check(accurate(alternating, 4.0) && alternating.worst <= 10.0,
        "alternating slow and fast batches preserve their aggregate throughput");
    items.assign(800, {0, 0.01, 0});
    items.insert(items.end(), 4000, {0, 0.004, 0});
    const auto faster = Replay("sustained-speedup", items, 12);
    Check(accurate(faster, 2.0) && faster.worst <= 5.0,
        "transient protection still learns a sustained faster device rate");
}
void NoisyTransitions() {
    TransferEtaEstimator model;
    model.Reset(0);
    TransferWorkProgress work{{}, {0, 4835, 0}, true};
    const unsigned burst[]{12, 4, 6, 3, 6, 4};
    double error = 0, worst = 0;
    unsigned samples = 0, missing = 0;
    for (uint64_t tick = 250; tick <= 14000; tick += 250) {
        work.completed.files += tick <= 8000 || tick > 9500 ? 25 : burst[(tick - 8250) / 250];
        model.Observe(tick, work);
        if (tick < 8000) continue;
        ++samples;
        if (!model.seconds()) { ++missing; continue; }
        const double current_error = std::abs(static_cast<double>(model.seconds()) - (49.5 - tick / 1000.0));
        error += current_error;
        worst = (std::max)(worst, current_error);
    }
    Check(missing <= 1 && worst <= 15 && error / (samples - missing) <= 6,
        "an irregular 1.5-second slowdown does not erase the established transfer history");
    model.Reset(0);
    work = {{}, {0, 4800, 0}, true};
    const unsigned sustained[]{4, 6, 5, 4, 6, 5};
    samples = missing = 0;
    error = worst = 0;
    for (uint64_t tick = 250; tick <= 26000; tick += 250) {
        work.completed.files += tick <= 8000 ? 25 : sustained[((tick - 8250) / 250) % 6];
        model.Observe(tick, work);
        if (tick < 14000) continue;
        ++samples;
        if (!model.seconds()) { ++missing; continue; }
        const double current_error = std::abs(static_cast<double>(model.seconds()) - (208.0 - tick / 1000.0));
        error += current_error;
        worst = (std::max)(worst, current_error);
    }
    Check(missing <= 1 && worst <= 12 && error / (samples - missing) <= 6,
        "a sustained slower rate with ordinary variation still replaces the old fast rate");
    model.Reset(0);
    work = {{}, {0, 4800, 0}, true};
    samples = missing = 0;
    error = worst = 0;
    for (uint64_t tick = 250; tick <= 26000; tick += 250) {
        work.completed.files += tick <= 8000 ? 25 : tick == 8250 ? 1 : 5;
        model.Observe(tick, work);
        if (tick < 14000) continue;
        ++samples;
        if (!model.seconds()) { ++missing; continue; }
        const double current_error = std::abs(static_cast<double>(model.seconds()) - (208.2 - tick / 1000.0));
        error += current_error;
        worst = (std::max)(worst, current_error);
    }
    Check(missing <= 1 && worst <= 12 && error / (samples - missing) <= 6,
        "an initial extreme outlier cannot block learning a subsequent stable slower rate");
}
void EarlyObservations() {
    TransferEtaEstimator control, faster;
    control.Reset(0); faster.Reset(0);
    TransferWorkProgress work{{}, {0, 10000, 0}, true};
    for (uint64_t tick = 265; tick <= 795; tick += 265) {
        work.completed.files += 10;
        control.Observe(tick, work); faster.Observe(tick, work);
    }
    work.completed.files = 38; control.Observe(1000, work);
    work.completed.files = 46; faster.Observe(1000, work);
    const auto normal_cost = control.ProjectedSeconds({0, 10000, 0});
    const auto faster_cost = faster.ProjectedSeconds({0, 10000, 0});
    Check(normal_cost > 0 && faster_cost > 0 && faster_cost < normal_cost,
        "first fit includes measured tail work when projecting the same remaining workload");
    control.Reset(0); faster.Reset(0);
    work.completed = {};
    for (const uint64_t tick : {265ull, 530ull, 876ull}) {
        work.completed.files += 10;
        control.Observe(tick, work); faster.Observe(tick, work);
    }
    work.completed.files = 38; control.Observe(1000, work);
    work.completed.files = 46; faster.Observe(1000, work);
    Check(control.ProjectedSeconds({0, 10000, 0}) > 0 &&
        control.ProjectedSeconds({0, 10000, 0}) == faster.ProjectedSeconds({0, 10000, 0}),
        "a 124ms first-fit tail does not bypass the minimum sample duration");
    control.Reset(0); work.completed = {};
    for (uint64_t tick = 250; tick <= 1000; tick += 250) {
        work.completed.files += 10; control.Observe(tick, work);
    }
    const auto previous = control.ProjectedSeconds({0, 10000, 0});
    work.completed.files += 20; control.Observe(1250, work);
    const auto refreshed = control.ProjectedSeconds({0, 10000, 0});
    Check(previous > 0 && refreshed > 0 && refreshed < previous,
        "an immature model refits real new work without waiting another full second");
    control.Reset(0); work.completed = {};
    for (uint64_t tick = 250; tick <= 1000; tick += 250) {
        work.completed.files += 10; control.Observe(tick, work);
    }
    work.completed.files += 250; control.Observe(1250, work);
    const auto inconsistent = control.seconds();
    for (uint64_t tick = 1500; tick <= 2500; tick += 250) {
        work.completed.files += 250; control.Observe(tick, work);
    }
    Check(inconsistent == 0 && control.seconds() > 0,
        "inconsistent early evidence withdraws an old fit and stable new work restores it");
}
void TimeWeightedStartup() {
    // Hold future work fixed; only already observed work/time enters the prediction.
    for (unsigned kind = 0; kind < 4; ++kind) {
        TransferEtaEstimator model;
        model.Reset(0);
        TransferWork completed{};
        double elapsed_mass = 0, work_mass = 0;
        uint64_t previous = 0;
        for (const uint64_t tick : {250ull, 1000ull, 1250ull, 2000ull}) {
            if (kind == 0 || kind == 3) completed.bytes += 10 * 1048576;
            if (kind == 1 || kind == 3) completed.files += 10;
            if (kind == 2 || kind == 3) completed.directories += 10;
            model.ObserveWork(tick, completed);
            const double dt = static_cast<double>(tick - previous) / 1000.0;
            const double decay = std::exp(-dt / 3.0);
            elapsed_mass = decay * elapsed_mass + (1.0 - decay);
            work_mass = decay * work_mass + (1.0 - decay) * 10.0 / dt;
            previous = tick;
        }
        TransferWork remaining{kind == 0 || kind == 3 ? 10000ull * 1048576 : 0,
            kind == 1 || kind == 3 ? 10000ull : 0, kind == 2 || kind == 3 ? 10000ull : 0};
        const double expected = 10000.0 * elapsed_mass / work_mass;
        const auto actual = model.ProjectedSeconds(remaining);
        std::printf("ETA_STARTUP kind=%u expected_s=%.6f projected_s=%llu\n",
            kind, expected, static_cast<unsigned long long>(actual));
        Check(actual > 0 && std::abs(static_cast<double>(actual) - expected) <= 2.0,
            "uniform startup uses elapsed-time-weighted work, not the fastest callback batches");
    }
    TransferEtaEstimator fine, coarse;
    fine.Reset(0); coarse.Reset(0);
    TransferWork completed{};
    for (uint64_t tick = 250; tick <= 2000; tick += 250) {
        completed.files += tick <= 1000 ? 5 : 10;
        fine.ObserveWork(tick, completed);
    }
    coarse.ObserveWork(500, {0, 10, 0});
    coarse.ObserveWork(1000, {0, 20, 0});
    coarse.ObserveWork(2000, {0, 60, 0});
    const auto fine_eta = fine.ProjectedSeconds({0, 10000, 0});
    const auto coarse_eta = coarse.ProjectedSeconds({0, 10000, 0});
    std::printf("ETA_PARTITION fine=%llu coarse=%llu\n",
        static_cast<unsigned long long>(fine_eta), static_cast<unsigned long long>(coarse_eta));
    Check(fine_eta > 0 && coarse_eta > 0 && std::abs(static_cast<double>(fine_eta) - coarse_eta) <= 1,
        "splitting identical constant-rate intervals does not change startup throughput");
}
void StartupIntervalRecovery() {
    constexpr uint64_t mib = 1048576;
    TransferEtaEstimator model;
    const auto cold_start = [&](bool known = true) {
        model.Reset(0);
        TransferWorkProgress work{{32 * mib, 0, 0}, {4096 * mib, 1, 0}, known};
        model.Observe(6000, work);
        return work;
    };
    auto work = cold_start();
    Check(model.seconds() == 0, "one slow startup interval does not justify a whole-file countdown");
    for (uint64_t tick : {6250ull, 6500ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    Check(model.seconds() == 0, "two fast intervals do not prematurely discard startup evidence");
    work.completed.bytes += 64 * mib; model.Observe(6750, work);
    const auto recovered = model.seconds();
    std::printf("ETA_BOOTSTRAP first_stable_tick=6750 eta=%llu\n", static_cast<unsigned long long>(recovered));
    Check(recovered >= 15 && recovered <= 16,
        "three consistent byte intervals recover a useful ETA after one slow startup interval");
    model.Observe(9750, work);
    Check(model.seconds() == 0, "startup recovery preserves the existing active-time stall deadline");

    model.Reset(0); work = {{32 * mib, 0, 0}, {4096 * mib, 1, 0}, true};
    model.Observe(999, work);
    for (uint64_t tick : {1249ull, 1499ull, 1749ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    Check(model.seconds() == 0, "a sub-warmup initial sample is not treated as a long startup interval");

    work = cold_start(false);
    for (uint64_t tick : {6250ull, 6500ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    work.total_known = true; work.completed.bytes += 32 * mib; model.Observe(6625, work);
    Check(model.seconds() == 0, "a first-fit tail cannot reduce the 750ms confirmation requirement");

    work = cold_start();
    for (uint64_t tick : {6250ull, 6500ull, 6750ull}) {
        work.completed.bytes += tick == 6500 ? 32 * mib : 64 * mib;
        model.Observe(tick, work);
    }
    Check(model.seconds() == 0, "inconsistent faster intervals retain startup uncertainty");

    model.Reset(0); work = {{}, {4096 * mib, 1, 0}, true};
    for (uint64_t tick : {6000ull, 12000ull, 18000ull}) {
        work.completed.bytes += 32 * mib; model.Observe(tick, work);
    }
    const auto slow = model.ProjectedSeconds({320 * mib, 0, 0});
    Check(slow >= 59 && slow <= 61, "a genuinely slow device retains its observed slow per-byte cost");

    model.Reset(0); work = {{32 * mib, 1, 0}, {4096 * mib, 11, 0}, true};
    model.Observe(6000, work);
    for (uint64_t tick : {6250ull, 6500ull, 6750ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    const auto mixed = model.ProjectedSeconds({320 * mib, 10, 0});
    Check(mixed > 50 && mixed < 65, "first-interval metadata cost is retained for future mixed work");

    work = cold_start(false);
    for (uint64_t tick : {6250ull, 6500ull, 6750ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    Check(model.seconds() == 0, "startup confirmation never invents an unknown total");
    work.total_known = true; model.Observe(6750, work);
    Check(model.seconds() >= 15 && model.seconds() <= 16,
        "known totals can use a confirmed startup rate without counting the same work twice");
    model.Reset(10000, {100 * mib, 0, 0});
    work = {{132 * mib, 0, 0}, {4196 * mib, 1, 0}, true}; model.Observe(16000, work);
    for (uint64_t tick : {16250ull, 16500ull, 16750ull}) {
        work.completed.bytes += 64 * mib; model.Observe(tick, work);
    }
    Check(model.seconds() >= 15 && model.seconds() <= 16,
        "reset re-arms startup handling using counter deltas rather than absolute counters");
}
void QueueStartupAndSlowdown() {
    std::vector<Item> queue(3, {1ull << 30, 1.5, 4});
    auto result = Replay("queue-repeated-preparation", queue, 1);
    Check(result.estimates * 2 >= result.samples && result.worst <= 2 &&
        result.error / static_cast<double>((std::max)(size_t{1}, result.estimates)) <= 1,
        "a multi-file queue does not retire a preparation cost that can recur");
    queue.clear();
    for (unsigned i = 0; i < 3; ++i) {
        queue.push_back({1ull << 30, 1.5, 4});
        for (unsigned j = 0; j < 1000; ++j) queue.push_back({4096, .001, 4096.0 / (256ull << 20)});
    }
    result = Replay("queue-large-small-preparation", queue, 1);
    Check(result.estimates * 100 >= result.samples * 65 && result.worst <= 3 &&
        result.error / static_cast<double>((std::max)(size_t{1}, result.estimates)) <= 1.5,
        "interleaved large and small files retain repeated preparation evidence");
    TransferEtaEstimator model;
    model.Reset(0);
    TransferWorkProgress work{{32ull << 20, 0, 0}, {4096ull << 20, 3, 0}, false};
    model.Observe(6000, work);
    for (uint64_t tick : {6250ull, 6500ull, 6750ull}) {
        work.completed.bytes += 64ull << 20; model.Observe(tick, work);
    }
    work.total_known = true;
    model.Observe(6750, work);
    Check(model.ProjectedSeconds({4096ull << 20, 0, 0}) == 0,
        "an unknown total cannot silently discard startup evidence before revealing a queue");
    queue.assign(100, {8ull << 20, .01, .03});
    queue.insert(queue.end(), 100, {8ull << 20, .01, .25});
    result = Replay("queue-eightfold-slowdown", queue, 7);
    Check(result.estimates * 10 >= result.samples * 9 && result.worst <= 2 &&
        result.error / static_cast<double>((std::max)(size_t{1}, result.estimates)) <= 1,
        "queue startup protection still adapts to a sustained eightfold data slowdown");
}
template<class Model>
void ObserveMode(Model& model, uint64_t tick, const TransferWorkProgress& work, bool concurrent) {
    if constexpr (requires { model.Observe(tick, work, concurrent); }) model.Observe(tick, work, concurrent);
    else model.Observe(tick, work);
}
void ConcurrentStartupCost() {
    struct Point { uint64_t tick, bytes, files; };
    // A retained real callback prefix; no finish time or future observation is used.
    const Point prefix[]{{265,14614528,447},{515,95092736,856},{765,172425216,1166},
        {1062,242286592,1762},{1312,348127232,2433},{1578,391446528,2730}};
    TransferEtaEstimator concurrent, serial;
    concurrent.Reset(0); serial.Reset(0);
    TransferWorkProgress work{{}, {3489660928ull,8195,0}, true};
    double time_mass = 0, byte_mass = 0;
    uint64_t last_tick = 0, last_bytes = 0;
    bool serial_preserved = true;
    const uint64_t expected_serial[]{6,4,4};
    for (size_t i = 0; i < std::size(prefix); ++i) {
        const auto& p = prefix[i]; work.completed = {p.bytes,p.files,0};
        ObserveMode(concurrent,p.tick,work,true); ObserveMode(serial,p.tick,work,false);
        const double dt = static_cast<double>(p.tick-last_tick)/1000.0, decay=std::exp(-dt/3.0);
        time_mass=decay*time_mass+(1-decay);
        byte_mass=decay*byte_mass+(1-decay)*static_cast<double>(p.bytes-last_bytes)/dt;
        last_tick=p.tick; last_bytes=p.bytes;
        if (i < 3) continue;
        const auto floor=static_cast<uint64_t>(std::ceil(static_cast<double>(work.total.bytes-p.bytes)*time_mass/byte_mass));
        std::printf("ETA_CONCURRENT tick=%llu serial=%llu concurrent=%llu observed_byte_floor=%llu\n",
            p.tick,serial.seconds(),concurrent.seconds(),floor);
        Check(concurrent.seconds() >= floor && concurrent.seconds() <= floor+1,
            "uncertain concurrent startup cannot extrapolate faster byte throughput than observed");
        serial_preserved &= serial.seconds()==expected_serial[i-3];
    }
    Check(serial_preserved,"the concurrent throughput safeguard leaves serial cost extrapolation unchanged");
    work.completed={475332608,2730,0};
    ObserveMode(concurrent,1906,work,true);ObserveMode(serial,1906,work,false);
    Check(concurrent.seconds()>0 && concurrent.seconds()==serial.seconds(),
        "an independent byte interval releases the mixed-progress throughput safeguard");
    concurrent.Reset(0); work={{},{20000ull<<20,1000,0},true};
    uint64_t tick=0;
    for(unsigned i=0;i<4;++i){tick+=i%2?400:300;work.completed.bytes+=100ull<<20;
        work.completed.files+=i%2?200:100;ObserveMode(concurrent,tick,work,true);}
    const auto fitted=concurrent.ProjectedSeconds({10000ull<<20,100,0});
    Check(fitted>=20 && fitted<=22,
        "a well-identified additive cost is not clamped merely because a parallel pool exists");
    concurrent.Reset(0);work={{},{3489660928ull,8195,0},true};
    for(size_t i=0;i<4;++i){const auto&p=prefix[i];work.completed={p.bytes,p.files,0};concurrent.Observe(p.tick,work);}
    Check(concurrent.seconds()==expected_serial[0],"reset and default serial mode do not retain a concurrent floor");
}
void Boundaries() {
    pulse::ops::TransferEtaEstimator directory_aging;
    directory_aging.Reset(0);
    TransferWorkProgress long_stream{{}, {600000, 6000, 2}, true};
    bool retained = true;
    for (uint64_t tick = 250; tick <= 20000; tick += 250) {
        long_stream.completed = {tick * 10, tick / 10, 1};
        directory_aging.Observe(tick, long_stream);
        if (tick >= 15000) retained = retained && directory_aging.seconds() > 0;
    }
    Check(retained && directory_aging.seconds() >= 35 && directory_aging.seconds() <= 45,
        "a pending directory finalization keeps ETA after its early sample ages out");
    directory_aging.Reset(0);
    for (uint64_t tick = 250; tick <= 20000; tick += 250) {
        long_stream.completed = {tick * 10, tick / 10, 0};
        directory_aging.Observe(tick, long_stream);
    }
    Check(directory_aging.seconds() == 0,
        "reset does not invent a directory cost that the new task never observed");
    Estimator estimator; estimator.Reset();
    TransferWorkProgress progress{{}, {0, 1000, 0}, true};
    for (uint64_t tick = 250; tick <= 4000; tick += 250) {
        progress.completed.files = tick / 25;
        estimator.Observe(tick, progress);
    }
    const auto before = estimator.Seconds();
    Check(before >= 19 && before <= 23, "stable item work is calibrated against actual elapsed active time");
    progress.total_known = false;
    estimator.Observe(4100, progress);
    Check(estimator.Seconds() == 0, "an unfinished enumeration never advertises a total-task countdown");
    progress.total_known = true;
    estimator.Observe(4200, progress);
    Check(estimator.Seconds() > 0, "known totals can reuse learned rates without restarting the task");
    progress.total.files = 240;
    estimator.Observe(4250, progress);
    Check(estimator.Seconds() >= 1 && estimator.Seconds() <= 3,
        "skipping work changes remaining totals without inflating completed-work samples");
    progress.total.files = 1000;
    estimator.Observe(4500, progress);
    for (uint64_t tick = 4750; tick <= 8500; tick += 250) estimator.Observe(tick, progress);
    Check(estimator.Seconds() == 0, "stalled work withdraws its stale countdown instead of promising the old time");
    progress.completed.files = 340;
    estimator.Observe(9000, progress);
    Check(estimator.Seconds() > 0, "progress after a temporary stall can produce a fresh estimate");
    progress.completed = progress.total;
    estimator.Observe(9050, progress);
    Check(estimator.Seconds() == 0, "no remaining work clears ETA without setting the operation terminal");
    estimator.Reset(); progress = {{}, {0, 1000, 0}, true};
    for (uint64_t tick = 250; tick <= 4000; tick += 250) {
        progress.completed.files = tick / 25; estimator.Observe(tick, progress);
    }
    const auto pause_eta = estimator.Seconds();
    estimator.Observe(4000, progress); // a human wait is omitted from the existing active-time clock
    progress.completed.files += 10; estimator.Observe(4250, progress);
    Check(pause_eta > 0 && estimator.Seconds() > 0 &&
        std::abs(static_cast<double>(pause_eta) - static_cast<double>(estimator.Seconds())) <= 1,
        "pause/resume keeps its learned model when no active time elapsed");
    progress.completed.files = 1; estimator.Observe(4500, progress);
    Check(estimator.Seconds() == 0, "counter rollback invalidates the old model rather than reusing a different work history");
    estimator.Reset(); progress = {{}, {1000000, 1000, 0}, true};
    for (uint64_t tick = 250; tick <= 4000; tick += 250) {
        progress.completed = {tick * 100, tick / 10, 0}; estimator.Observe(tick, progress);
    }
    progress.total = {1000000000, 401, 0}; estimator.Observe(4250, progress);
    Check(estimator.Seconds() == 0, "a collinear sample cannot justify extrapolation to a previously unseen workload mix");
    estimator.Reset(); progress = {{}, {1000000000, 0, 0}, true};
    for (uint64_t tick = 250; tick <= 4000; tick += 250) {
        progress.completed.bytes = tick * 10000; estimator.Observe(tick, progress);
    }
    Check(estimator.Seconds() > 0, "byte-only work produces a countdown before a simulated stall");
    const uint64_t before_batch = estimator.Seconds();
    progress.completed.bytes += 20000000;
    estimator.Observe(4250, progress);
    Check(estimator.Seconds() > 0 && estimator.Seconds() + 1 < before_batch,
        "newly completed batches refresh remaining work without waiting for another fit");
    for (uint64_t tick = 4500; tick <= 8500; tick += 250) estimator.Observe(tick, progress);
    Check(estimator.Seconds() == 0, "a byte-transfer stall also invalidates its previously positive countdown");
    estimator.Observe(1000, progress);
    Check(estimator.Seconds() == 0, "a backward clock invalidates the previous estimate safely");
    TransferEtaEstimator independent;
    independent.Reset(0);
    TransferWork separate;
    bool independent_costs = true;
    for (uint64_t tick = 250; tick <= 10000; tick += 250) {
        if ((tick / 250) % 2) separate.bytes += 1048576;
        else separate.files += 10;
        independent.ObserveWork(tick, separate);
        if (tick >= 3000) {
            const auto projected = independent.ProjectedSeconds({20 * 1048576, 1000, 0});
            independent_costs = independent_costs && projected >= 28 && projected <= 32;
        }
    }
    Check(independent_costs, "callback aggregation preserves independently observed byte and file costs across warmup");
    independent.Reset(0);
    separate = {};
    uint64_t varied_tick = 0;
    bool same_support_costs = true;
    for (unsigned i = 0; i < 40; ++i) {
        varied_tick += i % 2 ? 300 : 250;
        separate.bytes += 1048576;
        separate.files += i % 2 ? 14 : 10;
        independent.ObserveWork(varied_tick, separate);
        if (i >= 12) {
            const auto projected = independent.ProjectedSeconds({20 * 1048576, 1000, 0});
            same_support_costs = same_support_costs && projected >= 13 && projected <= 17;
        }
    }
    Check(same_support_costs, "callback aggregation retains cost information from modest changes in byte/file proportions");
    Check(sizeof(TransferEtaEstimator) <= 4096, "workload history has a fixed small memory bound independent of file count");
}
#ifdef _WIN32
void NativeCounters(bool parallel, unsigned workers) {
    using namespace pulse::ops;
    wchar_t exe[32768]{};
    if (!GetModuleFileNameW(nullptr, exe, static_cast<DWORD>(std::size(exe)))) {
        Check(false, "find executable directory for owned native fixture"); return;
    }
    const auto parent = std::filesystem::path(exe).parent_path() / L"bench_data";
    std::filesystem::create_directories(parent);
    const auto root = parent / (L"eta-counts-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
        std::to_wstring(GetTickCount64()) + (parallel ? L"-parallel-" : L"-serial-") + std::to_wstring(workers));
    if (!CreateDirectoryW(root.c_str(), nullptr)) {
        Check(false, "exclusively create a new owned native-delete fixture"); return;
    }
    std::filesystem::create_directory(root / L"a");
    std::filesystem::create_directory(root / L"b");
    for (const auto* folder : {L"a", L"b"})
        for (unsigned i = 0; i < 128; ++i) {
            std::ofstream file(root / folder / (std::to_wstring(i) + L".txt")); file << "owned eta fixture";
        }
    { std::ofstream file(root / L"root.txt"); file << "owned eta fixture"; }
    size_t files = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) if (entry.is_regular_file()) ++files;
    if (files != 257) { Check(false, "owned fixture setup completed before any deletion"); return; }
    std::atomic<bool> cancel{false};
    TransferWorkProgress last;
    uint64_t reports = 0;
    bool monotonic = true, unknown_is_not_complete = true;
    auto report = [&](const std::wstring&, float, const TransferWorkProgress* work = nullptr) {
        if (!work) return;
        monotonic = monotonic && work->completed.files >= last.completed.files &&
            work->completed.directories >= last.completed.directories;
        if (work->total_known && work->completed.files < work->total.files) unknown_is_not_complete = false;
        last = *work;
        ++reports;
    };
    const auto outcome = parallel ? native_delete::DeleteNativeParallel(root.wstring(), cancel, report, workers)
                                  : native_delete::DeleteNative(root.wstring(), cancel, report, workers);
    std::printf("NATIVE_ETA_COUNTERS mode=%s workers=%u reports=%llu files=%llu directories=%llu\n",
        parallel ? "parallel" : "serial", workers, static_cast<unsigned long long>(reports),
        static_cast<unsigned long long>(last.completed.files), static_cast<unsigned long long>(last.completed.directories));
    Check(outcome.done && !std::filesystem::exists(root), parallel ? "parallel owned deletion still removes its own root" : "serial owned deletion still removes its own root");
    Check(reports > 0 && monotonic && last.completed.files == 257 && last.completed.directories == 3,
        parallel ? "parallel delete exports actual monotonic child counts" : "serial delete exports actual monotonic child counts");
    Check(last.total_known && last.total.files == 257 && last.total.directories == 3 && unknown_is_not_complete,
        parallel ? "parallel totals become known only from actual traversal" : "serial totals become known only from actual traversal");
}
#endif
void LargeCopyPhases(bool baseline = false) {
    using pulse::ops::LargeCopyEtaEstimator;
    constexpr uint64_t bytes = 1ull << 30;
    constexpr uint64_t files = 10;
    LargeCopyEtaEstimator phases(bytes * files, files);
    TransferEtaEstimator old;
    old.Reset(0);
    TransferWorkProgress work{{}, {bytes * files, files, 0}, true};
    double phase_error = 0, old_error = 0, selected_worst = 0;
    size_t samples = 0, old_missing = 0;
    uint64_t tick = 0;
    for (uint64_t file = 0; file < files; ++file) {
        phases.Begin(file, tick, bytes);
        tick += 1500;
        phases.Prepared(file, tick);
        for (uint64_t chunk = 1; chunk <= 8; ++chunk) {
            tick += 250;
            phases.Progress(file, tick, bytes * chunk / 8);
            work.completed = {bytes * file + bytes * chunk / 8, file, 0};
            old.Observe(tick, work);
            if (file >= 3) {
                const double actual = static_cast<double>(files * 3600 - tick) / 1000.0;
                const auto fresh = phases.Seconds(tick), previous = old.seconds();
                const double fresh_error = std::abs(static_cast<double>(fresh) - actual);
                const double previous_error = std::abs(static_cast<double>(previous) - actual);
                phase_error += fresh_error;
                old_error += previous_error;
                if (!previous) ++old_missing;
                selected_worst = (std::max)(selected_worst, baseline ? previous_error : fresh_error);
                ++samples;
            }
        }
        tick += 100;
        phases.Finish(file, tick, true);
        work.completed = {bytes * (file + 1), file + 1, 0};
        old.Observe(tick, work);
    }
    std::printf("LARGE_PHASE_REPLAY files=10 setup_ms=1500 body_ms=2000 tail_ms=100 samples=%zu phase_mae=%.3f old_mae=%.3f old_missing=%zu selected_worst=%.3f\n",
        samples, phase_error / static_cast<double>(samples), old_error / static_cast<double>(samples), old_missing, selected_worst);
    Check(samples == 56 && selected_worst <= 1.0,
        "ten serial large files retain separately measured recurring setup without byte-time double counting");
    if (baseline) return;
    Check(phases.Seconds(tick) == 0, "completed phase queue withdraws ETA independently of coordinator retirement");

    const auto complete = [&](LargeCopyEtaEstimator& model, uint64_t id, uint64_t start) {
        model.Begin(id, start, bytes);
        model.Prepared(id, start + 1500);
        model.Progress(id, start + 3500, bytes);
        model.Finish(id, start + 3600, true);
    };
    const auto warm = [&](LargeCopyEtaEstimator& model) { complete(model, 0, 0); complete(model, 1, 3600); complete(model, 2, 7200); };
    LargeCopyEtaEstimator maturity(bytes * files, files);
    complete(maturity, 0, 0);
    Check(maturity.Seconds(3600) == 0, "one complete file cannot establish recurring large-copy phase costs");
    complete(maturity, 1, 3600);
    Check(maturity.Seconds(7200) == 0, "two completed files are still insufficient for phase replacement");
    complete(maturity, 2, 7200);
    Check(maturity.Seconds(10800) == 26, "three completed files establish setup plus data plus tail for seven pending files");
    maturity.Begin(3, 10800, bytes);
    Check(maturity.Seconds(11800) == 25, "active setup subtracts only its elapsed time instead of billing it twice");
    maturity.Prepared(3, 12300);
    maturity.Progress(3, 13300, bytes / 2);
    Check(maturity.Seconds(13300) == 23, "partial file predicts remaining bytes without charging another setup");
    Check(maturity.Seconds(16300) == 0, "phase prediction also withdraws after three seconds without byte progress");
    maturity.Progress(3, 14300, bytes);
    Check(maturity.Seconds(14350) == 22, "last-byte completion retains only the unconsumed current tail");
    maturity.Finish(3, 14400, true);
    Check(maturity.Seconds(14400) == 22, "worker completion advances phase totals before coordinator publication");
    maturity.Invalidate();
    complete(maturity, 4, 14400);
    Check(maturity.Seconds(18000) == 0, "pause or scheduler-wait invalidation never relearns within the same task");

    LargeCopyEtaEstimator overlap(bytes * files, files); warm(overlap);
    overlap.Begin(3, 10800, bytes); overlap.Begin(4, 10801, bytes);
    overlap.Prepared(3, 12300); overlap.Progress(3, 14300, bytes); overlap.Finish(3, 14400, true);
    Check(overlap.Seconds(14400) == 0, "actual overlap permanently disables summed serial phase prediction");
    LargeCopyEtaEstimator order(bytes * files, files); warm(order);
    order.Begin(3, 10800, bytes); order.Progress(3, 11050, bytes / 8);
    Check(order.Seconds(11050) == 0, "bytes before the prepared marker invalidate an unidentifiable setup");
    LargeCopyEtaEstimator clock(bytes * files, files); warm(clock);
    clock.Begin(3, 10800, bytes); clock.Prepared(3, 10799);
    Check(clock.Seconds(11600) == 0, "backward event timestamps invalidate phase samples");
    LargeCopyEtaEstimator failure(bytes * files, files); warm(failure);
    failure.Begin(3, 10800, bytes); failure.Finish(3, 10900, false);
    Check(failure.Seconds(10900) == 0, "failed or fallback attempts cannot contribute successful phase costs");
    LargeCopyEtaEstimator incomplete(bytes * files, files); warm(incomplete);
    incomplete.Begin(3, 10800, bytes); incomplete.Prepared(3, 12300); incomplete.Progress(3, 13300, bytes / 2);
    incomplete.Finish(3, 13400, true);
    Check(incomplete.Seconds(13400) == 0, "a successful finish with missing bytes invalidates inconsistent accounting");
    LargeCopyEtaEstimator identity(bytes * files, files); warm(identity);
    identity.Begin(0, 10800, bytes);
    Check(identity.Seconds(10800) == 0, "duplicate attempt identifiers cannot charge a completed file twice");
    LargeCopyEtaEstimator excess(bytes * files, files); warm(excess);
    excess.Begin(3, 10800, bytes); excess.Prepared(3, 12300); excess.Progress(3, 12400, bytes + 1);
    Check(excess.Seconds(12400) == 0, "progress beyond the declared file size invalidates instead of underflowing remaining bytes");
    LargeCopyEtaEstimator unknown(0, 0);
    Check(unknown.Seconds(0) == 0, "empty phase queues return unknown without division by zero");
    LargeCopyEtaEstimator fresh(bytes * files, files);
    Check(fresh.Seconds(7200) == 0, "a separate task cannot inherit completed phase samples");
    LargeCopyEtaEstimator overflow((std::numeric_limits<uint64_t>::max)(), 4);
    for (uint64_t id = 0; id < 3; ++id) {
        overflow.Begin(id, id * 2001, 1); overflow.Prepared(id, id * 2001 + 1);
        overflow.Progress(id, id * 2001 + 2001, 1); overflow.Finish(id, id * 2001 + 2001, true);
    }
    Check(overflow.Seconds(6003) == (std::numeric_limits<uint64_t>::max)(),
        "extreme finite projections saturate instead of overflowing integer seconds");
}
void LargeCopyConfidence() {
    using pulse::ops::LargeCopyEtaEstimator;
    constexpr uint64_t bytes = 4ull << 30;
    const auto sample = [&](LargeCopyEtaEstimator& model, uint64_t id, uint64_t& tick, uint64_t setup, uint64_t data) {
        model.Begin(id, tick, bytes);
        tick += setup; model.Prepared(id, tick);
        tick += data; model.Progress(id, tick, bytes);
        tick += 100; model.Finish(id, tick, true);
    };
    LargeCopyEtaEstimator variable(bytes * 12, 12);
    uint64_t tick = 0;
    sample(variable, 0, tick, 0, 8000);
    sample(variable, 1, tick, 1500, 8000);
    sample(variable, 2, tick, 6000, 8000);
    Check(variable.Seconds(tick) == 0, "high-variation setup samples cannot justify phase replacement");
    for (uint64_t id = 3; id < 11; ++id) sample(variable, id, tick, 1500, 8000);
    Check(variable.Seconds(tick) == 10, "stable replacement of the rolling setup window can regain confidence without resetting the task");
    LargeCopyEtaEstimator zero(bytes * 4, 4);
    tick = 0;
    for (uint64_t id = 0; id < 3; ++id) sample(zero, id, tick, 0, 8000);
    Check(zero.Seconds(tick) == 0, "zero measured setup adds no information warranting phase replacement");

    LargeCopyEtaEstimator phases(bytes * 4, 4);
    TransferEtaEstimator old; old.Reset(0);
    TransferWorkProgress work{{}, {bytes * 4, 4, 0}, true};
    tick = 0;
    bool withdrawn = true;
    double old_error = 0, selected_error = 0;
    size_t comparisons = 0;
    constexpr uint64_t finish = 3 * 17600 + 65600;
    for (uint64_t file = 0; file < 4; ++file) {
        phases.Begin(file, tick, bytes);
        tick += 1500; phases.Prepared(file, tick);
        const uint64_t interval = file < 3 ? 250 : 1000;
        for (uint64_t chunk = 1; chunk <= 64; ++chunk) {
            tick += interval;
            const uint64_t done = bytes * chunk / 64;
            phases.Progress(file, tick, done);
            work.completed = {file * bytes + done, file, 0};
            old.Observe(tick, work);
            const uint64_t original = old.seconds(), phase = phases.Seconds(tick);
            const uint64_t selected = original && phase ? phase : original;
            if (file == 3 && chunk >= 3) withdrawn &= phase == 0;
            if (file == 3 && chunk >= 5) {
                const double actual = static_cast<double>(finish - tick) / 1000.0;
                old_error += std::abs(static_cast<double>(original) - actual);
                selected_error += std::abs(static_cast<double>(selected) - actual);
                ++comparisons;
            }
        }
        tick += 100; phases.Finish(file, tick, true);
        work.completed = {(file + 1) * bytes, file + 1, 0};
        old.Observe(tick, work);
    }
    old_error /= static_cast<double>(comparisons);
    selected_error /= static_cast<double>(comparisons);
    std::printf("LARGE_PHASE_SLOWDOWN files=4 trained=3 current_cost_ratio=4 samples=%zu old_mae=%.3f selected_mae=%.3f withdrawn=%d\n",
        comparisons, old_error, selected_error, withdrawn ? 1 : 0);
    Check(withdrawn, "three sustained slow byte intervals withdraw stale completed-file phase costs");
    Check(tick == finish && comparisons == 60 && old_error <= 1.0 && selected_error <= old_error + 1e-9,
        "four-file slowdown preserves the aggregate model's real-time adaptation instead of overriding it");

    LargeCopyEtaEstimator fast(bytes * 4, 4);
    tick = 0;
    for (uint64_t id = 0; id < 3; ++id) sample(fast, id, tick, 1500, 8000);
    fast.Begin(3, tick, bytes); tick += 1500; fast.Prepared(3, tick);
    for (uint64_t part = 1; part <= 3; ++part) { tick += 125; fast.Progress(3, tick, bytes * part / 16); }
    Check(fast.Seconds(tick) > 0, "short fast callback bursts cannot satisfy the two-second mismatch persistence requirement");
    for (uint64_t part = 4; part <= 16; ++part) { tick += 125; fast.Progress(3, tick, bytes * part / 16); }
    Check(fast.Seconds(tick) == 0, "sustained faster data also retires old phase costs instead of overestimating the new regime");

    LargeCopyEtaEstimator transient(bytes * 4, 4);
    tick = 0;
    for (uint64_t id = 0; id < 3; ++id) sample(transient, id, tick, 1500, 8000);
    transient.Begin(3, tick, bytes); tick += 1500; transient.Prepared(3, tick);
    tick += 2000; transient.Progress(3, tick, bytes / 16);
    tick += 2000; transient.Progress(3, tick, bytes * 2 / 16);
    Check(transient.Seconds(tick) > 0, "two slow intervals alone do not establish a persistent regime change");
    for (unsigned i = 0; i < 5; ++i) transient.Progress(3, tick, bytes * 2 / 16);
    Check(transient.Seconds(tick) > 0, "duplicate byte snapshots cannot manufacture extra mismatch observations");
    tick += 500; transient.Progress(3, tick, bytes * 3 / 16);
    tick += 2000; transient.Progress(3, tick, bytes * 4 / 16);
    Check(transient.Seconds(tick) > 0, "a normal byte interval resets consecutive mismatch evidence");
}
void ResidualRecovery() {
    constexpr uint64_t mib = 1ull << 20;
    const auto stalled = [&](TransferEtaEstimator& model, uint64_t files = 1) {
        model.Reset(0);
        TransferWorkProgress work{{}, {1024 * mib, files, 0}, true};
        for (uint64_t tick = 250; tick <= 10000; tick += 250) {
            work.completed.bytes += 4 * mib;
            model.Observe(tick, work);
        }
        for (uint64_t tick = 10250; tick <= 14000; tick += 250) model.Observe(tick, work);
        return work;
    };
    const auto resume = [&](TransferEtaEstimator& model, TransferWorkProgress& work, uint64_t step = 4ull << 20) {
        for (uint64_t tick = 14250; tick <= 15250; tick += 250) {
            work.completed.bytes += step;
            model.Observe(tick, work);
        }
    };
    TransferEtaEstimator recovered;
    auto work = stalled(recovered);
    Check(recovered.seconds() == 0, "residual recovery does not override the existing three-second stall gate");
    work.completed.bytes += 4 * mib; recovered.Observe(14250, work);
    Check(recovered.seconds() == 0, "one post-stall observation cannot establish a recovery model");
    for (uint64_t tick = 14500; tick <= 15250; tick += 250) {
        work.completed.bytes += 4 * mib; recovered.Observe(tick, work);
    }
    Check(recovered.seconds() >= 52 && recovered.seconds() <= 54,
        "four causally consistent recent intervals recover while the long mixed-regime residual rejects");
    Check(recovered.ProjectedSeconds({64 * mib, 0, 0}) == 4,
        "recent recovery supports the existing workload projection without inventing progress");
    for (uint64_t tick = 15500; tick <= 18500; tick += 250) recovered.Observe(tick, work);
    Check(recovered.seconds() == 0, "a second stall withdraws a recent-window prediction just like a long prediction");
    recovered.Reset(19000, work.completed); recovered.Observe(20000, work);
    Check(recovered.seconds() == 0 && recovered.ProjectedSeconds({64 * mib, 0, 0}) == 0,
        "reset clears recent-window prediction and does not leak it into a new history");

    TransferEtaEstimator slower;
    auto slow_work = stalled(slower);
    resume(slower, slow_work, mib);
    Check(slower.seconds() >= 213 && slower.seconds() <= 217,
        "recovery learns a sustained slower byte regime instead of reusing last-valid fast coefficients");
    TransferEtaEstimator unknown;
    auto unknown_work = stalled(unknown);
    unknown_work.total_known = false;
    resume(unknown, unknown_work);
    Check(unknown.seconds() == 0, "unknown task totals remain unknown despite consistent recent throughput");
    TransferEtaEstimator missing;
    auto missing_work = stalled(missing, 3);
    resume(missing, missing_work);
    Check(missing.seconds() == 0, "byte-only recovery cannot invent metadata costs for multiple unfinished files");

    TransferEtaEstimator held_out;
    auto held_work = stalled(held_out);
    for (uint64_t tick = 14250; tick <= 15000; tick += 250) {
        held_work.completed.bytes += 4 * mib; held_out.Observe(tick, held_work);
    }
    held_work.completed.bytes += mib; held_out.Observe(15250, held_work);
    Check(held_out.seconds() == 0,
        "a fourth interval that disagrees with the preceding three rejects short-window recovery");
}
void Bench() {
    TransferEtaEstimator estimator; estimator.Reset(0);
    TransferWorkProgress progress{{}, {4000000000000ull, 1000000000, 0}, true};
    const auto begin = std::chrono::steady_clock::now();
    for (uint64_t i = 1; i <= 20000; ++i) {
        progress.completed = {i * 20000, i * 5, 0};
        estimator.Observe(i * 100, progress);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - begin).count();
    std::printf("ETA_BENCH observations=20000 simulated_ms=2000000 elapsed_us=%lld object_bytes=%zu final_eta=%llu\n",
        static_cast<long long>(elapsed), sizeof(estimator), static_cast<unsigned long long>(estimator.seconds()));
}
}
int main(int argc, char** argv) {
    legacy = argc > 1 && std::string_view(argv[1]) == "--legacy";
#ifndef _WIN32
    if (legacy) return 2;
#endif
    if (argc > 1 && std::string_view(argv[1]) == "--bench") { Bench(); return 0; }
    if (argc > 1 && std::string_view(argv[1]) == "--recovery") {
        ResidualRecovery();
        std::printf("RESIDUAL_RECOVERY_RESULT checks=%d failures=%d\n", checks, failures);
        return failures ? 1 : 0;
    }
    if (argc > 1 && (std::string_view(argv[1]) == "--phases" || std::string_view(argv[1]) == "--phase-baseline")) {
        LargeCopyPhases(std::string_view(argv[1]) == "--phase-baseline");
        if (std::string_view(argv[1]) == "--phases") LargeCopyConfidence();
        std::printf("LARGE_PHASE_RESULT checks=%d failures=%d\n", checks, failures);
        return failures ? 1 : 0;
    }
    Replays(); Boundaries();
    if (!legacy) { NoisyTransitions(); EarlyObservations(); TimeWeightedStartup(); StartupIntervalRecovery(); QueueStartupAndSlowdown(); ConcurrentStartupCost(); LargeCopyPhases(); LargeCopyConfidence(); ResidualRecovery(); }
#ifdef _WIN32
    if (!legacy) { NativeCounters(false, 1); NativeCounters(false, 4); NativeCounters(true, 4); }
#endif
    std::printf("TRANSFER_ETA_RESULT mode=%s checks=%d failures=%d\n", legacy ? "legacy" : "workload", checks, failures);
    return failures ? 1 : 0;
}
