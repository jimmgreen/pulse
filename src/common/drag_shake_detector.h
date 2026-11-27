#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace pulse::app {

// Deterministic drag-shake recognizer (matches staging_shelf_mockup.html):
// counts horizontal direction reversals whose stroke amplitude exceeds the
// sensitivity threshold within a rolling time window, with a post-fire cooldown.
class DragShakeDetector {
public:
    void SetSensitivity(int level) noexcept {
        level = std::clamp(level, 0, 2);
        if (level == sensitivity_) return;
        sensitivity_ = level;
        Reset();
    }
    int Sensitivity() const noexcept { return sensitivity_; }

    // Feeds one pointer X sample (pixels) at `dpi_scale` and `now_ms`.
    // Returns true on the sample that completes a shake gesture.
    bool Feed(int x_px, float dpi_scale, uint64_t now_ms) noexcept {
        if (now_ms < cooldown_until_) return false;
        const float s = (std::max)(0.5f, dpi_scale);
        // Low / Medium / High thresholds matching staging_shelf_mockup.html.
        float init_dip = 6.0f, turn_dip = 10.0f, amp_dip = 22.0f;
        uint64_t window_ms = 900;
        int need_rev = 3;
        if (sensitivity_ == 0) {
            init_dip = 8.0f; turn_dip = 12.0f; amp_dip = 40.0f; window_ms = 850; need_rev = 4;
        } else if (sensitivity_ == 2) {
            init_dip = 5.0f; turn_dip = 7.0f; amp_dip = 14.0f; window_ms = 1000; need_rev = 3;
        }
        const int init_px = (std::max)(2, static_cast<int>(std::lround(init_dip * s)));
        const int turn_px = (std::max)(3, static_cast<int>(std::lround(turn_dip * s)));
        const int amp_px = (std::max)(6, static_cast<int>(std::lround(amp_dip * s)));

        if (!has_anchor_) {
            has_anchor_ = true;
            anchor_ = extreme_ = x_px;
            dir_ = 0;
            return false;
        }
        const int dx = x_px - extreme_;
        if (dir_ == 0) {
            if (std::abs(dx) > init_px) {
                dir_ = dx > 0 ? 1 : -1;
                extreme_ = x_px;
            }
            return false;
        }
        if ((dx > 0 && dir_ > 0) || (dx < 0 && dir_ < 0)) {
            extreme_ = x_px;
            return false;
        }
        if (std::abs(dx) < turn_px) return false;

        const bool counted_stroke = std::abs(extreme_ - anchor_) >= amp_px;
        anchor_ = extreme_;
        extreme_ = x_px;
        dir_ = -dir_;

        // Keep the boundary sample if three reversals fit in exactly window_ms.
        int kept = 0;
        for (int i = 0; i < rev_count_; ++i) {
            if (now_ms - reversals_[i] <= window_ms)
                reversals_[kept++] = reversals_[i];
        }
        rev_count_ = kept;
        if (counted_stroke) reversals_[rev_count_++] = now_ms;

        if (rev_count_ >= need_rev) {
            Reset();
            cooldown_until_ = now_ms + 1200;
            return true;
        }
        return false;
    }

    void Reset() noexcept {
        has_anchor_ = false;
        dir_ = 0;
        anchor_ = 0;
        extreme_ = 0;
        rev_count_ = 0;
    }

private:
    int sensitivity_ = 1;
    bool has_anchor_ = false;
    int dir_ = 0;            // -1 left, +1 right, 0 undecided
    int anchor_ = 0;
    int extreme_ = 0;
    uint64_t reversals_[6]{};
    int rev_count_ = 0;
    uint64_t cooldown_until_ = 0;
};

} // namespace pulse::app
