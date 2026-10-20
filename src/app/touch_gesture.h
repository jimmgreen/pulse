#pragma once

#include <cstdint>
#include <cmath>
#include <algorithm>
#include <deque>

namespace pulse::app {

enum class TouchGestureState {
    Idle,
    Pending,              // Finger down, within deadzone, awaiting tap/hold/pan
    Panning,              // Finger moved beyond deadzone, locked scroll target
    HoldConsumed,         // Long-press threshold fired (context menu shown, tap aborted)
    Cancelled,            // Gesture aborted (capture lost, canceled flag, etc.)
    MultiTouchCancelled,  // Second finger touched down (abort tap/hold, consume until up)
    Ended,                // Finger lifted cleanly
};

enum class TouchActionKind {
    None,
    Tap,
    DoubleTap,
    Hold,
    PanStart,
    PanUpdate,
    PanEnd,
    Cancel,
};

struct TouchAction {
    TouchActionKind kind = TouchActionKind::None;
    float x = 0.0f;
    float y = 0.0f;
    float delta_x = 0.0f;
    float delta_y = 0.0f;
    float velocity_x = 0.0f; // DIP/sec
    float velocity_y = 0.0f; // DIP/sec
    int primary_axis = 0;    // 0: unconstrained, 1: horizontal, 2: vertical
};

struct TouchGestureConfig {
    float pan_deadzone_dip = 8.0f;
    uint64_t hold_timeout_ms = 600;
    float double_tap_distance_dip = 16.0f;
    uint64_t double_tap_time_ms = 400;
    float friction_k = 4.2f;        // Inertia decay factor: v = v0 * exp(-k * dt)
    float min_velocity_dip = 30.0f;  // Velocity cutoff DIP/sec
    float max_velocity_dip = 4000.0f;// Velocity clamp DIP/sec
};

class TouchGestureReducer {
public:
    explicit TouchGestureReducer(TouchGestureConfig config = {}) : config_(config) {}

    TouchGestureState State() const noexcept { return state_; }
    uint32_t ActivePointerId() const noexcept { return active_pointer_id_; }

    TouchAction OnDown(uint32_t pointer_id, float x, float y, uint64_t now_ms, float scale) noexcept {
        scale = (std::max)(0.5f, scale);

        // If another finger touches down while pending or panning:
        // Cancel tap/hold, transition to MultiTouchCancelled, consume until all fingers up.
        if (state_ == TouchGestureState::Pending || state_ == TouchGestureState::Panning) {
            state_ = TouchGestureState::MultiTouchCancelled;
            TouchAction action{};
            action.kind = TouchActionKind::Cancel;
            action.x = x;
            action.y = y;
            return action;
        }

        if (state_ == TouchGestureState::MultiTouchCancelled) {
            return {};
        }

        // Fresh contact
        state_ = TouchGestureState::Pending;
        active_pointer_id_ = pointer_id;
        start_x_ = x;
        start_y_ = y;
        last_x_ = x;
        last_y_ = y;
        down_time_ms_ = now_ms;
        last_time_ms_ = now_ms;
        primary_axis_ = 0;

        samples_.clear();
        samples_.push_back({ x, y, now_ms });

        return {};
    }

    TouchAction OnUpdate(uint32_t pointer_id, float x, float y, uint64_t now_ms, float scale) noexcept {
        if (pointer_id != active_pointer_id_) return {};
        if (state_ != TouchGestureState::Pending && state_ != TouchGestureState::Panning) return {};

        scale = (std::max)(0.5f, scale);
        const float deadzone_px = config_.pan_deadzone_dip * scale;

        RecordSample(x, y, now_ms);

        if (state_ == TouchGestureState::Pending) {
            const float dx = x - start_x_;
            const float dy = y - start_y_;
            const float dist_sq = dx * dx + dy * dy;
            if (dist_sq >= deadzone_px * deadzone_px) {
                // Crossed pan deadzone threshold
                state_ = TouchGestureState::Panning;

                // Lock primary axis to avoid diagonal drift
                if (std::abs(dx) > std::abs(dy) * 1.4f) {
                    primary_axis_ = 1; // Horizontal
                } else if (std::abs(dy) > std::abs(dx) * 1.4f) {
                    primary_axis_ = 2; // Vertical
                } else {
                    primary_axis_ = 0; // Both
                }

                TouchAction action{};
                action.kind = TouchActionKind::PanStart;
                action.x = x;
                action.y = y;
                action.delta_x = (primary_axis_ == 2) ? 0.0f : (x - last_x_);
                action.delta_y = (primary_axis_ == 1) ? 0.0f : (y - last_y_);
                action.primary_axis = primary_axis_;

                last_x_ = x;
                last_y_ = y;
                last_time_ms_ = now_ms;
                return action;
            }
            return {};
        }

        // In Panning state
        TouchAction action{};
        action.kind = TouchActionKind::PanUpdate;
        action.x = x;
        action.y = y;
        action.delta_x = (primary_axis_ == 2) ? 0.0f : (x - last_x_);
        action.delta_y = (primary_axis_ == 1) ? 0.0f : (y - last_y_);
        action.primary_axis = primary_axis_;

        last_x_ = x;
        last_y_ = y;
        last_time_ms_ = now_ms;
        return action;
    }

    TouchAction OnUp(uint32_t pointer_id, float x, float y, uint64_t now_ms, float scale) noexcept {
        if (pointer_id != active_pointer_id_) return {};
        scale = (std::max)(0.5f, scale);

        RecordSample(x, y, now_ms);

        if (state_ == TouchGestureState::Pending) {
            // Check double tap
            const float dt_dist_px = config_.double_tap_distance_dip * scale;
            const float dx = x - last_tap_x_;
            const float dy = y - last_tap_y_;
            const bool is_double = (now_ms - last_tap_time_ms_ <= config_.double_tap_time_ms) &&
                                   (dx * dx + dy * dy <= dt_dist_px * dt_dist_px);

            state_ = TouchGestureState::Ended;
            TouchAction action{};
            action.x = x;
            action.y = y;
            if (is_double) {
                action.kind = TouchActionKind::DoubleTap;
                last_tap_time_ms_ = 0; // Reset double tap
            } else {
                action.kind = TouchActionKind::Tap;
                last_tap_x_ = x;
                last_tap_y_ = y;
                last_tap_time_ms_ = now_ms;
            }
            return action;
        }

        if (state_ == TouchGestureState::Panning) {
            state_ = TouchGestureState::Ended;
            TouchAction action{};
            action.kind = TouchActionKind::PanEnd;
            action.x = x;
            action.y = y;
            action.primary_axis = primary_axis_;

            // Calculate release velocity in DIP/sec from recent samples
            CalculateVelocity(scale, action.velocity_x, action.velocity_y);
            if (primary_axis_ == 2) action.velocity_x = 0.0f;
            if (primary_axis_ == 1) action.velocity_y = 0.0f;

            return action;
        }

        state_ = TouchGestureState::Ended;
        return {};
    }

    TouchAction OnCancel(uint32_t pointer_id) noexcept {
        if (pointer_id == active_pointer_id_ || state_ == TouchGestureState::MultiTouchCancelled) {
            state_ = TouchGestureState::Cancelled;
            TouchAction action{};
            action.kind = TouchActionKind::Cancel;
            return action;
        }
        return {};
    }

    TouchAction CheckHold(uint64_t now_ms) noexcept {
        if (state_ == TouchGestureState::Pending && (now_ms - down_time_ms_ >= config_.hold_timeout_ms)) {
            state_ = TouchGestureState::HoldConsumed;
            TouchAction action{};
            action.kind = TouchActionKind::Hold;
            action.x = last_x_;
            action.y = last_y_;
            return action;
        }
        return {};
    }

    void Reset() noexcept {
        state_ = TouchGestureState::Idle;
        active_pointer_id_ = 0;
        samples_.clear();
        primary_axis_ = 0;
    }

private:
    struct Sample {
        float x = 0.0f;
        float y = 0.0f;
        uint64_t t_ms = 0;
    };

    void RecordSample(float x, float y, uint64_t now_ms) noexcept {
        samples_.push_back({ x, y, now_ms });
        // Keep samples from the last 120 ms (up to 8 samples)
        while (samples_.size() > 8 || (samples_.size() > 2 && now_ms - samples_.front().t_ms > 120)) {
            samples_.pop_front();
        }
    }

    void CalculateVelocity(float scale, float& out_vx, float& out_vy) const noexcept {
        out_vx = 0.0f;
        out_vy = 0.0f;
        if (samples_.size() < 2) return;

        const auto& first = samples_.front();
        const auto& last = samples_.back();
        const float dt_sec = static_cast<float>(last.t_ms - first.t_ms) / 1000.0f;
        if (dt_sec <= 0.005f || dt_sec > 0.200f) return;

        // Velocity in DIP per second
        float vx = ((last.x - first.x) / scale) / dt_sec;
        float vy = ((last.y - first.y) / scale) / dt_sec;

        // Clamp to maximum
        const float speed = std::sqrt(vx * vx + vy * vy);
        if (speed > config_.max_velocity_dip && speed > 0.001f) {
            const float factor = config_.max_velocity_dip / speed;
            vx *= factor;
            vy *= factor;
        }

        // Discard below minimum threshold
        if (std::abs(vx) < config_.min_velocity_dip) vx = 0.0f;
        if (std::abs(vy) < config_.min_velocity_dip) vy = 0.0f;

        out_vx = vx;
        out_vy = vy;
    }

    TouchGestureConfig config_;
    TouchGestureState state_ = TouchGestureState::Idle;
    uint32_t active_pointer_id_ = 0;

    float start_x_ = 0.0f;
    float start_y_ = 0.0f;
    float last_x_ = 0.0f;
    float last_y_ = 0.0f;
    uint64_t down_time_ms_ = 0;
    uint64_t last_time_ms_ = 0;
    int primary_axis_ = 0;

    float last_tap_x_ = 0.0f;
    float last_tap_y_ = 0.0f;
    uint64_t last_tap_time_ms_ = 0;

    std::deque<Sample> samples_;
};

// Exponential release inertia model: v(t) = v0 * exp(-k * dt)
class TouchInertiaTracker {
public:
    explicit TouchInertiaTracker(float friction_k = 4.2f, float min_speed_dip = 20.0f)
        : friction_k_(friction_k), min_speed_dip_(min_speed_dip) {}

    void Start(float vx_dip, float vy_dip) noexcept {
        vx_ = vx_dip;
        vy_ = vy_dip;
        active_ = (std::abs(vx_) >= min_speed_dip_ || std::abs(vy_) >= min_speed_dip_);
    }

    bool IsActive() const noexcept { return active_; }

    // Advances one simulation step with dt_seconds.
    // delta_x_dip and delta_y_dip output the movement distance in DIPs.
    // Returns true if still animating, false if halted.
    bool Step(float dt_sec, float& delta_x_dip, float& delta_y_dip) noexcept {
        if (!active_ || dt_sec <= 0.0f || dt_sec > 0.2f) {
            delta_x_dip = 0.0f;
            delta_y_dip = 0.0f;
            active_ = false;
            return false;
        }

        const float decay = std::exp(-friction_k_ * dt_sec);
        const float avg_vx = vx_ * (1.0f + decay) * 0.5f;
        const float avg_vy = vy_ * (1.0f + decay) * 0.5f;

        delta_x_dip = avg_vx * dt_sec;
        delta_y_dip = avg_vy * dt_sec;

        vx_ *= decay;
        vy_ *= decay;

        if (std::abs(vx_) < min_speed_dip_ && std::abs(vy_) < min_speed_dip_) {
            active_ = false;
        }
        return active_;
    }

    void Stop() noexcept {
        active_ = false;
        vx_ = 0.0f;
        vy_ = 0.0f;
    }

    void StopX() noexcept { vx_ = 0.0f; if (std::abs(vy_) < min_speed_dip_) active_ = false; }
    void StopY() noexcept { vy_ = 0.0f; if (std::abs(vx_) < min_speed_dip_) active_ = false; }

private:
    float friction_k_ = 4.2f;
    float min_speed_dip_ = 20.0f;
    float vx_ = 0.0f;
    float vy_ = 0.0f;
    bool active_ = false;
};

} // namespace pulse::app
