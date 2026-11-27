#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>

namespace pulse::bench::eta_reference {

// Independent mathematical implementations, reviewed 2026-11-25 against:
// https://github.com/console-rs/indicatif/blob/main/src/state.rs (Estimator)
// https://github.com/rclone/rclone/blob/master/fs/accounting/accounting.go (averageLoop)
// https://github.com/tqdm/tqdm/blob/master/tqdm/std.py (EMA, update, format_dict)
// https://github.com/Textualize/rich/blob/master/rich/progress.py (Task, Progress.update)
// These are algorithm references, not executions of the original packages.
// Each instance has ONE unit: bytes OR items. Neither models mixed operation cost.
inline constexpr double kUnknown = std::numeric_limits<double>::quiet_NaN();
inline bool Numeric(double value) { return std::isfinite(value) && value >= 0; }
inline double Remaining(uint64_t total, uint64_t done, bool known, double rate) {
    if (!known || !total) return kUnknown;
    if (done >= total) return 0;
    return rate > 0 && std::isfinite(rate) ? static_cast<double>(total - done) / rate : kUnknown;
}

class Indicatif {
public:
    void Reset(uint64_t tick, uint64_t done) { start_ = last_ = tick; done_ = done; single_ = twice_ = 0; }
    void Observe(uint64_t tick, uint64_t done) {
        if (tick < last_ || done < done_) { Reset(tick, done); return; }
        if (tick == last_ || done == done_) return;
        const double dt = static_cast<double>(tick - last_) / 1000;
        const double weight = Weight(dt);
        single_ = weight * single_ + (1 - weight) * static_cast<double>(done - done_) / dt;
        twice_ = weight * twice_ + (1 - weight) * single_ / (1 - Weight(static_cast<double>(tick - start_) / 1000));
        last_ = tick; done_ = done;
    }
    double Rate(uint64_t tick) const {
        if (tick <= start_ || tick < last_) return 0;
        const double weight = Weight(static_cast<double>(tick - last_) / 1000);
        const double norm = 1 - Weight(static_cast<double>(tick - start_) / 1000);
        return (twice_ * weight + single_ * weight / norm * (1 - weight)) / norm;
    }
private:
    static double Weight(double seconds) { return std::pow(0.1, seconds / 15); }
    uint64_t start_ = 0, last_ = 0, done_ = 0;
    double single_ = 0, twice_ = 0;
};

class Rclone {
public:
    void Reset(uint64_t tick, uint64_t done) { last_ = tick; seen_ = sampled_ = done; period_ = 0; rate_ = 0; }
    void Observe(uint64_t tick, uint64_t done) {
        if (tick < last_ || done < seen_) { Reset(tick, done); return; }
        // Virtual one-second timer: hold the latest previously observed counter.
        // An event exactly on a timer boundary precedes that timer. No interpolation
        // from future events; real rclone goroutine scheduling cannot be reconstructed.
        while (tick - last_ >= 1000) {
            last_ += 1000;
            const auto available = last_ == tick ? done : seen_;
            period_ = (std::min)(period_ + 1u, 16u);
            rate_ += (static_cast<double>(available - sampled_) - rate_) / period_;
            sampled_ = available;
        }
        seen_ = done;
    }
    double Rate(uint64_t) const { return rate_; }
private:
    uint64_t last_ = 0, seen_ = 0, sampled_ = 0;
    unsigned period_ = 0;
    double rate_ = 0;
};

class Tqdm {
public:
    void Reset(uint64_t tick, uint64_t done) { last_ = tick; done_ = done; delta_ = time_ = 0; }
    void Observe(uint64_t tick, uint64_t done) {
        if (tick < last_ || done < done_) { Reset(tick, done); return; }
        // Explicit miniters=1, mininterval=0.1, smoothing=0.3 configuration.
        // Do not emulate dynamic miniters, rendering or the monitor thread.
        if (done == done_ || tick - last_ < 100) return;
        delta_ = 0.7 * delta_ + 0.3 * static_cast<double>(done - done_);
        time_ = 0.7 * time_ + 0.3 * static_cast<double>(tick - last_) / 1000;
        // Equal update counts cancel the EMA bias-correction denominator.
        last_ = tick; done_ = done;
    }
    double Rate(uint64_t) const { return time_ > 0 ? delta_ / time_ : 0; }
private:
    uint64_t last_ = 0, done_ = 0;
    double delta_ = 0, time_ = 0;
};

class Rich {
public:
    void Reset(uint64_t tick, uint64_t done) { last_ = tick; done_ = done; total_ = 0; known_ = false; samples_.clear(); }
    void Observe(uint64_t tick, uint64_t done, uint64_t total, bool known) {
        if (tick < last_ || done < done_) { Reset(tick, done); return; }
        // Progress.update clears the speed window when a supplied total changes.
        if (known && (!known_ || total != total_)) samples_.clear();
        if (known) { total_ = total; known_ = true; }
        while (!samples_.empty() && tick - samples_.front().tick > 30000) samples_.pop_front();
        if (done > done_) {
            samples_.push_back({tick, done - done_});
            if (samples_.size() > 1000) samples_.pop_front();
        }
        last_ = tick; done_ = done;
    }
    double Rate(uint64_t) const {
        if (samples_.size() < 2 || samples_.back().tick == samples_.front().tick) return 0;
        double amount = 0;
        for (size_t i = 1; i < samples_.size(); ++i) amount += static_cast<double>(samples_[i].delta);
        return amount * 1000 / static_cast<double>(samples_.back().tick - samples_.front().tick);
    }
private:
    struct Sample { uint64_t tick, delta; };
    uint64_t last_ = 0, done_ = 0, total_ = 0;
    bool known_ = false;
    std::deque<Sample> samples_;
};

struct Models {
    Indicatif indicatif;
    Rclone rclone;
    Tqdm tqdm;
    Rich rich;
    void Reset(uint64_t tick, uint64_t done) {
        indicatif.Reset(tick, done); rclone.Reset(tick, done);
        tqdm.Reset(tick, done); rich.Reset(tick, done);
    }
    std::array<double, 4> Observe(uint64_t tick, uint64_t done, uint64_t total, bool known) {
        indicatif.Observe(tick, done); rclone.Observe(tick, done);
        tqdm.Observe(tick, done); rich.Observe(tick, done, total, known);
        return {Remaining(total, done, known, indicatif.Rate(tick)),
            Remaining(total, done, known, rclone.Rate(tick)),
            Remaining(total, done, known, tqdm.Rate(tick)),
            std::ceil(Remaining(total, done, known, rich.Rate(tick)))};
    }
};
} // namespace pulse::bench::eta_reference
