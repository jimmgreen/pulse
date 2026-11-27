#include "eta_reference_models.h"
#ifndef ETA_HEADER
#define ETA_HEADER "../ops/transfer_eta_estimator.h"
#endif
#include ETA_HEADER
#include <charconv>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace pulse::bench::eta_reference;
constexpr std::array<const char*, 9> kNames = {"pulse_mixed", "indicatif_bytes", "rclone_bytes", "tqdm_bytes",
    "rich_bytes", "indicatif_items", "rclone_items", "tqdm_items", "rich_items"};
constexpr const char* kRawHeader = "task,wall_tick,active_tick,bytes,files,dirs,total_bytes,total_files,total_dirs,total_known,eta,reset";
constexpr const char* kConcurrentRawHeader = "task,wall_tick,active_tick,bytes,files,dirs,total_bytes,total_files,total_dirs,total_known,eta,reset,concurrent";
constexpr const char* kFinishHeader = "task,start,finish,phase,failed";

std::vector<uint64_t> Parse(std::string line, size_t count) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::vector<uint64_t> values;
    size_t begin = 0;
    while (true) {
        const size_t end = line.find(',', begin);
        const size_t stop = end == std::string::npos ? line.size() : end;
        uint64_t value = 0;
        auto result = std::from_chars(line.data() + begin, line.data() + stop, value);
        if (begin == stop || result.ec != std::errc() || result.ptr != line.data() + stop)
            throw std::runtime_error("invalid unsigned integer CSV field");
        values.push_back(value);
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    if (values.size() != count) throw std::runtime_error("incorrect CSV field count");
    return values;
}
void Header(std::istream& input, const char* expected) {
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("missing CSV header");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line != expected) throw std::runtime_error("unexpected CSV header");
}
size_t RawHeader(std::istream& input) {
    std::string line;
    if (!std::getline(input, line)) throw std::runtime_error("missing CSV header");
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line == kRawHeader) return 12;
    if (line == kConcurrentRawHeader) return 13;
    throw std::runtime_error("unexpected raw CSV header");
}
std::vector<uint64_t> ParseRaw(const std::string& line, size_t columns) {
    auto values = Parse(line, columns);
    if (values[9] > 1 || values[11] > 2 || (columns == 13 && values[12] > 1))
        throw std::runtime_error("invalid known/reset/concurrent flag");
    return values;
}
template<class Estimator>
void ObserveModel(Estimator& model, uint64_t tick, const pulse::ops::TransferWorkProgress& work, bool concurrent) {
    // Keep the expression dependent so saved two-argument baseline headers compile.
    if constexpr (requires { model.Observe(tick, work, concurrent); })
        model.Observe(tick, work, concurrent);
    else
        model.Observe(tick, work);
}
uint64_t Add(uint64_t a, uint64_t b) {
    if (b > UINT64_MAX - a) throw std::runtime_error("item counter overflow");
    return a + b;
}
struct Stats {
    size_t eligible = 0, jumps = 0;
    double previous = kUnknown, first = kUnknown;
    std::vector<double> errors, common_errors, pulse_common_errors;
    void Score(double estimate, double actual, double elapsed, double pulse) {
        ++eligible;
        if (!Numeric(estimate)) { previous = kUnknown; return; }
        if (!Numeric(first)) first = elapsed;
        errors.push_back(std::abs(estimate - actual));
        if (Numeric(previous) && std::abs(estimate - previous) > 3) ++jumps;
        previous = estimate;
        if (Numeric(pulse)) {
            common_errors.push_back(std::abs(estimate - actual));
            pulse_common_errors.push_back(std::abs(pulse - actual));
        }
    }
};
double Mean(const std::vector<double>& v) {
    return v.empty() ? kUnknown : std::accumulate(v.begin(), v.end(), 0.0) / static_cast<double>(v.size());
}
double DisplaySeconds(double value) { return Numeric(value) ? std::ceil(value) : value; }
void Print(uint64_t id, size_t index, Stats stats) {
    std::sort(stats.errors.begin(), stats.errors.end());
    const double p95 = stats.errors.empty() ? kUnknown : stats.errors[static_cast<size_t>(std::ceil(0.95 * static_cast<double>(stats.errors.size()))) - 1];
    std::cout << "TASK=" << id << " model=" << kNames[index] << " samples=" << stats.eligible
        << " numeric=" << stats.errors.size() << " coverage="
        << (stats.eligible ? static_cast<double>(stats.errors.size()) / static_cast<double>(stats.eligible) : 0)
        << " mae=" << Mean(stats.errors) << " p95=" << p95
        << " max=" << (stats.errors.empty() ? kUnknown : stats.errors.back()) << " jumps_gt3=" << stats.jumps
        << " first_numeric_s=" << stats.first << " common_with_pulse=" << stats.common_errors.size()
        << " common_mae=" << Mean(stats.common_errors) << " pulse_common_mae=" << Mean(stats.pulse_common_errors) << '\n';
}
struct Task {
    pulse::ops::TransferEtaEstimator pulse;
    Models bytes, items;
    bool initialized = false;
    uint64_t reset = 0, last_score = 0, last_wall = 0, last_active = 0;
    size_t compared = 0, mismatch = 0;
    std::array<Stats, 9> stats;
};
struct Finish { uint64_t start, end, phase, failed; };

int Replay(const char* raw_path, const char* finish_path, const char* detail_path) {
    std::ifstream raw(raw_path), finishes(finish_path);
    if (!raw || !finishes) throw std::runtime_error("input file open failed");
    std::ofstream detail;
    if (detail_path) {
        // Never open the detail file over either input pathname.
        const auto output_path = std::filesystem::weakly_canonical(detail_path);
        if (output_path == std::filesystem::weakly_canonical(raw_path) ||
            output_path == std::filesystem::weakly_canonical(finish_path) ||
            (std::filesystem::exists(output_path) &&
                (std::filesystem::equivalent(output_path, raw_path) || std::filesystem::equivalent(output_path, finish_path))))
            throw std::runtime_error("detail output aliases an input path");
        detail.open(detail_path);
        if (!detail) throw std::runtime_error("detail file open failed");
        detail << "task,wall_tick,active_tick,elapsed_s,actual_s";
        for (const auto* name : kNames) detail << ',' << name;
        detail << '\n' << std::setprecision(12);
    }
    const size_t raw_columns = RawHeader(raw);
    Header(finishes, kFinishHeader);
    std::map<uint64_t, Finish> ends;
    std::string line;
    while (std::getline(finishes, line)) {
        if (line.empty()) continue;
        const auto v = Parse(line, 5);
        if (v[2] < v[1] || !ends.emplace(v[0], Finish{v[1], v[2], v[3], v[4]}).second)
            throw std::runtime_error("invalid or duplicate finish record");
    }
    if (ends.empty()) throw std::runtime_error("no finish records");
    std::map<uint64_t, Task> tasks;
    size_t rows = 0, scored = 0;
    while (std::getline(raw, line)) {
        if (line.empty()) continue;
        ++rows;
        const auto v = ParseRaw(line, raw_columns);
        auto& task = tasks[v[0]];
        if (task.initialized && (v[1] < task.last_wall || (!v[11] && v[2] < task.last_active)))
            throw std::runtime_error("nonmonotonic task clock");
        const auto items = Add(v[4], v[5]);
        const auto total_items = Add(v[7], v[8]);
        if (v[11] || !task.initialized) {
            task.pulse.Reset(v[2], {v[3], v[4], v[5]});
            task.bytes.Reset(v[2], v[3]); task.items.Reset(v[2], items);
            task.reset = v[2]; task.initialized = true;
            for (auto& stats : task.stats) stats.previous = kUnknown;
        }
        task.last_wall = v[1]; task.last_active = v[2];
        if (v[11] == 2) continue;
        ObserveModel(task.pulse, v[2], {{v[3], v[4], v[5]}, {v[6], v[7], v[8]}, v[9] != 0},
            raw_columns == 13 && v[12] != 0);
        const auto bytes_eta = task.bytes.Observe(v[2], v[3], v[6], v[9] != 0);
        const auto items_eta = task.items.Observe(v[2], items, total_items, v[9] != 0);
        std::array<double, 9> estimates;
        estimates[0] = task.pulse.seconds() ? static_cast<double>(task.pulse.seconds()) : kUnknown;
        std::copy(bytes_eta.begin(), bytes_eta.end(), estimates.begin() + 1);
        std::copy(items_eta.begin(), items_eta.end(), estimates.begin() + 5);
        // Compare displayed whole-second predictions, not mixed raw/rounded values.
        for (auto& estimate : estimates) estimate = DisplaySeconds(estimate);
        ++task.compared;
        if (task.pulse.seconds() != v[10]) ++task.mismatch;
        const auto it = ends.find(v[0]);
        // Actual worker finish is accessed only here, after all estimators ran.
        // Phase 7 is OpPhase::Completed. Failed/cancelled/missing tasks never score.
        if (it == ends.end() || it->second.phase != 7 || it->second.failed != 0) continue;
        const auto& finish = it->second;
        if (v[1] < finish.start || v[1] > finish.end) throw std::runtime_error("raw row outside worker lifetime");
        if (v[2] - task.reset < 1000 || finish.end - v[1] < 1000 || v[1] - task.last_score < 250) continue;
        task.last_score = v[1]; ++scored;
        const double actual = static_cast<double>(finish.end - v[1]) / 1000;
        const double elapsed = static_cast<double>(v[2] - task.reset) / 1000;
        for (size_t i = 0; i < estimates.size(); ++i) task.stats[i].Score(estimates[i], actual, elapsed, estimates[0]);
        if (detail.is_open()) {
            detail << v[0] << ',' << v[1] << ',' << v[2] << ',' << elapsed << ',' << actual;
            for (double estimate : estimates) { detail << ','; if (Numeric(estimate)) detail << estimate; }
            detail << '\n';
        }
    }
    if (!rows) throw std::runtime_error("no raw records");
    if (raw.bad() || finishes.bad()) throw std::runtime_error("input read failed");
    if (detail.is_open()) { detail.flush(); if (!detail) throw std::runtime_error("detail write failed"); }
    std::cout << std::fixed << std::setprecision(6)
        << "REFERENCE algorithm ports; active clock; all numeric ETA ceil to display seconds; rclone virtual 1s timer; "
        << "tqdm miniters=1/mininterval=0.1; rich update/30s/1000 samples/ceil. "
        << "bytes and items are separate single-unit models, not mixed-cost estimators.\n"
        << "SCORE success phase=7 failed=0; warmup>=1s; remaining>=1s; interval>=250ms; "
        << "first_numeric is first eligible numeric age since Reset; jumps use adjacent numeric scored points; "
        << "common columns are pairwise intersections with pulse; zero-total unit is unknown.\n";
    for (auto& [id, task] : tasks) {
        std::cout << "REPLAY task=" << id << " compared=" << task.compared << " recorded_mismatch=" << task.mismatch << '\n';
        for (size_t i = 0; i < kNames.size(); ++i) Print(id, i, task.stats[i]);
    }
    std::cout << "RESULT raw_rows=" << rows << " scored_points=" << scored << '\n';
    return 0;
}

int Selftest() {
    size_t failures = 0, checks = 0;
    const auto check = [&](bool pass, const char* label) {
        ++checks; if (!pass) ++failures;
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << label << '\n';
    };
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    check(near(DisplaySeconds(1.01), 2) && near(DisplaySeconds(2), 2) && near(DisplaySeconds(0), 0) &&
        !Numeric(DisplaySeconds(kUnknown)), "uniform display quantization preserves zero and unknown");
    Indicatif indicatif; indicatif.Reset(0, 0);
    for (uint64_t t = 1; t <= 15; ++t) indicatif.Observe(t * 1000, t);
    check(near(indicatif.Rate(15000), 1), "indicatif normalized constant rate");
    check(near(indicatif.Rate(30000), (0.9 * (0.09 / 0.99) + 0.09) / 0.99), "indicatif read-only stalled double smoothing");
    indicatif.Observe(31000, 0); indicatif.Observe(32000, 2);
    check(near(indicatif.Rate(32000), 2), "indicatif backwards-position reset");
    Rclone rclone; rclone.Reset(0, 0);
    rclone.Observe(1000, 10); rclone.Observe(2000, 30);
    check(near(rclone.Rate(2000), 15), "rclone startup mean periods 1 and 2");
    for (uint64_t t = 3; t <= 16; ++t) rclone.Observe(t * 1000, 30 + (t - 2) * 10);
    const double old = rclone.Rate(16000); rclone.Observe(17000, 180);
    check(near(rclone.Rate(17000), old + (10 - old) / 16), "rclone period capped at 16");
    rclone.Reset(0, 0); rclone.Observe(1500, 100);
    check(near(rclone.Rate(1500), 0), "rclone does not backfill future observation into earlier timer");
    rclone.Observe(2000, 100);
    check(near(rclone.Rate(2000), 50), "rclone counts observed bytes at next timer");
    Tqdm tqdm; tqdm.Reset(0, 0); tqdm.Observe(1000, 10); tqdm.Observe(3000, 30);
    check(near(tqdm.Rate(3000), 10), "tqdm ratio of delta EMAs");
    tqdm.Reset(0, 0); tqdm.Observe(1000, 10); tqdm.Observe(3000, 50);
    check(near(tqdm.Rate(3000), 14.1 / 0.81), "tqdm unequal intervals use delta ratio not EMA of rates");
    tqdm.Reset(0, 0); tqdm.Observe(50, 5); check(near(tqdm.Rate(50), 0), "tqdm minimum update interval");
    tqdm.Observe(100, 10); check(near(tqdm.Rate(100), 100), "tqdm pending increment retained");
    Rich rich; rich.Reset(0, 0); rich.Observe(1000, 100, 1000, true);
    check(near(rich.Rate(1000), 0), "rich first sample has no interval");
    rich.Observe(2000, 110, 1000, true);
    check(near(rich.Rate(2000), 10), "rich excludes first sample increment");
    rich.Observe(32000, 120, 1000, true);
    check(near(rich.Rate(32000), 1.0 / 3), "rich preserves exact 30-second boundary");
    rich.Observe(33001, 120, 1000, true);
    check(near(rich.Rate(33001), 0), "rich prunes expired samples on zero-progress update");
    rich.Observe(34000, 130, 2000, true);
    check(near(rich.Rate(34000), 0), "rich changed total clears speed window");
    rich.Reset(0, 0);
    for (uint64_t t = 1; t <= 1002; ++t) rich.Observe(t, t * (t + 1) / 2, 1000000, true);
    check(near(rich.Rate(1002), (1002.0 * 1003 / 2 - 6) / 0.999), "rich 1000-sample cap precedes speed calculation");
    Models models; models.Reset(0, 0);
    auto estimates = models.Observe(1000, 10, 100, false);
    check(std::none_of(estimates.begin(), estimates.end(), Numeric), "unknown total suppresses all ETA");
    estimates = models.Observe(2000, 20, 0, true);
    check(std::none_of(estimates.begin(), estimates.end(), Numeric), "zero-total dimension stays unsupported");
    models.Reset(2000, 20); estimates = models.Observe(2000, 20, 100, true);
    check(std::none_of(estimates.begin(), estimates.end(), Numeric), "reset and same-tick observations preserve no history");
    for (const auto* bad : {"1,-2", "1,2x", "1,", "1,18446744073709551616", "1,2,3"}) {
        bool rejected = false; try { (void)Parse(bad, 2); } catch (const std::exception&) { rejected = true; }
        check(rejected, "malformed CSV rejected");
    }
    Stats stats; stats.Score(10, 8, 1, 9); stats.Score(kUnknown, 8, 2, 9); stats.Score(20, 8, 3, kUnknown);
    check(stats.eligible == 3 && stats.errors.size() == 2 && stats.jumps == 0 && stats.common_errors.size() == 1,
        "coverage gaps and pairwise common support");
    std::cout << "SELFTEST checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}

int SchemaSelftest() {
    size_t checks = 0, failures = 0;
    const auto check = [&](bool pass, const char* label) {
        ++checks; if (!pass) ++failures;
        std::cout << (pass ? "[PASS] " : "[FAIL] ") << label << '\n';
    };
    struct Legacy {
        size_t calls = 0;
        void Observe(uint64_t, const pulse::ops::TransferWorkProgress&) { ++calls; }
    } legacy;
    struct Current {
        size_t calls = 0;
        bool concurrent = false;
        void Observe(uint64_t, const pulse::ops::TransferWorkProgress&, bool value) { ++calls; concurrent = value; }
    } current;
    const std::string row = "1,1000,1000,10,1,0,100,10,0,1,0,0";
    for (const auto* header : {kRawHeader, kConcurrentRawHeader}) {
        std::istringstream input(std::string(header) + "\r\n");
        const size_t columns = RawHeader(input);
        const auto values = ParseRaw(row + (columns == 13 ? ",1" : ""), columns);
        ObserveModel(current, values[2], {}, columns == 13 && values[12] != 0);
        check(current.concurrent == (columns == 13), columns == 13 ? "13-column concurrent=1 reaches model" : "12-column legacy row forwards false");
    }
    const auto zero = ParseRaw(row + ",0", 13);
    ObserveModel(current, zero[2], {}, zero[12] != 0);
    check(!current.concurrent && current.calls == 3, "13-column concurrent=0 reaches model");
    ObserveModel(legacy, 1000, {}, true);
    ObserveModel(legacy, 1000, {}, false);
    check(legacy.calls == 2, "dependent dispatch supports saved two-argument estimator");
    for (const auto& bad : {row + ",2", row + ",-1", row + ",true", row, row + ",0,1"}) {
        bool rejected = false;
        try { (void)ParseRaw(bad, 13); } catch (const std::exception&) { rejected = true; }
        check(rejected, "13-column malformed flag or field count rejected");
    }
    bool rejected = false;
    try { (void)ParseRaw(row + ",0", 12); } catch (const std::exception&) { rejected = true; }
    check(rejected, "legacy header rejects appended data column");
    rejected = false;
    try { std::istringstream input(std::string(kRawHeader) + ",parallel\n"); (void)RawHeader(input); }
    catch (const std::exception&) { rejected = true; }
    check(rejected, "raw header requires exact concurrent column name");
    std::cout << "SCHEMA_SELFTEST checks=" << checks << " failures=" << failures << '\n';
    return failures ? 1 : 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--selftest") return Selftest();
        if (argc == 2 && std::string(argv[1]) == "--schema-selftest") return SchemaSelftest();
        if (argc != 3 && argc != 4) {
            std::cerr << "usage: eta_reference_compare --selftest | --schema-selftest | raw-work.csv finish.csv [points.csv]\n";
            return 2;
        }
        return Replay(argv[1], argv[2], argc == 4 ? argv[3] : nullptr);
    } catch (const std::exception& error) {
        std::cerr << "ERROR " << error.what() << '\n';
        return 3;
    }
}
