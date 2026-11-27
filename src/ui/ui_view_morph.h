// ui_view_morph.h — Shared-element morph when a pane switches view mode.
//
// Target cells and labels are authoritative immediately. Existing icons settle
// briefly inside their target slots; interactive targets never travel through
// other rows. Wall-clock timing bounds this feedback even on slow frames.
// Only visible items are recorded, and tracking is bounded.
#pragma once
#include "ui_motion.h"
#include <cmath>
#include <cstddef>
#include <unordered_map>

namespace pulse::ui::motion {

// Critically damped spring x(t) = 1 - (1 + w t) e^(-w t), normalised so that
// x(1) == 1 exactly (no end snap). t in [0, 1].
inline float MorphSpring(float t) noexcept {
    t = std::clamp(t, 0.0f, 1.0f);
    constexpr float w = 7.0f;
    static const float norm = 1.0f - (1.0f + w) * std::exp(-w);
    return std::min(1.0f, (1.0f - (1.0f + w * t) * std::exp(-w * t)) / norm);
}

inline float SmoothStep(float edge0, float edge1, float x) noexcept {
    const float t = std::clamp((x - edge0) / (edge1 - edge0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

inline D2D1_RECT_F LerpRect(const D2D1_RECT_F& a, const D2D1_RECT_F& b, float k) noexcept {
    return D2D1::RectF(a.left + (b.left - a.left) * k, a.top + (b.top - a.top) * k,
                       a.right + (b.right - a.right) * k, a.bottom + (b.bottom - a.bottom) * k);
}

class ViewMorphMotion {
public:
    static constexpr uint32_t kMorphMs = 150;     // brief shared-element continuity
    static constexpr uint32_t kStaggerMs = 0;     // ready content never waits its turn
    static constexpr uint32_t kMaxStaggerMs = 0;
    static constexpr uint32_t kTotalMs = kMorphMs + kMaxStaggerMs;
    static constexpr size_t kMaxTracked = 1024;    // visible items only

    struct Rects {
        D2D1_RECT_F cell;
        D2D1_RECT_F icon;
    };
    // Geometry retained with the previous view for its artwork source.
    struct LayoutParams {
        bool valid = false;
        D2D1_RECT_F viewport{};
        float scroll_x = 0.0f;
        float scroll_y = 0.0f;
        float row_height_dip = 0.0f;
        size_t count = 0;
        float icon_dip = 0.0f;  // icon grids: custom thumbnail width (ViewLayoutParams)
    };

    struct Sample {
        bool animating = false;  // false: draw exactly as the target layout
        bool entering = false;   // was not on screen before the switch
        float progress = 1.0f;   // eased 0..1 for this item
        float opacity = 1.0f;    // actionable items remain fully visible
        D2D1_RECT_F cell{};
        D2D1_RECT_F icon{};
        D2D1_RECT_F from_icon{};
    };

    // folder: identity of what the pane shows (path + filter), deliberately
    // WITHOUT the view mode. mode: current view mode. allowed: false while
    // loading. Call once per DrawList.
    void BeginFrame(uint64_t folder, int mode, uint64_t frame, uint64_t now, bool allowed) {
        const bool continuous = valid_ && folder == folder_ &&
                                (frame == frame_ || frame == frame_ + 1);
        previous_.swap(current_);
        current_.clear();
        if (!continuous) {
            running_ = false;
            from_.clear();
            previous_.clear();
        } else if (mode != mode_) {
            if (allowed && !previous_.empty() && SystemAnimationsEnabled()) {
                // Retain the last rendered artwork as the source of a rapid switch.
                // New target cells remain authoritative throughout.
                from_.swap(previous_);
                from_mode_ = mode_;
                from_layout_ = layout_;
                start_ = now;
                last_now_ = now;
                elapsed_ = 0;
                running_ = true;
                frames_ = 0;
                text_warmup_next_ = 0;
            } else {
                running_ = false;
                from_.clear();
            }
        }
        if (running_ && now != start_) {
            elapsed_ = now - start_;
            last_now_ = now;
        }
        if (running_ && elapsed_ >= kTotalMs) {
            running_ = false;
            from_.clear();
        }
        if (running_) ++frames_;
        valid_ = true;
        folder_ = folder;
        mode_ = mode;
        frame_ = frame;
        now_ = now;
    }

    // Call right after BeginFrame with this frame's layout inputs.
    void NoteLayout(const D2D1_RECT_F& viewport, float scroll_x, float scroll_y,
                    float row_height_dip, size_t count, float icon_dip = 0.0f) noexcept {
        layout_ = LayoutParams{true, viewport, scroll_x, scroll_y, row_height_dip, count, icon_dip};
    }
    const LayoutParams& FromLayout() const noexcept { return from_layout_; }
    // Where the item was when the switch started / was drawn this frame.
    bool FromRects(uint64_t key, Rects& out) const {
        const auto it = from_.find(key);
        if (it == from_.end()) return false;
        out = it->second;
        return true;
    }
    bool CurrentRects(uint64_t key, Rects& out) const {
        const auto it = current_.find(key);
        if (it == current_.end()) return false;
        out = it->second;
        return true;
    }
    // The old-label pass is suppressed: new target text is immediately readable.
    float FromOpacity() const noexcept {
        return 0.0f; // New labels stay readable; do not superimpose the old text.
    }

    bool Running() const noexcept { return running_; }
    bool Finish() noexcept {
        const bool was_running = running_;
        running_ = false;
        elapsed_ = kTotalMs;
        from_.clear();
        return was_running;
    }
    int FromMode() const noexcept { return from_mode_; }
    uint32_t FramesSinceStart() const noexcept { return frames_; }
    int TextWarmupNext() const noexcept { return text_warmup_next_; }
    void AdvanceTextWarmup(int next) noexcept { text_warmup_next_ = next; }

    // Text and metadata never wait for decorative artwork to settle.
    float TextOpacity() const noexcept {
        return 1.0f;
    }

    // Preserve the sampling API; neither row order nor a rise delays content.
    Sample Get(uint64_t key, const D2D1_RECT_F& cell, const D2D1_RECT_F& icon,
               int order, float rise) const {
        Sample s;
        s.cell = cell;
        s.icon = icon;
        s.from_icon = icon;
        if (!running_) return s;
        const uint64_t delay = std::min<uint64_t>(
            static_cast<uint64_t>(std::max(0, order)) * kStaggerMs, kMaxStaggerMs);
        const uint64_t elapsed = elapsed_;
        const float t = elapsed <= delay ? 0.0f
            : static_cast<float>(elapsed - delay) / static_cast<float>(kMorphMs);
        const float p = MorphSpring(t);
        s.progress = p;
        s.animating = t < 1.0f;
        const auto it = from_.find(key);
        if (it != from_.end()) {
            s.from_icon = it->second.icon;
            // Moving a whole row leaves display and input using different
            // layouts. Keep the cell/name at the final position and let the
            // artwork settle within that same, already actionable slot.
            const float inset = 0.03f * (1.0f - p);
            const float dx = (icon.right - icon.left) * inset;
            const float dy = (icon.bottom - icon.top) * inset;
            s.icon = D2D1::RectF(icon.left + dx, icon.top + dy,
                               icon.right - dx, icon.bottom - dy);
        } else {
            s.entering = true;
            // A newly visible item has no previous spatial anchor.
            // Show it immediately rather than hiding actionable content.
            (void)rise;
            s.from_icon = s.icon;
        }
        return s;
    }

    // Remember where an item was actually drawn this frame.
    void Record(uint64_t key, const D2D1_RECT_F& cell, const D2D1_RECT_F& icon) {
        if (current_.size() >= kMaxTracked) return;
        current_[key] = Rects{cell, icon};
    }

    // An interrupted pane expires from the switch start, not its last frame.
    // Slow rendering never extends the wall-clock animation deadline.
    bool Active(uint64_t now) const noexcept {
        return running_ && now - start_ < kTotalMs + kSettleGraceMs;
    }

private:
    bool valid_ = false;
    bool running_ = false;
    uint64_t folder_ = 0;
    int mode_ = -1;
    int from_mode_ = -1;
    uint64_t frame_ = 0;
    uint64_t now_ = 0;
    uint64_t start_ = 0;
    uint64_t last_now_ = 0;
    uint64_t elapsed_ = 0;  // elapsed wall time since the most recent switch
    uint32_t frames_ = 0;
    int text_warmup_next_ = 0;
    LayoutParams layout_{};
    LayoutParams from_layout_{};
    std::unordered_map<uint64_t, Rects> previous_;
    std::unordered_map<uint64_t, Rects> current_;
    std::unordered_map<uint64_t, Rects> from_;
};

} // namespace pulse::ui::motion
