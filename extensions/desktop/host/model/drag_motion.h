// drag_motion.h - Reference-aligned macOS SwiftUI spring physics and smooth-sliding
// ghost preview state shared by desktop tiles and file groups.
#pragma once
#include "host/model/desktop_model.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pulse::desktop::model {

// Exact macOS SwiftUI spring & VSync ghost-glide parameters aligned 1:1 with widget_demo.cpp.
inline constexpr float kSpringResponseSec = 0.40f;
inline constexpr float kSpringDampingRatio = 0.86f;
inline constexpr float kSpringDurationSec = 0.65f;
inline constexpr double kGhostSmoothTauMs = 42.0;

// Evaluates critically-damped / underdamped spring curve in [0, ~1.02] -> 1.0.
inline float EvaluateSpringCurve(float t_sec,
                                 float response = kSpringResponseSec,
                                 float damping = kSpringDampingRatio) noexcept {
    if (t_sec <= 0.0f) return 0.0f;
    const float safe_resp = std::max(0.05f, response);
    const float w = 2.0f * 3.14159265f / safe_resp;
    if (damping >= 1.0f) {
        const float e = std::exp(-w * t_sec);
        return 1.0f - e * (1.0f + w * t_sec);
    }
    const float wd = w * std::sqrt(std::max(1e-6f, 1.0f - damping * damping));
    const float e = std::exp(-damping * w * t_sec);
    return 1.0f - e * (std::cos(wd * t_sec) + (damping * w / wd) * std::sin(wd * t_sec));
}

// Translucent drag-target ghost preview with exponential smooth slide (tau = 42 ms).
struct SmoothGhostState {
    bool visible = false;
    bool valid = true;
    bool merge_armed = false;
    float corner_radius_dip = 16.0f;
    double current_x = 0.0;
    double current_y = 0.0;
    RectI target{};

    void StartAt(const RectI& initial_target, float radius_dip) noexcept {
        visible = !initial_target.empty();
        valid = true;
        merge_armed = false;
        corner_radius_dip = radius_dip;
        target = initial_target;
        current_x = static_cast<double>(initial_target.x);
        current_y = static_cast<double>(initial_target.y);
    }

    void SetTarget(const RectI& next_target, bool is_valid, bool is_merge_armed) noexcept {
        if (next_target.empty()) {
            Hide();
            return;
        }
        if (!visible) {
            StartAt(next_target, corner_radius_dip);
        } else {
            target = next_target;
        }
        valid = is_valid;
        merge_armed = is_merge_armed;
    }

    void Hide() noexcept {
        visible = false;
        valid = true;
        merge_armed = false;
        target = {};
    }

    // Advances smooth ghost slide toward target. Returns true if another frame is needed.
    bool Step(double dt_ms, bool still_dragging, bool has_active_springs) noexcept {
        if (!visible) return false;
        const double safe_dt = std::clamp(dt_ms, 0.0, 50.0);
        if (safe_dt > 0.0) {
            const double k = 1.0 - std::exp(-safe_dt / kGhostSmoothTauMs);
            current_x += (static_cast<double>(target.x) - current_x) * k;
            current_y += (static_cast<double>(target.y) - current_y) * k;
        }

        const bool settled = std::fabs(static_cast<double>(target.x) - current_x) < 0.6 &&
                             std::fabs(static_cast<double>(target.y) - current_y) < 0.6;
        if (settled) {
            current_x = static_cast<double>(target.x);
            current_y = static_cast<double>(target.y);
        }
        if (!still_dragging && settled && !has_active_springs) {
            visible = false;
            return false;
        }
        return still_dragging || !settled || has_active_springs;
    }

    RectI CurrentRect() const noexcept {
        if (!visible || target.empty()) return {};
        return RectI{
            static_cast<int>(std::lround(current_x)),
            static_cast<int>(std::lround(current_y)),
            target.w,
            target.h,
        };
    }
};

struct SpringCardMove {
    std::string id;
    PointI from{};
    PointI to{};
    double start_ms = 0.0;
    float response = kSpringResponseSec;
    float damping = kSpringDampingRatio;
    float duration_sec = kSpringDurationSec;
};

// Manages concurrent post-drop spring snap-back visual moves (zero mutation of committed model coords).
class SpringMoveList {
public:
    bool Empty() const noexcept { return moves_.empty(); }
    size_t Size() const noexcept { return moves_.size(); }

    void Clear() noexcept { moves_.clear(); }

    void Cancel(std::string_view id) {
        moves_.erase(std::remove_if(moves_.begin(), moves_.end(),
                                    [&](const SpringCardMove& m) { return m.id == id; }),
                     moves_.end());
    }

    void AddOrReplace(std::string id, PointI from, PointI to, double now_ms,
                      float response = kSpringResponseSec,
                      float damping = kSpringDampingRatio,
                      float duration_sec = kSpringDurationSec) {
        Cancel(id);
        if (id.empty() || from == to) return;
        moves_.push_back(SpringCardMove{std::move(id), from, to, now_ms, response, damping, duration_sec});
    }

    PointI CurrentPosition(std::string_view id, PointI fallback, double now_ms) const noexcept {
        for (const auto& m : moves_) {
            if (m.id != id) continue;
            const float t_sec = static_cast<float>((now_ms - m.start_ms) / 1000.0);
            if (t_sec >= m.duration_sec) return m.to;
            const float p = EvaluateSpringCurve(t_sec, m.response, m.damping);
            return PointI{
                static_cast<int>(std::lround(static_cast<double>(m.from.x) +
                                             static_cast<double>(m.to.x - m.from.x) * p)),
                static_cast<int>(std::lround(static_cast<double>(m.from.y) +
                                             static_cast<double>(m.to.y - m.from.y) * p)),
            };
        }
        return fallback;
    }

    // Evaluates all active springs into `out_positions`. Returns true while any spring remains active.
    bool Step(double now_ms, std::unordered_map<std::string, PointI>& out_positions) {
        out_positions.clear();
        for (size_t i = 0; i < moves_.size();) {
            const auto& m = moves_[i];
            const float t_sec = static_cast<float>((now_ms - m.start_ms) / 1000.0);
            if (t_sec >= m.duration_sec) {
                moves_.erase(moves_.begin() + static_cast<ptrdiff_t>(i));
                continue;
            }
            const float p = EvaluateSpringCurve(t_sec, m.response, m.damping);
            out_positions[m.id] = PointI{
                static_cast<int>(std::lround(static_cast<double>(m.from.x) +
                                             static_cast<double>(m.to.x - m.from.x) * p)),
                static_cast<int>(std::lround(static_cast<double>(m.from.y) +
                                             static_cast<double>(m.to.y - m.from.y) * p)),
            };
            ++i;
        }
        return !moves_.empty();
    }

private:
    std::vector<SpringCardMove> moves_;
};

} // namespace pulse::desktop::model
