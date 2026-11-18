// ui_view_morph.h — Shared-element morph when a pane switches view mode.
//
// Switching Medium icons -> List (or any two modes) used to snap. Now every
// item that was on screen before the switch travels from its old cell/icon
// rect to its new one (FLIP), the thumbnail shrinks into the row icon, and
// items that were not on screen fade in with a small rise. Timing follows a
// critically damped spring: velocity starts at zero, 50% at ~95 ms, settles
// by kMorphMs with no overshoot; items start a few ms apart (stagger).
//
// Cost model: only items drawn this frame are recorded (a few hundred hash
// entries at most), nothing is allocated per frame once the maps have grown,
// and the whole morph is time-bounded (Active) so an interrupted morph never
// keeps the frame pump alive. Hit-testing always uses the target layout.
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
    static constexpr uint32_t kMorphMs = 400;      // one item's travel
    static constexpr uint32_t kStaggerMs = 6;      // per visible item
    static constexpr uint32_t kMaxStaggerMs = 90;  // cap: big folders stay quick
    static constexpr uint32_t kTotalMs = kMorphMs + kMaxStaggerMs;
    static constexpr size_t kMaxTracked = 1024;    // visible items only
    // Most a single frame may advance the morph. A slow frame (first layout
    // of a view: cold icon / text caches) then slows the glide for a moment
    // instead of skipping a third of it.
    static constexpr uint64_t kMaxStepMs = 33;

    struct Rects {
        D2D1_RECT_F cell;
        D2D1_RECT_F icon;
    };
    // The geometry a pane was laid out with, so the previous view can be
    // rebuilt exactly (its labels fade out where they were).
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
        float opacity = 1.0f;    // entering items fade in
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
                // previous_ holds what was drawn last frame (possibly mid-morph,
                // so a second switch continues from where items are now).
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
            elapsed_ += std::min<uint64_t>(now - last_now_, kMaxStepMs);
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
    // The previous view's labels (and items with no place in the new view)
    // fade out quickly while the shapes start travelling.
    float FromOpacity() const noexcept {
        if (!running_) return 0.0f;
        const float t = static_cast<float>(elapsed_) / static_cast<float>(kMorphMs);
        return 1.0f - SmoothStep(0.0f, 0.35f, t);
    }

    bool Running() const noexcept { return running_; }
    int FromMode() const noexcept { return from_mode_; }
    uint32_t FramesSinceStart() const noexcept { return frames_; }
    int TextWarmupNext() const noexcept { return text_warmup_next_; }
    void AdvanceTextWarmup(int next) noexcept { text_warmup_next_ = next; }

    // Opacity for the text / metadata layer: the new layout's labels arrive
    // once the shapes are mostly in place, so they never smear across.
    float TextOpacity() const noexcept {
        if (!running_) return 1.0f;
        const float t = static_cast<float>(elapsed_) / static_cast<float>(kMorphMs);
        return SmoothStep(0.30f, 0.85f, t);
    }

    // order: 0-based position among the items drawn this frame (stagger).
    // rise: entering items start this far below their target (pixels).
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
            s.cell = LerpRect(it->second.cell, cell, p);
            s.icon = LerpRect(it->second.icon, icon, p);
        } else {
            s.entering = true;
            s.opacity = p;
            const float dy = rise * (1.0f - p);
            s.cell.top += dy; s.cell.bottom += dy;
            s.icon.top += dy; s.icon.bottom += dy;
            s.from_icon = s.icon;
        }
        return s;
    }

    // Remember where an item was actually drawn this frame.
    void Record(uint64_t key, const D2D1_RECT_F& cell, const D2D1_RECT_F& icon) {
        if (current_.size() >= kMaxTracked) return;
        current_[key] = Rects{cell, icon};
    }

    // Time-bounded like the other motions: a pane that stops drawing mid-morph
    // must not keep the frame pump running. Measured from the last drawn
    // frame, since slow frames stretch the morph (kMaxStepMs).
    bool Active(uint64_t now) const noexcept {
        return running_ && now - last_now_ < kTotalMs + kSettleGraceMs;
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
    uint64_t elapsed_ = 0;  // morph clock: real time, capped per frame
    uint32_t frames_ = 0;
    int text_warmup_next_ = 0;
    LayoutParams layout_{};
    LayoutParams from_layout_{};
    std::unordered_map<uint64_t, Rects> previous_;
    std::unordered_map<uint64_t, Rects> current_;
    std::unordered_map<uint64_t, Rects> from_;
};

} // namespace pulse::ui::motion
