#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <mutex>
#include <unordered_set>

namespace pulse::ops {

// Opt-in for a fully enumerated, homogeneous serial CopyLargeFile queue.
// The owner invalidates on pauses, scheduler waits, fallback, skips or plan changes.
class LargeCopyEtaEstimator {
public:
    LargeCopyEtaEstimator(uint64_t total_bytes, uint64_t total_files)
        : total_bytes_(total_bytes), total_files_(total_files), valid_(total_bytes > 0 && total_files > 0) {}

    void Begin(uint64_t id, uint64_t tick, uint64_t total_bytes) {
        std::lock_guard lock(mutex_);
        if (!AcceptTime(tick)) return;
        if (active_ || completed_files_ >= total_files_ || total_bytes == 0 ||
            total_bytes > total_bytes_ - completed_bytes_ || !seen_.insert(id).second) {
            valid_ = false;
            return;
        }
        active_ = true;
        prepared_ = false;
        id_ = id;
        bytes_ = total_bytes;
        progress_ = 0;
        mismatch_direction_ = 0;
        mismatch_intervals_ = 0;
        mismatch_ms_ = 0;
        begin_tick_ = prepared_tick_ = progress_tick_ = tick;
    }

    void Prepared(uint64_t id, uint64_t tick) {
        std::lock_guard lock(mutex_);
        if (!AcceptEvent(id, tick)) return;
        if (prepared_) { valid_ = false; return; }
        prepared_ = true;
        prepared_tick_ = progress_tick_ = tick;
    }

    void Progress(uint64_t id, uint64_t tick, uint64_t completed_bytes) {
        std::lock_guard lock(mutex_);
        if (!AcceptEvent(id, tick)) return;
        if (!prepared_ || completed_bytes < progress_ || completed_bytes > bytes_) {
            valid_ = false;
            return;
        }
        if (completed_bytes > progress_) {
            const uint64_t elapsed = tick - progress_tick_;
            if (count_ >= 3 && elapsed > 0) {
                double data_ms = 0, bytes = 0;
                for (size_t i = 0; i < count_; ++i) {
                    data_ms += samples_[i].data_ms;
                    bytes += samples_[i].bytes;
                }
                const double expected = static_cast<double>(completed_bytes - progress_) * (data_ms / bytes);
                const double observed = static_cast<double>(elapsed);
                const int direction = observed > expected * 1.75 ? 1 : observed < expected * 0.55 ? -1 : 0;
                if (direction == 0 || direction != mismatch_direction_) {
                    mismatch_intervals_ = 0;
                    mismatch_ms_ = 0;
                }
                mismatch_direction_ = direction;
                if (direction != 0) {
                    mismatch_intervals_ = (std::min)(mismatch_intervals_ + 1, 3u);
                    mismatch_ms_ += observed;
                    if (mismatch_intervals_ >= 3 && mismatch_ms_ >= 2000) {
                        // Do not overwrite the aggregate model's adaptation with
                        // completed files from a demonstrably different I/O regime.
                        valid_ = false;
                        return;
                    }
                }
            }
            progress_ = completed_bytes;
            progress_tick_ = tick;
        }
    }

    void Finish(uint64_t id, uint64_t tick, bool success) {
        std::lock_guard lock(mutex_);
        if (!AcceptEvent(id, tick)) return;
        if (!success || !prepared_ || progress_ != bytes_ || progress_tick_ <= prepared_tick_) {
            valid_ = false;
            return;
        }
        samples_[next_] = {static_cast<double>(prepared_tick_ - begin_tick_),
            static_cast<double>(progress_tick_ - prepared_tick_),
            static_cast<double>(tick - progress_tick_), static_cast<double>(bytes_)};
        next_ = (next_ + 1) % samples_.size();
        count_ = (std::min)(count_ + 1, samples_.size());
        completed_bytes_ += bytes_; // Begin checked the remaining-byte bound.
        ++completed_files_;
        active_ = false;
        if ((completed_files_ == total_files_) != (completed_bytes_ == total_bytes_)) valid_ = false;
    }

    void Invalidate() {
        std::lock_guard lock(mutex_);
        valid_ = false;
    }

    uint64_t Seconds(uint64_t tick) const {
        std::lock_guard lock(mutex_);
        if (!valid_ || count_ < 3 || tick < last_tick_ || completed_files_ >= total_files_) return 0;
        // Match the normal estimator's stale-progress withdrawal; no synthetic progress.
        if (active_ && tick - progress_tick_ >= 3000) return 0;
        double setup = 0, data_ms = 0, tail = 0, bytes = 0;
        for (size_t i = 0; i < count_; ++i) {
            setup += samples_[i].setup_ms;
            data_ms += samples_[i].data_ms;
            tail += samples_[i].tail_ms;
            bytes += samples_[i].bytes;
        }
        setup /= static_cast<double>(count_);
        tail /= static_cast<double>(count_);
        if (!(setup > 0)) return 0;
        double setup_variance = 0;
        for (size_t i = 0; i < count_; ++i) {
            const double difference = samples_[i].setup_ms - setup;
            setup_variance += difference * difference;
        }
        setup_variance /= static_cast<double>(count_);
        // Variable preparation is not an identifiable repeating per-file cost.
        // Keep collecting samples so a later stable window can qualify again.
        if (std::sqrt(setup_variance) / setup > 0.5) return 0;
        if (!(bytes > 0) || !(data_ms > 0)) return 0;
        const uint64_t pending_files = total_files_ - completed_files_ - (active_ ? 1 : 0);
        const uint64_t remaining_bytes = total_bytes_ - completed_bytes_ - (active_ ? progress_ : 0);
        double estimate_ms = static_cast<double>(remaining_bytes) * (data_ms / bytes) +
            static_cast<double>(pending_files) * (setup + tail);
        if (active_) {
            if (!prepared_) estimate_ms += (std::max)(0.0, setup - static_cast<double>(tick - begin_tick_));
            estimate_ms += progress_ == bytes_
                ? (std::max)(0.0, tail - static_cast<double>(tick - progress_tick_)) : tail;
        }
        if (!std::isfinite(estimate_ms) || estimate_ms <= 0) return 0;
        const double seconds = std::ceil(estimate_ms / 1000.0);
        const double maximum = static_cast<double>((std::numeric_limits<uint64_t>::max)());
        return seconds >= maximum ? (std::numeric_limits<uint64_t>::max)() : static_cast<uint64_t>(seconds);
    }

private:
    bool AcceptTime(uint64_t tick) {
        if (!valid_) return false;
        if (tick < last_tick_) { valid_ = false; return false; }
        last_tick_ = tick;
        return true;
    }
    bool AcceptEvent(uint64_t id, uint64_t tick) {
        if (!AcceptTime(tick)) return false;
        if (!active_ || id != id_) { valid_ = false; return false; }
        return true;
    }
    struct Sample { double setup_ms = 0, data_ms = 0, tail_ms = 0, bytes = 0; };
    mutable std::mutex mutex_;
    uint64_t total_bytes_ = 0, total_files_ = 0, completed_bytes_ = 0, completed_files_ = 0;
    uint64_t id_ = 0, bytes_ = 0, progress_ = 0;
    uint64_t begin_tick_ = 0, prepared_tick_ = 0, progress_tick_ = 0, last_tick_ = 0;
    int mismatch_direction_ = 0;
    unsigned mismatch_intervals_ = 0;
    double mismatch_ms_ = 0;
    bool valid_ = false, active_ = false, prepared_ = false;
    std::unordered_set<uint64_t> seen_;
    std::array<Sample, 8> samples_{};
    size_t count_ = 0, next_ = 0;
};

} // namespace pulse::ops
