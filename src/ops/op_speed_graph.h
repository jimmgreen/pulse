#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>

namespace pulse::ops {

// The progress card's speed history (speed graph).
// OpsManager records it in the background for the running task, so a card
// first shown late (or reopened after being hidden) draws the whole run;
// the card keeps sampling the same way while it is visible.
struct OpGraphPoint {
    float x = 0.0f;   // progress 0..1, or 0 on the rolling time window
    double v = 0.0;   // bytes/s or items/s, the task's speed basis
};

struct OpSpeedGraph {
    static constexpr size_t kMaxProgressPoints = 240;
    static constexpr size_t kMaxTimePoints = 120;
    static constexpr uint64_t kIntervalMs = 250;

    std::deque<OpGraphPoint> points;
    bool by_progress = true;   // x is progress; false = last kMaxTimePoints samples
    uint64_t last_tick = 0;

    void Reset() {
        points.clear();
        by_progress = true;
        last_tick = 0;
    }

    // Completion is an event, not a periodic sample. Preserve the last measured
    // rate rather than adding the terminal status's zero speed.
    void Complete() {
        if (!by_progress || points.empty() || points.back().x >= 1.0f) return;
        points.push_back({1.0f, points.back().v});
    }

    bool Due(uint64_t now) const { return last_tick == 0 || now - last_tick >= kIntervalMs; }

    // percent < 0: no measurable progress, so a rolling time window instead.
    void Sample(uint64_t now, float percent, double value) {
        if (!Due(now)) return;
        last_tick = now;
        if (value < 0.0) value = 0.0;
        if (percent >= 0.0f) {
            float x = percent / 100.0f;
            if (x < 0.0f) x = 0.0f;
            if (x > 1.0f) x = 1.0f;
            if (!by_progress) { points.clear(); by_progress = true; }
            if (!points.empty() && x + 0.02f < points.back().x) points.clear();  // a new phase
            if (!points.empty() && x <= points.back().x + 0.0005f) {
                points.back().v = value;
                return;
            }
            points.push_back({ x, value });
            if (points.size() > kMaxProgressPoints) {
                // Halve the resolution, keeping the newest point exact.
                std::deque<OpGraphPoint> thinned;
                for (size_t i = 0; i < points.size(); i += 2) thinned.push_back(points[i]);
                if ((points.size() & 1) == 0) thinned.push_back(points.back());
                points.swap(thinned);
            }
        } else {
            if (by_progress) { points.clear(); by_progress = false; }
            points.push_back({ 0.0f, value });
            while (points.size() > kMaxTimePoints) points.pop_front();
        }
    }
};

} // namespace pulse::ops
