#pragma once
#include "operation_eta.h"
#include "transfer_eta_estimator.h"
#include <atomic>
#include <cwctype>
#include <mutex>
#include <optional>
#include <string>

namespace pulse::ops {

// Written by the existing enumerator, not by an additional scan. The owner
// supplies synchronization; snapshots contain no paths or per-file allocations.
struct RecursiveWorkStats {
    static constexpr size_t kDepths = 16;
    struct Level {
        uint64_t known = 0, listed = 0, samples = 0;
        uint64_t seen_files = 0, seen_children = 0, closed_files = 0, closed_children = 0;
        double mean_files = 0, m2_files = 0, mean_children = 0, m2_children = 0;
    };
    std::array<Level, kDepths> levels{};
    uint64_t revision = 0;
    bool reliable = true;
    static bool Add(uint64_t& value, uint64_t amount) {
        if (amount > UINT64_MAX - value) return false;
        value += amount;
        return true;
    }
    void Root() { ++levels[0].known; ++revision; }
    void Child(size_t parent_depth) {
        if (parent_depth + 1 >= kDepths) { reliable = false; ++revision; return; }
        reliable = Add(levels[parent_depth].seen_children, 1) && reliable;
        reliable = Add(levels[parent_depth + 1].known, 1) && reliable;
        ++revision;
    }
    void Files(size_t depth, uint64_t count) {
        if (!count) return;
        if (depth >= kDepths) { reliable = false; ++revision; return; }
        reliable = Add(levels[depth].seen_files, count) && reliable;
        ++revision;
    }
    void Listed(size_t depth, uint64_t files, uint64_t children, bool complete) {
        if (depth >= kDepths) { reliable = false; ++revision; return; }
        auto& level = levels[depth];
        ++level.listed;
        if (!complete) { reliable = false; ++revision; return; }
        reliable = Add(level.closed_files, files) && reliable;
        reliable = Add(level.closed_children, children) && reliable;
        level.seen_files = (std::max)(level.seen_files, level.closed_files);
        level.seen_children = (std::max)(level.seen_children, level.closed_children);
        ++level.samples;
        const double df = static_cast<double>(files) - level.mean_files;
        level.mean_files += df / static_cast<double>(level.samples);
        level.m2_files += df * (static_cast<double>(files) - level.mean_files);
        const double dc = static_cast<double>(children) - level.mean_children;
        level.mean_children += dc / static_cast<double>(level.samples);
        level.m2_children += dc * (static_cast<double>(children) - level.mean_children);
        ++revision;
    }
    uint64_t KnownDirectories() const {
        uint64_t count = 0;
        for (const auto& level : levels) if (!Add(count, level.known)) return UINT64_MAX;
        return count;
    }
    uint64_t SeenFiles() const {
        uint64_t count = 0;
        for (const auto& level : levels) if (!Add(count, level.seen_files)) return UINT64_MAX;
        return count;
    }
    bool Surprised() const {
        for (const auto& level : levels) {
            const uint64_t pending = level.known > level.listed ? level.known - level.listed : 0;
            if (!pending || level.samples < 3) continue;
            const double open = static_cast<double>(level.seen_files > level.closed_files ? level.seen_files - level.closed_files : 0);
            const double variance = level.samples > 1 ? level.m2_files / static_cast<double>(level.samples - 1) : 0;
            const double limit = static_cast<double>(pending) *
                (level.mean_files + (std::max)(32.0, 3 * std::sqrt(variance) + level.mean_files * 0.75));
            if (open > limit) return true;
        }
        return false;
    }
};

struct RecentCopyWork {
    TransferWork total;
    uint64_t copied_tick = 0;
};

// Counts are unverified priors, never filesystem identity or deletion authority.
// Nothing is persisted. A mutation epoch prevents a late copy from recording a
// hint across another operation; outside-process edits are handled as uncertainty.
class RecentCopyWorkCache {
public:
    static constexpr size_t kCapacity = 64;
    static constexpr uint64_t kLifetimeMs = 120000;
    uint64_t BeginMutation(bool preserve_hints = false) {
        std::lock_guard lock(mutex_);
        ++epoch_;
        if (!preserve_hints) { for (auto& entry : entries_) entry = {}; any_.store(false); }
        return epoch_;
    }
    uint64_t Epoch() const { std::lock_guard lock(mutex_); return epoch_; }
    bool Remember(const std::wstring& root, TransferWork total, uint64_t tick, uint64_t epoch) {
        if (root.empty() || total.directories == 0) return false;
        const std::wstring key = Key(root);
        if (key.empty()) return false;
        std::lock_guard lock(mutex_);
        if (epoch != epoch_) return false;
        size_t slot = entries_.size(), oldest = 0;
        for (size_t i = 0; i < entries_.size(); ++i) {
            if (entries_[i].key == key) { slot = i; break; }
            if (entries_[i].key.empty() && slot == entries_.size()) slot = i;
            if (entries_[i].work.copied_tick < entries_[oldest].work.copied_tick) oldest = i;
        }
        if (slot == entries_.size()) slot = oldest;
        entries_[slot] = {key, {total, tick}};
        any_.store(true);
        return true;
    }
    std::optional<RecentCopyWork> Take(const std::wstring& root, uint64_t tick) {
        if (!any_.load(std::memory_order_relaxed)) return {};
        const std::wstring key = Key(root);
        std::lock_guard lock(mutex_);
        std::optional<RecentCopyWork> result;
        bool remains = false;
        for (auto& entry : entries_) {
            if (entry.key.empty()) continue;
            const bool expired = tick < entry.work.copied_tick || tick - entry.work.copied_tick > kLifetimeMs;
            if (entry.key == key && !expired) result = entry.work;
            if (expired || Touches(entry.key, key)) entry = {};
            remains = remains || !entry.key.empty();
        }
        any_.store(remains);
        return result;
    }
    static std::wstring Key(std::wstring path) {
        for (auto& c : path) { if (c == L'/') c = L'\\'; c = static_cast<wchar_t>(std::towlower(c)); }
        if (path.starts_with(L"\\\\?\\unc\\")) path = L"\\\\" + path.substr(8);
        else if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
        while (path.size() > 3 && path.back() == L'\\') path.pop_back();
        return path;
    }
private:
    static bool Under(const std::wstring& path, const std::wstring& root) {
        return path.starts_with(root) && (path.size() == root.size() ||
            (!root.empty() && root.back() == L'\\') || path[root.size()] == L'\\');
    }
    static bool Touches(const std::wstring& a, const std::wstring& b) { return Under(a, b) || Under(b, a); }
    struct Entry { std::wstring key; RecentCopyWork work; };
    mutable std::mutex mutex_;
    std::array<Entry, kCapacity> entries_{};
    std::atomic<bool> any_{false};
    uint64_t epoch_ = 0;
};
inline RecentCopyWorkCache& RecentCopyHints() {
    // Worker shutdown may run from another global destructor. Keep this bounded
    // process-lifetime cache alive until the OS reclaims the process, not before workers stop.
    static auto* const hints = new RecentCopyWorkCache;
    return *hints;
}

class RecursiveEtaEstimator {
public:
    void Reset(uint64_t tick, std::optional<RecentCopyWork> hint = {}) {
        cost_.Reset(tick);
        started_ = progress_tick_ = last_tick_ = rate_tick_ = tick;
        last_work_ = {};
        rate_work_ = 0;
        hint_ = hint;
        rate_ = 0;
    }
    OperationEta Observe(uint64_t tick, const TransferWorkProgress& work, const RecursiveWorkStats* discovery,
                         uint64_t task, uint64_t completed_before_root = 0) {
        if (tick < last_tick_ || work.completed.files < last_work_.files || work.completed.directories < last_work_.directories)
            Reset(tick);
        OperationEta result;
        result.task_id = task;
        result.sample_tick = tick;
        const uint64_t done = Sum(work.completed);
        result.processed_items = done > UINT64_MAX - completed_before_root ? UINT64_MAX : done + completed_before_root;
        result.has_processed_items = true;
        result.stage = tick - started_ < 3000 ? EtaStage::Learning : EtaStage::Unknown;
        const uint64_t old = Sum(last_work_);
        if (done > old) progress_tick_ = tick;
        if (tick - rate_tick_ >= 250) {
            const double instant = static_cast<double>(done >= rate_work_ ? done - rate_work_ : 0) * 1000.0 /
                static_cast<double>(tick - rate_tick_);
            const double alpha = 1 - std::exp(-static_cast<double>(tick - rate_tick_) / 1500.0);
            rate_ = rate_ <= 0 ? instant : rate_ + alpha * (instant - rate_);
            rate_tick_ = tick;
            rate_work_ = done;
        }
        cost_.ObserveWork(tick, work.completed);
        last_tick_ = tick;
        last_work_ = work.completed;
        if (work.total_known && work.completed.files >= work.total.files &&
            work.completed.directories >= work.total.directories && work.completed.bytes >= work.total.bytes) {
            result.stage = EtaStage::Finishing;
            return result;
        }
        if (tick - started_ < 1000 || tick - progress_tick_ >= 3000 || done == 0) return result;
        // An unsupported sampling depth is not evidence that an independent copy prior is stale.
        if (discovery && discovery->Surprised()) hint_.reset();
        const uint64_t seen_files = discovery ? (std::max)(discovery->SeenFiles(), work.completed.files) : work.completed.files;
        const uint64_t seen_dirs = discovery ? (std::max)(discovery->KnownDirectories(), work.completed.directories) : work.completed.directories;
        TransferWork lower{}, upper{};
        bool predicted = false;
        if (work.total_known) {
            lower = upper = {work.total.bytes > work.completed.bytes ? work.total.bytes - work.completed.bytes : 0,
                work.total.files > work.completed.files ? work.total.files - work.completed.files : 0,
                work.total.directories > work.completed.directories ? work.total.directories - work.completed.directories : 0};
            result.source = EtaSource::KnownWork;
            predicted = true;
        }
        // Actual traversal evidence supersedes a copy receipt, which may already
        // describe a different population even while its broad bounds still fit.
        if (!predicted && discovery && Forecast(*discovery, work.completed, lower, upper)) {
            result.source = EtaSource::DirectorySamples;
            predicted = true;
        }
        if (!predicted && hint_) {
            const auto& h = hint_->total;
            if (static_cast<double>(seen_files) > static_cast<double>(h.files) * 1.5 ||
                static_cast<double>(seen_dirs) > static_cast<double>(h.directories) * 1.5 || done >= Sum(h)) {
                hint_.reset();
            } else {
                lower = {0, Remaining(static_cast<double>(h.files) * 0.7, work.completed.files),
                    Remaining(static_cast<double>(h.directories) * 0.7, work.completed.directories)};
                upper = {0, Remaining((std::max)(static_cast<double>(h.files) * 1.4, static_cast<double>(seen_files)), work.completed.files),
                    Remaining((std::max)(static_cast<double>(h.directories) * 1.4, static_cast<double>(seen_dirs)), work.completed.directories)};
                result.source = EtaSource::RecentCopy;
                predicted = true;
            }
        }
        if (!predicted) return result;
        const uint64_t pending_dirs = seen_dirs > work.completed.directories ? seen_dirs - work.completed.directories : 0;
        upper.directories = (std::max)(upper.directories, pending_dirs);
        auto seconds = [&](const TransferWork& remaining) {
            const uint64_t amount = Sum(remaining);
            if (amount == UINT64_MAX) return uint64_t{0};
            uint64_t value = cost_.ProjectedSeconds(remaining);
            if (!value && rate_ > 0 && amount > 0) {
                const double estimated = std::ceil(static_cast<double>(amount) / rate_);
                if (!std::isfinite(estimated) || estimated > 3153600000.0) return uint64_t{0};
                value = static_cast<uint64_t>(estimated);
            }
            return value;
        };
        const uint64_t lo = seconds(lower), hi = seconds(upper);
        if (!hi || hi > 3153600000ull || lo > 3153600000ull) return result;
        result.lower_seconds = (std::max)(uint64_t{1}, static_cast<uint64_t>(std::floor(static_cast<double>(lo) * 0.85)));
        result.upper_seconds = (std::max)(result.lower_seconds + 1, static_cast<uint64_t>(std::ceil(static_cast<double>(hi) * 1.2)));
        // Exhausting an unverified prior is not evidence of imminent completion.
        if (result.upper_seconds <= 2 || result.upper_seconds > (std::max)(uint64_t{30}, result.lower_seconds * 8)) {
            result.lower_seconds = result.upper_seconds = 0;
            result.source = EtaSource::None;
            return result;
        }
        result.stage = EtaStage::Range;
        return result;
    }
    static bool ForecastWork(const RecursiveWorkStats& stats, const TransferWork& completed,
                             TransferWork& lower, TransferWork& upper) {
        return Forecast(stats, completed, lower, upper);
    }
private:
    static uint64_t Sum(const TransferWork& work) { return work.files > UINT64_MAX - work.directories ? UINT64_MAX : work.files + work.directories; }
    static uint64_t Remaining(double total, uint64_t done) {
        if (!std::isfinite(total) || total > 1e15) return UINT64_MAX;
        return total > static_cast<double>(done) ? static_cast<uint64_t>(std::ceil(total - static_cast<double>(done))) : 0;
    }
    struct WorkRange { double files = 0, dirs = 0, low_files = 0, low_dirs = 0, high_files = 0, high_dirs = 0; bool valid = false; };
    static double Margin(double mean, double m2, uint64_t samples) {
        const double variance = samples > 1 ? m2 / static_cast<double>(samples - 1) : 0;
        return (std::max)(mean * 0.2, 2 * std::sqrt((std::max)(0.0, variance) * (1 + 1.0 / static_cast<double>(samples))));
    }
    static bool Forecast(const RecursiveWorkStats& stats, const TransferWork& done, TransferWork& lower, TransferWork& upper) {
        if (!stats.reliable || stats.Surprised()) return false;
        std::array<WorkRange, RecursiveWorkStats::kDepths> subtree{};
        for (size_t index = subtree.size(); index-- > 0;) {
            const auto& level = stats.levels[index];
            if (level.samples < 3) continue;
            if (level.mean_children > 0 && (index + 1 == subtree.size() || !subtree[index + 1].valid)) continue;
            const double fm = Margin(level.mean_files, level.m2_files, level.samples);
            const double cm = Margin(level.mean_children, level.m2_children, level.samples);
            auto& out = subtree[index];
            const WorkRange child = level.mean_children > 0 ? subtree[index + 1] : WorkRange{};
            out.files = level.mean_files + level.mean_children * child.files;
            out.dirs = 1 + level.mean_children * child.dirs;
            out.low_files = (std::max)(0.0, level.mean_files - fm) + (std::max)(0.0, level.mean_children - cm) * child.low_files;
            out.low_dirs = 1 + (std::max)(0.0, level.mean_children - cm) * child.low_dirs;
            out.high_files = level.mean_files + fm + (level.mean_children + cm) * child.high_files;
            out.high_dirs = 1 + (level.mean_children + cm) * child.high_dirs;
            out.valid = std::isfinite(out.high_files) && out.high_files + out.high_dirs < 1e15;
        }
        double lf = 0, hf = 0, ld = 0, hd = 0;
        bool frontier = false;
        for (size_t depth = 0; depth < stats.levels.size(); ++depth) {
            const auto& level = stats.levels[depth];
            const uint64_t pending = level.known > level.listed ? level.known - level.listed : 0;
            if (!pending) continue;
            frontier = true;
            if (!subtree[depth].valid) return false;
            const double p = static_cast<double>(pending);
            const double open_f = static_cast<double>(level.seen_files > level.closed_files ? level.seen_files - level.closed_files : 0);
            const double open_c = static_cast<double>(level.seen_children > level.closed_children ? level.seen_children - level.closed_children : 0);
            const double fm = Margin(level.mean_files, level.m2_files, level.samples);
            const double cm = Margin(level.mean_children, level.m2_children, level.samples);
            lf += (std::max)(0.0, p * (std::max)(0.0, level.mean_files - fm) - open_f);
            hf += (std::max)(0.0, p * (level.mean_files + fm) - open_f);
            if (level.mean_children > 0) {
                const auto& child = subtree[depth + 1];
                const double cl = (std::max)(0.0, p * (std::max)(0.0, level.mean_children - cm) - open_c);
                const double ch = (std::max)(0.0, p * (level.mean_children + cm) - open_c);
                lf += cl * child.low_files; hf += ch * child.high_files;
                ld += cl * child.low_dirs; hd += ch * child.high_dirs;
            }
        }
        // Child discovery precedes Listed in the existing enumerators. Once
        // every known directory is listed, no unlisted descendant is hidden
        // behind that frontier. Use its actual counts, not a stale prior; this
        // remains advisory and never grants total_known or completion authority.
        if (!frontier && stats.KnownDirectories() == 0) return false;
        const uint64_t seen = stats.SeenFiles();
        const double unreported = done.files > seen ? static_cast<double>(done.files - seen) : 0;
        const double pending_files = seen > done.files ? static_cast<double>(seen - done.files) : 0;
        const uint64_t known_dirs = stats.KnownDirectories();
        const double pending_dirs = known_dirs > done.directories ? static_cast<double>(known_dirs - done.directories) : 0;
        lower = {0, Remaining(pending_files + (std::max)(0.0, lf - unreported), 0), Remaining(ld + pending_dirs, 0)};
        upper = {0, Remaining(pending_files + (std::max)(0.0, hf - unreported), 0), Remaining(hd + pending_dirs, 0)};
        return lower.files != UINT64_MAX && upper.files != UINT64_MAX && upper.directories != UINT64_MAX;
    }
    TransferEtaEstimator cost_;
    std::optional<RecentCopyWork> hint_;
    TransferWork last_work_{};
    uint64_t started_ = 0, progress_tick_ = 0, last_tick_ = 0, rate_tick_ = 0, rate_work_ = 0;
    double rate_ = 0;
};

} // namespace pulse::ops
