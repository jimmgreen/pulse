#include "../app/touch_gesture.h"
#include <cstdio>
#include <cassert>
#include <initializer_list>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    std::printf("[%s] %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++g_failures;
}

void TestTap() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    const auto act_down = reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    Check(act_down.kind == TouchActionKind::None, "Tap: Down produces no immediate action");
    Check(reducer.State() == TouchGestureState::Pending, "Tap: State is Pending after Down");

    const auto act_up = reducer.OnUp(1, 102.0f, 101.0f, 1080, 1.0f);
    Check(act_up.kind == TouchActionKind::Tap, "Tap: Clean release produces Tap");
    Check(reducer.State() == TouchGestureState::Ended, "Tap: State is Ended after Up");
}

void TestDoubleTap() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    // First tap
    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    const auto act1 = reducer.OnUp(1, 101.0f, 101.0f, 1060, 1.0f);
    Check(act1.kind == TouchActionKind::Tap, "DoubleTap: First release is Tap");

    // Second tap within 200ms and close distance
    reducer.OnDown(1, 104.0f, 102.0f, 1200, 1.0f);
    const auto act2 = reducer.OnUp(1, 103.0f, 102.0f, 1260, 1.0f);
    Check(act2.kind == TouchActionKind::DoubleTap, "DoubleTap: Second release within interval is DoubleTap");

    // Third tap after timeout (500ms later)
    reducer.OnDown(1, 100.0f, 100.0f, 2000, 1.0f);
    const auto act3 = reducer.OnUp(1, 100.0f, 100.0f, 2060, 1.0f);
    Check(act3.kind == TouchActionKind::Tap, "DoubleTap: Tap after interval reset is single Tap");
}

void TestHold() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);

    // Before timeout
    const auto act_early = reducer.CheckHold(1300);
    Check(act_early.kind == TouchActionKind::None, "Hold: Early check produces no action");

    // At/after 600ms timeout
    const auto act_hold = reducer.CheckHold(1650);
    Check(act_hold.kind == TouchActionKind::Hold, "Hold: 650ms check fires Hold action");
    Check(reducer.State() == TouchGestureState::HoldConsumed, "Hold: State is HoldConsumed");

    // Release after hold consumed
    const auto act_up = reducer.OnUp(1, 100.0f, 100.0f, 1800, 1.0f);
    Check(act_up.kind == TouchActionKind::None, "Hold: Release after HoldConsumed does NOT fire Tap");
}

void TestPanningAndAxisLock() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);

    // Small move within 8 DIP deadzone
    const auto act_deadzone = reducer.OnUpdate(1, 103.0f, 102.0f, 1020, 1.0f);
    Check(act_deadzone.kind == TouchActionKind::None, "Pan: Sub-deadzone movement produces no pan");

    // Crossing deadzone horizontally (dx=20, dy=2)
    const auto act_start = reducer.OnUpdate(1, 120.0f, 102.0f, 1050, 1.0f);
    Check(act_start.kind == TouchActionKind::PanStart, "Pan: Crossing deadzone fires PanStart");
    Check(act_start.primary_axis == 1, "Pan: Horizontal motion locks horizontal axis");
    Check(act_start.delta_x > 0.0f, "Pan: delta_x is positive");
    Check(act_start.delta_y == 0.0f, "Pan: delta_y is locked to zero on horizontal axis");

    // PanUpdate
    const auto act_update = reducer.OnUpdate(1, 140.0f, 105.0f, 1080, 1.0f);
    Check(act_update.kind == TouchActionKind::PanUpdate, "Pan: Continuing movement fires PanUpdate");

    // PanEnd with release velocity
    const auto act_end = reducer.OnUp(1, 160.0f, 105.0f, 1100, 1.0f);
    Check(act_end.kind == TouchActionKind::PanEnd, "Pan: Release fires PanEnd");
    Check(act_end.velocity_x > 0.0f, "Pan: Release calculates positive horizontal velocity");
    Check(act_end.velocity_y == 0.0f, "Pan: Vertical velocity is zero due to horizontal axis lock");
}

void TestMultiTouchCancellation() {
    using namespace pulse::app;
    TouchGestureReducer reducer;

    reducer.OnDown(1, 100.0f, 100.0f, 1000, 1.0f);
    Check(reducer.State() == TouchGestureState::Pending, "MultiTouch: Finger 1 starts Pending");

    // Finger 2 arrives while finger 1 is pending
    const auto act_multi = reducer.OnDown(2, 250.0f, 250.0f, 1020, 1.0f);
    Check(act_multi.kind == TouchActionKind::Cancel, "MultiTouch: Finger 2 fires Cancel action");
    Check(reducer.State() == TouchGestureState::MultiTouchCancelled,
          "MultiTouch: State transitions to MultiTouchCancelled");

    // Updates and releases do not fire Tap or Pan
    const auto act_up1 = reducer.OnUp(1, 100.0f, 100.0f, 1080, 1.0f);
    Check(act_up1.kind == TouchActionKind::None, "MultiTouch: Finger 1 Up does not fire Tap");
}

void TestInertiaSimulation() {
    using namespace pulse::app;
    TouchInertiaTracker tracker(4.2f, 20.0f);

    tracker.Start(800.0f, 0.0f);
    Check(tracker.IsActive(), "Inertia: Tracker is active after Start");

    float total_dx = 0.0f;
    float total_dy = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;

    int steps = 0;
    while (tracker.Step(0.016f, dx, dy)) {
        total_dx += dx;
        total_dy += dy;
        ++steps;
        if (steps > 200) break; // Safeguard against infinite loop
    }

    Check(!tracker.IsActive(), "Inertia: Tracker naturally halts after friction decay");
    Check(total_dx > 100.0f, "Inertia: Total displacement is significant and positive");
    Check(total_dy == 0.0f, "Inertia: Zero initial vy produces zero vertical displacement");
    Check(steps > 10 && steps < 150, "Inertia: Step count is bounded within reasonable range");
}

void TestDirectTouchScrollOwnership() {
    using namespace pulse::app;
    for (const bool horizontal : { false, true }) {
        float x = 70.0f;
        float y = 200.0f;
        float target_x = 900.0f; // A previous wheel animation still has distance left.
        float target_y = 800.0f;
        float& position = horizontal ? x : y;
        float& target = horizontal ? target_x : target_y;
        const float previous = position;
        const auto drag = ApplyTouchScrollAxis(position, target, -40.0f, 1000.0f);
        Check(drag.changed && !drag.blocked && position == previous + 40.0f,
              "Touch ownership: direct drag follows the finger on either scroll axis");
        Check(target == position,
              "Touch ownership: drag replaces a stale wheel animation target");

        TouchInertiaTracker tracker;
        tracker.StartForScrollAxis(-600.0f, -600.0f, horizontal);
        bool monotonic = true;
        bool synchronized = true;
        int frames = 0;
        while (tracker.IsActive() && frames < 240) {
            float dx = 0.0f;
            float dy = 0.0f;
            const float before = position;
            if (tracker.Step(0.016f, dx, dy)) {
                const auto step = ApplyTouchScrollAxis(position, target,
                    horizontal ? dx : dy, 1000.0f);
                if (step.blocked) tracker.Stop();
            }
            // Reproduce the paint-time wheel response between touch ticks:
            // a synchronized target must never pull against direct manipulation.
            synchronized = synchronized && target == position;
            const float before_paint = position;
            position += (target - position) * 0.25f;
            synchronized = synchronized && position == before_paint;
            monotonic = monotonic && position >= before;
            ++frames;
        }
        Check(monotonic && synchronized && frames > 1 && !tracker.IsActive(),
              "Touch ownership: release inertia stays monotonic across interleaved wheel frames");
        Check(horizontal ? y == 200.0f : x == 70.0f,
              "Touch ownership: scrolling does not move the unsupported axis");
    }
}

void TestTouchInertiaBoundaries() {
    using namespace pulse::app;
    for (const bool horizontal : { false, true }) {
        for (const float direction : { -1.0f, 1.0f }) {
            float position = direction > 0.0f ? 1.0f : 99.0f;
            float target = 50.0f;
            TouchInertiaTracker tracker;
            tracker.StartForScrollAxis(direction * 2000.0f, direction * 2000.0f, horizontal);
            float dx = 0.0f;
            float dy = 0.0f;
            Check(tracker.Step(0.016f, dx, dy), "Touch boundary: release begins in motion");
            const auto step = ApplyTouchScrollAxis(position, target,
                horizontal ? dx : dy, 100.0f);
            if (step.blocked) tracker.Stop();
            Check(step.blocked && !tracker.IsActive() &&
                  position == (direction > 0.0f ? 0.0f : 100.0f) && target == position,
                  "Touch boundary: both ends stop inertia immediately and synchronize the target");
            dx = 1.0f;
            dy = 1.0f;
            Check(!tracker.Step(0.016f, dx, dy) && dx == 0.0f && dy == 0.0f,
                  "Touch boundary: later ticks cannot produce edge jitter");
        }
    }

    float position = 75.0f;
    float target = 600.0f;
    const auto empty = ApplyTouchScrollAxis(position, target, -10.0f, 0.0f);
    Check(empty.blocked && position == 0.0f && target == 0.0f,
          "Touch boundary: content shrinking to empty clamps and stops motion");
    position = 75.0f;
    const auto shrink = ApplyTouchScrollAxis(position, target, -5.0f, 30.0f);
    Check(shrink.blocked && position == 30.0f && target == position,
          "Touch boundary: refreshed smaller bounds never leave a stale target");

    TouchInertiaTracker tracker;
    tracker.StartForScrollAxis(800.0f, 0.0f, false);
    Check(!tracker.IsActive(), "Touch axis: horizontal velocity cannot animate a vertical target");
    tracker.StartForScrollAxis(0.0f, 800.0f, true);
    Check(!tracker.IsActive(), "Touch axis: vertical velocity cannot animate a horizontal target");
}

void TestCancelledPanCannotRestartInertia() {
    using namespace pulse::app;
    TouchGestureReducer reducer;
    reducer.OnDown(1, 100.0f, 200.0f, 1000, 1.0f);
    Check(reducer.OnUpdate(1, 100.0f, 160.0f, 1020, 1.0f).kind == TouchActionKind::PanStart,
          "Touch cancel: begin with a real pan");
    Check(reducer.OnCancel(1).kind == TouchActionKind::Cancel,
          "Touch cancel: cancellation consumes the active pan");
    Check(reducer.OnUpdate(1, 100.0f, 120.0f, 1040, 1.0f).kind == TouchActionKind::None,
          "Touch cancel: stale updates do not continue a canceled pan");
    const auto up = reducer.OnUp(1, 100.0f, 100.0f, 1060, 1.0f);
    Check(up.kind == TouchActionKind::None && up.velocity_x == 0.0f && up.velocity_y == 0.0f,
          "Touch cancel: stale release cannot restart inertia after target invalidation");
    TouchInertiaTracker tracker;
    tracker.Start(10.0f, 10.0f);
    tracker.StopX();
    tracker.StopY();
    Check(!tracker.IsActive(), "Touch cancel: stopping an axis cannot activate a rejected release");
    tracker.Start(100.0f, 100.0f);
    float dx = 0.0f;
    float dy = 0.0f;
    tracker.Step(1.0f, dx, dy);
    tracker.StopX();
    Check(!tracker.IsActive(), "Touch cancel: stopping an axis cannot revive an expired simulation");
}

void TestInertiaSettlesWithoutCutoff() {
    using namespace pulse::app;
    for (const bool horizontal : {false, true}) {
        for (const float direction : {-1.0f, 1.0f}) {
            TouchInertiaTracker tracker;
            tracker.StartForScrollAxis(direction * 20.0f, direction * 20.0f, horizontal);
            float dx = 0.0f;
            float dy = 0.0f;
            Check(tracker.Step(0.016f, dx, dy) && tracker.IsActive(),
                  "Settle: launch threshold does not truncate the first slow frame");
            float previous_distance = std::abs(horizontal ? dx : dy);
            bool smooth = previous_distance > 0.0f;
            bool final_step_applied = false;
            int frames = 1;
            while (tracker.IsActive() && frames < 120) {
                const bool moved = tracker.Step(0.016f, dx, dy);
                const float distance = std::abs(horizontal ? dx : dy);
                smooth = smooth && moved && distance <= previous_distance + 0.00001f &&
                    (horizontal ? dy == 0.0f : dx == 0.0f) &&
                    (horizontal ? dx : dy) * direction >= 0.0f;
                if (!tracker.IsActive()) final_step_applied = moved && distance > 0.0f && distance < 0.02f;
                previous_distance = distance;
                ++frames;
            }
            Check(smooth && frames > 2 && !tracker.IsActive(),
                  "Settle: slow tail decreases monotonically to rest on either axis and direction");
            Check(final_step_applied,
                  "Settle: final subpixel displacement is applied even when no next frame is needed");
            Check(!tracker.Step(0.016f, dx, dy) && dx == 0.0f && dy == 0.0f,
                  "Settle: subsequent frames stay completely stationary");
        }
    }
}

void TestInertiaIsFrameRateIndependent() {
    using namespace pulse::app;
    const float irregular[] = {0.006f, 0.011f, 0.008f, 0.024f, 0.007f, 0.016f};
    constexpr double speed = 800.0;
    constexpr double friction = 4.2;
    constexpr double brake = 20.0;
    const double stop_time = std::log1p(speed / brake) / friction;
    const double expected_distance = speed / friction - brake * stop_time;
    for (const float frame_rate : {60.0f, 90.0f, 120.0f, 144.0f, 0.0f}) {
        TouchInertiaTracker tracker;
        tracker.Start(0.0f, -static_cast<float>(speed));
        double distance = 0.0;
        double elapsed = 0.0;
        float position = 300.0f;
        float target = 900.0f;
        bool monotonic = true;
        int frames = 0;
        while (tracker.IsActive() && frames < 1000) {
            const float dt = frame_rate > 0.0f ? 1.0f / frame_rate : irregular[frames % 6];
            float dx = 0.0f;
            float dy = 0.0f;
            const float before = position;
            if (tracker.Step(dt, dx, dy)) {
                distance -= dy;
                ApplyTouchScrollAxis(position, target, dy, 2000.0f);
            }
            elapsed += dt;
            monotonic = monotonic && position >= before && target == position;
            ++frames;
        }
        Check(!tracker.IsActive() && std::abs(distance - expected_distance) < 0.002,
              "Frame pacing: 60/90/120/144 Hz and irregular frames preserve analytic release distance");
        Check(monotonic && elapsed >= stop_time - 0.0001 && elapsed < stop_time + 0.025,
              "Frame pacing: no reversal or stale target while settling in finite time");
    }
}

} // namespace

int main() {
    TestTap();
    TestDoubleTap();
    TestHold();
    TestPanningAndAxisLock();
    TestMultiTouchCancellation();
    TestInertiaSimulation();
    TestDirectTouchScrollOwnership();
    TestTouchInertiaBoundaries();
    TestCancelledPanCannotRestartInertia();
    TestInertiaSettlesWithoutCutoff();
    TestInertiaIsFrameRateIndependent();

    std::printf("touch_gesture_test: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
