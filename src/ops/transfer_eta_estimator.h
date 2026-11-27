#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>

namespace pulse::ops {

// Estimation-only counters, independent of selected roots, undo and terminal status.
struct TransferWork {
    uint64_t bytes = 0;
    uint64_t files = 0;        // leaf operations, including reparse points
    uint64_t directories = 0; // directory operations: create/finalize, or traversal deletion
};
struct RecursiveWorkStats;
struct TransferWorkProgress {
    TransferWork completed;
    TransferWork total;
    bool total_known = false;
    const RecursiveWorkStats* discovery = nullptr; // synchronous advisory snapshot; never retained
};

class TransferEtaEstimator {
public:
    void Reset(uint64_t tick, TransferWork completed = {}) {
        // Old ring slots are outside count_; skips/renames need not clear 2 KiB per item.
        head_ = count_ = 0;
        changed_samples_ = 0;
        changed_direction_ = 0;
        changed_seconds_ = 0;
        started_ = sample_tick_ = progress_tick_ = tick;
        fit_tick_ = estimate_tick_ = 0;
        sampled_ = completed;
        total_ = {};
        total_known_ = fitted_ = pooled_metadata_ = using_short_ = long_pooled_metadata_ = false;
        present_ = 0;
        coefficients_ = scale_ = {};
        normalized_ = {};
        relative_error_ = 0;
        byte_floor_cost_ = byte_floor_mix_ = 0;
        seconds_ = 0;
        initial_window_ = true;
        concurrent_ = false;
        initialized_ = true;
    }

    void Observe(uint64_t tick, const TransferWorkProgress& work, bool concurrent = false) {
        if (!initialized_ || tick < sample_tick_ || Regressed(work.completed, sampled_)) {
            Reset(tick, work.completed);
            return;
        }
        concurrent_ = concurrent;
        const bool totals_changed = work.total.bytes != total_.bytes || work.total.files != total_.files ||
            work.total.directories != total_.directories || work.total_known != total_known_;
        total_ = work.total;
        total_known_ = work.total_known;
        const auto delta = Difference(work.completed, sampled_);
        const bool progressed = delta[0] > 0 || delta[1] > 0 || delta[2] > 0;
        if (progressed) progress_tick_ = tick;
        // The first fit must not ignore nearly a full interval of completed work.
        // Unknown totals keep normal sampling; short callback bursts are not samples.
        const bool first_fit_tail = work.total_known && fit_tick_ == 0 && tick - started_ >= kWarmupMs &&
            count_ >= 3 && tick - sample_tick_ >= kSampleMs / 2;
        if ((tick - sample_tick_ >= kSampleMs || first_fit_tail) && progressed) {
            const double elapsed = static_cast<double>(tick - sample_tick_) / 1000.0;
            double expected = 0;
            bool covered = fitted_ && !using_short_;
            Vector expected_work = delta;
            if (pooled_metadata_) { expected_work[1] += expected_work[2]; expected_work[2] = 0; }
            for (size_t i = 0; i < 3; ++i) {
                if (expected_work[i] <= 0) continue;
                if (!(present_ & (1u << i))) { covered = false; break; }
                expected += coefficients_[i] * expected_work[i] / scale_[i];
            }
            const bool comparable = covered && expected > 0.001;
            const int direction = !comparable ? 0 : elapsed > expected * 1.75 ? 1 : elapsed < expected * 0.55 ? -1 : 0;
            if (direction != 0) {
                if (direction != changed_direction_) { changed_samples_ = 0; changed_seconds_ = 0; }
                ++changed_samples_;
                changed_ratios_[(changed_samples_ - 1) % changed_ratios_.size()] = elapsed / expected;
                changed_seconds_ += elapsed;
            } else { changed_samples_ = 0; changed_seconds_ = 0; }
            changed_direction_ = direction;
            samples_[head_] = {tick, elapsed, delta, concurrent};
            head_ = (head_ + 1) % samples_.size();
            count_ = (std::min)(count_ + 1, samples_.size());
            // Completed work proves a speedup sooner; a slowdown needs another
            // sample to distinguish persistent I/O pressure from a brief stall.
            // Confirm a slow regime using its recent consistency and duration.
            // Short noisy stalls retain the rolling fit; old outliers cannot
            // prevent a later genuinely steady slowdown from being learned.
            bool stable_change = true;
            const size_t ratio_count = (std::min)(static_cast<size_t>(changed_samples_), changed_ratios_.size());
            if (ratio_count > 0) {
                const auto limits = std::minmax_element(changed_ratios_.begin(), changed_ratios_.begin() + ratio_count);
                stable_change = *limits.second <= *limits.first * 2.0;
            }
            if ((direction < 0 || stable_change) && changed_samples_ >= (direction < 0 ? 3u : 4u) && changed_seconds_ >= (direction < 0 ? 0.75 : 2.0)) {
                std::array<Sample, 12> recent;
                const size_t keep = (std::min)({static_cast<size_t>(changed_samples_), count_, recent.size()});
                for (size_t i = 0; i < keep; ++i) {
                    recent[i] = samples_[(head_ + samples_.size() - keep + i) % samples_.size()];

                }
                std::copy_n(recent.begin(), keep, samples_.begin());
                count_ = head_ = keep;
                changed_samples_ = 0;
                changed_direction_ = 0;
                changed_seconds_ = 0;
                fit_tick_ = 0;
                fitted_ = false;
            }
            sample_tick_ = tick;
            sampled_ = work.completed;
        }
        RetireStartupInterval();
        if (!work.total_known || tick - started_ < kWarmupMs || tick - progress_tick_ >= kStallMs) {
            seconds_ = 0;
            return;
        }
        // Refresh an immature model as new samples arrive, then use the normal cadence.
        const uint64_t fit_interval = count_ < 12 ? kSampleMs : kFitMs;
        const bool refit = count_ >= 3 && (fit_tick_ == 0 || tick - fit_tick_ >= fit_interval);
        if (refit) Fit(tick);
        if (work.completed.bytes >= work.total.bytes && work.completed.files >= work.total.files &&
            work.completed.directories >= work.total.directories) {
            seconds_ = 0;
            return;
        }
        // Reuse the fitted coefficients between fits, but do not leave the displayed
        // remaining work almost a second behind a fast small-file completion stream.
        if (totals_changed || refit || tick - estimate_tick_ >= kSampleMs) {
            estimate_tick_ = tick;
            Predict(work);
        }
    }

    uint64_t seconds() const { return seconds_; } // 0 means insufficient information, not completion
    // Learn costs from observed work without claiming that an operation's total is known.
    void ObserveWork(uint64_t tick, TransferWork completed) { Observe(tick, {completed, completed, true}); }
    uint64_t ProjectedSeconds(const TransferWork& remaining) const { return Estimate(Difference(remaining, {})); }
    static constexpr uint64_t kSampleMs = 250;
    static constexpr uint64_t kFitMs = 1000;
    static constexpr uint64_t kWarmupMs = 1000;
    static constexpr uint64_t kStallMs = 3000;

private:
    using Vector = std::array<double, 3>;
    using Matrix = std::array<Vector, 3>;
    struct Sample { uint64_t tick = 0; double seconds = 0; Vector work{}; bool concurrent = false; };
    static Vector Difference(const TransferWork& end, const TransferWork& begin) {
        return {static_cast<double>(end.bytes > begin.bytes ? end.bytes - begin.bytes : 0) / 1048576.0,
            static_cast<double>(end.files > begin.files ? end.files - begin.files : 0),
            static_cast<double>(end.directories > begin.directories ? end.directories - begin.directories : 0)};
    }
    static bool Regressed(const TransferWork& a, const TransferWork& b) {
        return a.bytes < b.bytes || a.files < b.files || a.directories < b.directories;
    }
    static double Dot(const Vector& a, const Vector& b) {
        return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
    }
    static bool Solve(Matrix a, Vector b, unsigned mask, Vector& x) {
        x = {};
        for (size_t i = 0; i < 3; ++i) {
            if (!(mask & (1u << i))) {
                for (size_t j = 0; j < 3; ++j) a[i][j] = a[j][i] = 0;
                a[i][i] = 1;
                b[i] = 0;
            }
        }
        for (size_t i = 0; i < 3; ++i) {
            size_t pivot = i;
            for (size_t j = i + 1; j < 3; ++j)
                if (std::abs(a[j][i]) > std::abs(a[pivot][i])) pivot = j;
            if (std::abs(a[pivot][i]) < 1e-12) return false;
            std::swap(a[i], a[pivot]); std::swap(b[i], b[pivot]);
            const double divisor = a[i][i];
            for (size_t j = i; j < 3; ++j) a[i][j] /= divisor;
            b[i] /= divisor;
            for (size_t row = 0; row < 3; ++row) {
                if (row == i) continue;
                const double factor = a[row][i];
                for (size_t j = i; j < 3; ++j) a[row][j] -= factor * a[i][j];
                b[row] -= factor * b[i];
            }
        }
        x = b;
        return std::isfinite(x[0]) && std::isfinite(x[1]) && std::isfinite(x[2]);
    }
    void RetireStartupInterval() {
        if (!initial_window_) return;
        if ((fitted_ && !using_short_) || count_ >= 12) { initial_window_ = false; return; }
        // A queue can pay setup again for later files. Never classify that cost
        // as one-time startup until the remaining work is known to be one file.
        if (!total_known_) return;
        if (total_.files <= sampled_.files || total_.files - sampled_.files != 1 ||
            total_.directories > sampled_.directories) {
            initial_window_ = false;
            return;
        }
        if (count_ < 4) return;
        const size_t first_index = (head_ + samples_.size() - count_) % samples_.size();
        const auto& first = samples_[first_index];
        if (first.seconds < static_cast<double>(kWarmupMs) / 1000 ||
            first.work[0] <= 0 || first.work[1] > 0 || first.work[2] > 0) return;
        double minimum = std::numeric_limits<double>::infinity(), maximum = 0, elapsed = 0;
        for (size_t i = 1; i < count_; ++i) {
            const auto& sample = samples_[(first_index + i) % samples_.size()];
            if (sample.work[0] <= 0 || sample.work[1] > 0 || sample.work[2] > 0) return;
            const double cost = sample.seconds / sample.work[0];
            minimum = (std::min)(minimum, cost);
            maximum = (std::max)(maximum, cost);
            elapsed += sample.seconds;
        }
        // The first interval can include one-time setup before any byte report.
        // Retire it only after >=3 independent, consistent, faster byte intervals.
        // No counter, active clock, mixed-work observation or later stall is erased.
        if (elapsed < 0.75 || maximum > minimum * 1.25 ||
            first.seconds / first.work[0] <= maximum * 1.75) return;
        --count_;
        fit_tick_ = 0;
        initial_window_ = false;
    }
    void Fit(uint64_t tick, bool allow_recovery = true) {
        fit_tick_ = tick;
        if (using_short_) pooled_metadata_ = long_pooled_metadata_;
        using_short_ = false;
        Matrix gram{};
        Vector rhs{};
        double target = 0, weighted_seconds = 0, byte_seconds = 0;
        Vector byte_work{};
        bool independent_bytes = false, directory_work = false, all_parallel = true;
        Vector weighted_work{};
        size_t usable = 0, directory_samples = 0;
        const auto accumulate = [&](const Sample& sample) {
            all_parallel &= sample.concurrent;
            const uint64_t age = tick - sample.tick;
            const double weight = std::exp(-static_cast<double>(age) / 3000.0) / sample.seconds;
            target += weight * sample.seconds * sample.seconds;
            // Integrate the time-decay kernel over the observed interval. This
            // gives equal elapsed time equal weight despite irregular callbacks.
            if (count_ < 12) {
                const double time_weight = std::exp(-static_cast<double>(age) / 3000.0) *
                    -std::expm1(-sample.seconds / 3.0) / sample.seconds;
                weighted_seconds += time_weight * sample.seconds;
                for (size_t i = 0; i < 3; ++i) weighted_work[i] += time_weight * sample.work[i];
                if (sample.work[0] > 0) {
                    byte_seconds += time_weight * sample.seconds;
                    for (size_t i = 0; i < 3; ++i) byte_work[i] += time_weight * sample.work[i];
                    independent_bytes |= sample.work[1] == 0 && sample.work[2] == 0;
                }
                directory_work |= sample.work[2] > 0;
            }
            for (size_t row = 0; row < 3; ++row) {
                rhs[row] += weight * sample.work[row] * sample.seconds;
                for (size_t col = 0; col < 3; ++col)
                    gram[row][col] += weight * sample.work[row] * sample.work[col];
            }
            ++usable;
            if (sample.work[2] > 0) ++directory_samples;
        };
        // Callback bursts are not independent device-speed observations. Once
        // warm, fit one-second work/time sums so alternating batches do not
        // bias the least-squares fit towards the biggest, fastest callback.
        Sample batch;

        for (size_t i = 0; i < count_; ++i) {
            const auto& sample = samples_[(head_ + samples_.size() - count_ + i) % samples_.size()];
            if (tick - sample.tick > 12000) continue;
            // Do not collapse independently observed costs into a fixed mix.
            // Aggregate callback jitter only within the same work composition.
            bool same_mix = true;
            double minimum_ratio = std::numeric_limits<double>::infinity(), maximum_ratio = 0;
            for (size_t j = 0; j < 3; ++j) {
                if ((batch.work[j] > 0) != (sample.work[j] > 0)) same_mix = false;
                if (batch.work[j] > 0 && sample.work[j] > 0) {
                    const double ratio = sample.work[j] / batch.work[j];
                    minimum_ratio = (std::min)(minimum_ratio, ratio);
                    maximum_ratio = (std::max)(maximum_ratio, ratio);
                }
            }
            if (batch.seconds > 0 && (!same_mix || maximum_ratio > minimum_ratio * (1.0 + 1e-6))) {
                accumulate(batch);
                batch = {};
            }
            batch.concurrent = batch.seconds == 0 ? sample.concurrent : batch.concurrent && sample.concurrent;
            batch.tick = sample.tick;
            batch.seconds += sample.seconds;
            for (size_t j = 0; j < 3; ++j) batch.work[j] += sample.work[j];
            if (count_ < 12 || batch.seconds >= 1.0) {
                accumulate(batch);
                batch = {};
            }
        }
        if (batch.seconds > 0) accumulate(batch);
        fitted_ = false;
        byte_floor_cost_ = byte_floor_mix_ = 0;
        if (usable < 3 || target <= 0) return;
        // If files and directories advance together, their separate costs are
        // not identifiable. Pool metadata until the observations separate them.
        const double joint = std::sqrt(gram[1][1] * gram[2][2]);
        // A directory created before a long file stream may leave this window
        // while its finalization is still pending. Keep the learned pooled
        // metadata model until fresh directory samples can separate the costs.
        pooled_metadata_ = (joint > 0 && (directory_samples < 3 || gram[1][2] / joint > 0.95)) ||
            (pooled_metadata_ && directory_samples == 0 && gram[1][1] > 0);
        if (pooled_metadata_) {
            gram[0][1] += gram[0][2]; gram[1][0] = gram[0][1];
            gram[1][1] += 2 * gram[1][2] + gram[2][2];
            rhs[1] += rhs[2]; rhs[2] = 0;
            weighted_work[1] += weighted_work[2]; weighted_work[2] = 0;
            for (size_t i = 0; i < 3; ++i) gram[2][i] = gram[i][2] = 0;
        }
        unsigned present = 0;
        for (size_t i = 0; i < 3; ++i) {
            scale_[i] = std::sqrt(gram[i][i]);
            if (scale_[i] > 1e-12) present |= 1u << i;
        }
        normalized_ = {};
        Vector scaled_rhs{};
        for (size_t row = 0; row < 3; ++row) {
            if (!(present & (1u << row))) continue;
            scaled_rhs[row] = rhs[row] / scale_[row];
            for (size_t col = 0; col < 3; ++col)
                if (present & (1u << col)) normalized_[row][col] = gram[row][col] / scale_[row] / scale_[col];
        }
        Matrix regularized = normalized_;
        for (size_t i = 0; i < 3; ++i) regularized[i][i] += kRidge;
        double best = std::numeric_limits<double>::infinity();
        // Three features: enumerate seven active sets for nonnegative ridge regression.
        // This avoids negative per-file costs when bytes and item counts are correlated.
        for (unsigned mask = 1; mask < 8; ++mask) {
            if ((mask & present) != mask) continue;
            Vector candidate{};
            if (!Solve(regularized, scaled_rhs, mask, candidate) ||
                candidate[0] < 0 || candidate[1] < 0 || candidate[2] < 0) continue;
            double residual = target - 2 * Dot(candidate, scaled_rhs);
            for (size_t row = 0; row < 3; ++row)
                for (size_t col = 0; col < 3; ++col)
                    residual += candidate[row] * normalized_[row][col] * candidate[col];
            const double objective = residual + kRidge * Dot(candidate, candidate);
            if (objective < best) {
                best = objective;
                coefficients_ = candidate;
                relative_error_ = std::sqrt((std::max)(0.0, residual) / target);
            }
        }
        // A single observed work composition identifies throughput, not separate
        // item costs. Calibrate it from decayed work/time sums rather than the
        // least-squares ratio, which overweights short fast callback batches.
        // Only bootstrap the immature model; the mature rolling fit deliberately
        // resists brief stalls. Preserve its ridge shrinkage and both guards.
        bool uniform = true;
        for (size_t i = 0; i < 3; ++i)
            for (size_t j = i + 1; j < 3; ++j)
                if ((present & (1u << i)) && (present & (1u << j)) && normalized_[i][j] < 1.0 - 1e-9)
                    uniform = false;
        if (uniform && count_ < 12 && std::isfinite(best)) {
            Vector scaled_work{};
            for (size_t i = 0; i < 3; ++i)
                if (present & (1u << i)) scaled_work[i] = weighted_work[i] / scale_[i];
            const double predicted_seconds = Dot(coefficients_, scaled_work);
            if (predicted_seconds > 0) {
                double dimensions = 0;
                for (size_t i = 0; i < 3; ++i) if (present & (1u << i)) ++dimensions;
                const double correction = weighted_seconds / predicted_seconds * dimensions / (dimensions + kRidge);
                for (double& coefficient : coefficients_) coefficient *= correction;
                double residual = target - 2 * Dot(coefficients_, scaled_rhs);
                for (size_t row = 0; row < 3; ++row)
                    for (size_t col = 0; col < 3; ++col)
                        residual += coefficients_[row] * normalized_[row][col] * coefficients_[col];
                relative_error_ = std::sqrt((std::max)(0.0, residual) / target);
            }
        }
        // Concurrent byte and item progress can make an immature additive fit
        // invent a byte rate faster than observed aggregate throughput. Until a
        // byte-only interval identifies that cost, bound large mix extrapolations.
        if (count_ < 12 && all_parallel && relative_error_ > 0.10 && !independent_bytes && !directory_work && byte_work[0] > 0 && byte_work[1] > 0) {
            byte_floor_cost_ = byte_seconds / byte_work[0];
            byte_floor_mix_ = byte_work[0] / byte_work[1];
        }
        present_ = present;
        fitted_ = std::isfinite(best) && relative_error_ <= 0.65;
        if (!fitted_ && allow_recovery && std::isfinite(best) && relative_error_ > 0.65) TryRecentRecovery(tick);
        if (fitted_ && !using_short_) initial_window_ = false;
    }
    void TryRecentRecovery(uint64_t tick) {
        // Preserve the long history; a rejected mixed-regime fit may recover only
        // when three recent observations predict the fourth at the existing bounds.
        constexpr size_t width = 4;
        if (count_ < width) return;
        TransferEtaEstimator recent = *this;
        double duration = 0;
        for (size_t i = 0; i < width; ++i) {
            recent.samples_[i] = samples_[(head_ + samples_.size() - width + i) % samples_.size()];
            duration += recent.samples_[i].seconds;
        }
        if (duration < 0.75) return;
        recent.count_ = recent.head_ = width - 1;
        recent.Fit(recent.samples_[width - 2].tick, false);
        if (!recent.fitted_) return;
        Vector held_out = recent.samples_[width - 1].work;
        if (recent.pooled_metadata_) { held_out[1] += held_out[2]; held_out[2] = 0; }
        Vector scaled{};
        double expected = 0;
        for (size_t i = 0; i < 3; ++i) {
            if (held_out[i] <= 0) continue;
            if (!(recent.present_ & (1u << i))) return;
            scaled[i] = held_out[i] / recent.scale_[i];
            expected += recent.coefficients_[i] * scaled[i];
        }
        Matrix regularized = recent.normalized_;
        for (size_t i = 0; i < 3; ++i) regularized[i][i] += kRidge;
        Vector projected{};
        if (!Solve(regularized, scaled, recent.present_, projected)) return;
        const double magnitude = Dot(scaled, scaled);
        if (magnitude > 0 && kRidge * kRidge * Dot(projected, projected) > magnitude * 0.09) return;
        const double observed = recent.samples_[width - 1].seconds;
        if (!(expected > 0.001) || observed < expected * 0.55 || observed > expected * 1.75) return;
        recent.count_ = recent.head_ = width;
        recent.Fit(tick, false);
        if (!recent.fitted_) return;
        fitted_ = true;
        using_short_ = true;
        long_pooled_metadata_ = pooled_metadata_;
        pooled_metadata_ = recent.pooled_metadata_;
        coefficients_ = recent.coefficients_;
        scale_ = recent.scale_;
        normalized_ = recent.normalized_;
        present_ = recent.present_;
        relative_error_ = recent.relative_error_;
        byte_floor_cost_ = recent.byte_floor_cost_;
        byte_floor_mix_ = recent.byte_floor_mix_;
    }
    void Predict(const TransferWorkProgress& work) { seconds_ = Estimate(Difference(work.total, work.completed)); }
    uint64_t Estimate(Vector remaining) const {
        if (!fitted_) return 0;
        if (pooled_metadata_) { remaining[1] += remaining[2]; remaining[2] = 0; }
        Vector scaled{};
        for (size_t i = 0; i < 3; ++i) {
            if (remaining[i] <= 0) continue;
            if (!(present_ & (1u << i))) {
                // Midway through a single large file, byte observations suffice until its close.
                if (i == 1 && remaining[i] <= 1 && remaining[0] > 0) continue;
                return 0;
            }
            scaled[i] = remaining[i] / scale_[i];
        }
        // A uniform run cannot identify separate byte and per-item costs. Do not
        // extrapolate that fit to an unseen workload mix merely because its residual is small.
        Matrix regularized = normalized_;
        for (size_t i = 0; i < 3; ++i) regularized[i][i] += kRidge;
        Vector projected{};
        if (!Solve(regularized, scaled, present_, projected)) return 0;
        const double magnitude = Dot(scaled, scaled);
        if (magnitude > 0 && kRidge * kRidge * Dot(projected, projected) > magnitude * 0.09) return 0;
        double estimate = Dot(coefficients_, scaled);
        if (concurrent_ && remaining[1] > 1 && remaining[2] == 0 && byte_floor_cost_ > 0 &&
            remaining[0] / remaining[1] > byte_floor_mix_ * 2.0)
            estimate = (std::max)(estimate, remaining[0] * byte_floor_cost_);
        if (!std::isfinite(estimate) || estimate <= 0) return 0;
        const double maximum = static_cast<double>((std::numeric_limits<uint64_t>::max)());
        return estimate >= maximum ? (std::numeric_limits<uint64_t>::max)()
            : (std::max)(uint64_t{1}, static_cast<uint64_t>(std::ceil(estimate)));
    }

    static constexpr double kRidge = 0.001;
    std::array<Sample, 48> samples_{};
    size_t head_ = 0, count_ = 0;
    std::array<double, 4> changed_ratios_{};
    unsigned changed_samples_ = 0;
    int changed_direction_ = 0;
    double changed_seconds_ = 0;
    uint64_t started_ = 0, sample_tick_ = 0, progress_tick_ = 0, fit_tick_ = 0, estimate_tick_ = 0;
    TransferWork sampled_{}, total_{};
    bool initial_window_ = false, concurrent_ = false;
    bool initialized_ = false, total_known_ = false, fitted_ = false, pooled_metadata_ = false, using_short_ = false;
    bool long_pooled_metadata_ = false;
    unsigned present_ = 0;
    Vector coefficients_{}, scale_{};
    Matrix normalized_{};
    double relative_error_ = 0;
    double byte_floor_cost_ = 0, byte_floor_mix_ = 0;
    uint64_t seconds_ = 0;
};

} // namespace pulse::ops



